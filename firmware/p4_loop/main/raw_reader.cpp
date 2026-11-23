#include "raw_reader.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>

#include "diskio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "ff.h"
#include "freertos/task.h"
#include "sd_card.h"
#include "usb_stick.h"

static const char *TAG = "raw";
static std::atomic<uint64_t> s_io_bytes{0};

RawFile::~RawFile() { heap_caps_free(blk_); }

static constexpr size_t kBlk = 4096;
static constexpr size_t kFatWin = 64 * 1024;

// Follow the FAT32 chain from sclust, reading the FAT in kFatWin pieces with
// disk_read(), and merge consecutive clusters into extents.
bool RawFile::build_extents_fat32(void *fsv, uint32_t sclust, const char *vfs_path)
{
    FATFS *fs = static_cast<FATFS *>(fsv);
    const uint64_t cl_bytes = (uint64_t)fs->csize * ss_;
    uint64_t need = (size_ + cl_bytes - 1) / cl_bytes;  // clusters holding the file
    uint8_t *win = static_cast<uint8_t *>(heap_caps_aligned_alloc(64, kFatWin, MALLOC_CAP_SPIRAM));
    if (!win) return false;
    const uint32_t win_secs = kFatWin / ss_;
    LBA_t win_first = (LBA_t)-1;  // first FAT sector held in win
    uint32_t win_n = 0;
    auto next = [&](uint32_t c, uint32_t &out) -> bool {
        const uint64_t byte = (uint64_t)c * 4;
        const LBA_t sec = fs->fatbase + (LBA_t)(byte / ss_);
        const LBA_t fat_end = fs->fatbase + fs->fsize;  // fsize = sectors per FAT
        if (c >= fs->n_fatent || sec >= fat_end) return false;
        if (win_first == (LBA_t)-1 || sec < win_first || sec >= win_first + win_n) {
            win_n = (uint32_t)std::min<LBA_t>(win_secs, fat_end - sec);
            if (disk_read(fs->pdrv, win, sec, win_n) != RES_OK) return false;
            win_first = sec;
        }
        const uint8_t *e = win + (sec - win_first) * ss_ + byte % ss_;
        out = (e[0] | (e[1] << 8) | (e[2] << 16) | ((uint32_t)e[3] << 24)) & 0x0FFFFFFF;
        return true;
    };
    bool ok = true;
    uint32_t c = sclust;
    uint64_t off = 0;
    while (need && ok) {
        if (c < 2 || c >= fs->n_fatent) { ok = false; break; }
        uint32_t run = 1, n = 0;
        while (run < need) {
            if (!next(c + run - 1, n)) { ok = false; break; }
            if (n != c + run || n >= fs->n_fatent) break;
            run++;
        }
        if (!ok) break;
        ext_.push_back({off, run * cl_bytes, (uint32_t)(fs->database + (LBA_t)(c - 2) * fs->csize)});
        off += run * cl_bytes;
        need -= run;
        if (need) {
            if (!next(c + run - 1, n)) { ok = false; break; }  // n: first cluster of the next run
            c = n;
        }
    }
    heap_caps_free(win);
    if (!ok) ESP_LOGW(TAG, "%s: broken FAT chain at cluster %u", vfs_path, (unsigned)c);
    return ok;
}

