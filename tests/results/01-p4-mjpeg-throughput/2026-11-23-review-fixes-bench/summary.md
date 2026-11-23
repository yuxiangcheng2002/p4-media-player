# Bench check of the review fixes (2026-11-23)

Board: Unit PoE-P4 rev v1.3. Test images E and F: the same source built with
`PROJECT_VER=ota-test-E` / `ota-test-F`. Raw serial logs are kept privately.

| Fix | How | Result |
|---|---|---|
| USB unmount waits for readers | drive pulled during playback | "stick removed", reads fail, no "still held" warning, no crash |
| microSD fallback | new 32 GB card in the add-on slot, drive pulled | card mounted 3.3 s later (61 ms probe), test clip plays |
| USB first | drive plugged back while the card plays | drive takes over at once |
| No update while unconfirmed | E (then F) booting pending verify, /update holds itself | "running image not confirmed yet: ... not installed now" |
| Roll back when videos are found but none plays | /media holds only a clip the decoder rejects | 20 s later "nothing played - rolling back", previous image boots |
| Attempt count and skip | E installed twice without confirming | "attempt 2 of 2", then "installed 2 times without confirming: skipped" |
| Confirm clears the count | F installed and confirmed, then the older image flashed over USB | it installs F as "attempt 1 of 2" (not 2), F confirms again |
| Fill colours | amber install screen, dark blue idle | seen as yellow/amber and blue (were green before) |

Found on the way and fixed:
- After a software reset the previous picture (amber) stayed on screen:
  the driver's zeroed frame buffers sit in the cache while scan-out reads
  PSRAM. Display init now draws black; all-files-fail shows dark red.
- After a USB reset during heavy reads, the card reader missed its first
  SCSI commands and the drive never mounted (one attempt only). MSC install
  now retries up to 5 times.

- Flat dark blue flashes at power-up: the idle screen (dark blue fill) shown
  when no drive had mounted within 3 s and no microSD video was present; a
  card reader powering up with the board can take longer. The screen now
  stays black and the idle pattern (colour bars on black) appears only
  after 10 s without any source.

Cold power-up: every time the board is plugged in, the first boot is reset by
a watchdog. Raw ROM reset code 0x07 (RESET_REASON_CORE_MWDT): the main
watchdog reset the digital core during early boot (ROM / bootloader, where
the flash-boot watchdog is armed); the second boot runs normally. Traced and fixed later the same day: see
`../2026-11-23-coldboot-wdt/summary.md`.

Production firmware after these tests: reads /config.txt, plays
/media/*.avi, first frame at app 691 ms.

Not covered: the dark red all-fail screen and the MSC install retry (not
triggered since).
