# HDMI output on different monitors (2026-11-25)

**Result.** Two DSI timing bugs in the firmware made every output mode except
M5's 1080p shear or stay dark. With both fixed, 720p at 60.06 Hz plays cleanly
on both monitors tried; 1080p (35 Hz or 30 Hz) plays on one of them, the other
(an ASUS VS229H PC monitor) refuses any 1080p mode as "Out of range". The
output mode is now a drive setting (`mode =` in `/config.txt`, see
`../2026-11-25-mode-config/summary.md`).

Notes on this record: the builds named below (`build`, `build.std`,
`build.720p`, `build.720std`, option `P4LOOP_STD_RATES`) were temporary
compile-time variants, since replaced by the run-time `mode` setting:
`build.std` corresponds to `mode = 1080p30`, `build.720std` to `mode = 720`,
`build` to `mode = 1080`. Commit hashes refer to the private development
history. Raw serial logs (`*.log`) are kept privately and not published.
Observations of the screens are the operator's; the board cannot see whether
a monitor shows the picture.

Setup: the bench Unit PoE-P4 + Display Out, PNY Elite-X Fit drive, two
monitors: the bench monitor (model unknown, its menu could not be opened) and
an ASUS VS229H. Test material: the film, and a 10 s diagnostic clip
(`tools/make_diag_clip.py`: flat near-white patches written directly in
YCbCr, and a white bar moving 24 px per frame).

## The pixel clock constraint
The P4 DPI clock is an integer divider of 240, 160 or 40 MHz
(`mipi_dsi_hal_host_dpi_calculate_divider`), so CEA's 74.25 MHz cannot be
made; every mode runs at 80 MHz (240/3). M5's 1080p timing at 80 MHz is
2080x1096 = 38.46 kHz / 35.09 Hz (VIC 0), not a CEA mode. Many PC monitors
want 50 Hz or more, which 1080p cannot reach at 80 MHz.

## What the board can see (`main/hdmi_monitor.cpp`)
- Hot-plug detect (LT8912B 0xC1 bit 7): 0 with no cable, 1 on the bench
  monitor. On insertion it bounced 24 times in 12 s, then held. On the ASUS
  it reads 0 (0xC1 = 0x3c) although the monitor shows the picture, so HPD is
  not evidence of a picture.
- Bridge input readback (main page 0x9C-0x9F) matches the programmed totals
  in every mode (e.g. 0x0448 = 1096 lines), i.e. the LT8912B receives a
  steady stream.
- EDID: no ACK at I2C 0x50 with a monitor attached. The Display Out does not
  route the HDMI DDC lines to the P4, so a monitor's supported modes cannot be
  read from the board.

## Before the fixes
| Mode (80 MHz) | Bench monitor | ASUS VS229H |
|---|---|---|
| M5 1080p, 2080x1096, 35.09 Hz, VIC 0 | picture | "Out of range" |
| padded 1080p, 2370x1125, 30.00 Hz, VIC 34 | dark (film playing) | not tried |
| M5 720p, 1650x750, 64.65 Hz, VIC 4 | dark | not tried |
| padded 720p, 1778x750, 59.99 Hz, VIC 4 | dark | signal accepted, picture broken |

On the ASUS the padded 720p picture was sheared: the moving bar of the
diagnostic clip came out as a long diagonal, bands were offset, and bright
areas showed dotted runs. On the board side everything looked normal (30 fps,
no late frames).

## The two bugs
Found with two independent model reviews of the code (read-only, same
problem statement); each claim was checked against the IDF and driver sources
before it was acted on.
1. `dc.flags.disable_lp` was not set, so the DSI host could drop to LP-11 in
   long horizontal blanking; every timing preset in `esp_lcd_lt8912b.h` sets
   it. M5's 1080p has only 160 px (2 us) of blanking, too short for an LP
   round trip, which is why it was clean. With this fixed alone, the ASUS
   showed the top ~45% of the diagnostic clip clean and the rest shearing
   progressively, about 1.5 px per line.
2. IDF 5.4.2 converts each horizontal segment to DSI byte clocks as
   px x 1000 Mbps / 80 MHz / 8 = px x 1.5625 and truncates
   (`mipi_dsi_hal_host_dpi_set_horizontal_timing`). The 1778 px line lost
   2.125 byte clocks (about 1.36 px) per line, accumulating down the frame.
   M5's 1080p segments are all multiples of 16 px, so it had no error. Every
   mode now uses multiples of 16 (checked by a static_assert in
   `video_mode.cpp`): 1080p30 2368x1125, 720p 1776x750.

## After the fixes
| Mode (80 MHz) | Bench monitor | ASUS VS229H |
|---|---|---|
| `1080`: 2080x1096, 38.46 kHz, 35.09 Hz, VIC 0 | clean | "Out of range" |
| `1080p30`: 2368x1125, 33.78 kHz, 30.03 Hz, VIC 34 | clean | "Out of range" |
| `720`: 1776x750, 45.05 kHz, 60.06 Hz, VIC 4 | clean | clean |

The bench monitor's earlier dark screens were the DSI bugs, not a check on
pixel counts. Only two monitors have been tried.

## Other observations in the same runs
- USB drive drop-outs: with the drive in the Host port throughout, the
  console showed enumeration failures (`ENUM: CHECK_SHORT_DEV_DESC FAILED`)
  and the drive mounted again only after a manual re-plug. The firmware now
  power-cycles the USB root port when no drive is mounted (`USB_RECOVER`,
  first after 15 s, then backing off to once a minute). The cause of the
  failures is not known.
- With the recovery firmware: power pulled and restored gave a clean cold
  boot (reset reason 0x01); pulling the drive while playing fell back to the
  microSD clip within 3 s and the drive re-mounted 0.4-3.9 s after it went
  back in; HDMI unplugged and re-plugged during playback caused no USB errors.

## Not tested
More monitors and TVs; whether the YUV-to-RGB conversion of the DSI bridge
clamps or wraps out-of-range values (about 0.1% of the film's pixels convert
above 255; no artefact attributable to it has been seen since the fixes).
