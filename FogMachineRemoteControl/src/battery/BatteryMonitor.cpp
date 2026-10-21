#include "battery/BatteryMonitor.h"

#include "Defaults.h"
#include "Pins.h"

void BatteryMonitor::begin() {
    pinMode(pin_, INPUT);
    if (CHARGER_CHG_PIN >= 0) pinMode(CHARGER_CHG_PIN, Defaults::CHARGER_CHG_ACTIVE_HIGH ? INPUT_PULLDOWN : INPUT_PULLUP);
    if (CHARGER_PWR_PIN >= 0) pinMode(CHARGER_PWR_PIN, Defaults::CHARGER_PWR_ACTIVE_HIGH ? INPUT_PULLDOWN : INPUT_PULLUP);
    mv_ = readMillivoltsNow();
    lastMs_ = millis();
}

uint16_t BatteryMonitor::readMillivoltsNow() const {
    uint32_t acc = 0;
    analogReadMilliVolts(pin_);                                          // discard: the first sample after idle is off with a high-impedance divider
    for (int i = 0; i < 16; ++i) { acc += analogReadMilliVolts(pin_); delayMicroseconds(300); }   // factory-calibrated, in mV at the pin
    return static_cast<uint16_t>((acc / 16) * 2);                     // divider halves the battery voltage
}

void BatteryMonitor::update(uint32_t nowMs) {
    if (nowMs - lastMs_ < 1000) return;
    lastMs_ = nowMs;
    const uint16_t fresh = readMillivoltsNow();
    mv_ = mv_ ? static_cast<uint16_t>((mv_ * 3 + fresh) / 4) : fresh;  // light smoothing
}

uint8_t BatteryMonitor::percent() const {
    const uint16_t* c = settings_.get().batCalMv;
    const int v = mv_;
    if (v <= c[0]) return 0;
    if (v >= c[2]) return 100;
    if (v <= c[1]) return static_cast<uint8_t>((v - c[0]) * 50 / (c[1] - c[0]));
    return static_cast<uint8_t>(50 + (v - c[1]) * 50 / (c[2] - c[1]));
}

bool BatteryMonitor::charging() const {
    if (CHARGER_CHG_PIN < 0) return false;
    const int lvl = digitalRead(CHARGER_CHG_PIN);
    return Defaults::CHARGER_CHG_ACTIVE_HIGH ? (lvl == HIGH) : (lvl == LOW);
}

bool BatteryMonitor::usbPower() const {
    if (CHARGER_PWR_PIN < 0) return false;
    const int lvl = digitalRead(CHARGER_PWR_PIN);
    return Defaults::CHARGER_PWR_ACTIVE_HIGH ? (lvl == HIGH) : (lvl == LOW);
}
