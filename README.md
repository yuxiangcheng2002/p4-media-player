# p4-media-player

A small HDMI video looper for exhibitions, built on an ESP32-P4: copy a video
onto a USB drive, plug it in, and it loops on the screen. The board boots
straight into the player (no general-purpose operating system) and only ever
reads the drive.

Hardware, as tested:
- M5Stack Unit PoE-P4 (ESP32-P4 rev v1.3; the PoE-P4X with rev 3.x silicon
  needs ESP-IDF >= 6.1 and is untested)
- M5Stack AddOn Display Out (LT8912B MIPI-DSI to HDMI bridge), connected with
  the 24-pin FPC cable that ships with it
- a USB-C drive (stick or USB-C card reader) in the Unit's USB-C Host port
- an HDMI monitor

Status (alpha), measured on one board with one USB-C card reader
(`tests/results/01-p4-mjpeg-throughput/2026-11-23-boot-time-2/`):
- 1080p30 playback, 0 late frames over a 138 s pass that includes the loop
  point.
- About 1.06 s from a USB reset to the first frame (cold power-on adds the
  drive's own start-up).
- The whole rig draws about 0.2 A at 5 V (peak about 0.3 A).
- Firmware updates from the drive, with rollback, verified on the bench.

Not yet tested: long unattended runs, repeated power pulls during playback,
power from a monitor's USB port, PoE.

## How it works
- Videos are MJPEG in AVI, 1920x1080, true 4:2:2. The P4 has a hardware JPEG
  decoder but no H.264 decoder, so videos are transcoded once on a computer.
- Each frame is decoded straight into one of three YUV422 frame buffers in
  PSRAM; the DSI bridge converts to RGB888 on the way out. RGB888 frame
  buffers cost too much PSRAM bandwidth at 1080p (about 24 fps on the bench).
- The file's sectors are read directly from the USB drive by a read-ahead
  task into a 6 MB ring (20-22 MB/s on the bench), instead of FatFs' reads
  of one cluster at a time (about 13.5 MB/s).
- The AVI's `idx1` index gives every frame's position, so the loop wraps from
  the last frame to the first without reopening the file.

## Quick start
Tools for the computer side: Python 3 with Pillow and pyserial
(`pip install pillow pyserial`), ffmpeg/ffprobe (macOS: `brew install ffmpeg`).
`tools/make_drive.py` is macOS only (diskutil, ditto); elsewhere, format the
drive as MBR + FAT32 and create the folders by hand.

From the repository root:
1. Build and flash the firmware once over USB (ESP-IDF v5.4.2):
   see [firmware/p4_loop/README.md](firmware/p4_loop/README.md).
2. Transcode a video:
   ```bash
   tools/p4_transcode.py input.mp4 out --quality 90 --max-kb 512
   ```
3. Lay out a freshly formatted FAT32 drive (videos first, so they are
   contiguous), with a copy of this repository in `/kit`:
   ```bash
   tools/make_drive.py /Volumes/P4VIDEO --media out/input_p4_1080p_422_q90_max512k.avi \
       --firmware firmware/p4_loop/build/p4_loop.bin --kit-git https://github.com/yuxiangcheng2002/p4-media-player
   ```
4. Plug the drive into the Unit's USB-C **Host** port and power up.

The drive layout (`/media`, `/config.txt`, `/update`, `/kit`), the settings,
and firmware updates from the drive are described in
[firmware/p4_loop/README.md](firmware/p4_loop/README.md). Updates are not
signed: anyone who can write the drive can install firmware, so set
`update = off` for unattended shows.

## Repository layout
```
firmware/p4_loop/   ESP-IDF firmware
tools/              p4_transcode.py, make_drive.py, serial_capture.py,
                    ota_rollback_test.py, clip and bench-frame generators
tests/results/      bench measurement summaries (raw logs not published)
experiments/        the first experiment's plan (historical)
docs/               the options survey that led to the ESP32-P4 (historical)
```

## Limits
- 1920x1080 at up to 30 fps (the LT8912B driver's 1080p mode), no audio.
- FAT32: at most 4 GB per file. At q90 with a 512 KB frame cap that was
  about 9.7 minutes for the test clip, and would be about 4.5 minutes if
  every frame hit the cap.
- Frames larger than the cap are re-encoded at a lower quality, so very
  detailed footage loses some quality in its busiest seconds.

## License
GPL-3.0-only, see [LICENSE](LICENSE). Third-party sources and licenses are
listed in [NOTICE](NOTICE).
