# p4_loop: ESP32-P4 HDMI video looper firmware

ESP-IDF firmware for the M5Stack Unit PoE-P4 with the AddOn Display Out
(LT8912B HDMI bridge). It reads MJPEG AVI files from a USB drive, decodes
each frame with the P4's hardware JPEG engine straight into a YUV422 frame
buffer, and loops them on HDMI at 1920x1080. It boots straight into the
player (ESP-IDF/FreeRTOS, no general-purpose OS) and never writes the
drive, so a power cut during playback should leave nothing to repair; this
is by design and has not yet been tested with repeated power pulls. A power
cut while a firmware update is being written is covered by the A/B slots
(see below).

Measured on one Unit PoE-P4 (ESP32-P4 rev v1.3, the PoE-P4 rather than the
PoE-P4X) with a USB-C SD card reader, see `tests/results/`:
- 1080p 4:2:2 MJPEG at 30 fps from a USB drive, 0 late frames over a
  138 s pass including the loop point.
- Reset to first frame about 1.06 s (measured with a USB reset).
- Cold power-on: before the bootloader fix (`dcdc_settle`, below) every
  power-up also went through one watchdog reset, and plug to picture was
  about 2.6-2.8 s (filmed once, including the monitor locking on). With the
  fix the watchdog reset is gone; plug to picture has not been timed again.
- Raw USB read rate 20-22 MB/s; frames are capped at 512 KB so the worst
  one-second burst stays below that.

## Hardware setup
- Display Out add-on on the Unit's 24P FPC (the cable ships with the add-on
  and is required: it carries I2C, reset and 3.3 V to the bridge); HDMI to
  the monitor.
- USB drive (MBR + FAT32) in the Unit's **USB-C port labelled Host**: a USB-C
  stick, or a USB-C card reader. Not the add-on's USB-A port: that port is on
  the P4's full-speed PHY, which the ESP-IDF host stack does not drive, and
  12 Mbit/s cannot feed 1080p MJPEG. The add-on's microSD slot also works but
  reads only ~12 MB/s (SDMMC through the GPIO matrix, 40 MHz).
- Source order: the USB drive first; the microSD only when no drive has
  mounted within 3 s after the display comes up (videos in the card's root;
  no config or updates from the card). A drive plugged in later takes over.
- Programming / console: the Unit's other USB-C, USB-Serial-JTAG.

## The drive
```
/media/*.avi      videos, played in name order, looped
/config.txt       optional settings (see below)
/update/*.bin     new firmware, installed when the drive is next mounted
/kit/             this project (source, tools, docs, firmware binary)
```
`tools/make_drive.py` lays it out (and can reformat a drive with `--erase`).
Copy videos onto a freshly formatted drive before anything else so each file
is contiguous; fragmented files still play but read slower. A drive with
`.avi` files in its root and no `/media` folder also plays.

Videos: `tools/p4_transcode.py input.mp4 outdir --quality 90 --max-kb 512`
writes 1920x1080 true 4:2:2 MJPEG AVI at the source frame rate (30 fps or
below), no audio. FAT32 limits one file to 4 GB. How long that lasts depends
on the content: the 2-minute test clip averaged 7.4 MB/s (about 9.7 minutes
per 4 GB); if every frame hit the 512 KB cap it would be about 4.5 minutes.

Playback loops the current file; `n` on the console moves to the next file.
With several files and no `file =` setting, the player starts with the first
in name order and moves on only on `n` or when a file cannot be played.

`/config.txt` ("key = value", `#` comments):
- `file = clip.avi`: loop only this file (exact name in /media). If it is
  not found, the player logs a warning and plays the other videos.
- `update = off`: ignore `/update` (default on). Only `on/yes/true/1` turn
  updates on; anything else turns them off.

## Firmware updates from the drive
Put a `p4_loop.bin` (from `idf.py build`, `build/p4_loop.bin`) into
`/update/` and plug the drive in (or power up with it in place). The player reads the image's descriptor; if it
differs from the running firmware it writes it into the other app slot
(amber screen for 2-3 s), checks its SHA-256, and restarts into it. The new
firmware confirms itself once it has shown a frame and run 3 more seconds,
or after 20 s if there is nothing to play. If videos were found but none
played within 20 s, it marks itself invalid and the bootloader returns to
the previous firmware; the same happens if it resets before confirming. An
image installed twice without confirming is skipped from then on. The file
can stay on the drive: an image equal to the running one is ignored.

Verified on the bench (`tests/results/01-p4-mjpeg-throughput/2026-11-23-ota/`):
install, a reset inside the confirm window rolling back, reinstall, confirm.
Later fixes (clearing the install count on confirm, the amber colour, rolling
back when videos were found but none played, refusing a new update while the
running image is still unconfirmed) were checked on the board in
`tests/results/01-p4-mjpeg-throughput/2026-11-23-review-fixes-bench/`. Not
yet triggered on the board: the dark red all-fail screen and the USB drive
install retry.

Drive updates replace the application only, never the bootloader. The
`dcdc_settle` fix below lives in the bootloader, so a board flashed before it
needs one USB flash (`idf.py -p <port> flash`) to get it.

There is no signature check: anyone who can write the drive can install
firmware. For an unattended show, set `update = off`.

The first flash of a build with OTA slots must be over USB (it changes the
partition table).

