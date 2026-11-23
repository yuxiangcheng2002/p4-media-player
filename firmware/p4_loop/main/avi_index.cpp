#include "avi_index.h"

#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <vector>

#include "esp_log.h"

static const char *TAG = "avi";

static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

// Walk the chunks of one LIST body [start, end) and collect video frames.
// Recurses into nested LISTs ('rec ' groups) so either muxing style works.
// All positions are 64-bit and clamped to the file, so a damaged size field
// cannot wrap around or loop forever; nesting is limited.
static void walk_list(ByteSource &src, uint64_t start, uint64_t end, uint64_t file_size, AviInfo &out, int depth)
{
    if (depth > 4) return;
    end = std::min(end, file_size);
    uint64_t pos = start;
    uint8_t hdr[12];
    while (pos + 8 <= end) {
        if (!src.pread(pos, hdr, 8)) return;
        const uint64_t size = le32(hdr + 4);
        const uint64_t body = pos + 8;
        const bool list = !memcmp(hdr, "LIST", 4);
        // A data chunk running past its parent ends the walk; a LIST that
        // does (a truncated copy) is walked up to the end of the file.
        if (!list && body + size > end) return;
        if (list) {
            if (size < 4 || !src.pread(body, hdr + 8, 4)) return;
            if (!memcmp(hdr + 8, "hdrl", 4) || !memcmp(hdr + 8, "movi", 4) || !memcmp(hdr + 8, "rec ", 4)) {
                walk_list(src, body + 4, body + size, file_size, out, depth + 1);
            }
        } else if (!memcmp(hdr, "avih", 4) && size >= 40) {
            uint8_t avih[40];
            if (src.pread(body, avih, sizeof(avih))) {
                out.us_per_frame = le32(avih + 0);
                out.width = le32(avih + 32);
                out.height = le32(avih + 36);
            }
        } else if (hdr[2] == 'd' && (hdr[3] == 'c' || hdr[3] == 'b') && size > 0 && body <= UINT32_MAX) {
            // "00dc" compressed video / "00db" uncompressed; MJPEG uses dc.
            out.frames.push_back({(uint32_t)body, (uint32_t)size});
            if (size > out.max_frame) out.max_frame = (uint32_t)size;
        }
        pos = body + size + (size & 1);  // RIFF chunks are word aligned
    }
}

// Fast path: read the legacy 'idx1' index (16 bytes per chunk) in one go
// instead of seeking to every chunk header. Walking 3752 headers took 2.2 s
// over USB and 1.2 s from the SD slot (2026-11-23); idx1 is ~60 KB.
// Offsets in idx1 are relative to the 'movi' fourcc in most files and
// absolute in some, so the first video entry is checked against the file.
// Every entry must lie inside the file, or the fast path is abandoned.
static bool read_idx1(ByteSource &src, uint64_t movi_fourcc, uint64_t idx_pos, uint64_t idx_size,
                      uint64_t file_size, AviInfo &out)
{
    constexpr uint64_t kMaxIdx = 16u << 20;  // ~1 M chunks, hours of video
    if (idx_size > kMaxIdx || idx_pos + idx_size > file_size) return false;
    std::vector<uint8_t> idx(idx_size);
    if (!src.pread(idx_pos, idx.data(), idx_size)) return false;
    const uint64_t n = idx_size / 16;
    uint64_t base = UINT64_MAX;
    for (uint64_t k = 0; k < n; k++) {
        const uint8_t *e = idx.data() + 16 * k;
        if (!(e[2] == 'd' && (e[3] == 'c' || e[3] == 'b'))) continue;
        const uint64_t off = le32(e + 8), size = le32(e + 12);
        if (size == 0) continue;
        if (base == UINT64_MAX) {
            // Decide the base on the first video entry: its chunk header must
            // read back as the same fourcc and size.
            for (uint64_t cand : std::initializer_list<uint64_t>{movi_fourcc, 0u}) {
                uint8_t h[8];
                if (cand + off + 8 <= file_size && src.pread(cand + off, h, 8) && !memcmp(h, e, 4) &&
                    le32(h + 4) == size) {
                    base = cand;
                    break;
                }
            }
            if (base == UINT64_MAX) return false;
        }
        const uint64_t data = base + off + 8;
        if (data + size > file_size || data > UINT32_MAX) {
            out.frames.clear();
            return false;
        }
        out.frames.push_back({(uint32_t)data, (uint32_t)size});
        if (size > out.max_frame) out.max_frame = (uint32_t)size;
    }
    return !out.frames.empty();
}

