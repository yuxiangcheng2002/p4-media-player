// MJPEG loop player for experiment 01.
//
// Data path:  USB stick --read()--> input ring (PSRAM) --JPEG HW--> DPI frame buffer
//             --esp_lcd_panel_draw_bitmap (pointer switch, no copy)--> DSI --> LT8912B --> HDMI
//
// Seamless loop: the reader wraps its frame index at the end of the file and keeps
// prefetching, so frame 0 of the next pass is already in the ring when the last
// frame is shown. Nothing is reopened at the seam.
//
// Zero-copy present: the JPEG engine decodes straight into one of three DPI
// frame buffers, and draw_bitmap() with a pointer inside a frame buffer only
// switches the scan-out index (esp_lcd_panel_dpi.c). This needs the decoded
// size to equal the frame-buffer size exactly: 1920x1080 works for true 4:2:2
// (luma 2x1, 8-line MCU) and 4:4:4; 16-line MCUs (4:2:0, and ffmpeg's "4:2:2",
// which is luma 2x2) decode to 1088 lines and fall back to scratch + copy.
//
// Frame buffers default to YUV422 (P4LOOP_FB_FMT 4): half the PSRAM traffic of
// RGB888, which is what makes 1080p30 fit (bench 2026-11-22). The DSI bridge
// converts to RGB888 for the LT8912B.
#include "player.h"

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include <memory>

#include "avi_index.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/jpeg_decode.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_lt8912b.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "raw_reader.h"
#include "usb_stick.h"

static const char *TAG = "player";

PlayerControl g_ctl;

static constexpr int kNumFb = 3;
static constexpr int kNumSlots = 6;  // compressed frames prefetched ahead of the decoder

static esp_lcd_panel_handle_t s_panel;
static uint8_t *s_fb[kNumFb];
static size_t s_fb_size;

// Vsync bookkeeping: a frame buffer may be rewritten only after the panel has
// finished at least one refresh since it stopped being the current buffer.
static volatile uint32_t s_vsync;
static uint32_t s_retired_at[kNumFb];
static int s_cur;  // frame buffer currently selected for scan-out

// ---------------------------------------------------------------- display

// esp_lcd renamed on_refresh_done to on_frame_buf_complete in v6; pick whichever exists.
template <typename T, typename = void>
struct has_frame_buf_complete : std::false_type {};
template <typename T>
struct has_frame_buf_complete<T, decltype((void)T::on_frame_buf_complete, void())> : std::true_type {};
template <typename T, typename Fn>
static void set_vsync_cb(T &cbs, Fn fn, std::true_type) { cbs.on_frame_buf_complete = fn; }
template <typename T, typename Fn>
static void set_vsync_cb(T &cbs, Fn fn, std::false_type) { cbs.on_refresh_done = fn; }

static bool IRAM_ATTR on_vsync(esp_lcd_panel_handle_t, esp_lcd_dpi_panel_event_data_t *, void *)
{
    s_vsync = s_vsync + 1;
    return false;
}

static void attach_vsync()
{
    esp_lcd_dpi_panel_event_callbacks_t cbs = {};
    set_vsync_cb(cbs, on_vsync, has_frame_buf_complete<esp_lcd_dpi_panel_event_callbacks_t>{});
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_register_event_callbacks(s_panel, &cbs, nullptr));
}

// ---- DSI plumbing shared by the HDMI path and the DSI-only bench path.
// Lane setup, PHY LDO and timing follow M5GFX's Panel_LT8912B, which M5 tested
// on this board: 2 lanes at 1000 Mbps, LDO ch 3 at 2500 mV, 80 MHz pixel clock
// (1080p: 2080x1096 total, ~35 Hz; 720p: 1650x750, ~64.6 Hz).
static esp_ldo_channel_handle_t s_ldo;
static esp_lcd_dsi_bus_handle_t s_dsi_bus;
// Index into kYuvOrders. The JPEG engine writes UYVY: with YUYV (0) the
// monitor showed luma as chroma (a grey source came out bright areas magenta,
// dark areas bright green). Changing the order with 'c' while playing returns
// ESP_OK but does not change the picture, so the order is set at build time
// (-DP4LOOP_YUV_ORDER=n).
#ifndef P4LOOP_YUV_ORDER
#define P4LOOP_YUV_ORDER 1
#endif
static int s_yuv_order = P4LOOP_YUV_ORDER;

static bool dsi_bus_up()
{
    if (!s_ldo) {
        esp_ldo_channel_config_t lc = {};
        lc.chan_id = 3;
        lc.voltage_mv = 2500;
        if (esp_ldo_acquire_channel(&lc, &s_ldo) != ESP_OK) {
            ESP_LOGE(TAG, "MIPI PHY LDO (ch 3) unavailable");
            return false;
        }
    }
    if (!s_dsi_bus) {
        esp_lcd_dsi_bus_config_t bc = {};
        bc.bus_id = 0;
        bc.num_data_lanes = 2;
        bc.phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT;
        bc.lane_bit_rate_mbps = 1000;
        if (esp_lcd_new_dsi_bus(&bc, &s_dsi_bus) != ESP_OK) {
            ESP_LOGE(TAG, "DSI bus init failed");
            return false;
        }
    }
    return true;
}

