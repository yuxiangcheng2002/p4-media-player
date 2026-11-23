# Boot to first frame, second round (2026-11-23)

Same board, port and drive as `../2026-11-23-boot-time/` (Unit PoE-P4 rev
v1.3; fresh FAT32 card in a USB-C reader on the
Host port; a 4K source clip transcoded to 1080p q90 with a 512 KB frame cap).
Firmware prints `BOOT <step> app_ms=` marks until the first frame.

## Step A (runs 1-3): smaller image, QIO flash, USB debounce 100 ms

- Production image embeds only the colour-bar frame: 2.0 MB -> 0.58 MB
  (full bench set: `-DP4LOOP_BENCH_FULL=1`).
- `CONFIG_ESPTOOLPY_FLASHMODE_QIO`, `CONFIG_USB_HOST_DEBOUNCE_DELAY_MS=100`
  (USB 2.0 minimum), bootloader log level WARN.

cpu_start 488-491 -> 282-285 ms after reset. FIRST_FRAME app 1361 -> 1248 ms.

The marks then showed two ~250 ms steps, both walking the file's FAT chain
one 512-byte sector at a time over USB:
`jpeg_engine 653 -> file_open 901` (VFS open() builds a fast-seek link map
because CONFIG_FATFS_USE_FASTSEEK=y) and `slots_allocated 955 -> raw_open
1214` (FatFs CREATE_LINKMAP in RawFile::open).

## Step B (runs 4-6): one FAT walk, read in 64 KB pieces

RawFile reads the FAT32 chain itself with 64 KB disk_read() calls and is
opened once in player_run; the AVI index, the first-frame header check and
the read-ahead all read through it (`ByteSource`). The VFS fd is opened only
when the raw path fails. Host check: frame tables identical to the previous
indexer on 7 test clips.

| mark (app ms) | run 4 | run 5 | run 6 |
|---|---|---|---|
| usb_connected | 267 | 267 | 267 |
| DISPLAY_UP | 281 | 281 | 281 |
| msc_installed | 556 | 556 | 557 |
| STICK_MOUNTED | 650 | 650 | 650 |
| file_open (raw map built) | 690 | 690 | 690 |
| indexed | 698 | 698 | 698 |
| slots_allocated | 739 | 739 | 739 |
| FIRST_FRAME | 775 | 775 | 775 |

Reset to first frame: ~282 + 775 = **~1.06 s** (round 1: ~1.85 s; start of
the day: ~4.3 s).

A 138 s pass across one loop, 30.02-30.07 fps every second, 0 late,
prefetch >= 4 after the first second (0 only while the queue first fills).

## What is left (app ms)

- 0 -> 267: USB attach + 100 ms debounce + reset + enumeration.
- 267 -> 556: MSC driver install: the reader answers the first command with
  UNIT ATTENTION (sense 06/28) and the driver retries until the card is
  ready. Device-bound; a USB stick may be faster than a card reader.
- 556 -> 650: FAT mount (msc_host_vfs_register).
- 699 -> 739: allocating six 512 KB input slots.
- before app start: ~170 ms PSRAM 200 MHz DQS tuning in the 2nd-stage
  startup (`MSPI DQS: tuning success` 311 -> 480 ms in the log of the earlier,
  slower boot; the step itself did not change).
