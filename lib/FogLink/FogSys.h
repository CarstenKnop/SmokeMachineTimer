// FogSys.h - small platform helpers shared by both firmwares (reset reasons, lifetime counters, loop timing).
#pragma once

#include <Arduino.h>
#include <esp_system.h>
#include "FogStore.h"

namespace Fog {

// Maps esp_reset_reason() to the short names used by the USB protocol.
const char* resetReasonName(uint8_t reason);

// Boot statistics. The counters are incremented in RAM at once but written to NVS later (commitBoot): a supply that
// has just come back may still be chattering, and a flash write is the worst thing to do at that moment.
struct BootRecord {
    LifeCounters life;
    uint8_t reason = 0;       // esp_reset_reason_t
    bool dirty = false;       // not yet stored
};
BootRecord beginBoot(const char* ns);
void commitBoot(const char* ns, BootRecord& rec);   // writes if dirty; cheap to call repeatedly

// Tracks the slowest main-loop pass: a stall here is exactly what makes a radio queue overflow.
class LoopMeter {
public:
    void mark();                                   // call once per loop() pass
    uint32_t maxUs() const { return maxUs_; }
    uint32_t loopsPerSecond() const { return lps_; }

private:
    uint32_t lastUs_ = 0, maxUs_ = 0, lps_ = 0, count_ = 0, windowStart_ = 0;
};

}  // namespace Fog