## Build and flash
Unit PoE-P4 (ESP32-P4 rev < 3.0), ESP-IDF v5.4.2:
```bash
. "$IDF_PATH/export.sh"
cd firmware/p4_loop
idf.py build
idf.py -p <port> flash monitor
```
Unit PoE-P4X (rev 3.x) needs ESP-IDF >= 6.1 (untested):
```bash
idf.py -B build.p4x -D SDKCONFIG=sdkconfig.p4x -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.p4x.defaults" build
```
Other builds:
- `-DP4LOOP_720P=1`: 1280x720@60 mode.
- `-DP4LOOP_BENCH_FULL=1`: embed the full decode-bench frame set (`b` key).
- `-DP4LOOP_FB_FMT=1`: RGB888 frame buffers (too slow at 1080p; comparison only).
- `-DP4LOOP_YUV_ORDER=n`: YUV422 byte order index (0 YUYV, 1 UYVY, 2 VYUY,
  3 YVYU; default 1 = UYVY, which is what the JPEG engine writes).

Bootloader components (`bootloader_components/`):
- `dcdc_settle` (always built): waits 20 ms instead of IDF's 1 ms before the
  internal LDO is turned off after the core supply moves to the DCDC. With
  1 ms, every cold power-up of the one Unit PoE-P4 tested (13 of 13) hung
  in the bootloader and was reset by the watchdog (up to about 1 s lost);
  with 20 ms, 6 of 6 captured cold plugs booted cleanly. Written against
  IDF 5.4.2. The build warns on other versions (the P4X build with
  IDF >= 6.1 will warn; the fix is unverified there): IDF may have changed
  the call it lengthens, and then the fix silently does nothing. To check a
  build: cold-plug the board about 5 times, waiting 10 s each time; every
  `BOOT_SUMMARY` line must show ROM code 0x01 (power-on), not 0x07 (main
  watchdog). Only a USB flash installs a new bootloader.
- `boot_crumbs` (diagnostic, off): records how far each boot got in two LP
  registers that survive resets; the app prints it as `BOOT_CRUMBS`. Enable:
  `rm -rf build/bootloader build/bootloader-prefix`, then
  `P4LOOP_BOOT_CRUMBS=1 idf.py reconfigure build`. Without it the app
  prints `BOOT_CRUMBS none`. Stage codes are listed at the top of
  `bootloader_components/boot_crumbs/crumbs.c`.

## Serial output
- Boot banner: chip revision, HDMI mode, reset reason (`BROWNOUT` = the
  supply sagged, e.g. a weak monitor USB port), with the raw ROM code. After
  a cold plug the host often opens the console after the banner, so 8 s
  after boot `BOOT_SUMMARY` repeats the reset reason (ROM code 0x01 = clean
  power-on; 0x07 = main watchdog).
- `DISPLAY_UP`, `STICK_MOUNTED`, `BOOT <step>`, `FIRST_FRAME` with
  `app_ms=` (ms since app start; ~0.3 s of ROM + bootloader comes before).
- Once per second:
  `STAT fps= read_ms=avg/max dec_ms=avg/max wait_ms=avg/max late= copy= MBps= io_MBps= q_min= loop= frame=`
  - `late` = frames more than one period behind schedule.
  - `q_min` = lowest prefetch depth (0 after the first second means storage
    is the bottleneck).
- `UPDATE ...` lines from the drive updater.
- Keys: `s` status, `f` free-run (decode ceiling), `v` per-frame CSV, `r`
  reset stats, `p` pause, `n` next file, `h` help. `b` runs the decode bench
  only when the display failed to start. `c` cycles the YUV422 byte order in
  the converter, but the change does not reach the screen while playing; use
  `-DP4LOOP_YUV_ORDER`.

## LED and screen
LED: red = booting / error, blue = waiting for a drive, green = playing.

Screen: black from power-up; colour bars on black after 10 s without any
drive or microSD video; dark red when every file failed to play; amber while
a firmware update is written.

## Sources the hardware facts came from
- LT8912B driver: Espressif `esp_lcd_lt8912b` (^0.2.1, from esp-bsp), which
  takes our DPI config; its register tables match M5GFX's `Panel_LT8912B`.
  I2C (port 1, SDA GPIO0, SCL GPIO1, 0x48/0x49/0x4A), DSI 2 lanes at 1000 Mbps,
  LDO channel 3 at 2500 mV and the 1080p30 / 720p60 timing (80 MHz pclk) are
  taken from M5GFX `M5UnitPoEP4DisplayOut.h` / `Panel_LT8912B.inl` at commit
  28d4794, which M5 tested on this board. Reset is GPIO4 (Unit schematic J6).
- YUV422 -> RGB is the only DPI colour conversion in IDF 5.4.2
  (`esp_lcd_dpi_panel_set_color_conversion`); RGB565 -> RGB888 does not run.
- LED pins R=GPIO17, G=GPIO15, B=GPIO16, common anode:
  `m5stack/M5Unit-PoE-P4-UserDemo` `main/app_led/led_ctrl.h`.
- HS-only USB host on P4: ESP-IDF v5.4.2 `components/usb/usb_host.c`.
- Zero-copy present (draw_bitmap with a frame-buffer pointer switches buffers):
  ESP-IDF v5.4.2 `components/esp_lcd/dsi/esp_lcd_panel_dpi.c`.
- JPEG output rounded to MCU size: ESP-IDF v5.4.2
  `components/esp_driver_jpeg/jpeg_parse_marker.c`.
