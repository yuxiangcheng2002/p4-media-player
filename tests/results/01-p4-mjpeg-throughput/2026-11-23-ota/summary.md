# Firmware update from the drive: install, rollback, confirm (2026-11-23)

Board: Unit PoE-P4 rev v1.3, partition table with ota_0/ota_1 (6 MB each),
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`. Running image A (the
firmware of that day, flashed over USB into ota_0). Drive: `/update/p4_loop.bin` = image C, the
same source built with `PROJECT_VER=ota-test-C` (different ELF SHA-256).
Script: `tools/ota_rollback_test.py`. Raw serial logs are kept privately and not published.

| t (s) | event |
|---|---|
| 38.4 | reset; A boots |
| 39.5 | A finds C, installs into ota_1 (651904 B) |
| 41.6 | SHA-256 + esp_ota_end OK, set boot partition, restart (install 2.0 s) |
| 42.0 | C boots, `pending verify`; ignores /update (same ELF) |
| 42.8 | C's first frame; the script resets the chip inside the confirm window |
| 43.3 | bootloader rolled back: A boots |
| 43.9 | A installs C again (second try), 2.8 s |
| 47.1 | C boots pending verify, first frame at app 783 ms |
| 51.0 | `UPDATE confirmed (ESP_OK)` |
| 51-150 | C plays, 30 fps, 0 late |

Then image A' was flashed over USB: it skipped C
("installed 2 times"), which exercised the skip rule but showed a bug: the
count was not cleared when C confirmed, so a good image would later be
refused. Fixed in update.cpp (confirm erases the image's count); the fix is
built and flashed but **not yet exercised on the bench** (needs a fresh test
image on the drive).

Also seen on the monitor: the amber "installing" screen showed green.
`player_fill` still packed YUYV after the switch to UYVY; fixed to pack in
the converter's order. Not yet seen on screen.

Later change (not part of this run): the confirm watch now rolls back
instead of confirming when a source with videos was found but no frame was
shown within 20 s.
