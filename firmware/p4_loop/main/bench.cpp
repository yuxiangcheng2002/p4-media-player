#include "bench.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "driver/jpeg_decode.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "player.h"

static const char *TAG = "bench";

// Frames from tools/make_bench_frames.sh (EMBED_FILES). The colour-bar frame
// ("flat") exists for both resolutions, since the mode is chosen at run time;
// the full decode bench set is 1080p only.
extern const uint8_t flat_start[] asm("_binary_flat_422_jpg_start");
extern const uint8_t flat_end[] asm("_binary_flat_422_jpg_end");
extern const uint8_t flat720_start[] asm("_binary_flat720_422_jpg_start");
extern const uint8_t flat720_end[] asm("_binary_flat720_422_jpg_end");
#if P4LOOP_BENCH_FULL
extern const uint8_t photo_start[] asm("_binary_photo_422_jpg_start");
extern const uint8_t photo_end[] asm("_binary_photo_422_jpg_end");
extern const uint8_t typical_start[] asm("_binary_typical_422_jpg_start");
extern const uint8_t typical_end[] asm("_binary_typical_422_jpg_end");
extern const uint8_t noisy_start[] asm("_binary_noisy_422_jpg_start");
extern const uint8_t noisy_end[] asm("_binary_noisy_422_jpg_end");
extern const uint8_t typical444_start[] asm("_binary_typical_444_jpg_start");
extern const uint8_t typical444_end[] asm("_binary_typical_444_jpg_end");
#endif

struct Frame {
    const char *name;
    const uint8_t *start, *end;
    uint8_t *psram;  // copy in PSRAM: the JPEG DMA reads its input from RAM, not flash
    size_t len;
};

static Frame s_frames[] = {
    {"flat", flat_start, flat_end, nullptr, 0},
#if P4LOOP_BENCH_FULL
    {"photo", photo_start, photo_end, nullptr, 0},
    {"typical", typical_start, typical_end, nullptr, 0},
    {"noisy", noisy_start, noisy_end, nullptr, 0},
    {"typical444", typical444_start, typical444_end, nullptr, 0},
#endif
};

struct Series {
    uint32_t n = 0;
    uint64_t sum = 0;
    uint32_t min = UINT32_MAX, max = 0;
    void add(uint32_t v) { n++; sum += v; min = std::min(min, v); max = std::max(max, v); }
    double avg_ms() const { return n ? sum / (double)n / 1000.0 : 0; }
};

static const jpeg_decode_cfg_t kDecCfg = {
    .output_format = P4LOOP_JPEG_OUT,
    .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
    .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
};

static bool load_frames()
{
    if (video_width() == 1280) {  // before the first copy: the 720p colour bars
        s_frames[0].start = flat720_start;
        s_frames[0].end = flat720_end;
    }
    for (auto &f : s_frames) {
        if (f.psram) continue;
        f.len = f.end - f.start;
        jpeg_decode_memory_alloc_cfg_t mc = {.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER};
        size_t cap = 0;
        f.psram = static_cast<uint8_t *>(jpeg_alloc_decoder_mem((f.len + 4095) & ~4095u, &mc, &cap));
        if (!f.psram) return false;
        memcpy(f.psram, f.start, f.len);
    }
    return true;
}

// Decode n times into one scratch buffer; no scan-out involvement.
static void decode_only(const char *phase, uint8_t *out, size_t out_size, int n)
{
    for (auto &f : s_frames) {
        Series s;
        for (int i = 0; i < n; i++) {
            uint32_t got = 0;
            int64_t t0 = esp_timer_get_time();
            esp_err_t err = jpeg_decoder_process(player_jpeg(), &kDecCfg, f.psram, f.len, out, out_size, &got);
            uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
            if (err != ESP_OK) {
                printf("BENCH phase=%s frame=%s ERROR %s\n", phase, f.name, esp_err_to_name(err));
                break;
            }
            s.add(us);
        }
        printf("BENCH phase=%s frame=%s bytes=%u n=%" PRIu32 " dec_ms avg=%.2f min=%.2f max=%.2f fps_ceiling=%.1f\n",
               phase, f.name, (unsigned)f.len, s.n, s.avg_ms(), s.min / 1000.0, s.max / 1000.0,
               s.avg_ms() > 0 ? 1000.0 / s.avg_ms() : 0);
        vTaskDelay(1);
    }
}

