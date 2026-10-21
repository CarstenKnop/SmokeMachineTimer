// EspOtaTarget.h - the application slot that is NOT running, as an OtaTarget (docs/OTA_UPDATE.md).
//
// A thin layer over esp_partition / esp_ota_ops: all decisions (sessions, chunks, hash, timeouts) are in lib/FogLink/FogOta.*,
// which is tested on a PC. This file only maps six calls to the IDF.
#pragma once

#include <esp_partition.h>
#include "FogOta.h"

class EspOtaTarget : public Fog::OtaTarget {
public:
    // Finds the running and the other OTA slot. Call once at start-up.
    void begin();

    bool available() override { return next_ != nullptr; }
    uint32_t slotSize() override { return next_ ? next_->size : 0; }
    uint8_t slotIndex() override { return next_ ? static_cast<uint8_t>(next_->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN) : 0; }
    bool eraseSector(uint32_t sector) override;
    bool write(uint32_t offset, const uint8_t* data, size_t len) override;
    bool read(uint32_t offset, uint8_t* out, size_t len) override;
    bool activate() override;

    // After an update the boot loader runs the new image in "pending verify": if it resets before confirming, the boot loader goes back
    // to the previous image. Call this once the firmware has proven itself (a few seconds of normal running).
    static void confirmRunningImage();

private:
    const esp_partition_t* next_ = nullptr;      // nullptr: no second slot (or it would be the running one)
};
