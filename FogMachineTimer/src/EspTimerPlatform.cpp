#include "EspTimerPlatform.h"

#include "FogLog.h"
#include "Pins.h"

using namespace Fog;

void EspTimerPlatform::begin(const TimerSettings& s, int8_t txPowerQdbm) {
    if (!radio.begin(s.channel, txPowerQdbm)) {
        FLOGE("radio", "ESP-NOW init failed");
    }
#if FOG_STATUS_LED_GPIO >= 0
    pinMode(FOG_STATUS_LED_GPIO, OUTPUT);
    digitalWrite(FOG_STATUS_LED_GPIO, LOW);
#endif
}

bool EspTimerPlatform::send(const uint8_t* mac, const uint8_t* data, size_t len) {
    const uint8_t next = static_cast<uint8_t>((qHead_ + 1) % kQueue);
    if (next == qTail_ || len > kMaxFrame) { ++txQueueDrops; return false; }
    Queued& e = q_[qHead_];
    e.unicast = mac != nullptr;
    if (mac) memcpy(e.mac, mac, 6);
    e.len = static_cast<uint8_t>(len);
    memcpy(e.data, data, len);
    qHead_ = next;
    return true;
}

void EspTimerPlatform::service(uint32_t nowMs, TimerNode& node) {
    radio.service(nowMs);

    bool ok;
    while (radio.pollTxResult(ok)) {
        if (inFlightUnicast_) node.noteTxResult(ok);
        inFlightUnicast_ = false;
    }
    if (qTail_ != qHead_ && !radio.txBusy()) {
        Queued& e = q_[qTail_];
        if (radio.send(e.unicast ? e.mac : nullptr, e.data, e.len)) {
            inFlightUnicast_ = e.unicast;
            if (tap) tap(true, e.unicast ? e.mac : nullptr, 0, e.data, e.len);
        }
        qTail_ = static_cast<uint8_t>((qTail_ + 1) % kQueue);    // a refused frame is dropped; the protocol retries at a higher level
    }

    // Hand received frames to the protocol, bounded so a flood cannot starve the output logic.
    FogRadio::Rx rx;
    for (int i = 0; i < 6 && radio.pollRx(rx); ++i) {
        if (tap) tap(false, rx.mac, rx.rssi, rx.data, rx.len);
        node.onFrame(rx.mac, rx.rssi, rx.data, rx.len, nowMs);
        ledUntil_ = nowMs + 40;
    }
}

void EspTimerPlatform::updateLed(uint32_t nowMs, TimerNode& node, bool ledEnabled) {
    node.identifying = identifying(nowMs);
#if FOG_STATUS_LED_GPIO >= 0
    bool on = false;
    if (node.identifying) on = ((nowMs / 100) % 2) == 0;                          // 5 Hz blink
    else if (ledEnabled) on = (static_cast<int32_t>(ledUntil_ - nowMs) > 0) || ((nowMs % 2000) < 30);   // heartbeat blip + activity
    digitalWrite(FOG_STATUS_LED_GPIO, on ? HIGH : LOW);
#else
    (void)ledEnabled;
#endif
}

void EspTimerPlatform::fillInfo(InfoPayload& info) {
    info.resetReason = resetReason;
    info.uptimeS = millis() / 1000u;
    info.freeHeap = ESP.getFreeHeap();
    info.minFreeHeap = ESP.getMinFreeHeap();
    info.loopMaxUs = loop.maxUs();
    info.rxDropped = radio.stats().rxDrop;
    info.txDropped = radio.stats().txDrop + txQueueDrops;
    info.rstPower = life.rstPower;
    info.rstBrownout = life.rstBrownout;
    info.rstWdt = life.rstWdt;
    info.rstPanic = life.rstPanic;
    info.rstSw = life.rstSw;
    info.rstOther = static_cast<uint16_t>(life.rstOther + life.rstDeepSleep);
}
