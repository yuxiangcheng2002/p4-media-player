// p4_loop - ESP32-P4 HDMI video looper firmware (p4-media-player).
//
// Boot: banner -> USB host -> HDMI up -> wait for the drive -> /config.txt,
// /update/*.bin (firmware update, see update.h) -> loop /media/*.avi.
// Serial console (USB-Serial-JTAG, single letters): see print_help().
// LED (common anode, low = on; pins from M5Stack's PoE-P4 user demo):
//   red = booting / error, blue = waiting for a drive, green = playing.
#include <cinttypes>
#include <cstdio>
#include <string>
#include <strings.h>
#include <vector>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_private/esp_clk.h"
#include "esp_rom_sys.h"
#include "soc/lp_system_reg.h"
#include "soc/soc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "player.h"
#include "bench.h"
#include "config.h"
#include "update.h"
#include "sd_card.h"
#include "usb_stick.h"

static const char *TAG = "main";

static constexpr gpio_num_t PIN_LED_R = GPIO_NUM_17;
static constexpr gpio_num_t PIN_LED_G = GPIO_NUM_15;
static constexpr gpio_num_t PIN_LED_B = GPIO_NUM_16;

static void led(bool r, bool g, bool b)
{
    gpio_set_level(PIN_LED_R, !r);
    gpio_set_level(PIN_LED_G, !g);
    gpio_set_level(PIN_LED_B, !b);
}

static void led_init()
{
    gpio_config_t io = {};
    io.pin_bit_mask = (1ULL << PIN_LED_R) | (1ULL << PIN_LED_G) | (1ULL << PIN_LED_B);
    io.mode = GPIO_MODE_OUTPUT;
    gpio_config(&io);
    led(true, false, false);
}

static void print_help()
{
    printf("keys: s=status  f=toggle free-run (decode ceiling; fps capped by ~35 Hz refresh)  v=toggle per-frame CSV\n"
           "      r=reset stats  p=pause/resume  n=next file  h=help\n"
           "      (c=cycle YUV order: does not reach the screen while playing; b=decode bench: only without a display)\n"
           "CSV: F,frame,bytes,read_us,dec_us,fbwait_us,present_interval_us\n");
}

static void console_task(void *)
{
    // Blocking stdin on USB-Serial-JTAG needs the driver, not the default ROM path.
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    if (usb_serial_jtag_driver_install(&cfg) == ESP_OK) usb_serial_jtag_vfs_use_driver();
    // stdin is line-buffered by default: single keys without Enter never
    // reached fgetc (2026-11-23, 'n' ignored). Unbuffered makes them immediate.
    setvbuf(stdin, nullptr, _IONBF, 0);
    for (;;) {
        int ch = fgetc(stdin);
        if (ch == EOF) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        switch (ch) {
        case 's': g_ctl.dump_status = true; break;
        case 'f': g_ctl.free_run = !g_ctl.free_run; printf("free-run %s\n", g_ctl.free_run ? "ON" : "off"); break;
        case 'v': g_ctl.verbose = !g_ctl.verbose; printf("per-frame CSV %s\n", g_ctl.verbose ? "ON" : "off"); break;
        case 'r': g_ctl.reset_stats = true; printf("stats reset\n"); break;
        case 'p': g_ctl.paused = !g_ctl.paused; printf("%s\n", g_ctl.paused ? "paused" : "resumed"); break;
        case 'n': g_ctl.next_file = true; break;
        case 'c': player_cycle_color_order(); break;
        case 'b': g_ctl.bench = true; printf("bench queued\n"); break;
        case 'h': case '?': print_help(); break;
        default: break;
        }
    }
}

// Brown-out resets matter here: the player may be fed from a monitor's USB port.
static const char *reset_reason_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SW: return "SW";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: case ESP_RST_TASK_WDT: case ESP_RST_WDT: return "WATCHDOG";
    case ESP_RST_EXT: return "EXTERNAL";
    case ESP_RST_USB: return "USB";
    default: return "OTHER";
    }
}

