#include "FogSys.h"

namespace Fog {

const char* resetReasonName(uint8_t reason) {
    switch (static_cast<esp_reset_reason_t>(reason)) {
        case ESP_RST_POWERON:   return "poweron";
        case ESP_RST_EXT:       return "ext";
        case ESP_RST_SW:        return "sw";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:       return "wdt";
        case ESP_RST_DEEPSLEEP: return "deepsleep";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_USB:       return "usb";
        default:                return "other";
    }
}

BootRecord beginBoot(const char* ns) {
    const esp_reset_reason_t reason = esp_reset_reason();
    BootRecord rec;
    rec.reason = static_cast<uint8_t>(reason);
    BlobStore<LifeCounters> store(ns, "life", 1);
    LifeCounters& c = rec.life;
    store.load(c);
    ++c.boots;
    auto bump = [](uint16_t& v) { if (v < 0xFFFF) ++v; };
    switch (reason) {
        case ESP_RST_POWERON:   bump(c.rstPower); break;
        case ESP_RST_BROWNOUT:  bump(c.rstBrownout); break;
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:       bump(c.rstWdt); break;
        case ESP_RST_PANIC:     bump(c.rstPanic); break;
        case ESP_RST_SW:        bump(c.rstSw); break;
        case ESP_RST_DEEPSLEEP: bump(c.rstDeepSleep); break;
        default:                bump(c.rstOther); break;
    }
    rec.dirty = true;
    return rec;
}

void commitBoot(const char* ns, BootRecord& rec) {
    if (!rec.dirty) return;
    BlobStore<LifeCounters> store(ns, "life", 1);
    store.save(rec.life);
    rec.dirty = false;
}

void LoopMeter::mark() {
    const uint32_t now = micros();
    if (lastUs_) {
        const uint32_t d = now - lastUs_;
        if (d > maxUs_) maxUs_ = d;
    }
    lastUs_ = now;
    ++count_;
    const uint32_t ms = now / 1000;
    if (ms - windowStart_ >= 1000) {
        lps_ = count_;
        count_ = 0;
        windowStart_ = ms;
    }
}

}  // namespace Fog
