#include "EspOtaTarget.h"

#include <esp_ota_ops.h>
#include "FogLog.h"

using namespace Fog;

void EspOtaTarget::begin() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);      // the OTA slot after the running one
    next_ = nullptr;
    if (!running || !next) return;
    // With a single OTA slot the "next" one wraps around to the running one: never write that.
    if (next->address == running->address) return;
    if (next->type != ESP_PARTITION_TYPE_APP || next->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MIN || next->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MAX) return;
    next_ = next;
    FLOGI("ota", "update slot %s at 0x%X (%u KB); running from %s", next->label, (unsigned)next->address, (unsigned)(next->size / 1024), running->label);
}

bool EspOtaTarget::eraseSector(uint32_t sector) {
    if (!next_) return false;
    return esp_partition_erase_range(next_, sector * Ota::kSectorBytes, Ota::kSectorBytes) == ESP_OK;
}

bool EspOtaTarget::write(uint32_t offset, const uint8_t* data, size_t len) {
    if (!next_) return false;
    return esp_partition_write(next_, offset, data, len) == ESP_OK;
}

bool EspOtaTarget::read(uint32_t offset, uint8_t* out, size_t len) {
    if (!next_) return false;
    return esp_partition_read(next_, offset, out, len) == ESP_OK;
}

bool EspOtaTarget::activate() {
    if (!next_) return false;
    // verifies the image (header, checksum, SHA-256) and, if it is good, makes it the boot slot
    const esp_err_t err = esp_ota_set_boot_partition(next_);
    if (err != ESP_OK) FLOGE("ota", "esp_ota_set_boot_partition failed: %d", (int)err);
    return err == ESP_OK;
}

void EspOtaTarget::confirmRunningImage() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) FLOGI("ota", "new firmware confirmed");
    }
}
