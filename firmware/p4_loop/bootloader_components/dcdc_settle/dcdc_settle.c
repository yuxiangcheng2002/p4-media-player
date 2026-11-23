// Longer settle time for the core supply switch at boot (ESP32-P4, IDF 5.4.2).
//
// rtc_clk_init (2nd-stage bootloader) moves the HP core supply from the
// internal LDO to the DCDC: it enables the DCDC, waits 1 ms, then turns the
// LDO off. On the one Unit PoE-P4 tested, every cold power-up (13 of 13) then
// stalled the CPU inside a 50 us busy-wait (rtc_clk_8m_enable) until the main
// watchdog reset the chip (ROM reset code 0x07), costing up to about 1 s.
// Located with boot breadcrumbs. Hypothesis: the DCDC is not ready within
// 1 ms after a cold power-up (the rail was not probed). With a 20 ms wait, all
// 6 cold plugs whose reset reason was captured (two builds) booted with
// POWERON and no watchdog reset
// (tests/results/01-p4-mjpeg-throughput/2026-11-23-coldboot-wdt/summary.md).
// 20 ms was chosen with margin; the shortest working wait was not measured.
//
// The 1 ms is the first esp_rom_delay_us call made directly by rtc_clk_init,
// and the only one there of 1000 us, so that call alone is lengthened.
// Optional boot_crumbs (bootloader_components/boot_crumbs) get notified
// through weak hooks, because only one component can wrap a given symbol.
#include <stdbool.h>
#include <stdint.h>
#include "soc/rtc.h"

#define DCDC_SETTLE_US 20000u
#define IDF_DCDC_SETTLE_US 1000u

// Defined by boot_crumbs when that component is enabled.
void boot_crumbs_rtc_clk_init(bool enter) __attribute__((weak));
void boot_crumbs_delay(bool enter) __attribute__((weak));

static bool s_in_rtc_clk_init;  // .bss, cleared before bootloader_clock_configure runs
static bool s_settled;

void __real_rtc_clk_init(rtc_clk_config_t cfg);
void __wrap_rtc_clk_init(rtc_clk_config_t cfg)
{
    if (boot_crumbs_rtc_clk_init) boot_crumbs_rtc_clk_init(true);
    s_in_rtc_clk_init = true;
    __real_rtc_clk_init(cfg);
    s_in_rtc_clk_init = false;
    if (boot_crumbs_rtc_clk_init) boot_crumbs_rtc_clk_init(false);
}

void __real_esp_rom_delay_us(uint32_t us);
void __wrap_esp_rom_delay_us(uint32_t us)
{
    if (s_in_rtc_clk_init && !s_settled && us == IDF_DCDC_SETTLE_US) {
        s_settled = true;
        us = DCDC_SETTLE_US;
    }
    if (boot_crumbs_delay) boot_crumbs_delay(true);
    __real_esp_rom_delay_us(us);
    if (boot_crumbs_delay) boot_crumbs_delay(false);
}