// Full display path: wait for a free frame buffer, decode into it, present.
static void decode_present(int n)
{
    for (auto &f : s_frames) {
        Series dec, wait, interval;
        int64_t last = 0;
        uint32_t v0 = player_vsync_count();
        int64_t t_start = esp_timer_get_time();
        for (int i = 0; i < n; i++) {
            uint32_t w = 0;
            int k = player_next_fb(&w);
            if (k < 0) {
                printf("BENCH phase=scanout-present frame=%s ERROR no refresh in 200 ms: scan-out not running\n", f.name);
                return;
            }
            uint32_t got = 0;
            int64_t t0 = esp_timer_get_time();
            esp_err_t err = jpeg_decoder_process(player_jpeg(), &kDecCfg, f.psram, f.len, player_fb(k),
                                                 player_fb_size(), &got);
            dec.add((uint32_t)(esp_timer_get_time() - t0));
            if (err != ESP_OK) {
                printf("BENCH phase=scanout-present frame=%s ERROR %s\n", f.name, esp_err_to_name(err));
                break;
            }
            player_present(k);
            int64_t now = esp_timer_get_time();
            if (last) interval.add((uint32_t)(now - last));
            last = now;
            wait.add(w);
        }
        double secs = (esp_timer_get_time() - t_start) / 1e6;
        uint32_t vs = player_vsync_count() - v0;
        printf("BENCH phase=scanout-present frame=%s n=%" PRIu32 " fps=%.2f refresh_hz=%.2f dec_ms avg=%.2f max=%.2f "
               "fbwait_ms avg=%.2f max=%.2f interval_ms max=%.2f\n",
               f.name, dec.n, dec.n / secs, vs / secs, dec.avg_ms(), dec.max / 1000.0, wait.avg_ms(),
               wait.max / 1000.0, interval.max / 1000.0);
        vTaskDelay(1);
    }
}

bool bench_show_pattern()
{
    if (!load_frames()) return false;
    uint32_t w = 0;
    int k = player_next_fb(&w);
    if (k < 0) return false;
    uint32_t got = 0;
    const Frame &f = s_frames[0];  // "flat": ffmpeg testsrc, same as tools/make_bench_frames.sh
    if (jpeg_decoder_process(player_jpeg(), &kDecCfg, f.psram, f.len, player_fb(k), player_fb_size(), &got) != ESP_OK) {
        return false;
    }
    player_present(k);
    return true;
}

void bench_run(bool have_display)
{
    if (video_width() != 1920) {
        printf("BENCH skipped: the bench frames are 1080p; set mode=1080 to run it\n");
        return;
    }
    printf("BENCH start mode=%dx%d fb_fmt=%d fb_bytes_per_px=%d have_display=%d psram_free=%u\n", video_width(), video_height(), P4LOOP_FB_FMT, P4LOOP_FB_BYTES, have_display,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    if (!load_frames()) {
        ESP_LOGE(TAG, "no PSRAM for input frames");
        return;
    }
    // Scratch sized for 16-line MCU rounding, so any sampling decodes here.
    const size_t out_size = (size_t)video_width() * ((video_height() + 15) / 16 * 16) * P4LOOP_FB_BYTES;
    jpeg_decode_memory_alloc_cfg_t mc = {.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER};
    size_t cap = 0;
    uint8_t *scratch = static_cast<uint8_t *>(jpeg_alloc_decoder_mem(out_size, &mc, &cap));
    if (!scratch) {
        ESP_LOGE(TAG, "no PSRAM for the scratch frame");
        return;
    }

    // Test 3: decode speed with nothing else touching PSRAM (only before any scan-out exists).
    static bool scanout_running = false;
    if (!have_display && !scanout_running) decode_only("idle", scratch, cap, 60);

    // Test 4: the same decodes while DPI reads ~6.2 MB per refresh from PSRAM.
    if (!have_display && !scanout_running) {
        if (!player_display_init_dsi_only()) {
            free(scratch);
            return;
        }
        scanout_running = true;
        vTaskDelay(pdMS_TO_TICKS(200));  // let a few refreshes run
    }
    decode_only("scanout-decode", scratch, cap, 60);
    decode_present(120);
    free(scratch);
    printf("BENCH done (a DPI 'underrun' error above means PSRAM could not keep up)\n");
}
