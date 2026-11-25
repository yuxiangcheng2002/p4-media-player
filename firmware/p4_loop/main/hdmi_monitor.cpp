// HDMI link monitor; see hdmi_monitor.h for what it prints and why.
//
// What the P4 can see of the HDMI side (HDMI gives a source no "picture
// accepted" signal, so these are the only clues):
// - Hot-plug detect: LT8912B main page 0xC1 bit 7 (same read as the driver's
//   _panel_lt8912b_get_hpd). High when a monitor is attached and powered
//   enough to assert HPD; many monitors drop it in standby or on another input.
// - The bridge's measurement of our DSI input, main 0x9C-0x9F (what the driver
//   logs once at init as "Detected MIPI input"): steady values mean the bridge
//   receives a stable stream; zeros or jumping values mean it does not.
// - The bridge's pixel-clock synthesiser (DDS), CEC/DSI page 0x0C-0x0F: the
//   driver's disabled read_dds() treats 0x0E == 0xD2 with 0xD0 < 0x0D < 0xFF
//   as "stable"; we only print the raw bytes.
// - EDID at I2C 0x50, only if the Display Out board routes the HDMI DDC lines
//   to the P4's I2C (not known from the schematics we have; the probe answers
//   it). EDID says which modes the monitor accepts, which is the question when
//   one monitor shows the picture and another stays dark.
#include "hdmi_monitor.h"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "hdmi";