// Bring-up aid: list every address that ACKs on the LT8912B's bus (port 1,
// SDA GPIO0 / SCL GPIO1 per M5GFX + M5Unified). The bridge should answer at
// 0x48/0x49/0x4A; an empty bus means the add-on is absent, unseated or unpowered.
static void i2c_scan_display_bus()
{
    i2c_master_bus_config_t bc = {};
    bc.i2c_port = I2C_NUM_1;
    bc.sda_io_num = GPIO_NUM_0;
    bc.scl_io_num = GPIO_NUM_1;
    bc.clk_source = I2C_CLK_SRC_DEFAULT;
    bc.glitch_ignore_cnt = 7;
    bc.flags.enable_internal_pullup = true;
    i2c_master_bus_handle_t bus = nullptr;
    if (i2c_new_master_bus(&bc, &bus) != ESP_OK) {
        printf("I2C_SCAN could not open port 1 (still held by the display driver?)\n");
        return;
    }
    printf("I2C_SCAN port1 sda=0 scl=1:");
    int found = 0;
    for (uint16_t a = 0x08; a < 0x78; a++) {
        if (i2c_master_probe(bus, a, 20) == ESP_OK) { printf(" 0x%02X", a); found++; }
    }
    printf(found ? "\n" : " none\n");
    i2c_del_master_bus(bus);
}

// Boot breadcrumbs written by bootloader_components/boot_crumbs (LP store
// registers 11/12): which boot this is since power-on, and where the previous
// boot got to. Stage 1 = bootloader entered, 2 = bootloader init done,
// 3 = application running.
// After a cold plug the host often opens the USB console after the banner has
// gone by (2026-11-23), so the reset reason and crumbs are printed again later
// as BOOT_SUMMARY.
static char s_crumbs_line[200] = "BOOT_CRUMBS none";

static void boot_summary_task(void *)
{
    vTaskDelay(pdMS_TO_TICKS(8000));
    printf("BOOT_SUMMARY reset reason rom code 0x%02x core0, 0x%02x core1 | %s\n",
           (unsigned)esp_rom_get_reset_reason(0), (unsigned)esp_rom_get_reset_reason(1), s_crumbs_line);
    vTaskDelete(nullptr);
}

static void print_boot_crumbs()
{
    const uint32_t cur = REG_READ(LP_SYSTEM_REG_LP_STORE11_REG);
    const uint32_t prev = REG_READ(LP_SYSTEM_REG_LP_STORE12_REG);
    if ((cur >> 16) != 0xB0C7) {
        printf("BOOT_CRUMBS none (bootloader without crumbs)\n");
        return;
    }
    // Step order must match the WRAP_* list in bootloader_components/boot_crumbs/crumbs.c.
    static const char *const kStepNames[] = {
        "bootloader_init_mem", "bootloader_clock_configure", "bootloader_console_init", "cache_hal_init",
        "mmu_hal_init", "bootloader_flash_update_id", "bootloader_flash_xmc_startup",
        "bootloader_read_bootloader_header", "bootloader_check_bootloader_validity",
        "bootloader_init_spi_flash", "bootloader_config_wdt"};
    const unsigned st = prev & 0xFF;
    char where[128] = "";
    // Sub-steps of bootloader_clock_configure (0x40-0x55, crumbs.c).
    static const char *const kClkNames[] = {
        "esp_rom_output_tx_wait_idle (first)", "rtc_clk_init", "esp_rom_delay_us", "rtc_clk_xtal_freq_update",
        "rtc_clk_cpu_freq_get_config", "rtc_clk_cpu_freq_mhz_to_config", "rtc_clk_cpu_freq_set_config",
        "rtc_clk_8m_enable", "rtc_clk_fast_src_set", "rtc_clk_slow_src_set",
        "esp_rom_output_tx_wait_idle (in rtc_clk_init)"};
    if (st >= 0xC0 && (st - 0xC0) / 2 < sizeof(kClkNames) / sizeof(kClkNames[0])) {
        snprintf(where, sizeof(where), " (bootloader_clock_configure: in esp_rom_delay_us, %s %s)",
                 (st & 1) ? "after returning from" : "called from", kClkNames[(st - 0xC0) / 2]);
    } else if (st >= 0x40 && (st - 0x40) / 2 < sizeof(kClkNames) / sizeof(kClkNames[0])) {
        snprintf(where, sizeof(where), " (bootloader_clock_configure: %s %s)", (st & 1) ? "returned from" : "inside",
                 kClkNames[(st - 0x40) / 2]);
    } else if (st >= 0x10 && (st - 0x10) / 2 < sizeof(kStepNames) / sizeof(kStepNames[0])) {
        snprintf(where, sizeof(where), " (%s %s)", (st & 1) ? "returned from" : "inside",
                 kStepNames[(st - 0x10) / 2]);
    }
    snprintf(s_crumbs_line, sizeof(s_crumbs_line),
             "BOOT_CRUMBS boot #%u since power-on; previous boot reached stage 0x%02x%s%s",
             (unsigned)((cur >> 8) & 0xFF), st, where, (prev >> 16) == 0xB0C7 ? "" : " (none recorded)");
    printf("%s\n", s_crumbs_line);
    REG_WRITE(LP_SYSTEM_REG_LP_STORE11_REG, (cur & ~0xFFu) | 3);
}

