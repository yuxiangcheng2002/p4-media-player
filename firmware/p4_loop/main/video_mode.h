#pragma once
// HDMI output mode, chosen at run time: the drive's /config.txt says
// `mode = 1080 | 1080p30 | 720` (default 1080p30). The choice is kept in NVS
// because the display starts before the drive is read. When a drive asks for a different mode than the one running, the
// player stores it and restarts once (about 1 s).
//
// Why several modes: the P4 makes an 80 MHz pixel clock at best (integer
// divider of 240 MHz), so CEA timings (74.25 MHz) cannot be produced exactly.
// 1080p at 80 MHz tops out near 35 Hz, which many PC monitors refuse ("Out of
// range" on an ASUS VS229H); 720p at 60 Hz is accepted by both monitors tried
// (2026-11-25). See tests/results/01-p4-mjpeg-throughput/2026-11-25-hdmi-monitor/.
#include <cstdint>

enum class VideoMode : uint8_t {
    M1080 = 0,     // M5's 1080p: 2080x1096, 35.09 Hz, VIC 0 (the default until 2026-11-25)
    M1080P30 = 1,  // 1080p with CEA 1080p30 line/frame rates: 2368x1125, 30.03 Hz, VIC 34 (default)
    M720 = 2,      // 720p with CEA 720p60 line/frame rates: 1776x750, 60.06 Hz, VIC 4
};

struct ModeTiming {
    const char *name;  // the config.txt value
    uint16_t hfp, hs, hbp, hact, vfp, vs, vbp, vact;
    bool hpol, vpol;
    uint8_t vic;
};

static constexpr int kPixelClockMHz = 80;  // 240 MHz / 3, see player.cpp

// Default when the drive gives no mode: 30 Hz matches 30 fps
// content (35 Hz shows frames unevenly) and is the CEA 1080p30 rate. Seen
// working on the bench monitor only; the ASUS VS229H refuses any 1080 mode.
static constexpr VideoMode kDefaultMode = VideoMode::M1080P30;

const ModeTiming &mode_timing(VideoMode m);
// Reads the stored mode from NVS (kDefaultMode if none). Call once, before the display.
void video_mode_load();
VideoMode video_mode();
const ModeTiming &video_timing();
int video_width();
int video_height();
double video_refresh_hz();

// "1080", "1080p30", "720" (case-insensitive) -> mode; false if not recognised.
bool video_mode_parse(const char *s, VideoMode &out);
// Stores `want` for the next boot if it differs from the running mode.
// Returns true when the caller should restart to apply it.
bool video_mode_request(VideoMode want);
