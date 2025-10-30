#pragma once
#include <Arduino.h>
#include "Defaults.h"

class TimerChannelSettings {
public:
    void begin(void (*factoryResetCallback)());
    uint8_t getChannel() const { return activeChannel_; }
    uint8_t getStoredChannel() const { return storedChannel_; }
    bool storeChannel(uint8_t channel);
    bool setChannel(uint8_t channel);
    void apply();
    void applyTransient(uint8_t channel);
    bool isChannelSupported(uint8_t channel) const;
    void resetToDefault();
private:
    void load();
    void write();
    void runFactoryReset();
    static constexpr uint8_t MAGIC = 0xC8;
    static constexpr uint8_t VERSION = 1;
    static constexpr int ADDR_BASE = 112;
    static constexpr int ADDR_MAGIC = ADDR_BASE + 0;
    static constexpr int ADDR_VER   = ADDR_BASE + 1;
    static constexpr int ADDR_VALUE = ADDR_BASE + 2;
    uint8_t storedChannel_ = Defaults::DEFAULT_CHANNEL;
    uint8_t activeChannel_ = Defaults::DEFAULT_CHANNEL;
    bool valid_ = false;
    void (*factoryResetCb_)() = nullptr;
};
