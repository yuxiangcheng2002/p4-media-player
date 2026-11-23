// Boot breadcrumbs in LP system store registers (kept through core and
// watchdog resets, cleared by power-on), used to find where the first boot
// after a cold power-up dies (2026-11-23: every power-on is followed by a
// main-watchdog reset, ROM code 0x07, before the application starts).
//
// LP_SYSTEM_REG_LP_STORE11/12 are not used by the ROM or ESP-IDF 5.4.2 on the
// ESP32-P4 (rom/rtc.h lists STORE0-10).
//   STORE11 = 0xB0C7 << 16 | boot count since power-on << 8 | stage
//   STORE12 = STORE11 as the previous boot left it
// Stages: 1 = 2nd-stage bootloader entered, 2 = bootloader_init done,
// 3 = application running (set by the app, main.cpp).
// Inside bootloader_init (2026-11-23, after the cold plugs showed stage 1):
// 0x10 + 2*i = entered step i, 0x11 + 2*i = step i returned, where the steps
// are the out-of-line callees wrapped with ld --wrap (CMakeLists.txt), in
// call order. kStepNames in main.cpp must match this order.
#include <stdbool.h>
#include <stdint.h>
#include "soc/lp_system_reg.h"
#include "soc/soc.h"

#define CRUMB_MAGIC 0xB0C7u

void bootloader_hooks_include(void) {}

void bootloader_before_init(void)
{
    uint32_t prev = REG_READ(LP_SYSTEM_REG_LP_STORE11_REG);
    uint32_t count = ((prev >> 16) == CRUMB_MAGIC) ? ((prev >> 8) & 0xFF) : 0;
    REG_WRITE(LP_SYSTEM_REG_LP_STORE12_REG, ((prev >> 16) == CRUMB_MAGIC) ? prev : 0);
    count = (count + 1) & 0xFF;
    REG_WRITE(LP_SYSTEM_REG_LP_STORE11_REG, (CRUMB_MAGIC << 16) | (count << 8) | 1);
}

void bootloader_after_init(void)
{
    uint32_t v = REG_READ(LP_SYSTEM_REG_LP_STORE11_REG);
    REG_WRITE(LP_SYSTEM_REG_LP_STORE11_REG, (v & ~0xFFu) | 2);
}

// ---- steps inside bootloader_init, wrapped with -Wl,--wrap=<name> ----
static inline void crumb_stage(uint32_t stage)
{
    uint32_t v = REG_READ(LP_SYSTEM_REG_LP_STORE11_REG);
    REG_WRITE(LP_SYSTEM_REG_LP_STORE11_REG, (v & ~0xFFu) | (stage & 0xFF));
}

typedef int esp_err_t;  // avoid esp_err.h; same type as in ESP-IDF

#define WRAP_VOID(i, name)                     \
    void __real_##name(void);                  \
    void __wrap_##name(void)                   \
    {                                          \
        crumb_stage(0x10 + 2 * (i));           \
        __real_##name();                       \
        crumb_stage(0x11 + 2 * (i));           \
    }
#define WRAP_ERR(i, name)                      \
    esp_err_t __real_##name(void);             \
    esp_err_t __wrap_##name(void)              \
    {                                          \
        crumb_stage(0x10 + 2 * (i));           \
        esp_err_t r = __real_##name();         \
        crumb_stage(0x11 + 2 * (i));           \
        return r;                              \
    }

WRAP_VOID(0, bootloader_init_mem)
// bootloader_clock_configure, opened up (second cold-plug run, 2026-11-23:
// both cold boots died inside it). While it runs, s_clk_window is set and the
// wrappers below record sub-steps as stage 0x40-0x57 (names in main.cpp).
static int s_clk_window;   // .bss: cleared by bootloader_clear_bss_section, which runs first
static int s_in_rtc_init;

