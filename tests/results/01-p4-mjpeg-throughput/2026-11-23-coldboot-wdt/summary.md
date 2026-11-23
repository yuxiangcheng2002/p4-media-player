# Watchdog reset on every cold power-up (2026-11-23)

Board: Unit PoE-P4, ESP32-P4 rev v1.3, ROM esp32p4-eco2-20240710, IDF 5.4.2.
One board was tested. Raw serial logs, the video and the timing script named
below are not published.

Outcome: the first boot after power-on hung in the 2nd-stage bootloader's
clock and supply setup and was reset by the main watchdog. A 20 ms wait
after the core supply moves to the DCDC removed it
(`firmware/p4_loop/bootloader_components/dcdc_settle`).

Every cold plug of the board's USB-C cable (power-on) is followed by a boot
whose reset reason is WATCHDOG, raw ROM code 0x07 (RESET_REASON_CORE_MWDT:
the main watchdog reset the digital core). USB-Serial-JTAG resets (0x17) and
software resets (0x0C) never show it.

| Build | Cold plugs | Result |
|---|---|---|
| QIO flash (production) | 2 | 0x07 both times |
| DIO flash (test build) | 3 | 0x07 every time |

So the QIO change is not the cause. The first boot after power-on dies before
the application starts; its console output is lost because the host has not
opened the USB port yet. The RTC timer is cleared by this reset too ("8 ms
since power-on"), so the time lost cannot be read on the board.

A public report on another ESP32-P4 board with the same ROM build (rev v1.0,
IDF 5.5.0) shows the same sequence on its UART console: `rst:0x1 (POWERON)`,
the ROM loads the 2nd-stage bootloader, then `rst:0x7 (HP_SYS_HP_WDT_RESET)`
before the bootloader prints anything, and a normal second boot
(https://github.com/lvschouwen/lichenos-why2025-badge/issues/1). Espressif's
ESP32-P4 errata describe a similar power-on failure that the watchdog
resolves, listed there for chip revision v3.0 only (as read on 2026-11-23;
not linked here).

Side effect seen twice: after the cold boot, the USB-Serial-JTAG console
disappeared from the host about 0.2 s into the application and did not come
back (`timing-dio.log`, logged by a port-polling script); playback
continued. Debug console only.

Plug to picture, filmed with a phone (30 fps, DIO build, drive in the Host
port; before the fix): the connector sound is at 2.40-2.65 s,
the monitor shows its "no signal" box until frame 155 and the film's first
(white) frame at frame 156 = 5.20 s. So about 2.6-2.8 s from plugging in to a
picture. Of that, the firmware's own path is ~1.0 s (reset to first frame
over USB), and the rest is the extra first boot that the watchdog ends plus
the monitor locking onto a new HDMI signal after "no signal"; this video
cannot separate those two. Upper bound for the watchdog cost: ~1.7 s.

At this point it was not known where the first boot hangs (ROM or
bootloader); the sections below find it.

## Boot breadcrumbs (later on 2026-11-23)
LP_SYSTEM_REG_LP_STORE11/12 survive the watchdog reset and are cleared by
power-on; the bootloader writes a stage code there, the app prints it as
`BOOT_CRUMBS`.

| Log | Build | Cold plugs | Previous (first) boot reached |
|---|---|---|---|
| `crumbs-coldplug.log` | hooks before/after `bootloader_init` | 2 | stage 1: inside `bootloader_init` |
| `crumbs-steps-coldplug.log` | + ld `--wrap` on each `bootloader_init` callee | 2 | stage 0x12: inside `bootloader_clock_configure` |
| `crumbs-clk-coldplug.log` | + wraps on calls inside `bootloader_clock_configure` | 2 | 0x44: inside an `esp_rom_delay_us` (caller not distinguished) |
| `crumbs-delay-coldplug.log` | delay stage = 0x80 or the caller's code | 2 | plug 1: boot #1, none recorded (WDT reset, crumbs absent; unexplained); plug 2: 0xCE = in the 50 us delay of `rtc_clk_8m_enable` |

So the first boot after power-on runs ROM, enters the 2nd-stage bootloader,
gets through the switch of the core supply from the internal LDO to the DCDC
(enable DCDC, 1 ms, LDO off) and the CPU switch to 90 MHz from CPLL, then
stops dead inside a 50 us busy-wait: the CPU itself stalls, as from a supply
dip, until the main watchdog resets it.

## DCDC settle experiment
Build: the wrapper of `esp_rom_delay_us` turns the 1000 us delay made directly
by `rtc_clk_init` (the DCDC settle time before the LDO goes off) into 20 ms
(at the time a define in boot_crumbs; now `DCDC_SETTLE_US` in
`bootloader_components/dcdc_settle/dcdc_settle.c`).
The app also reprints the reset reason and crumbs 8 s after boot
(`BOOT_SUMMARY`), because after a clean cold boot the host opens the console
too late to see the banner.

| Log | Cold plugs | Result |
|---|---|---|
| `dcdc-settle-20ms-coldplug.log` | 2 | console attached after the banner (no WDT reset lines; no proof either way) |
| `dcdc-settle-20ms-after-flash.log` | 0 | the reset after flashing only; shows BOOT_SUMMARY works |
| `dcdc-settle-20ms-coldplug-2.log` | 5 | 4 with BOOT_SUMMARY: all reset reason 0x01 (POWERON), boot #1, no watchdog reset; 1 unplugged before the summary, no WDT lines |

Before: every cold plug, 13 of 13 over the day (QIO 2, DIO 3, crumb runs
4 x 2), had the watchdog reset. With the 20 ms settle: 0 of 4 confirmed. Cause, as far as
measured: at power-up the core supply is handed to the DCDC before it is
stable (a hypothesis consistent with where the CPU stops and with the fix;
the rail was not probed). Not measured: the DCDC rail itself (no scope on it), the shortest wait
that works, and plug-to-picture time with the fix (expect about 1 s less).

## Permanent fix: `bootloader_components/dcdc_settle`
Same 20 ms wait, now in its own bootloader component (crumbs off by default).
`dcdc-settle-component-coldplug.log`, 3 cold plugs: 2 with BOOT_SUMMARY, both
reset reason 0x01 (POWERON), no watchdog reset; the third ran normally but the
capture was stopped before its summary printed (no watchdog banner either).
Across both 20 ms builds: 6 of 6 confirmed cold plugs without the reset.