static void fill_dpi_config(esp_lcd_dpi_panel_config_t &dc)
{
    dc = {};
    dc.virtual_channel = 0;
    dc.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
    dc.dpi_clock_freq_mhz = 80;
    dc.pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB888;  // what the LT8912B receives
#if P4LOOP_FB_FMT == 2
    dc.in_color_format = LCD_COLOR_FMT_RGB565;          // what sits in PSRAM
    dc.out_color_format = LCD_COLOR_FMT_RGB888;
#elif P4LOOP_FB_FMT == 3
    dc.pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565;
#elif P4LOOP_FB_FMT == 4
    dc.in_color_format = LCD_COLOR_FMT_YUV422;
    dc.out_color_format = LCD_COLOR_FMT_RGB888;
#endif
    dc.num_fbs = kNumFb;
    dc.video_timing.h_size = P4LOOP_WIDTH;
    dc.video_timing.v_size = P4LOOP_HEIGHT;
#if P4LOOP_WIDTH == 1920
    dc.video_timing.hsync_pulse_width = 32; dc.video_timing.hsync_back_porch = 80; dc.video_timing.hsync_front_porch = 48;
    dc.video_timing.vsync_pulse_width = 5;  dc.video_timing.vsync_back_porch = 8;  dc.video_timing.vsync_front_porch = 3;
#else
    dc.video_timing.hsync_pulse_width = 40; dc.video_timing.hsync_back_porch = 220; dc.video_timing.hsync_front_porch = 110;
    dc.video_timing.vsync_pulse_width = 5;  dc.video_timing.vsync_back_porch = 20;  dc.video_timing.vsync_front_porch = 5;
#endif
    dc.flags.use_dma2d = true;
}

#if P4LOOP_FB_FMT == 4
static const lcd_yuv422_pack_order_t kYuvOrders[] = {LCD_YUV422_PACK_ORDER_YUYV, LCD_YUV422_PACK_ORDER_UYVY,
                                                     LCD_YUV422_PACK_ORDER_VYUY, LCD_YUV422_PACK_ORDER_YVYU};
static const char *const kYuvOrderNames[] = {"YUYV", "UYVY", "VYUY", "YVYU"};
#endif

// YUV422 -> RGB888 in the DSI bridge (the only conversion IDF 5.4.2 supports).
// Byte order: see s_yuv_order. JFIF is full range.
static esp_err_t apply_color_conversion()
{
#if P4LOOP_FB_FMT == 4
    esp_lcd_color_conv_config_t cc = {};
    cc.in_color_range = LCD_COLOR_RANGE_FULL;
    cc.out_color_range = LCD_COLOR_RANGE_FULL;
    cc.spec.yuv.conv_std = LCD_YUV_CONV_STD_BT601;
    cc.spec.yuv.yuv422.in_pack_order = kYuvOrders[s_yuv_order];
    return esp_lcd_dpi_panel_set_color_conversion(s_panel, &cc);
#else
    return ESP_OK;
#endif
}

void player_cycle_color_order()
{
#if P4LOOP_FB_FMT == 4
    if (!s_panel) return;
    s_yuv_order = (s_yuv_order + 1) % 4;
    esp_err_t err = apply_color_conversion();
    printf("YUV422 pack order %s (%s)\n", kYuvOrderNames[s_yuv_order], esp_err_to_name(err));
#else
    g_ctl.rgb_swap = !g_ctl.rgb_swap;
    printf("rgb order %s\n", g_ctl.rgb_swap ? "RGB" : "BGR");
#endif
}

static void adopt_panel(const char *what)
{
    void *fb[kNumFb] = {};
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(s_panel, kNumFb, &fb[0], &fb[1], &fb[2]));
    for (int i = 0; i < kNumFb; i++) s_fb[i] = static_cast<uint8_t *>(fb[i]);
    s_fb_size = (size_t)P4LOOP_WIDTH * P4LOOP_HEIGHT * P4LOOP_FB_BYTES;
    attach_vsync();
    ESP_LOGI(TAG, "%s %dx%d, fb format %d (%d B/px), fb %p %p %p", what, P4LOOP_WIDTH, P4LOOP_HEIGHT,
             P4LOOP_FB_FMT, P4LOOP_FB_BYTES, s_fb[0], s_fb[1], s_fb[2]);
}

// LT8912B through Espressif's esp_lcd_lt8912b driver, which takes our own DPI
// config (M5GFX's driver hard-codes RGB888 frame buffers). Its register tables
// match M5GFX's Panel_LT8912B; the timing below is M5's, tested on this board.
//
// Reset: Unit PoE-P4 schematic (SCH_StickPoE Main V0.2, J6 FPC_24P): GPIO4 =
// G4_LCD_RST -> Display Out RESET_N -> Q1A level shifter -> LT8912B RST_N, with
// the add-on's 3V3 pull-up R1 not fitted, so the line floats unless driven.
// The driver pulses it (reset_gpio_num) in esp_lcd_panel_reset().
static constexpr gpio_num_t PIN_LT8912B_RESET = GPIO_NUM_4;

