#pragma once
#include <Arduino.h>
#include <vector>

// RemoteChannelManager coordinates persisted channel preference, active channel application,
// and Wi-Fi spectrum surveys used to rank candidate channels for ESP-NOW.
class RemoteChannelManager {
public:
    struct Candidate {
        uint8_t channel;     // Wi-Fi channel number (1-13)
        uint16_t apCount;     // number of access points detected on this channel
        uint32_t sumAbsRssi;  // sum of absolute RSSI magnitudes (lower => quieter)
    };

    enum class SurveyState : uint8_t { Idle, Running, Complete, Failed };

    // Begin by validating persisted storage. The provided reset callback is invoked when
    // stored values fall outside supported ranges so the caller can wipe EEPROM first.
    void begin(void (*factoryResetCallback)(), size_t eepromSize = 512);

    uint8_t getStoredChannel() const { return storedChannel_; }
    uint8_t getActiveChannel() const { return activeChannel_; }
    bool isChannelSupported(uint8_t channel) const;

    // Persist a new preferred channel if it changes. Returns true on write.
    bool storeChannel(uint8_t channel);

    // Apply the stored preference or a specific channel to the radio immediately.
    void applyStoredChannel();
    void applyChannel(uint8_t channel);

    // Channel survey lifecycle -------------------------------------------------
    bool requestSurvey();              // start async scan when idle
    SurveyState getSurveyState() const { return surveyState_; }
    bool pollSurvey();                 // call from loop(); returns true once scan ends
    const std::vector<Candidate>& getCandidates() const { return candidates_; }
    void clearSurvey();

private:
    void writeStorage();
    void loadFromStorage();
    void runFactoryReset();

    // EEPROM layout (placed below DeviceManager block, above RemoteConfig block)
    static constexpr uint8_t MAGIC = 0xC7;
    static constexpr uint8_t VERSION = 1;
    static constexpr int ADDR_BASE   = 360;
    static constexpr int ADDR_MAGIC  = ADDR_BASE + 0;
    static constexpr int ADDR_VER    = ADDR_BASE + 1;
    static constexpr int ADDR_VALUE  = ADDR_BASE + 2;

    uint8_t storedChannel_ = 1;
    uint8_t activeChannel_ = 1;
    bool storageValid_ = false;
    void (*factoryResetCb_)() = nullptr;
    SurveyState surveyState_ = SurveyState::Idle;
    std::vector<Candidate> candidates_;
};
