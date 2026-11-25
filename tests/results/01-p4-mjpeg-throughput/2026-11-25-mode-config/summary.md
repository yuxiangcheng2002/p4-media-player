# HDMI mode from config.txt (2026-11-25)

**Result.** `mode = 1080 | 1080p30 | 720` in the drive's `/config.txt`
selects the HDMI output (`main/video_mode.*`); a change is stored in NVS and
applied by one restart. Switching to 720 from the drive worked on the ASUS
VS229H. Videos of the other resolution are skipped when the drive is listed.
The default with no `mode` line is `1080p30`.

Raw logs and CSV files named here are kept privately and not published;
commit hashes refer to the private development history.

Setup: the bench Unit PoE-P4 + Display Out, PNY Elite-X Fit drive holding
both 1080p and 720p videos.

## Runs
1. Config without a mode line, NVS empty, firmware whose default was still
   `1080` (`01-no-mode-key-boot.log`): boots 2080x1096 at 35.09 Hz,
   `config: ... mode=(default)`, first frame app 937 ms, 30 fps, 0 late.
2. `mode = 720` written to the drive on a computer, drive back in the P4, P4
   cold-plugged (`02-mode720-coldplug.log`). The console attached too late for
   the first boot, but the second shows `rst:0xc (SW_CPU_RESET)` 1173 ms
   after power-on, `HDMI mode 720`, `config: ... mode=720`, and the 720p
   diagnostic clip playing at 30.05 fps, 0 late. The ASUS showed it. Later
   boots start in 720 directly.
3. Resolution filter (`match_mode` in main.cpp, `avi_peek_size`). Reading the
   AVI header through the VFS `fopen` cost 118 ms for 5 files
   (`03-filter-mode720-boot.log`), because the VFS open builds a fast-seek map
   over the whole FAT chain; reading through FatFs `f_open` directly costs
   28 ms (`04-filter-fatfs-boot.log`). First frame app 959 ms, against
   1418 ms when a 1080p file had to fail first.

## Not tested
Booting with no `mode` line on the `1080p30` default; switching back from 720
by removing the line; `mode = 1080p30` set explicitly; a malformed value; the
deferral while a freshly installed firmware is unconfirmed.
