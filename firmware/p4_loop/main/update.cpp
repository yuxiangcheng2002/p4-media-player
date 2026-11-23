#include "update.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <strings.h>
#include <sys/stat.h>
#include <vector>

#include "esp_app_format.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "player.h"
#include "usb_stick.h"

static const char *TAG = "update";
static constexpr int kMaxTries = 2;

static bool find_bin(const std::string &dir, std::string &out)
{
    DIR *d = opendir(dir.c_str());
    if (!d) return false;
    std::vector<std::string> bins;
    while (struct dirent *e = readdir(d)) {
        const char *n = e->d_name;
        size_t len = strlen(n);
        if (n[0] == '.' || len < 5 || e->d_type == DT_DIR) continue;  // also skips macOS "._x.bin"
        if (strcasecmp(n + len - 4, ".bin") == 0) bins.emplace_back(n);
    }
    closedir(d);
    if (bins.empty()) return false;
    std::sort(bins.begin(), bins.end());
    if (bins.size() > 1) ESP_LOGW(TAG, "%u .bin files in %s, using %s", (unsigned)bins.size(), dir.c_str(), bins[0].c_str());
    out = dir + "/" + bins[0];
    return true;
}

static void hex8(const uint8_t *sha, char out[9])
{
    for (int i = 0; i < 4; i++) sprintf(out + 2 * i, "%02x", sha[i]);
}

static nvs_handle_t open_nvs()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    nvs_handle_t h = 0;
    if (err != ESP_OK || nvs_open("update", NVS_READWRITE, &h) != ESP_OK) return 0;
    return h;
}

UpdateResult update_from_drive(const char *mount)
{
    std::string path;
    if (!find_bin(std::string(mount) + "/update", path)) return UpdateResult::None;

    // While this firmware is still on probation, the other slot holds the
    // only known-good image: never overwrite it.
    esp_ota_img_states_t run_state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &run_state) == ESP_OK &&
        run_state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGW(TAG, "running image not confirmed yet: %s not installed now", path.c_str());
        return UpdateResult::Skipped;
    }

    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return UpdateResult::Failed;
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        fclose(f);
        return UpdateResult::Failed;
    }
    const size_t size = (size_t)st.st_size;

    // Header, first segment header, then the app descriptor.
    esp_image_header_t hdr;
    esp_image_segment_header_t seg;
    esp_app_desc_t desc;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1 || fread(&seg, sizeof(seg), 1, f) != 1 ||
        fread(&desc, sizeof(desc), 1, f) != 1 || hdr.magic != ESP_IMAGE_HEADER_MAGIC ||
        desc.magic_word != ESP_APP_DESC_MAGIC_WORD) {
        ESP_LOGE(TAG, "%s is not an ESP app image", path.c_str());
        fclose(f);
        return UpdateResult::Failed;
    }
    desc.version[sizeof(desc.version) - 1] = 0;  // from the drive: bound it before logging
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_app_desc_t cur = {};
    if (esp_ota_get_partition_description(running, &cur) == ESP_OK &&
        memcmp(cur.app_elf_sha256, desc.app_elf_sha256, sizeof(desc.app_elf_sha256)) == 0) {
        ESP_LOGI(TAG, "%s is the running firmware (%s), nothing to do", path.c_str(), desc.version);
        fclose(f);
        return UpdateResult::Same;
    }
    if (hdr.chip_id != ESP_CHIP_ID_ESP32P4) {
        ESP_LOGE(TAG, "%s is not built for the ESP32-P4", path.c_str());
        fclose(f);
        return UpdateResult::Failed;
    }
    if (!hdr.hash_appended || size <= 32) {
        ESP_LOGE(TAG, "%s has no appended SHA-256; refusing", path.c_str());
        fclose(f);
        return UpdateResult::Failed;
    }
    const esp_partition_t *target = esp_ota_get_next_update_partition(nullptr);
    if (!target || size > target->size) {
        ESP_LOGE(TAG, "no OTA slot for a %u B image (partition table without OTA slots?)", (unsigned)size);
        fclose(f);
        return UpdateResult::Failed;
    }

    // Attempt count per image, so an image that never confirms (crash, hang,
    // or power cut in the confirm window) is not reinstalled forever.
    char key[12] = "t";
    hex8(desc.app_elf_sha256, key + 1);
    nvs_handle_t nvs = open_nvs();
    uint8_t tries = 0;
    if (nvs) nvs_get_u8(nvs, key, &tries);
    if (tries >= kMaxTries) {
        ESP_LOGW(TAG, "%s (%s) was installed %u times without confirming: skipped", path.c_str(), desc.version,
                 (unsigned)tries);
        if (nvs) nvs_close(nvs);
        fclose(f);
        return UpdateResult::Skipped;
    }

    ESP_LOGW(TAG, "installing %s: version %s -> %s, %u B into %s", path.c_str(), cur.version, desc.version,
             (unsigned)size, target->label);
    // Internal RAM: flash writes cannot source from PSRAM while the cache is
    // off. Allocated before esp_ota_begin, which erases the target slot.
    constexpr size_t kChunk = 16 * 1024;
    uint8_t *buf = static_cast<uint8_t *>(heap_caps_malloc(kChunk, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!buf) {
        ESP_LOGE(TAG, "no memory for the update buffer");
        if (nvs) nvs_close(nvs);
        fclose(f);
        return UpdateResult::Failed;
    }
    player_fill(40, 30, 0);  // amber screen while flashing
    esp_ota_handle_t h = 0;
    esp_err_t err = esp_ota_begin(target, size, &h);  // erases only what the image needs
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    uint8_t tail[32];
    size_t pos = 0;
    const size_t body = size - 32;  // the last 32 bytes are the digest of everything before
    rewind(f);
    while (err == ESP_OK && pos < size) {
        size_t n = fread(buf, 1, std::min(kChunk, size - pos), f);
        if (n == 0) { err = ESP_FAIL; break; }
        if (pos < body) mbedtls_sha256_update(&sha, buf, std::min(n, body - pos));
        for (size_t i = 0; i < n; i++) {
            if (pos + i >= body) tail[pos + i - body] = buf[i];
        }
        err = esp_ota_write(h, buf, n);
        pos += n;
    }
    fclose(f);
    heap_caps_free(buf);
    uint8_t digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    if (err == ESP_OK && memcmp(digest, tail, 32) != 0) {
        ESP_LOGE(TAG, "SHA-256 mismatch: file damaged, not installed");
        err = ESP_ERR_INVALID_CRC;
    }
    if (err != ESP_OK) {
        if (h) esp_ota_abort(h);
        ESP_LOGE(TAG, "update failed: %s", esp_err_to_name(err));
        if (nvs) nvs_close(nvs);
        return UpdateResult::Failed;
    }
    err = esp_ota_end(h);  // verifies the image again as written in flash
    if (err == ESP_OK) err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "update failed at end: %s", esp_err_to_name(err));
        if (nvs) nvs_close(nvs);
        return UpdateResult::Failed;
    }
    if (nvs) {
        nvs_set_u8(nvs, key, tries + 1);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    ESP_LOGW(TAG, "UPDATE installed %s, restarting", desc.version);
    return UpdateResult::Applied;
}

