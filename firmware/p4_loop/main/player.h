#pragma once
// Display bring-up (LT8912B via esp_lcd_lt8912b, DSI/DPI) and the MJPEG loop player.
#include <atomic>
#include "driver/jpeg_types.h"
#include <cstdint>

// Panel mode. LT8912B on the Display Out add-on supports only these two
// (M5GFX M5UnitPoEP4DisplayOut::isSupportedTiming; timings copied from M5GFX).
#ifndef P4LOOP_WIDTH
#define P4LOOP_WIDTH 1920
#define P4LOOP_HEIGHT 1080
#define P4LOOP_REFRESH 30
#endif

// Frame-buffer format (build with -DP4LOOP_FB_FMT=n):
//   1 RGB888 in PSRAM, RGB888 on DSI (what M5GFX's driver used; ~24 fps at 1080p)
//   2 RGB565 in PSRAM, RGB888 on DSI. NOT supported by the IDF 5.4.2 DPI
//     driver (only YUV422->RGB conversion exists); scan-out does not start
//     (bench 2026-11-22). Kept to document the dead end.
//   3 RGB565 in PSRAM, RGB565 on DSI (the LT8912B would need RGB565 input)
//   4 YUV422 in PSRAM, RGB888 on DSI via the bridge's YUV->RGB converter (default)
// 2-byte formats halve scan-out and decode-write PSRAM traffic.
#ifndef P4LOOP_FB_FMT
#define P4LOOP_FB_FMT 4  // YUV422: the format that sustains 1080p30 (bench 2026-11-22)
#endif
#if P4LOOP_FB_FMT == 1
#define P4LOOP_FB_BYTES 3
#define P4LOOP_JPEG_OUT JPEG_DECODE_OUT_FORMAT_RGB888
#elif P4LOOP_FB_FMT == 4
#define P4LOOP_FB_BYTES 2
#define P4LOOP_JPEG_OUT JPEG_DECODE_OUT_FORMAT_YUV422
#else
#define P4LOOP_FB_BYTES 2
#define P4LOOP_JPEG_OUT JPEG_DECODE_OUT_FORMAT_RGB565
#endif

struct PlayerControl {
    std::atomic<bool> free_run{false};   // decode/present as fast as possible (throughput ceiling)
    std::atomic<bool> verbose{false};    // one CSV line per frame
    std::atomic<bool> paused{false};
    std::atomic<bool> next_file{false};  // leave the current file
    std::atomic<bool> rgb_swap{false};   // flip JPEG RGB/BGR output order
    std::atomic<bool> reset_stats{false};
    std::atomic<bool> dump_status{false};
    std::atomic<bool> bench{false};      // rerun the decode bench
};
extern PlayerControl g_ctl;

// Returns false if the HDMI bridge could not be initialised.
bool player_display_init();
// Paint a solid colour into every frame buffer (used for "no stick" / error screens).
void player_fill(uint8_t r, uint8_t g, uint8_t b);
// 'c' key: cycle the YUV422 byte order (fmt 4) or flip R/B (fmt 1) until colours look right.
void player_cycle_color_order();
// Plays one AVI file in a seamless loop until next_file is requested, the stick
// is removed, or a fatal error. Returns false on error.
bool player_run(const char *path);
// Boot-time trace: prints "BOOT <what> app_ms=<esp_timer ms>" until the first
// frame is on screen, then nothing (used to find where boot time goes).
void boot_mark(const char *what);
// True once a video frame has reached the screen (since boot).
bool player_has_presented();

// ---- bench support (bench.cpp)
#include "driver/jpeg_decode.h"
// DSI + DPI scan-out with M5's timing but no LT8912B: real PSRAM load, no picture.
bool player_display_init_dsi_only();
jpeg_decoder_handle_t player_jpeg();
uint8_t *player_fb(int k);
size_t player_fb_size();
uint32_t player_vsync_count();
// Next frame buffer that is safe to overwrite (waits for a vsync if needed);
// -1 if no refresh arrives within 200 ms (scan-out not running).
int player_next_fb(uint32_t *wait_us);
// Switch scan-out to frame buffer k at the next vsync.
void player_present(int k);
