# 01 — P4 MJPEG throughput (USB stick → JPEG HW decode → DSI → LT8912B → HDMI)

Historical: the plan written before the first flash. Results are in
`tests/results/01-p4-mjpeg-throughput/`; the current design is described in
`firmware/p4_loop/README.md` (e.g. frame buffers are now YUV422, not RGB888).

## Question
Can an M5Stack Unit PoE-P4 (ESP32-P4) with the Display Out add-on sustain 1080p30 MJPEG playback from a
USB stick to HDMI? If not, what is the bottleneck and the best stable mode?

## Primary variable
Source clip: 1920×1080 4:2:2 (zero-copy path), 1920×1080 4:2:2 with noise
(worst-case frame size), 1920×1080 4:2:0 (copy fallback, 1088-line decode),
1280×720 4:2:0 (fallback mode, needs a 720p60 build).

Firmware: `firmware/p4_loop/` (see its README). Stick in the Unit's USB-C
Host port.

## Fixed conditions
- Clips from `tools/make_clips.sh` (10 s, 30 fps, timestamp overlay in the
  pattern, MJPEG in AVI).
- HDMI mode 1920×1080@30 (LT8912B BSP limit); 1280×720@60 for the fallback.
- Frame buffers RGB888 (fixed by M5's LT8912B driver), 3 buffers, zero-copy present.
- Stick: FAT32/MBR, file in root, mounted read-only.

## Measurements (logged per frame over serial, raw kept)
1. USB read time per frame and sustained MB/s.
2. JPEG decode time per frame.
3. Frames presented per second, and dropped frames over 10 min.
4. Seam: whether the counter jumps 9.9x → 0.00 with no repeated/black frame
   (film the screen at 60/120 fps with a phone; keep the video).
5. Boot: power-on to first frame on screen (film it).
6. Supply current (see Power below).

## Stop / pass criteria
- Pass: ≥ 30 fps sustained for 10 min, no drop, seam clean, boot ≤ 3 s.
- Fail fast: decode alone < 30 fps at 1080p → go to 720p arm and record.

## Bench notes
- Check the serial port before flashing; the boot banner prints the chip
  revision (rev < 3.0 needs ESP-IDF 5.4.x, the PoE-P4X rev 3.x needs >= 6.1).

## Power (can it run from the monitor's USB port?)
Instrument: inline USB-C power meter with min/max/average (logging if
available) between the supply and the Unit's power USB-C. The stick draws
through the board, so it is included.
1. Bench supply first: record idle (waiting for stick), boot peak, and
   steady playback average/peak for each clip; the 4:2:2 noisy clip is the
   worst case.
2. Then power the Unit from the monitor's USB ports (each type the monitor
   has: USB-A 2.0 / 3.x / charging, USB-C): 30 min playback. Count resets:
   the boot banner prints the reset reason (`BROWNOUT` = supply sagged).
3. Port behaviour: is it powered without the monitor's upstream USB cable?
   Does it switch off in monitor standby, and does the player come back to
   looping when the monitor turns on?
- Pass: playback average and boot peak both <= 80 % of the port's rating,
  zero resets in 30 min.

## Results
`tests/results/01-p4-mjpeg-throughput/<YYYY-MM-DD>-<case>/` — one summary per
case. (Raw serial logs and the clips used are not published.)