void __real_bootloader_clock_configure(void);
void __wrap_bootloader_clock_configure(void)
{
    crumb_stage(0x12);
    s_clk_window = 1;
    __real_bootloader_clock_configure();
    s_clk_window = 0;
    crumb_stage(0x13);
}

static inline void clk_crumb(uint32_t stage)
{
    if (s_clk_window) crumb_stage(stage);
}

#define WRAP_CLK(code, ret, name, params, args)            \
    ret __real_##name params;                              \
    ret __wrap_##name params                               \
    {                                                      \
        clk_crumb(code);                                   \
        ret r = __real_##name args;                        \
        clk_crumb((code) + 1);                             \
        return r;                                          \
    }
#define WRAP_CLK_V(code, name, params, args)               \
    void __real_##name params;                             \
    void __wrap_##name params                              \
    {                                                      \
        clk_crumb(code);                                   \
        __real_##name args;                                \
        clk_crumb((code) + 1);                             \
    }

#include "soc/rtc.h"

// rtc_clk_init and esp_rom_delay_us are wrapped by dcdc_settle (the fix);
// it calls these hooks.
void boot_crumbs_rtc_clk_init(bool enter)
{
    clk_crumb(enter ? 0x42 : 0x43);
    s_in_rtc_init = enter;
}

void __real_esp_rom_output_tx_wait_idle(uint8_t n);
void __wrap_esp_rom_output_tx_wait_idle(uint8_t n)
{
    clk_crumb(s_in_rtc_init ? 0x54 : 0x40);
    __real_esp_rom_output_tx_wait_idle(n);
    clk_crumb(s_in_rtc_init ? 0x55 : 0x41);
}

// esp_rom_delay_us is called from several of the steps above, so a plain
// code would hide which one (third cold-plug run, 2026-11-23: stage 0x44).
// Inside a delay the stage is 0x80 | the caller's current code: 0xC2 = the
// 1 ms delay right after rtc_clk_init switches the core supply to the DCDC,
// 0xCC = a delay inside rtc_clk_cpu_freq_set_config (CPLL), and so on.
static uint32_t s_delay_caller;

void boot_crumbs_delay(bool enter)
{
    if (!s_clk_window) return;
    if (enter) {
        s_delay_caller = REG_READ(LP_SYSTEM_REG_LP_STORE11_REG) & 0x7F;
        crumb_stage(0x80 | s_delay_caller);
    } else {
        crumb_stage(s_delay_caller);
    }
}

WRAP_CLK_V(0x46, rtc_clk_xtal_freq_update, (soc_xtal_freq_t f), (f))
WRAP_CLK_V(0x48, rtc_clk_cpu_freq_get_config, (rtc_cpu_freq_config_t *c), (c))
WRAP_CLK(0x4a, bool, rtc_clk_cpu_freq_mhz_to_config, (uint32_t mhz, rtc_cpu_freq_config_t *c), (mhz, c))
WRAP_CLK_V(0x4c, rtc_clk_cpu_freq_set_config, (const rtc_cpu_freq_config_t *c), (c))
WRAP_CLK_V(0x4e, rtc_clk_8m_enable, (bool en), (en))
WRAP_CLK_V(0x50, rtc_clk_fast_src_set, (soc_rtc_fast_clk_src_t src), (src))
WRAP_CLK_V(0x52, rtc_clk_slow_src_set, (soc_rtc_slow_clk_src_t src), (src))
WRAP_VOID(2, bootloader_console_init)
WRAP_VOID(3, cache_hal_init)
WRAP_VOID(4, mmu_hal_init)
WRAP_VOID(5, bootloader_flash_update_id)
WRAP_ERR(6, bootloader_flash_xmc_startup)
WRAP_ERR(7, bootloader_read_bootloader_header)
WRAP_ERR(8, bootloader_check_bootloader_validity)
WRAP_ERR(9, bootloader_init_spi_flash)
WRAP_VOID(10, bootloader_config_wdt)