bool player_display_init()
{
#if P4LOOP_FB_FMT == 2 || P4LOOP_FB_FMT == 3
    // The LT8912B takes RGB888 only (esp_lcd_lt8912b README); fmt 2 does not
    // scan out at all on IDF 5.4.2. Both stay bench-only.
    ESP_LOGW(TAG, "FB format %d: HDMI path disabled (bench only)", P4LOOP_FB_FMT);
    return false;
#endif
    if (!dsi_bus_up()) return false;

    i2c_master_bus_config_t ib = {};
    ib.i2c_port = I2C_NUM_1;
    ib.sda_io_num = GPIO_NUM_0;  // SYS_SDA, per M5Unified + Arduino variant
    ib.scl_io_num = GPIO_NUM_1;  // SYS_SCL
    ib.clk_source = I2C_CLK_SRC_DEFAULT;
    ib.glitch_ignore_cnt = 7;
    ib.flags.enable_internal_pullup = true;  // the Unit also has 2.2 k pull-ups (R5/R6)
    i2c_master_bus_handle_t i2c = nullptr;
    if (i2c_new_master_bus(&ib, &i2c) != ESP_OK) {
        ESP_LOGE(TAG, "I2C port 1 init failed");
        return false;
    }
    const uint8_t addrs[3] = {LT8912B_IO_I2C_MAIN_ADDRESS, LT8912B_IO_I2C_CEC_ADDRESS, LT8912B_IO_I2C_AVI_ADDRESS};
    esp_lcd_panel_io_handle_t ios[3] = {};
    bool ok = true;
    for (int i = 0; i < 3 && ok; i++) {
        esp_lcd_panel_io_i2c_config_t ic = {};
        ic.dev_addr = addrs[i];
        ic.control_phase_bytes = 1;
        ic.lcd_cmd_bits = 8;
        ic.lcd_param_bits = 8;
        ic.flags.disable_control_phase = 1;
        ic.scl_speed_hz = 100000;
        ok = esp_lcd_new_panel_io_i2c(i2c, &ic, &ios[i]) == ESP_OK;
    }
    esp_lcd_panel_lt8912b_io_t io = {};
    io.main = ios[0];
    io.cec_dsi = ios[1];
    io.avi = ios[2];

    esp_lcd_dpi_panel_config_t dc;
    fill_dpi_config(dc);
    lt8912b_vendor_config_t vc = {};
#if P4LOOP_WIDTH == 1920
    vc.video_timing = {48, 32, 80, 1920, 2080, 3, 5, 8, 1080, 1096, true, false, 0, LT8912B_ASPECT_RATION_16_9, 80};
#else
    vc.video_timing = {110, 40, 220, 1280, 1650, 5, 5, 20, 720, 750, true, true, 4, LT8912B_ASPECT_RATION_16_9, 80};
#endif
    vc.mipi_config.dsi_bus = s_dsi_bus;
    vc.mipi_config.dpi_config = &dc;
    vc.mipi_config.lane_num = 2;
    esp_lcd_panel_dev_config_t pc = {};
    pc.reset_gpio_num = PIN_LT8912B_RESET;
    pc.bits_per_pixel = 24;
    pc.vendor_config = &vc;

    if (ok) ok = esp_lcd_new_panel_lt8912b(&io, &pc, &s_panel) == ESP_OK;
    if (ok) ok = apply_color_conversion() == ESP_OK;
    if (ok) {
        esp_lcd_panel_reset(s_panel);
        vTaskDelay(pdMS_TO_TICKS(50));  // let the bridge settle after RESET_N rises
        ok = esp_lcd_panel_init(s_panel) == ESP_OK;
    }
    if (!ok) {
        ESP_LOGE(TAG, "LT8912B init failed (no I2C answer: FPC cable?)");
        // Release everything so the bench (and the I2C scan) can use the hardware.
        if (s_panel) { esp_lcd_panel_del(s_panel); s_panel = nullptr; }
        for (auto h : ios) if (h) esp_lcd_panel_io_del(h);
        i2c_del_master_bus(i2c);
        return false;
    }
    adopt_panel("HDMI");
    return true;
}

// DSI host + DPI scan-out without the LT8912B, for the bench: same lanes,
// clock and timing as the HDMI path, so PSRAM carries the real scan-out load.
// The lanes drive an empty connector, which is harmless for D-PHY outputs.
bool player_display_init_dsi_only()
{
    if (!dsi_bus_up()) return false;
    esp_lcd_dpi_panel_config_t dc;
    fill_dpi_config(dc);
    if (esp_lcd_new_panel_dpi(s_dsi_bus, &dc, &s_panel) != ESP_OK) {
        ESP_LOGE(TAG, "DPI panel create failed");
        return false;
    }
    ESP_ERROR_CHECK(apply_color_conversion());
    if (esp_lcd_panel_init(s_panel) != ESP_OK) {
        ESP_LOGE(TAG, "DPI panel init failed");
        return false;
    }
    adopt_panel("DSI-only scan-out (no bridge)");
    return true;
}

