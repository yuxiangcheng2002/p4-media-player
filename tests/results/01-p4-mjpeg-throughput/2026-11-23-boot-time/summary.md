# Boot to first frame, USB Host drive (2026-11-23)

Board: Unit PoE-P4 rev v1.3.
Media: fresh FAT32 card in a USB-C reader on the Unit's USB-C Host port,
a 4K source clip transcoded to 1080p q90 with a 512 KB frame cap (3752 frames, 1 fragment).
Three resets via `tools/serial_capture.py --reset`. Raw serial logs are kept privately and not published.

Changes against the previous build (app FIRST_FRAME 3866 ms):
1. `avi_index.cpp` reads the AVI's `idx1` table in one read instead of walking
   every chunk header (falls back to the walk for files without idx1 or for
   OpenDML files). Host check: identical frame tables to the walk on 5 files.
2. `main.cpp` starts the USB host first thing, so enumeration runs while the
   display comes up.

| milestone (app ms, esp_timer) | run 1 | run 2 | run 3 | before |
|---|---|---|---|---|
| DISPLAY_UP | 282 | 283 | 284 | ~252 |
| STICK_MOUNTED | 762 | 761 | 766 | ~1070 |
| index | 10 ms (idx1) | 11 ms | 10 ms | 2206 ms (walk) |
| FIRST_FRAME | 1361 | 1360 | 1366 | 3866 |

Reset to app start (cpu_start) is ~490 ms, so reset to first frame is
~1.85 s (was ~4.3 s). Playback afterwards: 30.03 fps, 0 late, q_min 6.

Log timestamps `I (n)` in the app run ahead of esp_timer and the host clock;
use `app_ms` lines and the host column for timing.

Where the remaining ~1.36 s of app time goes (host-clock deltas, run 1):
- 0 -> 282 ms: LT8912B and DSI bring-up (USB enumerates in parallel).
- 282 -> 762 ms: waiting for USB enumeration + MSC mount.
- 762 -> ~1160 ms: file list, open, first-frame format check, idx1 read.
- ~1160 -> ~1360 ms: raw-reader open (FatFs link map over USB, ~200 ms)
  and the first read-ahead + decode.