bool avi_build_index(ByteSource &src, AviInfo &out)
{
    out = AviInfo{};
    const uint64_t file_size = src.size();
    if (file_size <= 12) return false;

    // Pass 1: top-level layout only (hdrl for avih, where movi and idx1 are,
    // and whether OpenDML 'AVIX' segments follow).
    uint8_t hdr[12];
    if (!src.pread(0, hdr, 12) || memcmp(hdr, "RIFF", 4) ||
        memcmp(hdr + 8, "AVI ", 4)) {
        ESP_LOGE(TAG, "not an AVI file");
        return false;
    }
    const uint64_t riff_declared_end = 8 + (uint64_t)le32(hdr + 4);
    const uint64_t riff_end = std::min(riff_declared_end, file_size);
    uint64_t movi_fourcc = 0, idx_pos = 0, idx_size = 0;
    uint64_t pos = 12;
    while (pos + 8 <= riff_end) {
        if (!src.pread(pos, hdr, 8)) break;
        const uint64_t size = le32(hdr + 4);
        const bool list = !memcmp(hdr, "LIST", 4);
        if (list && (pos + 12 > riff_end || !src.pread(pos + 8, hdr + 8, 4))) break;
        if (list && !memcmp(hdr + 8, "hdrl", 4)) {
            walk_list(src, pos + 12, pos + 8 + size, file_size, out, 1);  // header only: avih, no frames
        } else if (list && !memcmp(hdr + 8, "movi", 4)) {
            movi_fourcc = pos + 8;
        } else if (!memcmp(hdr, "idx1", 4)) {
            idx_pos = pos + 8;
            idx_size = size;
        }
        pos += 8 + size + (size & 1);
    }
    const bool odml = riff_declared_end + 12 <= file_size;  // more RIFF segments follow
    if (idx_size && movi_fourcc && !odml && read_idx1(src, movi_fourcc, idx_pos, idx_size, file_size, out)) {
        ESP_LOGI(TAG, "%ux%u, %u frames (idx1), %u us/frame, max frame %u B", (unsigned)out.width,
                 (unsigned)out.height, (unsigned)out.frames.size(), (unsigned)out.us_per_frame,
                 (unsigned)out.max_frame);
        return true;
    }

    // Slow path: walk every chunk header (no idx1, or OpenDML files > 1 GB).
    out.frames.clear();
    out.max_frame = 0;
    pos = 0;
    while (pos + 12 <= file_size) {
        if (!src.pread(pos, hdr, 12)) break;
        if (memcmp(hdr, "RIFF", 4) != 0) break;
        const uint64_t size = le32(hdr + 4);
        if (memcmp(hdr + 8, "AVI ", 4) != 0 && memcmp(hdr + 8, "AVIX", 4) != 0) {
            ESP_LOGE(TAG, "not an AVI file");
            return false;
        }
        walk_list(src, pos + 12, pos + 8 + size, file_size, out, 1);
        pos += 8 + size + (size & 1);
    }
    ESP_LOGI(TAG, "%ux%u, %u frames (walked), %u us/frame, max frame %u B",
             (unsigned)out.width, (unsigned)out.height, (unsigned)out.frames.size(),
             (unsigned)out.us_per_frame, (unsigned)out.max_frame);
    return !out.frames.empty();
}