// ---- bench hooks: same buffer discipline as player_run()
uint8_t *player_fb(int k) { return s_fb[k]; }
size_t player_fb_size() { return s_fb_size; }
uint32_t player_vsync_count() { return s_vsync; }

int player_next_fb(uint32_t *wait_us)
{
    int k = (s_cur + 1) % kNumFb;
    int64_t t0 = esp_timer_get_time();
    while ((int32_t)(s_vsync - s_retired_at[k]) <= 0) {
        // No refresh for 200 ms means scan-out is not running at all.
        if (esp_timer_get_time() - t0 > 200000) return -1;
        vTaskDelay(1);
    }
    if (wait_us) *wait_us = (uint32_t)(esp_timer_get_time() - t0);
    return k;
}

void player_present(int k)
{
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, P4LOOP_WIDTH, P4LOOP_HEIGHT, s_fb[k]);
    s_retired_at[s_cur] = s_vsync;
    s_cur = k;
}

void player_fill(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_panel) return;
    for (int i = 0; i < kNumFb; i++) {
        uint8_t *p = s_fb[i];
#if P4LOOP_FB_FMT == 4
        // BT.601 full range, packed in the converter's byte order (s_yuv_order).
        // It used to be hard-coded YUYV: after the switch to UYVY the amber
        // update screen showed green.
        const uint8_t y = (77 * r + 150 * g + 29 * b) >> 8;
        const uint8_t u = ((-43 * r - 85 * g + 128 * b) >> 8) + 128;
        const uint8_t v = ((128 * r - 107 * g - 21 * b) >> 8) + 128;
        uint8_t q[4];
        switch (kYuvOrders[s_yuv_order]) {
        case LCD_YUV422_PACK_ORDER_UYVY: q[0] = u; q[1] = y; q[2] = v; q[3] = y; break;
        case LCD_YUV422_PACK_ORDER_VYUY: q[0] = v; q[1] = y; q[2] = u; q[3] = y; break;
        case LCD_YUV422_PACK_ORDER_YVYU: q[0] = y; q[1] = v; q[2] = y; q[3] = u; break;
        default:                         q[0] = y; q[1] = u; q[2] = y; q[3] = v; break;
        }
        for (size_t k = 0; k + 3 < s_fb_size; k += 4) memcpy(p + k, q, 4);
#elif P4LOOP_FB_FMT != 1
        const uint16_t c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
        for (size_t k = 0; k < s_fb_size; k += 2) { p[k] = c & 0xFF; p[k + 1] = c >> 8; }
#else
        // DPI RGB888 byte order in memory is B, G, R.
        for (size_t k = 0; k < s_fb_size; k += 3) { p[k] = b; p[k + 1] = g; p[k + 2] = r; }
#endif
        // Write every buffer back now: a dirty cache line evicted later would
        // overwrite what the JPEG DMA decodes into that buffer.
        esp_cache_msync(p, s_fb_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    }
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, P4LOOP_WIDTH, P4LOOP_HEIGHT, s_fb[0]);
    s_cur = 0;
    for (int i = 0; i < kNumFb; i++) s_retired_at[i] = s_vsync;
}

// ---------------------------------------------------------------- reader

struct Slot {
    uint8_t *buf;        // own copy (FatFs path, or a frame straddling the ring end)
    const uint8_t *data; // what the decoder reads: buf or straight into the read-ahead ring
    uint64_t spos_end;   // ring stream position after this frame (release point)
    size_t cap;
    uint32_t len;
    uint32_t index;     // frame index in file
    uint32_t read_us;
    bool wrapped;       // first frame of a new pass
};

class ReadAhead;
struct ReaderCtx {
    int fd;          // -1 when raw is set
    RawFile *raw;    // raw sector source, opened by player_run; null = FatFs fd
    const char *path;
    ReadAhead *volatile ra;  // set by the reader before the first slot when raw reads are used
    const AviInfo *avi;
    Slot slots[kNumSlots];
    QueueHandle_t q_free, q_full;
    volatile bool stop;
    volatile bool done;
    volatile bool error;
};

// Forward-only buffered file reader. Two measured problems (2026-11-23, 32 GB
// SD card, 1.7 MB/s and falling with file position):
// - read() straight into a PSRAM slot: the SD driver cannot DMA into it, so it
//   falls back to one 512-byte single-block read per sector;
// - lseek() per frame: FatFs re-walks the cluster chain.
// So the file is read sequentially in large 512-aligned chunks into an aligned,
// DMA-capable internal-RAM buffer (multi-block reads), and frames are copied
// out of it. A seek happens only when a frame lies outside the next chunk
// (the wrap back to frame 0). Works the same for the USB stick.
struct StreamReader {
    int fd = -1;
    uint8_t *buf = nullptr;
    size_t cap = 0;
    uint64_t buf_off = 0;  // file offset of buf[0], always 512-aligned
    size_t buf_len = 0;
    uint64_t file_pos = UINT64_MAX;  // where the fd currently is

