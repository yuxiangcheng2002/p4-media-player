# 2026-11-22 decode bench, 720p60 mode — Unit PoE-P4, ESP32-P4 rev v1.3

*Superseded: early bench without HDMI (FPC cable missing at the time); YUV422 byte order and colours were later settled as UYVY. Current results: `2026-11-23-boot-time-2/`.*

Same bench and conditions as `../2026-11-22-bench/summary.md`, built with
`-DP4LOOP_720P=1` (a build option since replaced by `mode = 720` in
`config.txt`): 1280x720 frames (tools/make_bench_frames.sh, bench/720),
DSI-only scan-out with M5GFX's 720p timing (80 MHz pclk, 1650x750 total,
measured 64.2-64.8 Hz refresh), RGB888, 3 frame buffers. Raw serial logs are kept privately and not published.

## Decode time per 1280x720 frame (ms), 60 runs each

| frame | bytes | idle | with scan-out |
|---|---|---|---|
| flat 4:2:2 | 47,752 | 12.11 | 14.04 |
| photo 4:2:2 | 63,542 | 12.08 | 14.08 |
| typical 4:2:2 | 116,457 | 12.09 | 14.26 |
| noisy 4:2:2 | 319,228 | 12.10 | 14.91 |
| typical 4:4:4 | 150,530 | 18.08 | 18.09 |

## Full path (wait, decode into frame buffer, present), 120 frames, back to back

| frame | fps | refresh Hz | decode avg ms |
|---|---|---|---|
| flat 4:2:2 | 71.21 | 64.69 | 13.90 |
| photo 4:2:2 | 71.11 | 64.59 | 13.96 |
| typical 4:2:2 | 70.07 | 64.81 | 14.14 |
| noisy 4:2:2 | 66.42 | 64.20 | 14.86 |
| typical 4:4:4 | 55.25 | 64.46 | 18.09 |

## DPI underruns
None (the only grep hit is the bench's own closing message).

## Conclusion
720p works with a wide margin: decoding back to back reaches 66-71 fps with
scan-out running and no underruns; 720p30 content needs 14-15 ms of decode
per 33 ms frame (~45 % duty). Scan-out cost is ~15 % here against ~55 % at
1080p. Boot: app starts 401 ms after reset (smaller image than the 1080p
bench build).
