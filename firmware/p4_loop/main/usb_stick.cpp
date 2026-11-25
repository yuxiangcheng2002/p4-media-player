#include "usb_stick.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <dirent.h>
#include <strings.h>

#include "diskio_impl.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "player.h"
#include "usb/msc_host.h"
#include "usb/msc_host_vfs.h"
#include "usb/usb_host.h"

static const char *TAG = "usb";

static QueueHandle_t s_evt_q;
static EventGroupHandle_t s_flags;
static constexpr EventBits_t MOUNTED = BIT0;

static msc_host_device_handle_t s_dev;
static msc_host_vfs_handle_t s_vfs;
static volatile int s_pdrv = -1;
static std::atomic<int> s_holds{0};
// Set when a drive enumerated but the MSC driver could not take it; the
// watchdog below then power-cycles the port instead of waiting for a re-plug.
static std::atomic<bool> s_install_failed{false};

int usb_stick_drive() { return s_pdrv; }
void usb_stick_hold() { s_holds++; }
void usb_stick_release() { s_holds--; }

static void msc_cb(const msc_host_event_t *event, void *arg)
{
    // Runs in the MSC background task; hand off to our task, never block here.
    xQueueSend(s_evt_q, event, 0);
}

static void usb_lib_task(void *arg)
{
    for (;;) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

static void mount_task(void *arg)
{
    msc_host_event_t ev;
    for (;;) {
        if (xQueueReceive(s_evt_q, &ev, portMAX_DELAY) != pdTRUE) continue;
        if (ev.event == msc_host_event_t::MSC_DEVICE_CONNECTED) {
            boot_mark("usb_connected");
            if (s_dev) {  // one drive at a time; a second one is ignored
                ESP_LOGW(TAG, "second USB drive (addr %u) ignored", ev.device.address);
                continue;
            }
            ESP_LOGI(TAG, "stick connected, addr %u", ev.device.address);
            // A reader caught mid-transfer by a chip reset can miss its first
            // SCSI commands (seen 2026-11-23 after a USB reset: inquiry failed
            // and the drive never mounted). Retry before giving up.
            esp_err_t ierr = ESP_FAIL;
            for (int attempt = 1; attempt <= 5; attempt++) {
                ierr = msc_host_install_device(ev.device.address, &s_dev);
                if (ierr == ESP_OK) break;
                s_dev = nullptr;
                ESP_LOGW(TAG, "install attempt %d failed (%s), retrying", attempt, esp_err_to_name(ierr));
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            if (ierr != ESP_OK) {
                ESP_LOGE(TAG, "install device failed - the port will be power-cycled");
                s_install_failed = true;
                continue;
            }
            boot_mark("msc_installed");
            msc_host_device_info_t info;
            if (msc_host_get_device_info(s_dev, &info) == ESP_OK) {
                ESP_LOGI(TAG, "capacity %llu MB, sector %u B",
                         (unsigned long long)info.sector_count * info.sector_size / (1024 * 1024),
                         (unsigned)info.sector_size);
            }
            // Read-only use: the player never opens anything for writing, so a
            // power cut cannot leave the stick's FAT half-written.
            esp_vfs_fat_mount_config_t mcfg = {};
            mcfg.format_if_mount_failed = false;
            mcfg.max_files = 2;
            mcfg.allocation_unit_size = 0;
            // msc_host_vfs_register takes the first free FatFs drive; note
            // which one, so raw reads go to this drive and not the microSD.
            BYTE pdrv = FF_DRV_NOT_USED;
            ff_diskio_get_drive(&pdrv);
            esp_err_t err = msc_host_vfs_register(s_dev, USB_MOUNT_POINT, &mcfg, &s_vfs);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "mount failed: %s (FAT32 expected)", esp_err_to_name(err));
                msc_host_uninstall_device(s_dev);
                s_dev = nullptr;
                continue;
            }
            s_pdrv = pdrv;
            ESP_LOGI(TAG, "mounted at %s", USB_MOUNT_POINT);
            xEventGroupSetBits(s_flags, MOUNTED);
        } else if (ev.event == msc_host_event_t::MSC_DEVICE_DISCONNECTED) {
            if (!s_dev || ev.device.handle != s_dev) continue;  // not the drive we use
            ESP_LOGW(TAG, "stick removed");
            xEventGroupClearBits(s_flags, MOUNTED);
            // Readers stop when MOUNTED drops; unmount only once they let go.
            for (int i = 0; i < 300 && s_holds.load() > 0; i++) vTaskDelay(pdMS_TO_TICKS(10));
            if (s_holds.load() > 0) ESP_LOGW(TAG, "drive still held after 3 s, unmounting anyway");
            s_pdrv = -1;
            if (s_vfs) { msc_host_vfs_unregister(s_vfs); s_vfs = nullptr; }
            msc_host_uninstall_device(s_dev);
            s_dev = nullptr;
        }
    }
}