    bool init(int f)
    {
        fd = f;
        for (size_t c = 128 * 1024; c >= 16 * 1024 && !buf; c /= 2) {
            buf = static_cast<uint8_t *>(heap_caps_aligned_alloc(64, c, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
            cap = c;
        }
        if (buf) ESP_LOGI(TAG, "stream reader: %u KB internal DMA buffer", (unsigned)(cap / 1024));
        return buf != nullptr;
    }
    ~StreamReader() { heap_caps_free(buf); }

    bool refill(uint64_t want)
    {
        uint64_t next = buf_off + buf_len;
        // Sequential if the wanted byte is in the chunk that follows the buffer.
        uint64_t start = (buf_len && want >= next && want < next + cap) ? next : (want & ~uint64_t(511));
        if (start != file_pos && lseek(fd, (off_t)start, SEEK_SET) < 0) return false;
        ssize_t r = read(fd, buf, cap);
        if (r <= 0) return false;
        buf_off = start;
        buf_len = (size_t)r;
        file_pos = start + r;
        return true;
    }

    bool get(uint64_t off, uint8_t *dst, size_t len)
    {
        while (len) {
            if (!(off >= buf_off && off < buf_off + buf_len) && !refill(off)) return false;
            size_t n = std::min<uint64_t>(len, buf_off + buf_len - off);
            memcpy(dst, buf + (off - buf_off), n);
            dst += n; off += n; len -= n;
        }
        return true;
    }
};

// Fill slots in frame order. Preferred source: raw sector reads of the file's
// cluster runs by a read-ahead task (raw_reader.h); fallback: the buffered
// FatFs StreamReader above, for fragmented files.
static void reader_loop(ReaderCtx *c)
{
    const auto &frames = c->avi->frames;
    ReadAhead ra;
    StreamReader sr;
    bool use_raw = false;
    if (c->raw) {
        // SD: big reads DMA straight into PSRAM. USB: msc_host bounces each
        // transfer through an internal-RAM buffer of the request size, and a
        // 128 KB request timed out (2026-11-23; the DWC transfer size field
        // apparently tops out just below 128 KB), so 127 KB per request.
        const bool usb = strncmp(c->path, USB_MOUNT_POINT "/", sizeof(USB_MOUNT_POINT)) == 0;
        const size_t chunk = usb ? 127 * 1024 : 512 * 1024;
        const uint64_t begin = frames.front().offset;
        const uint64_t end = frames.back().offset + frames.back().size;
        use_raw = ra.start(c->raw, begin, end, chunk, 6u << 20);  // ~6 MB ring
        if (use_raw) c->ra = &ra;
    }
    if (!use_raw && (c->fd < 0 || !sr.init(c->fd))) {
        ESP_LOGE(TAG, "no internal DMA memory for the stream reader");
        c->error = true;
        return;
    }
    boot_mark("reader_started");
    ESP_LOGI(TAG, "source: %s", use_raw ? "raw sectors + read-ahead" : "FatFs stream reader");
    uint32_t i = 0;
    uint64_t pass = 0;
    bool wrapped = false;
    while (!c->stop) {
        int s;
        if (xQueueReceive(c->q_free, &s, pdMS_TO_TICKS(50)) != pdTRUE) continue;
        Slot &sl = c->slots[s];
        const AviFrame &f = frames[i];
        int64_t t0 = esp_timer_get_time();
        bool ok;
        if (use_raw) {
            const uint64_t spos = pass * ra.span() + (f.offset - ra.begin());
            sl.data = ra.acquire(spos, f.size, sl.buf);
            sl.spos_end = spos + f.size;
            ok = sl.data != nullptr;
        } else {
            ok = sr.get(f.offset, sl.buf, f.size);
            sl.data = sl.buf;
            sl.spos_end = 0;
        }
        if (!ok) {
            if (c->stop) break;  // teardown unblocked acquire(); not a read error
            ESP_LOGE(TAG, "read failed at frame %u (errno %d) - stick removed?", (unsigned)i, errno);
            c->error = true;
            break;
        }
        sl.len = f.size;
        sl.index = i;
        sl.read_us = (uint32_t)(esp_timer_get_time() - t0);
        sl.wrapped = wrapped;
        wrapped = false;
        xQueueSend(c->q_full, &s, portMAX_DELAY);
        if (++i == frames.size()) { i = 0; wrapped = true; pass++; }
    }
    // A slot handed to the decoder may point into the ring: after an error,
    // keep the ring until player_run has left its decode loop (it sets stop).
    while (!c->stop) vTaskDelay(pdMS_TO_TICKS(5));
    c->ra = nullptr;
    ra.stop();
}  // destructors free the stream buffer and read-ahead chunks here

static void reader_task(void *arg)
{
    auto *c = static_cast<ReaderCtx *>(arg);
    // vTaskDelete() does not unwind the stack, so all RAII objects live in
    // reader_loop(), which returns first (a leak of 128 KB per file, 2026-11-23).
    reader_loop(c);
    c->done = true;
    vTaskDelete(nullptr);
}

// ---------------------------------------------------------------- stats

struct Stats {
    int64_t t_start;
    uint32_t frames, late, copies;
    uint64_t bytes;
    uint64_t read_sum, dec_sum, wait_sum;
    uint32_t read_max, dec_max, wait_max;
    uint32_t q_min;
    void reset()
    {
        *this = Stats{};
        t_start = esp_timer_get_time();
        q_min = kNumSlots;
    }
};

static void print_stats(const Stats &s, uint32_t loops, uint32_t idx, uint32_t total)
{
    double dt = (esp_timer_get_time() - s.t_start) / 1e6;
    if (dt <= 0 || s.frames == 0) return;
    double n = s.frames;
    // io_MBps: bytes the read-ahead task actually pulled from storage. When the
    // decoder keeps up, it equals MBps; when storage is the limit, it is the
    // storage ceiling.
    const double io = ReadAhead::take_io_bytes() / dt / 1048576.0;
    printf("STAT fps=%.2f read_ms=%.2f/%.2f dec_ms=%.2f/%.2f wait_ms=%.2f/%.2f late=%" PRIu32
           " copy=%" PRIu32 " MBps=%.2f io_MBps=%.2f q_min=%" PRIu32 " loop=%" PRIu32 " frame=%" PRIu32 "/%" PRIu32
           " psram_free=%u mode=%s\n",
           n / dt, s.read_sum / n / 1000.0, s.read_max / 1000.0, s.dec_sum / n / 1000.0,
           s.dec_max / 1000.0, s.wait_sum / n / 1000.0, s.wait_max / 1000.0, s.late, s.copies,
           s.bytes / dt / 1048576.0, io, s.q_min, loops, idx, total,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           g_ctl.free_run ? "free" : "paced");
}

// ---------------------------------------------------------------- player

static jpeg_decoder_handle_t s_jpeg;

jpeg_decoder_handle_t player_jpeg()
{
    if (!s_jpeg) {
        jpeg_decode_engine_cfg_t ecfg = {.intr_priority = 0, .timeout_ms = 200};
        ESP_ERROR_CHECK(jpeg_new_decoder_engine(&ecfg, &s_jpeg));
    }
    return s_jpeg;
}

static uint32_t wait_fb_free(int k)
{
    // Busy frame buffers are rare (only if decode outruns a refresh); 1 ms polls are fine.
    int64_t t0 = esp_timer_get_time();
    while ((int32_t)(s_vsync - s_retired_at[k]) <= 0) vTaskDelay(1);
    return (uint32_t)(esp_timer_get_time() - t0);
}

static volatile bool s_booted = false;

bool player_has_presented() { return s_booted; }

void boot_mark(const char *what)
{
    if (!s_booted) printf("BOOT %s app_ms=%lld\n", what, esp_timer_get_time() / 1000);
}

static bool player_run_impl(const char *path);

bool player_run(const char *path)
{
    // Keep the USB drive mounted until every reader of it has stopped.
    const bool usb = strncmp(path, USB_MOUNT_POINT "/", sizeof(USB_MOUNT_POINT)) == 0;
    if (usb) usb_stick_hold();
    const bool ok = player_run_impl(path);
    if (usb) usb_stick_release();
    return ok;
}

static bool player_run_impl(const char *path)
{
    player_jpeg();  // create the decoder engine once
    boot_mark("jpeg_engine");

    // Files come from the USB drive (preferred) or the add-on's microSD. The
    // drive's removal shows in usb_stick_mounted(); an SD pull shows up as a
    // read error; an SD file also stops when a USB drive mounts.
    const bool from_usb = strncmp(path, USB_MOUNT_POINT "/", sizeof(USB_MOUNT_POINT)) == 0;
    // Raw sector source first (one FAT walk, see raw_reader.h); the VFS fd
    // only when that fails (fragmented beyond reason, non-FAT, ...).
    std::unique_ptr<RawFile> raw(new RawFile);
    int fd = -1;
    std::unique_ptr<FdSource> fd_src;
    if (!raw->open(path)) {
        raw.reset();
        fd = open(path, O_RDONLY);
        if (fd < 0) {
            ESP_LOGE(TAG, "open %s failed (errno %d)", path, errno);
            return false;
        }
        fd_src.reset(new FdSource(fd));
    }
    ByteSource &src = raw ? static_cast<ByteSource &>(*raw) : *fd_src;
    boot_mark("file_open");
    int64_t t_idx = esp_timer_get_time();
    static AviInfo avi;  // large vector lives in PSRAM via malloc
    if (!avi_build_index(src, avi)) {
        ESP_LOGE(TAG, "%s: no MJPEG frames found", path);
        if (fd >= 0) close(fd);
        return false;
    }
    ESP_LOGI(TAG, "indexed %s in %lld ms", path, (esp_timer_get_time() - t_idx) / 1000);
    boot_mark("indexed");

    // Read the first frame's header to choose zero-copy or scratch path.
    uint8_t hdr[4096];
    ssize_t hn = (ssize_t)std::min<size_t>(sizeof(hdr), avi.frames[0].size);
    if (!src.pread(avi.frames[0].offset, hdr, hn)) hn = -1;
    jpeg_decode_picture_info_t pic = {};
    if (hn <= 0 || jpeg_decoder_get_info(hdr, hn, &pic) != ESP_OK) {
        ESP_LOGE(TAG, "first frame is not a JPEG the decoder understands");
        if (fd >= 0) close(fd);
        return false;
    }
    if (pic.width != P4LOOP_WIDTH || pic.height != P4LOOP_HEIGHT) {
        ESP_LOGE(TAG, "video is %ux%u but HDMI mode is %dx%d - re-encode with tools/p4_transcode.py",
                 (unsigned)pic.width, (unsigned)pic.height, P4LOOP_WIDTH, P4LOOP_HEIGHT);
        if (fd >= 0) close(fd);
        return false;
    }
#if P4LOOP_FB_FMT == 4
    // The engine cannot turn 4:4:4 (or 4:2:0) into YUV422 output.
    if (pic.sample_method != JPEG_DOWN_SAMPLING_YUV422) {
        ESP_LOGE(TAG, "%s: JPEG sampling is not 4:2:2; YUV422 frame buffers need 4:2:2 (tools/p4_transcode.py)", path);
        if (fd >= 0) close(fd);
        return false;
    }
#endif
    // Decoded height is rounded up to the luma MCU height, read from the SOF
    // marker: 8 for true 4:2:2 / 4:4:4, 16 for 4:2:0 and ffmpeg's 2x2 "4:2:2".
    uint32_t mcu_h = 8;
    for (ssize_t i = 2; i + 12 < hn; ) {
        if (hdr[i] != 0xFF) break;
        const uint8_t m = hdr[i + 1];
        const uint32_t len = (hdr[i + 2] << 8) | hdr[i + 3];
        if (m == 0xC0 || m == 0xC1 || m == 0xC2) { mcu_h = (hdr[i + 11] & 0x0F) * 8; break; }
        if (len < 2) break;  // malformed marker
        i += 2 + len;
    }
    if (mcu_h == 0) mcu_h = 8;  // sampling factor 0 in a damaged SOF
    const uint32_t dec_h = (pic.height + mcu_h - 1) / mcu_h * mcu_h;
    const uint32_t dec_w = (pic.width + 15) / 16 * 16;
    const size_t dec_size = (size_t)dec_w * dec_h * P4LOOP_FB_BYTES;
    const bool zero_copy = dec_size <= s_fb_size;
    boot_mark("header_checked");
    uint8_t *scratch = nullptr;
    size_t scratch_size = 0;
    if (!zero_copy) {
        jpeg_decode_memory_alloc_cfg_t mc = {.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER};
        scratch = static_cast<uint8_t *>(jpeg_alloc_decoder_mem(dec_size, &mc, &scratch_size));
        if (!scratch) { ESP_LOGE(TAG, "no memory for scratch buffer"); if (fd >= 0) close(fd); return false; }
        ESP_LOGW(TAG, "sampling %d decodes to %ux%u > frame buffer: scratch + copy path (encode 4:2:2 for zero-copy)",
                 (int)pic.sample_method, (unsigned)dec_w, (unsigned)dec_h);
    }

    // Input ring.
    auto *rc = new ReaderCtx{};
    rc->fd = fd;
    rc->raw = raw.get();
    rc->path = path;
    rc->avi = &avi;
    rc->q_free = xQueueCreate(kNumSlots, sizeof(int));
    rc->q_full = xQueueCreate(kNumSlots, sizeof(int));
    size_t cap = (avi.max_frame + 4095) & ~4095u;
    bool alloc_ok = true;
    for (int s = 0; s < kNumSlots; s++) {
        jpeg_decode_memory_alloc_cfg_t mc = {.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER};
        rc->slots[s].buf = static_cast<uint8_t *>(jpeg_alloc_decoder_mem(cap, &mc, &rc->slots[s].cap));
        if (!rc->slots[s].buf) { alloc_ok = false; break; }
        xQueueSend(rc->q_free, &s, 0);
    }
    if (!alloc_ok) {
        ESP_LOGE(TAG, "no memory for %d x %u B input slots", kNumSlots, (unsigned)cap);
        // fallthrough to cleanup below
    }

    boot_mark("slots_allocated");
    const uint32_t period_us = avi.us_per_frame ? avi.us_per_frame : 33333;
    ESP_LOGI(TAG, "playing %s: %u frames, period %u us, %s", path, (unsigned)avi.frames.size(),
             (unsigned)period_us, zero_copy ? "zero-copy" : "copy");

    bool ok = alloc_ok;
    if (ok && xTaskCreatePinnedToCore(reader_task, "reader", 8192, rc, 6, nullptr, 0) != pdPASS) {
        ESP_LOGE(TAG, "could not start the reader task");
        ok = alloc_ok = false;  // teardown must not wait for a reader that never ran
    }

    static bool first_frame_logged = false;
    Stats st;
    st.reset();
    uint32_t loops = 0, last_idx = 0;
    int k = (s_cur + 1) % kNumFb;  // never decode into the buffer on screen
    int64_t next_t = esp_timer_get_time();
    int64_t last_present = next_t;
    int64_t last_print = next_t;

    while (ok) {
        // USB first: a drive that mounts while the microSD plays takes over.
        if (g_ctl.next_file || (from_usb != usb_stick_mounted()) || rc->error) break;
        if (g_ctl.reset_stats.exchange(false)) st.reset();
        if (g_ctl.dump_status.exchange(false)) print_stats(st, loops, last_idx, avi.frames.size());
        if (g_ctl.paused) { vTaskDelay(pdMS_TO_TICKS(20)); next_t = esp_timer_get_time(); continue; }

        UBaseType_t depth = uxQueueMessagesWaiting(rc->q_full);
        st.q_min = std::min<uint32_t>(st.q_min, depth);
        int s;
        if (xQueueReceive(rc->q_full, &s, pdMS_TO_TICKS(500)) != pdTRUE) {
            ESP_LOGW(TAG, "reader starved for 500 ms");
            continue;
        }
        Slot &sl = rc->slots[s];
        if (sl.wrapped) loops++;

        uint32_t wait_us = zero_copy ? wait_fb_free(k) : 0;
        uint8_t *dst = zero_copy ? s_fb[k] : scratch;
        size_t dst_size = zero_copy ? s_fb_size : scratch_size;

        jpeg_decode_cfg_t dcfg = {
            .output_format = P4LOOP_JPEG_OUT,
            .rgb_order = g_ctl.rgb_swap ? JPEG_DEC_RGB_ELEMENT_ORDER_RGB : JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
            .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
        };
        uint32_t out_size = 0;
        int64_t t0 = esp_timer_get_time();
        esp_err_t err = jpeg_decoder_process(s_jpeg, &dcfg, sl.data, sl.len, dst, dst_size, &out_size);
        uint32_t dec_us = (uint32_t)(esp_timer_get_time() - t0);
        uint32_t read_us = sl.read_us, len = sl.len, idx = sl.index;
        // The engine has consumed the frame: let read-ahead reuse its ring bytes, free the slot.
        if (rc->ra && sl.spos_end) rc->ra->release(sl.spos_end);
        xQueueSend(rc->q_free, &s, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "decode failed at frame %u: %s", (unsigned)idx, esp_err_to_name(err));
            continue;
        }

        // Pace to the file's frame rate unless measuring the ceiling.
        int64_t now = esp_timer_get_time();
        if (!g_ctl.free_run) {
            next_t += period_us;
            if (now > next_t + period_us) {  // more than a frame behind: count it and resync
                st.late++;
                next_t = now;
            } else {
                while (next_t - esp_timer_get_time() > 2000) vTaskDelay(1);
                while (esp_timer_get_time() < next_t) {}
            }
        }

        // Present. Zero-copy: pointer switch at the next vsync. Copy path: the driver
        // copies into the *current* frame buffer, so it can tear; it is a fallback.
        esp_lcd_panel_draw_bitmap(s_panel, 0, 0, P4LOOP_WIDTH, P4LOOP_HEIGHT, dst);
        int64_t t_present = esp_timer_get_time();
        if (zero_copy) {
            // The buffer we just left stays on screen until the next vsync.
            s_retired_at[s_cur] = s_vsync;
            s_cur = k;
            k = (k + 1) % kNumFb;
        } else {
            st.copies++;
        }

        if (!first_frame_logged) {
            first_frame_logged = true;
            printf("FIRST_FRAME app_ms=%lld\n", t_present / 1000);
            s_booted = true;
        }
        if (g_ctl.verbose) {
            printf("F,%u,%u,%u,%u,%u,%lld\n", (unsigned)idx, (unsigned)len, (unsigned)read_us,
                   (unsigned)dec_us, (unsigned)wait_us, (t_present - last_present));
        }
        last_present = t_present;
        last_idx = idx;

        st.frames++;
        st.bytes += len;
        st.read_sum += read_us; st.read_max = std::max(st.read_max, read_us);
        st.dec_sum += dec_us;   st.dec_max = std::max(st.dec_max, dec_us);
        st.wait_sum += wait_us; st.wait_max = std::max(st.wait_max, wait_us);

        if (t_present - last_print >= 1000000) {
            print_stats(st, loops, idx, avi.frames.size());
            st.reset();
            last_print = t_present;
        }
    }

    // Tear down: stop the reader before freeing anything it touches. First
    // unblock a reader waiting in acquire() (the decoder no longer releases
    // ring space), then signal stop; the reader clears rc->ra only after it
    // has seen stop, so rc->ra is still valid here.
    if (ReadAhead *ra = rc->ra) ra->request_stop();
    rc->stop = true;
    while (alloc_ok && !rc->done) vTaskDelay(pdMS_TO_TICKS(5));
    for (int s = 0; s < kNumSlots; s++) free(rc->slots[s].buf);
    vQueueDelete(rc->q_free);
    vQueueDelete(rc->q_full);
    bool had_error = rc->error;
    delete rc;
    free(scratch);
    if (fd >= 0) close(fd);
    g_ctl.next_file = false;
    return ok && !had_error;
}
