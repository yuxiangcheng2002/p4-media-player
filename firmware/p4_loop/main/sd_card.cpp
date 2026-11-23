#include "sd_card.h"

#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <strings.h>

#include "driver/sdmmc_host.h"
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sd";

struct SdPins {
    const char *name;
    int clk, cmd, d0, d1, d2, d3;
};

// Order matters: the card-connector reading of the schematic is the most
// direct evidence, so it goes first.
static const SdPins kCandidates[] = {
    {"card-connector (J2)", 11, 10, 12, 13, 8, 9},
    {"connector net names", 12, 13, 8, 9, 10, 11},
    {"esp-hosted variant",  12, 13, 11, 10, 9, 8},
};

static sdmmc_card_t *s_card;
static int s_pdrv = -1;

int sd_card_drive() { return s_pdrv; }

bool sd_card_mount()
{
    esp_vfs_fat_sdmmc_mount_config_t mcfg = {};
    mcfg.format_if_mount_failed = false;  // never touch a card we cannot read
    mcfg.max_files = 2;
    mcfg.allocation_unit_size = 0;

    for (const SdPins &p : kCandidates) {
        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        host.slot = SDMMC_HOST_SLOT_1;
        host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

        sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
        slot.width = 4;
        slot.clk = (gpio_num_t)p.clk;
        slot.cmd = (gpio_num_t)p.cmd;
        slot.d0 = (gpio_num_t)p.d0;
        slot.d1 = (gpio_num_t)p.d1;
        slot.d2 = (gpio_num_t)p.d2;
        slot.d3 = (gpio_num_t)p.d3;
        // External 10k pull-ups exist; the internal ones are harmless backup.
        slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

        int64_t t0 = esp_timer_get_time();
        esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, &mcfg, &s_card);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "mounted with %s mapping (clk %d cmd %d d0-3 %d %d %d %d) in %lld ms",
                     p.name, p.clk, p.cmd, p.d0, p.d1, p.d2, p.d3,
                     (esp_timer_get_time() - t0) / 1000);
            sdmmc_card_print_info(stdout, s_card);
            s_pdrv = ff_diskio_get_pdrv_card(s_card);
            return true;
        }
        ESP_LOGW(TAG, "%s mapping failed: %s", p.name, esp_err_to_name(err));
    }
    ESP_LOGE(TAG, "no card mounted (empty slot, not FAT32, or wiring not as assumed)");
    return false;
}

std::vector<std::string> sd_card_list_avi()
{
    std::vector<std::string> out;
    DIR *d = opendir(SD_MOUNT_POINT);
    if (!d) return out;
    while (struct dirent *e = readdir(d)) {
        const char *n = e->d_name;
        size_t len = strlen(n);
        if (n[0] == '.' || len < 5 || e->d_type == DT_DIR) continue;  // skip macOS "._" files
        if (strcasecmp(n + len - 4, ".avi") == 0) out.emplace_back(n);
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}