// Drive watchdog. Seen 2026-11-25 with the drive sitting in the port the whole
// time: enumeration failed (ENUM ... CHECK_SHORT_DEV_DESC FAILED) and IDF 5.4.2
// does not retry until the device disconnects, so the player stayed without
// its drive until someone re-plugged it. The same can follow a "stick removed"
// glitch. Root-port power off/on makes the host see the drive as newly
// attached and enumerate it again. It runs only while no drive is mounted and
// no USB device is enumerated (or one enumerated but was refused): first after
// 15 s, then backing off to once a minute, so a player that runs from microSD
// with the port empty only toggles an idle port. The board's VBUS stays on (no
// switch on the Unit is known), so a drive that has locked up itself is not
// power-cycled by this.
static void watch_task(void *arg)
{
    const int64_t first_us = 15 * 1000000LL, max_us = 60 * 1000000LL;
    int64_t lost_since = esp_timer_get_time(), backoff = first_us;
    int cycles = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        const int64_t now = esp_timer_get_time();
        if (usb_stick_mounted()) {
            lost_since = now;
            backoff = first_us;
            continue;
        }
        uint8_t addrs[4];
        int n = 0;
        usb_host_device_addr_list_fill(sizeof addrs, addrs, &n);
        if (n > 0 && !s_install_failed) {
            lost_since = now;  // enumerated, MSC still installing (a card reader can take seconds)
            continue;
        }
        if (now - lost_since < backoff) continue;
        cycles++;
        printf("USB_RECOVER cycle=%d no drive for %lld s (devices=%d install_failed=%d): root port off/on\n", cycles,
               (now - lost_since) / 1000000, n, (int)s_install_failed.load());
        usb_host_lib_set_root_port_power(false);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_err_t e = usb_host_lib_set_root_port_power(true);
        if (e != ESP_OK) ESP_LOGE(TAG, "root port power on: %s", esp_err_to_name(e));
        s_install_failed = false;
        lost_since = esp_timer_get_time();
        backoff = backoff * 2 > max_us ? max_us : backoff * 2;
    }
}

void usb_stick_start()
{
    s_evt_q = xQueueCreate(4, sizeof(msc_host_event_t));
    s_flags = xEventGroupCreate();

    usb_host_config_t host_cfg = {};
    host_cfg.skip_phy_setup = false;
    host_cfg.intr_flags = ESP_INTR_FLAG_LEVEL1;
    ESP_ERROR_CHECK(usb_host_install(&host_cfg));
    xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096, nullptr, 10, nullptr, 0);

    msc_host_driver_config_t msc_cfg = {};
    msc_cfg.create_backround_task = true;  // (sic) field name in usb_host_msc
    msc_cfg.task_priority = 9;
    msc_cfg.stack_size = 4096;
    msc_cfg.core_id = 0;
    msc_cfg.callback = msc_cb;
    ESP_ERROR_CHECK(msc_host_install(&msc_cfg));
    xTaskCreatePinnedToCore(mount_task, "usb_mount", 4096, nullptr, 8, nullptr, 0);
    xTaskCreatePinnedToCore(watch_task, "usb_watch", 3072, nullptr, 3, nullptr, 0);
}

bool usb_stick_wait_mounted(int timeout_ms)
{
    return xEventGroupWaitBits(s_flags, MOUNTED, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms)) & MOUNTED;
}

bool usb_stick_mounted() { return xEventGroupGetBits(s_flags) & MOUNTED; }

static std::vector<std::string> list_avi_in(const char *dir, const char *prefix)
{
    std::vector<std::string> out;
    DIR *d = opendir(dir);
    if (!d) return out;
    while (struct dirent *e = readdir(d)) {
        const char *n = e->d_name;
        size_t len = strlen(n);
        // Skip macOS AppleDouble files ("._clip.avi"), which are not video.
        if (n[0] == '.' || len < 5 || e->d_type == DT_DIR) continue;
        if (strcasecmp(n + len - 4, ".avi") == 0) out.emplace_back(std::string(prefix) + n);
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> usb_stick_list_avi()
{
    // Drive layout (tools/make_drive.py): /media videos, /config.txt,
    // /update firmware, /kit source and docs. Only /media is scanned, so the
    // kit's own files never reach the player.
    auto out = list_avi_in(USB_MOUNT_POINT "/media", "media/");
    if (out.empty()) out = list_avi_in(USB_MOUNT_POINT, "");
    return out;
}
