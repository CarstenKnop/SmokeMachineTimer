#include "config/TimerChannelSettings.h"
#include <EEPROM.h>
#include <esp_wifi.h>

namespace {
    constexpr uint8_t kMinChannel = 1;
    constexpr uint8_t kMaxChannel = 13;
}

void TimerChannelSettings::begin(void (*factoryResetCallback)()) {
    factoryResetCb_ = factoryResetCallback;
    load();
    if (!valid_ || !isChannelSupported(storedChannel_)) {
        runFactoryReset();
    }
}

bool TimerChannelSettings::isChannelSupported(uint8_t channel) const {
    return channel >= kMinChannel && channel <= kMaxChannel;
}

void TimerChannelSettings::load() {
    valid_ = false;
    storedChannel_ = Defaults::DEFAULT_CHANNEL;
    uint8_t magic = 0;
    EEPROM.get(ADDR_MAGIC, magic);
    if (magic != MAGIC) {
        return;
    }
    uint8_t version = 0;
    EEPROM.get(ADDR_VER, version);
    if (version != VERSION) {
        return;
    }
    EEPROM.get(ADDR_VALUE, storedChannel_);
    valid_ = true;
}

void TimerChannelSettings::write() {
    EEPROM.put(ADDR_MAGIC, MAGIC);
    EEPROM.put(ADDR_VER, VERSION);
    EEPROM.put(ADDR_VALUE, storedChannel_);
    EEPROM.commit();
}

bool TimerChannelSettings::setChannel(uint8_t channel) {
    if (!isChannelSupported(channel)) {
        return false;
    }
    if (storedChannel_ == channel && valid_) {
        return false;
    }
    storedChannel_ = channel;
    valid_ = true;
    write();
    apply();
    return true;
}

void TimerChannelSettings::apply() const {
    if (!isChannelSupported(storedChannel_)) {
        return;
    }
    esp_wifi_set_channel(storedChannel_, WIFI_SECOND_CHAN_NONE);
}

void TimerChannelSettings::runFactoryReset() {
    if (factoryResetCb_) {
        factoryResetCb_();
    }
    storedChannel_ = Defaults::DEFAULT_CHANNEL;
    valid_ = true;
    write();
}

void TimerChannelSettings::resetToDefault() {
    storedChannel_ = Defaults::DEFAULT_CHANNEL;
    valid_ = true;
    write();
    apply();
}
