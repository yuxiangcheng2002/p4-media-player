# 2026-11-22 1080p bench with 2-byte frame buffers

*Superseded: early bench without HDMI (FPC cable missing at the time); YUV422 byte order and colours were later settled as UYVY. Current results: `2026-11-23-boot-time-2/`.*

Same bench as `2026-11-22-bench/summary.md` (1080p, DSI-only scan-out with
M5GFX timing, 80 MHz pclk, ~35 Hz), varying only the frame-buffer format
(`-DP4LOOP_FB_FMT=n`, see firmware/p4_loop/main/player.h). Raw serial logs are kept privately and not published.

| fmt | PSRAM fb | on DSI | scan-out runs | decode under scan-out (4:2:2) | full path fps | underruns |
|---|---|---|---|---|---|---|
| 1 (baseline) | RGB888 | RGB888 | yes | 41.9-44.0 ms | 22.8-24.1 | 268 |
| 2 | RGB565 | RGB888 | **no** (no refresh events) | 27.0 ms (no load) | — | 0 |
| 3 | RGB565 | RGB565 | yes, 34.8-35.1 Hz | 27.0 ms | 36.9 | 0 |
| 4 | YUV422 | RGB888 | yes, 35.1 Hz | 27.0 ms | 36.9 | 0 |

- fmt 2 is a dead end: IDF 5.4.2's DPI driver only implements YUV422 -> RGB
  conversion in the DSI bridge (`esp_lcd_dpi_panel_set_color_conversion`
  rejects anything else), and with RGB565 in / RGB888 out the scan-out never
  produces a refresh.
- With 2-byte frame buffers the decode is no slower than with no scan-out at
  all (27.0 ms for every 4:2:2 frame, 70 KB to 700 KB), and the pipeline
  holds ~37 fps with zero underruns: **1080p30 is reachable**.
- 4:4:4 JPEG cannot be decoded to YUV422 (driver: "Detected YUV444 but want
  to convert to YUV422 ... not supported"); content must be true 4:2:2.
- fmt 4 keeps RGB888 on the DSI link, so the LT8912B setup from M5GFX stays
  valid. fmt 3 would need the LT8912B switched to RGB565 MIPI input (unknown
  whether it supports it).
- Not verified without the FPC cable: the YUV422 byte order (YUYV assumed)
  and colour range (full range assumed) in fmt 4, i.e. that colours are right.