bool RawFile::open(const char *vfs_path)
{
    ext_.clear();
    // "/usb/media/x.avi" -> drive of the USB mount + "media/x.avi". The drive
    // comes from the mount, not from matching names: the microSD could hold a
    // file of the same name. No stat(): its 32-bit off_t breaks at 2 GB,
    // while FAT32 files reach 4 GB (FatFs sizes are unsigned 32-bit).
    int drive = -1;
    if (strncmp(vfs_path, USB_MOUNT_POINT "/", sizeof(USB_MOUNT_POINT)) == 0) drive = usb_stick_drive();
    else if (strncmp(vfs_path, SD_MOUNT_POINT "/", sizeof(SD_MOUNT_POINT)) == 0) drive = sd_card_drive();
    const char *name = vfs_path[0] == '/' ? strchr(vfs_path + 1, '/') : nullptr;
    if (drive < 0 || !name) return false;
    name++;
    if (!blk_) blk_ = static_cast<uint8_t *>(heap_caps_aligned_alloc(64, kBlk, MALLOC_CAP_SPIRAM));
    if (!blk_) return false;
    blk_off_ = UINT64_MAX;

    // FIL embeds a FF_MAX_SS (4 KB) sector buffer: keep it off the task stack
    // (it overflowed the 4 KB reader stack, 2026-11-23).
    std::unique_ptr<FIL> filp(new FIL);
    FIL &fil = *filp;
    char path[300];
    snprintf(path, sizeof(path), "%d:/%s", drive, name);
    if (f_open(&fil, path, FA_READ) != FR_OK) return false;
    FATFS *fs = fil.obj.fs;
#if FF_MAX_SS != FF_MIN_SS
    ss_ = fs->ssize;
#else
    ss_ = FF_MAX_SS;
#endif
    pdrv_ = fs->pdrv;
    size_ = f_size(&fil);
    const uint64_t cl_bytes = (uint64_t)fs->csize * ss_;
    bool ok;
    if (fs->fs_type == FS_FAT32) {
        ok = build_extents_fat32(fs, fil.obj.sclust, vfs_path);
    } else {
        // FAT12/16/exFAT: FatFs link map (sector-by-sector FAT reads).
        // tbl[0] = table size, then (run length, start cluster) pairs.
        static DWORD tbl[256];
        tbl[0] = sizeof(tbl) / sizeof(tbl[0]);
        fil.cltbl = tbl;
        ok = f_lseek(&fil, CREATE_LINKMAP) == FR_OK;
        uint64_t off = 0;
        for (DWORD *p = tbl + 1; ok && *p; p += 2) {
            ext_.push_back({off, p[0] * cl_bytes, (uint32_t)(fs->database + (LBA_t)(p[1] - 2) * fs->csize)});
            off += p[0] * cl_bytes;
        }
    }
    f_close(&fil);
    if (!ok || ext_.empty()) {
        ESP_LOGW(TAG, "%s: no cluster map - using FatFs reads", vfs_path);
        ext_.clear();
        return false;
    }
    ESP_LOGI(TAG, "%s: drive %d, %u fragment(s), cluster %u KB, sector %u B", vfs_path, drive,
             (unsigned)ext_.size(), (unsigned)(cl_bytes / 1024), (unsigned)ss_);
    return true;
}

bool RawFile::pread(uint64_t off, void *dstv, size_t n)
{
    uint8_t *dst = static_cast<uint8_t *>(dstv);
    if (off > size_ || n > size_ - off) return false;
    // Small read inside one 4 KB block: serve it from the block cache.
    const uint64_t b = off / kBlk * kBlk;
    if (off + n <= b + kBlk) {
        if (blk_off_ != b) {
            const size_t len = (size_t)std::min<uint64_t>(kBlk, (size_ - b + ss_ - 1) / ss_ * ss_);
            if (!read(b, blk_, len)) { blk_off_ = UINT64_MAX; return false; }
            blk_off_ = b;
        }
        memcpy(dst, blk_ + (off - b), n);
        return true;
    }
    // Larger read (the AVI idx1 table): sector-aligned pieces of at most
    // 64 KB through one temporary buffer. One USB request must stay below
    // 128 KB (a 128 KB request timed out, 2026-11-23), and a long video's
    // idx1 is larger than that.
    constexpr size_t kPiece = 64 * 1024;
    uint8_t *tmp = static_cast<uint8_t *>(heap_caps_aligned_alloc(64, kPiece, MALLOC_CAP_SPIRAM));
    if (!tmp) return false;
    bool ok = true;
    while (ok && n) {
        const uint64_t a = off / ss_ * ss_;
        const size_t head = (size_t)(off - a);
        const size_t take = std::min(n, kPiece - head);
        const size_t span = (size_t)((head + take + ss_ - 1) / ss_ * ss_);
        ok = read(a, tmp, span);
        if (ok) memcpy(dst, tmp + head, take);
        dst += take; off += take; n -= take;
    }
    heap_caps_free(tmp);
    return ok;
}

