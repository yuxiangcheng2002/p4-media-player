# 2026-11-22 decode bench (decode speed, scan-out load, boot) — Unit PoE-P4, ESP32-P4 rev v1.3

*Superseded: early bench without HDMI (FPC cable missing at the time); YUV422 byte order and colours were later settled as UYVY. Current results: `2026-11-23-boot-time-2/`.*

Firmware: p4_loop bench build of 2026-11-22, IDF 5.4.2, CPU 360 MHz,
PSRAM 200 MHz, PSRAM memtest off. No LT8912B (FPC cable missing): scan-out runs
DSI-only with M5GFX's 1080p timing (80 MHz pclk, 2080x1096 total, measured
35.0-35.2 Hz refresh, RGB888, 3 frame buffers). Frames embedded in flash,
encoded by tools/make_bench_frames.sh (Pillow, quality 90). Raw serial logs are kept privately and not published.

## Decode time per 1920x1080 frame (ms), 60 runs each

| frame | bytes | idle | with scan-out |
|---|---|---|---|
| flat 4:2:2 | 86,604 | 26.99 | 41.86 |
| photo 4:2:2 | 117,572 | 26.98 | 41.97 |
| typical 4:2:2 | 242,412 | 27.00 | 42.43 |
| noisy 4:2:2 | 700,365 | 27.00 | 43.96 |
| typical 4:4:4 | 313,776 | 40.44 | (underruns, see below) |

- Idle: decode time is independent of compressed size; 4:2:2 ceiling 37 fps,
  4:4:4 24.7 fps.
- With scan-out reading PSRAM, 4:2:2 decode slows by ~55 % to 42-44 ms
  (22.7-23.9 fps ceiling).

## Full path (wait for free buffer, decode into it, present), 120 frames

| frame | fps | refresh Hz | decode avg ms |
|---|---|---|---|
| flat 4:2:2 | 24.07 | 35.10 | 41.53 |
| photo 4:2:2 | 24.01 | 35.01 | 41.64 |
| typical 4:2:2 | 23.73 | 35.20 | 42.13 |
| noisy 4:2:2 | 22.83 | 35.01 | 43.79 |
| typical 4:4:4 | 23.38 | 27.27 | 40.48 |

## DPI underruns ("can't fetch data from external memory fast enough")
268 in total. None while decoding the flat and photo frames (86-118 KB);
they start with the noisy decode under scan-out and recur during the
typical, noisy and 4:4:4 present runs. 4:4:4 also drags the refresh to 27 Hz.
An underrun is a visible glitch on a real screen.

## Boot (test 5)
"cpu_start: Pro cpu start user code" at 507 ms after reset with the PSRAM
memtest off, against 1054 ms with it on (earlier logs, smaller app). The
memtest itself took ~760 ms. With this 1.9 MB bench image, PSRAM is found
at 496 ms against 282 ms with the 0.55 MB app: loading and verifying the
bigger image costs ~210 ms.

## Conclusions
1. 1080p30 MJPEG with RGB888 scan-out is not reachable on this board: the
   pipeline tops out at ~24 fps and starves the display on realistic frames.
2. ffmpeg's mjpeg "4:2:2" uses 2x2 luma sampling (16-line MCU) and decodes to
   1088 lines on the P4; true 4:2:2 (2x1 luma, libjpeg/Pillow) decodes to 1080
   and is required for zero-copy. tools/make_clips.sh still uses ffmpeg's
   encoder and needs changing before clips are made for the P4.
