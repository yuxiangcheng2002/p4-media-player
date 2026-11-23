#pragma once
// Minimal MJPEG-in-AVI indexer: finds every video chunk ("##dc"/"##db") in the
// file so the player can read frames by (offset, size) and wrap at the end
// without reopening the file. That wrap is what makes the loop seamless.
#include <cstdint>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

// Random-access byte source for the indexer: the raw sector reader
// (raw_reader.h) on the fast path, a plain fd as the fallback.
struct ByteSource {
    virtual ~ByteSource() = default;
    virtual bool pread(uint64_t off, void *dst, size_t n) = 0;
    virtual uint64_t size() const = 0;
};

struct FdSource : ByteSource {
    int fd;
    explicit FdSource(int f) : fd(f) {}
    bool pread(uint64_t off, void *dst, size_t n) override
    {
        if (lseek(fd, (off_t)off, SEEK_SET) < 0) return false;
        uint8_t *p = static_cast<uint8_t *>(dst);
        while (n) {
            ssize_t r = read(fd, p, n);
            if (r <= 0) return false;
            p += r;
            n -= r;
        }
        return true;
    }
    uint64_t size() const override
    {
        off_t cur = lseek(fd, 0, SEEK_CUR), end = lseek(fd, 0, SEEK_END);
        lseek(fd, cur, SEEK_SET);
        return end < 0 ? 0 : (uint64_t)end;
    }
};

struct AviFrame {
    uint32_t offset;  // absolute file offset of the JPEG data
    uint32_t size;    // JPEG byte count
};

struct AviInfo {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t us_per_frame = 0;  // from avih; 0 if absent
    uint32_t max_frame = 0;     // largest frame, sizes the read buffers
    std::vector<AviFrame> frames;
};

// Returns true on success. Walks RIFF 'AVI ' and any OpenDML 'AVIX' segments
// (ffmpeg writes those past 1 GB). Reads only chunk headers.
bool avi_build_index(ByteSource &src, AviInfo &out);