bool RawFile::read(uint64_t off, uint8_t *dst, size_t len)
{
    while (len) {
        auto it = std::find_if(ext_.begin(), ext_.end(),
                               [&](const Extent &e) { return off >= e.off && off < e.off + e.len; });
        if (it == ext_.end()) return false;
        const size_t n = (size_t)std::min<uint64_t>(len, it->off + it->len - off);
        const uint32_t sector = it->sector + (uint32_t)((off - it->off) / ss_);
        if (disk_read(pdrv_, dst, sector, n / ss_) != RES_OK) return false;
        s_io_bytes += n;
        dst += n;
        off += n;
        len -= n;
    }
    return true;
}

uint64_t ReadAhead::take_io_bytes() { return s_io_bytes.exchange(0); }

bool ReadAhead::start(RawFile *f, uint64_t begin, uint64_t end, size_t chunk, size_t ring_bytes)
{
    f_ = f;
    const uint64_t ss = f->sector_size();
    begin_ = begin / ss * ss;
    end_ = (end + ss - 1) / ss * ss;
    chunk_ = chunk / ss * ss;
    ring_ = ring_bytes / chunk_ * chunk_;
    // 64-byte aligned PSRAM: the SD host can DMA straight into it.
    buf_ = static_cast<uint8_t *>(heap_caps_aligned_alloc(64, ring_, MALLOC_CAP_SPIRAM));
    if (!buf_) {
        ESP_LOGE(TAG, "no PSRAM for a %u KB read-ahead ring", (unsigned)(ring_ / 1024));
        return false;
    }
    if (end_ <= begin_) {
        heap_caps_free(buf_);
        buf_ = nullptr;
        return false;
    }
    written_ = 0;
    released_ = 0;
    stop_ = false;
    done_ = false;
    error_ = false;
    ESP_LOGI(TAG, "read-ahead ring %u KB, %u KB per read", (unsigned)(ring_ / 1024), (unsigned)(chunk_ / 1024));
    // Above the player's reader (6) so storage I/O never waits behind it.
    if (xTaskCreatePinnedToCore(io_task, "readahead", 4096, this, 7, nullptr, 0) != pdPASS) {
        done_ = true;  // nothing to wait for in stop()
        heap_caps_free(buf_);
        buf_ = nullptr;
        return false;
    }
    return true;
}

void ReadAhead::io_task(void *arg)
{
    auto *ra = static_cast<ReadAhead *>(arg);
    const uint64_t span = ra->end_ - ra->begin_;
    while (!ra->stop_) {
        const uint64_t w = ra->written_.load();
        // All sizes are sector multiples, so every read lands sector-aligned
        // both in the file and in the ring. A read never crosses the file's
        // end (the loop point) or the ring's end.
        const uint64_t in_file = w % span;
        const size_t ri = (size_t)(w % ra->ring_);
        size_t n = (size_t)std::min<uint64_t>(ra->chunk_, span - in_file);
        n = std::min(n, ra->ring_ - ri);
        if (w + n - ra->released_.load() > ra->ring_) {  // ring full: decoder is behind
            vTaskDelay(1);
            continue;
        }
        if (!ra->f_->read(ra->begin_ + in_file, ra->buf_ + ri, n)) {
            ESP_LOGE(TAG, "raw read failed at %llu", (unsigned long long)(ra->begin_ + in_file));
            ra->error_ = true;
            break;
        }
        ra->written_.store(w + n);
    }
    ra->done_ = true;
    vTaskDelete(nullptr);
}

const uint8_t *ReadAhead::acquire(uint64_t spos, size_t len, uint8_t *scratch)
{
    while (written_.load() < spos + len) {
        if (error_ || stop_) return nullptr;
        vTaskDelay(1);
    }
    const size_t ri = (size_t)(spos % ring_);
    if (ri + len <= ring_) return buf_ + ri;
    const size_t first = ring_ - ri;  // straddles the ring end: copy once
    memcpy(scratch, buf_ + ri, first);
    memcpy(scratch + first, buf_, len - first);
    return scratch;
}

void ReadAhead::stop()
{
    stop_ = true;
    while (!done_) vTaskDelay(pdMS_TO_TICKS(5));
    heap_caps_free(buf_);
    buf_ = nullptr;
}