namespace {
struct Ctx {
    i2c_master_bus_handle_t bus;
    esp_lcd_panel_io_handle_t main, cec;
    int hact, vact, htotal, vtotal, pclk_mhz;
};
Ctx s_ctx;
volatile int s_hpd = -1;

uint8_t rd(esp_lcd_panel_io_handle_t io, uint8_t reg)
{
    uint8_t v = 0;
    if (esp_lcd_panel_io_rx_param(io, reg, &v, 1) != ESP_OK) return 0xEE;  // 0xEE marks a failed read
    return v;
}

bool edid_block(i2c_master_dev_handle_t dev, int block, uint8_t out[128])
{
    uint8_t off = (uint8_t)(block * 128);  // blocks 0 and 1 only (no segment pointer)
    for (int i = 0; i < 128; i += 16) {
        uint8_t o = off + i;
        if (i2c_master_transmit_receive(dev, &o, 1, out + i, 16, 100) != ESP_OK) return false;
    }
    return true;
}

void print_hex(const char *tag, const uint8_t *b, int n)
{
    for (int i = 0; i < n; i += 32) {
        char line[100];
        int p = 0;
        for (int j = i; j < i + 32 && j < n; j++) p += snprintf(line + p, sizeof line - p, "%02x", b[j]);
        printf("%s %03d %s\n", tag, i, line);
    }
}

// Parse and print the parts of EDID that decide whether our mode is accepted:
// identity, the monitor's declared range limits, its preferred (native) timing,
// and, from a CEA extension, the list of video codes (VICs) it supports.
void read_edid()
{
    if (i2c_master_probe(s_ctx.bus, 0x50, 50) != ESP_OK) {
        printf("HDMI_EDID none (no answer at 0x50: DDC not on this I2C bus, or no monitor)\n");
        return;
    }
    i2c_device_config_t dc = {};
    dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dc.device_address = 0x50;
    dc.scl_speed_hz = 100000;
    i2c_master_dev_handle_t dev = nullptr;
    if (i2c_master_bus_add_device(s_ctx.bus, &dc, &dev) != ESP_OK) return;
    uint8_t b0[128];
    if (!edid_block(dev, 0, b0)) {
        printf("HDMI_EDID read failed\n");
        i2c_master_bus_rm_device(dev);
        return;
    }
    static const uint8_t hdr[8] = {0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0};
    uint8_t sum = 0;
    for (int i = 0; i < 128; i++) sum += b0[i];
    printf("HDMI_EDID header=%s checksum=%s ext=%d\n", memcmp(b0, hdr, 8) ? "bad" : "ok", sum ? "bad" : "ok",
           b0[126]);
    print_hex("HDMI_EDID_RAW0", b0, 128);
    char mfg[4] = {(char)('@' + ((b0[8] >> 2) & 31)), (char)('@' + (((b0[8] & 3) << 3) | (b0[9] >> 5))),
                   (char)('@' + (b0[9] & 31)), 0};
    printf("HDMI_EDID mfg=%s product=0x%04x year=%d edid=%d.%d\n", mfg, b0[10] | (b0[11] << 8), 1990 + b0[17],
           b0[18], b0[19]);

    const double hk = s_ctx.pclk_mhz * 1000.0 / s_ctx.htotal;      // our line rate, kHz
    const double vz = hk * 1000.0 / s_ctx.vtotal;                   // our frame rate, Hz
    for (int d = 54; d <= 108; d += 18) {
        const uint8_t *p = b0 + d;
        int pclk10k = p[0] | (p[1] << 8);
        if (pclk10k) {  // detailed timing descriptor
            int ha = p[2] | ((p[4] & 0xf0) << 4), hb = p[3] | ((p[4] & 0x0f) << 8);
            int va = p[5] | ((p[7] & 0xf0) << 4), vb = p[6] | ((p[7] & 0x0f) << 8);
            double hz = pclk10k * 1e4 / ((double)(ha + hb) * (va + vb));
            printf("HDMI_EDID timing%s %dx%d total %dx%d pclk %.2f MHz %.2f Hz\n", d == 54 ? " (preferred)" : "",
                   ha, va, ha + hb, va + vb, pclk10k / 100.0, hz);
        } else if (p[3] == 0xfc || p[3] == 0xff) {  // name, serial
            char s[14] = {};
            for (int i = 0; i < 13 && p[5 + i] != 0x0a; i++) s[i] = (char)p[5 + i];
            printf("HDMI_EDID %s=\"%s\"\n", p[3] == 0xfc ? "name" : "serial", s);
        } else if (p[3] == 0xfd) {  // range limits (offset flags in byte 4 ignored: they add 255 for >255)
            int vmin = p[5], vmax = p[6], hmin = p[7], hmax = p[8];
            if (p[4] & 1) vmin += 255;
            if (p[4] & 2) vmax += 255;
            if (p[4] & 4) hmin += 255;
            if (p[4] & 8) hmax += 255;
            printf("HDMI_EDID range v=%d-%d Hz h=%d-%d kHz pclk<=%d MHz\n", vmin, vmax, hmin, hmax, p[9] * 10);
            if (vz < vmin || vz > vmax || hk < hmin || hk > hmax)
                printf("HDMI_WARN our mode (%.2f Hz, %.2f kHz) is outside this monitor's declared range\n", vz, hk);
        }
    }
    if (b0[126] >= 1) {
        uint8_t b1[128];
        if (edid_block(dev, 1, b1)) {
            print_hex("HDMI_EDID_RAW1", b1, 128);
            if (b1[0] == 0x02) {  // CEA-861 extension: walk the data blocks for the video block
                int dtd = b1[2];
                char vics[200];
                int n = 0;
                vics[0] = 0;
                for (int i = 4; i < dtd && i < 127;) {
                    int tag = b1[i] >> 5, len = b1[i] & 31;
                    if (tag == 2)
                        for (int k = 1; k <= len && i + k < 127; k++)
                            n += snprintf(vics + n, sizeof vics - n, "%s%d", n ? "," : "", b1[i + k] & 0x7f);
                    i += 1 + len;
                }
                printf("HDMI_EDID cea vics=%s%s\n", n ? vics : "(none)",
                       (b1[3] & 0x40) ? " basic_audio" : "");
                // VIC 34 = 1920x1080p30, 4 = 1280x720p60: the standard modes ours approximate.
            }
        }
    }
    i2c_master_bus_rm_device(dev);
}

void monitor_task(void *)
{
    int last = -1;
    int64_t last_print = 0;
    uint32_t edges = 0;
    for (;;) {
        uint8_t c1 = rd(s_ctx.main, 0xc1);
        int hpd = c1 == 0xEE ? -1 : (c1 & 0x80) ? 1 : 0;
        int64_t now = esp_timer_get_time();
        bool changed = hpd != last;
        if (changed || now - last_print >= 10 * 1000000LL) {
            if (changed && last >= 0) edges++;
            uint8_t h_l = rd(s_ctx.main, 0x9c), h_h = rd(s_ctx.main, 0x9d);
            uint8_t v_l = rd(s_ctx.main, 0x9e), v_h = rd(s_ctx.main, 0x9f);
            uint8_t d0 = rd(s_ctx.cec, 0x0c), d1 = rd(s_ctx.cec, 0x0d), d2 = rd(s_ctx.cec, 0x0e),
                    d3 = rd(s_ctx.cec, 0x0f);
            printf("HDMI hpd=%d c1=0x%02x in_h=0x%02x%02x in_v=0x%02x%02x dds=%02x%02x%02x%02x%s edges=%lu up_ms=%lld\n",
                   hpd, c1, h_h, h_l, v_h, v_l, d0, d1, d2, d3,
                   (d2 == 0xd2 && d1 > 0xd0 && d1 < 0xff) ? "(stable)" : "", (unsigned long)edges, now / 1000);
            last_print = now;
            s_hpd = hpd;
            if (changed && hpd == 1) {
                vTaskDelay(pdMS_TO_TICKS(200));  // let the monitor's DDC come up after HPD
                read_edid();
            }
            last = hpd;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}
}  // namespace

int hdmi_monitor_hpd() { return s_hpd; }

void hdmi_monitor_start(i2c_master_bus_handle_t bus, esp_lcd_panel_io_handle_t io_main,
                        esp_lcd_panel_io_handle_t io_cec_dsi, int hact, int vact, int htotal, int vtotal,
                        int pclk_mhz, int vic)
{
    s_ctx = {bus, io_main, io_cec_dsi, hact, vact, htotal, vtotal, pclk_mhz};
    const double hk = pclk_mhz * 1000.0 / htotal, vz = hk * 1000.0 / vtotal;
    printf("HDMI_OUT %dx%d total %dx%d pclk %d MHz -> %.2f kHz, %.2f Hz, VIC %d%s\n", hact, vact, htotal, vtotal,
           pclk_mhz, hk, vz, vic, vic ? " claimed in the AVI infoframe (the CEA mode runs at 74.25 MHz)" : " (no CEA mode claimed)");
    // An EDID probe with no monitor is answered by nobody; the task re-reads on HPD rise.
    if (xTaskCreatePinnedToCore(monitor_task, "hdmi_mon", 4096, nullptr, 2, nullptr, 0) != pdPASS)
        ESP_LOGE(TAG, "monitor task not started");
}
