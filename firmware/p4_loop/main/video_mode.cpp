#include "video_mode.h"

#include <strings.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "mode";

// Horizontal segments must be multiples of 16 px: IDF 5.4.2 converts each to
// DSI byte clocks as px * 1000 Mbps / 80 MHz / 8 = px * 1.5625 and truncates
// (mipi_dsi_hal_host_dpi_set_horizontal_timing); a shortfall accumulates down
// the frame and the picture shears (2026-11-25, ASUS photos).
static constexpr ModeTiming kModes[] = {
    {"1080", 48, 32, 80, 1920, 3, 5, 8, 1080, true, false, 0},          // 2080x1096: 38.46 kHz, 35.09 Hz
    {"1080p30", 256, 48, 144, 1920, 4, 5, 36, 1080, true, true, 34},    // 2368x1125: 33.78 kHz, 30.03 Hz
    {"720", 240, 32, 224, 1280, 5, 5, 20, 720, true, true, 4},          // 1776x750: 45.05 kHz, 60.06 Hz
};

static constexpr bool aligned(const ModeTiming &t)
{
    return t.hfp % 16 == 0 && t.hs % 16 == 0 && t.hbp % 16 == 0 && t.hact % 16 == 0;
}
static_assert(aligned(kModes[0]) && aligned(kModes[1]) && aligned(kModes[2]),
              "horizontal timing must be multiples of 16 px at 1000 Mbps / 80 MHz");

static VideoMode s_mode = kDefaultMode;

const ModeTiming &mode_timing(VideoMode m) { return kModes[(int)m < 3 ? (int)m : 0]; }
VideoMode video_mode() { return s_mode; }
const ModeTiming &video_timing() { return mode_timing(s_mode); }
int video_width() { return video_timing().hact; }
int video_height() { return video_timing().vact; }

double video_refresh_hz()
{
    const ModeTiming &t = video_timing();
    const double htotal = t.hfp + t.hs + t.hbp + t.hact, vtotal = t.vfp + t.vs + t.vbp + t.vact;
    return kPixelClockMHz * 1e6 / (htotal * vtotal);
}

static nvs_handle_t open_nvs()
{
    // update.cpp initialises the same default partition; a second init is harmless.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    nvs_handle_t h = 0;
    if (err != ESP_OK || nvs_open("display", NVS_READWRITE, &h) != ESP_OK) return 0;
    return h;
}

void video_mode_load()
{
    nvs_handle_t h = open_nvs();
    uint8_t v = 0;
    if (h && nvs_get_u8(h, "mode", &v) == ESP_OK && v < 3) s_mode = (VideoMode)v;
    if (h) nvs_close(h);
}

bool video_mode_parse(const char *s, VideoMode &out)
{
    for (int i = 0; i < 3; i++) {
        if (strcasecmp(s, kModes[i].name) == 0) {
            out = (VideoMode)i;
            return true;
        }
    }
    return false;
}

bool video_mode_request(VideoMode want)
{
    if (want == s_mode) return false;
    nvs_handle_t h = open_nvs();
    // Only restart when the new mode is really stored, or the player would
    // restart in a loop.
    bool ok = h && nvs_set_u8(h, "mode", (uint8_t)want) == ESP_OK && nvs_commit(h) == ESP_OK;
    if (h) nvs_close(h);
    if (!ok) {
        ESP_LOGE(TAG, "could not store mode %s; staying in %s", mode_timing(want).name, video_timing().name);
        return false;
    }
    return true;
}
