#pragma once
// Raw sector reads of a FAT file plus a zero-copy read-ahead ring.
//
// Why (measured 2026-11-23): FatFs issues at most one cluster per disk_read,
// and this 32 GB card has 32 KB clusters, so every SD command or USB BOT
// transfer moved 32 KB and paid its fixed overhead each time: 10.5 MB/s on the
// SD slot (bus ceiling 20 MB/s), 13.5 MB/s over USB HS, while the card reads
// 92 MB/s on a Mac. Here the file's cluster runs are looked up once through
// FatFs's link map and read with large disk_read() calls into one PSRAM ring
// by a task that runs ahead of the decoder.
//
// Zero-copy: a first version copied frames out of 127 KB chunks with memcpy;
// under PSRAM contention (scan-out + decode + USB DMA) that copy ran at
// ~9 MB/s and became the bottleneck (and starved IDLE0). Now the decoder
// reads each frame straight from the ring; only a frame that straddles the
// ring's end is copied once.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "avi_index.h"

// Boot time (2026-11-23): the file's cluster chain used to be walked twice at
// ~250 ms each over USB, once by the VFS open() (CONFIG_FATFS_USE_FASTSEEK
// builds a link map on every read-only open) and once by FatFs CREATE_LINKMAP
// here, because FatFs reads the FAT one 512-byte sector at a time. Now the
// chain is walked once, from the FAT read in 64 KB pieces, and the index,
// the header check and the read-ahead all read through this object; the VFS
// fd is opened only when the raw path fails.
class RawFile : public ByteSource {
public:
    ~RawFile() override;
    // vfs_path like "/usb/media/name.avi" (any folder on the volume).
    bool open(const char *vfs_path);
    // off and len multiples of the sector size; dst 64-byte aligned.
    bool read(uint64_t off, uint8_t *dst, size_t len);
    // Any offset/length/buffer (small reads go through a 4 KB block cache).
    // Not thread-safe; used before the read-ahead task starts.
    bool pread(uint64_t off, void *dst, size_t n) override;
    uint64_t size() const override { return size_; }
    size_t sector_size() const { return ss_; }
    size_t fragments() const { return ext_.size(); }

private:
    struct Extent {
        uint64_t off;     // file offset
        uint64_t len;     // bytes (whole clusters)
        uint32_t sector;  // first sector on the volume's drive
    };
    bool build_extents_fat32(void *fs, uint32_t sclust, const char *vfs_path);
    std::vector<Extent> ext_;
    uint8_t *blk_ = nullptr;       // 4 KB block cache for pread()
    uint64_t blk_off_ = UINT64_MAX;
    uint64_t size_ = 0;
    size_t ss_ = 512;
    uint8_t pdrv_ = 0;
};

// Streams file bytes [begin, end) over and over (the loop) into a PSRAM ring.
// Positions are "stream positions": bytes since start, counting every pass;
// a byte at file offset o in pass p sits at p * span() + (o - begin()).
class ReadAhead {
public:
    bool start(RawFile *f, uint64_t begin, uint64_t end, size_t chunk, size_t ring_bytes);
    // Pointer to [spos, spos + len): into the ring when contiguous, otherwise
    // copied into scratch. Blocks until the bytes are read; nullptr on error/stop.
    const uint8_t *acquire(uint64_t spos, size_t len, uint8_t *scratch);
    // Everything before spos_end is no longer needed (frames are released in order).
    void release(uint64_t spos_end) { released_.store(spos_end); }
    void stop();
    // Make a blocked acquire() return nullptr (teardown from another task).
    void request_stop() { stop_ = true; }
    ~ReadAhead() { stop(); }
    uint64_t begin() const { return begin_; }
    uint64_t span() const { return end_ - begin_; }
    // Bytes moved by all read-ahead tasks since the last call (for STAT io_MBps).
    static uint64_t take_io_bytes();

private:
    static void io_task(void *arg);
    RawFile *f_ = nullptr;
    uint64_t begin_ = 0, end_ = 0;
    size_t chunk_ = 0, ring_ = 0;
    uint8_t *buf_ = nullptr;
    std::atomic<uint64_t> written_{0};   // stream bytes available in the ring
    std::atomic<uint64_t> released_{0};  // stream bytes the decoder is done with
    volatile bool stop_ = false, done_ = true, error_ = false;
};