static volatile int s_media = -1;  // -1 unknown, 0 no videos, 1 videos found

void update_note_media(bool present) { s_media = present ? 1 : 0; }

static void confirm_task(void *)
{
    // Up to 20 s for a frame to reach the screen.
    for (int i = 0; i < 200 && !player_has_presented(); i++) vTaskDelay(pdMS_TO_TICKS(100));
    if (!player_has_presented() && s_media == 1) {
        // Videos were found but none played: treat this image as broken.
        printf("UPDATE videos found but nothing played in 20 s - rolling back\n");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_err_t rb = esp_ota_mark_app_invalid_rollback_and_reboot();
        // Only returns on failure (e.g. no other valid image): stay unconfirmed,
        // so the next reset still rolls back.
        printf("UPDATE rollback failed (%s); left unconfirmed\n", esp_err_to_name(rb));
        vTaskDelete(nullptr);
    }
    // A shown frame gets 3 s more; with nothing to play, 20 s was the wait.
    if (player_has_presented()) vTaskDelay(pdMS_TO_TICKS(3000));
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    // A confirmed image is good: forget its install count, so it is not
    // refused later as "never confirmed" (seen on the bench 2026-11-23 after
    // an install that rolled back once and then confirmed).
    esp_app_desc_t cur;
    if (err == ESP_OK && esp_ota_get_partition_description(esp_ota_get_running_partition(), &cur) == ESP_OK) {
        char key[12] = "t";
        hex8(cur.app_elf_sha256, key + 1);
        if (nvs_handle_t nvs = open_nvs()) {
            nvs_erase_key(nvs, key);
            nvs_commit(nvs);
            nvs_close(nvs);
        }
    }
    printf("UPDATE confirmed (%s)\n", esp_err_to_name(err));
    vTaskDelete(nullptr);
}

void update_start_confirm_watch()
{
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) != ESP_OK ||
        st != ESP_OTA_IMG_PENDING_VERIFY) {
        return;
    }
    printf("UPDATE pending verify: confirming after the first frame + 3 s (or 20 s with no videos)\n");
    xTaskCreatePinnedToCore(confirm_task, "ota_confirm", 4096, nullptr, 2, nullptr, 0);
}
