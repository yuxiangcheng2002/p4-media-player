# USB-C drives for the Host port (2026-11-25)

Raw logs and CSV files named here are kept privately and not published;
commit hashes refer to the private development history.

Drives: SanDisk Extreme Fit USB-C 128 GB (one of two), PNY Elite-X Fit
Type-C 64 GB (one of a 3-pack). Both are short plug-style drives meant to stay
in the Unit's USB-C Host port. Both came formatted FAT32 on MBR (not exFAT,
as had been assumed), with only vendor files on them.

Each was laid out with `tools/make_drive.py --erase` (MBR + FAT32, volume
P4VIDEO, 32 KiB clusters): `/media` with the 1080p q90 film (927 MB; the
SanDisk also got the 209 MB noisy ceiling clip afterwards, the PNY in the same
run), `/config.txt` template, empty `/update`, `/kit` cloned from the public
repo plus `kit/firmware-bin/p4_loop.bin` (dcdc_settle build).

## On the Mac (USB 3 port, uncached file I/O; raw numbers in `mac-speed.csv`)
| Drive | Read, 4 MiB requests | Read, 64 KiB requests | Write |
|---|---|---|---|
| SanDisk Extreme Fit 128 GB | 384 MB/s | 206 MB/s | ~180 MB/s |
| PNY Elite-X Fit 64 GB | 208 MB/s | 166 MB/s | 18 MB/s |

Both read far faster than the P4's USB 2.0 Host port can take (the reader
measured 20-22 MB/s with a card reader), so reading should not separate them
on the board. Writing does on the computer: refilling a PNY with 1.1 GB took
about 90 s against about 15 s for the SanDisk.

## On the P4
Board: the bench Unit PoE-P4 + Display Out, firmware with the dcdc_settle
bootloader fix (flashed 2026-11-23). Soft reset over the
console, then `n` at 40 s to switch to the noisy clip.

### PNY Elite-X Fit 64 GB (`p4-pny-reset-noisy.log`)
- Enumerated and mounted: `usb: capacity 59152 MB`, mounted at 488 ms of app
  time; both files contiguous enough (film 14 fragments, noisy 4).
- Film (40 s): 30.02 fps, late=0, q_min 4-6 after the first second (the first
  STAT shows q_min=0 while the queue fills at start), io_MBps mean 5.4,
  peaks 15.0 in busy scenes.
- Noisy clip, read ceiling (47 s, q_min=0 throughout): io_MBps mean 19.75,
  range 19.48-20.01. Same as the card reader's 20-22 MB/s, so the PNY's slow
  write speed costs nothing on the board; the Host port is the limit.

## Not tested
PNY on the P4: supply current, cold plugs with `BOOT_SUMMARY`. SanDisk
Extreme Fit 128 GB on the P4: nothing yet.