static void banner()
{
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    const esp_app_desc_t *app = esp_app_get_description();
    char sha[17] = {};
    esp_app_get_elf_sha256(sha, sizeof(sha));
    printf("\n=== p4_loop (p4-media-player) ===\n"
           "app %s  built %s %s  elf %s\n"
           "IDF %s  chip rev v%d.%d  cores %d\n"
           "PSRAM %u KB free  internal %u KB free\n"
           "HDMI mode %dx%d@%d\n"
           "reset reason %s (rom code 0x%02x core0, 0x%02x core1), %llu ms since power-on\n",
           app->version, app->date, app->time, sha, esp_get_idf_version(),
           ci.revision / 100, ci.revision % 100, ci.cores,
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
           P4LOOP_WIDTH, P4LOOP_HEIGHT, P4LOOP_REFRESH, reset_reason_name(esp_reset_reason()),
           // Raw cause (soc/reset_reasons.h): "WATCHDOG" covers several
           // watchdogs: 0x07/0x0B MWDT, 0x09/0x0D/0x10 RTC WDT, 0x12 super WDT,
           // 0x13 power glitch, 0x0F brown-out.
           (unsigned)esp_rom_get_reset_reason(0), (unsigned)esp_rom_get_reset_reason(1),
           // RTC time keeps counting through core and watchdog resets and starts
           // at power-on: on a cold boot it shows how long the boot took in
           // total, including any reset before this one.
           (unsigned long long)(esp_clk_rtc_time() / 1000));
    print_help();
}

extern "C" void app_main(void)
{
    // Boot time is read from the bootloader log ("I (n) cpu_start: Pro cpu start
    // user code", ms since reset). The RTC timer is not a substitute: it keeps
    // counting across USB/software resets (2026-11-22 it read 5003 s).
    led_init();
    banner();
    print_boot_crumbs();
    xTaskCreatePinnedToCore(boot_summary_task, "boot_summary", 3072, nullptr, 1, nullptr, 0);
    xTaskCreatePinnedToCore(console_task, "console", 4096, nullptr, 3, nullptr, 0);
    // Start the USB host first: enumerating a drive takes ~1 s and now runs
    // while the HDMI bridge and the SD slot initialise (boot time, 2026-11-23).
    usb_stick_start();

    if (!player_display_init()) {
        ESP_LOGE(TAG, "display init failed - check the Display Out add-on and FPC");
        i2c_scan_display_bus();
        // The microSD lines reach the Unit through the same side header as the
        // add-on's 5 V, so a card that mounts proves that header is mated.
        printf("SD_PROBE %s\n", sd_card_mount() ? "mounted" : "failed");
        // No picture possible, but decode speed and scan-out PSRAM load still are.
        bench_run(false);
        for (;;) {
            led(true, false, false); vTaskDelay(pdMS_TO_TICKS(200));
            led(false, false, false); vTaskDelay(pdMS_TO_TICKS(200));
            if (g_ctl.bench.exchange(false)) bench_run(false);
        }
    }
    printf("DISPLAY_UP app_ms=%lld\n", esp_timer_get_time() / 1000);
    update_start_confirm_watch();  // no-op unless this is a freshly installed image

    // Source order: the USB drive on the Unit's USB-C Host port first (faster,
    // and it carries /config.txt and /update); the add-on's microSD only when
    // no drive has mounted within 3 s. A drive that appears later takes over.
    static bool sd_ok = false, sd_tried = false;
    bool idle_shown = false;
    int64_t no_source_since = 0;  // when "no drive, no card" began (0 = a source is present)
    for (;;) {
        if (!usb_stick_wait_mounted(3000)) {
            if (!sd_tried) {
                sd_tried = true;
                sd_ok = sd_card_mount();
            }
            auto sd_files = sd_ok ? sd_card_list_avi() : std::vector<std::string>{};
            if (!sd_files.empty()) {
                printf("SD_MOUNTED app_ms=%lld files=%u\n", esp_timer_get_time() / 1000, (unsigned)sd_files.size());
                update_note_media(true);
                size_t cur = 0, failed = 0;
                while (!usb_stick_mounted() && failed < sd_files.size()) {
                    led(false, true, false);
                    std::string path = std::string(SD_MOUNT_POINT "/") + sd_files[cur];
                    if (player_run(path.c_str())) {
                        failed = 0;
                    } else {
                        failed++;  // every file failing in a row: card pulled or unreadable
                        led(true, false, false);
                        vTaskDelay(pdMS_TO_TICKS(1000));
                    }
                    cur = (cur + 1) % sd_files.size();
                }
                idle_shown = false;
                no_source_since = 0;
                continue;
            }
            if (!no_source_since) {
                no_source_since = esp_timer_get_time();
                led(false, false, true);  // the LED says "waiting" at once
                update_note_media(false);
                ESP_LOGW(TAG, "no drive - plug a FAT32 drive into the Unit's USB-C *Host* port");
            }
            // The screen stays black for the first 10 s: a card reader can take
            // longer than 3 s to appear after power-up, and an idle screen
            // flashing up meanwhile is distracting.
            if (!idle_shown && esp_timer_get_time() - no_source_since >= 10 * 1000000LL) {
                // Idle screen: the testsrc colour bars on black.
                player_fill(0, 0, 0);
                bench_show_pattern();
                idle_shown = true;
            }
            continue;
        }
        idle_shown = false;
        no_source_since = 0;
        printf("STICK_MOUNTED app_ms=%lld\n", esp_timer_get_time() / 1000);

        // Drive layout: /media, /config.txt, /update, /kit (tools/make_drive.py).
        usb_stick_hold();  // released below, before playback takes its own hold
        PlayerConfig cfg;
        config_load(USB_MOUNT_POINT "/config.txt", cfg);
        if (cfg.update && update_from_drive(USB_MOUNT_POINT) == UpdateResult::Applied) {
            vTaskDelay(pdMS_TO_TICKS(100));  // let the log drain
            esp_restart();
        }
        boot_mark("config_update_checked");
        auto files = usb_stick_list_avi();
        if (!cfg.file.empty()) {
            // file = name in /media, or a path from the drive root
            std::vector<std::string> pick;
            for (const auto &f : files) {
                std::string want = cfg.file;
            while (!want.empty() && want[0] == '/') want.erase(0, 1);
            if (strcasecmp(f.c_str(), want.c_str()) == 0 || strcasecmp(f.c_str(), ("media/" + want).c_str()) == 0) {
                pick.push_back(f);
            }
            }
            if (pick.empty()) ESP_LOGW(TAG, "config file=%s not found in /media - playing all", cfg.file.c_str());
            else files = pick;
        }
        boot_mark("listed");
        usb_stick_release();
        update_note_media(!files.empty());
        if (files.empty()) {
            ESP_LOGE(TAG, "no .avi files in /media or the drive's root");
            led(true, false, false);
            player_fill(60, 0, 0);
            while (usb_stick_mounted()) vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        for (size_t i = 0; i < files.size(); i++) printf("  [%u] %s\n", (unsigned)i, files[i].c_str());

        size_t cur = 0, failed = 0;
        while (usb_stick_mounted()) {
            led(false, true, false);
            std::string path = std::string(USB_MOUNT_POINT "/") + files[cur];
            if (player_run(path.c_str())) {
                failed = 0;
            } else {
                led(true, false, false);
                // Every file failed in a row: say so on screen (dark red)
                // instead of leaving the last picture up.
                if (++failed == files.size()) {
                    ESP_LOGE(TAG, "none of the %u video(s) can be played - see the messages above",
                             (unsigned)files.size());
                    player_fill(60, 0, 0);
                }
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            cur = (cur + 1) % files.size();  // 'n' or an unplayable file moves on
        }
    }
}
