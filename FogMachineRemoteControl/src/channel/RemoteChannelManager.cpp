#include "channel/RemoteChannelManager.h"
#include <EEPROM.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <algorithm>
#include "Defaults.h"

#ifndef WIFI_SCAN_RUNNING
#define WIFI_SCAN_RUNNING (-1)
#endif

namespace {
    constexpr uint8_t kMinChannel = 1;
    constexpr uint8_t kMaxChannel = 13;
}

void RemoteChannelManager::begin(void (*factoryResetCallback)(), size_t /*eepromSize*/) {
    factoryResetCb_ = factoryResetCallback;
    loadFromStorage();
    if (!storageValid_ || !isChannelSupported(storedChannel_)) {
        runFactoryReset();
    }
    activeChannel_ = storedChannel_;
}

bool RemoteChannelManager::isChannelSupported(uint8_t channel) const {
    return channel >= kMinChannel && channel <= kMaxChannel;
}

bool RemoteChannelManager::storeChannel(uint8_t channel) {
    if (!isChannelSupported(channel)) {
        return false;
    }
    if (storedChannel_ == channel && storageValid_) {
        return false; // no change
    }
    storedChannel_ = channel;
    storageValid_ = true;
    writeStorage();
    activeChannel_ = channel;
    return true;
}

void RemoteChannelManager::applyStoredChannel() {
    applyChannel(storedChannel_);
}

void RemoteChannelManager::applyChannel(uint8_t channel) {
    if (!isChannelSupported(channel)) {
        return;
    }
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    activeChannel_ = channel;
}

bool RemoteChannelManager::requestSurvey() {
    if (surveyState_ == SurveyState::Running) {
        return false;
    }
    // Ensure we are in STA mode before scanning. ESP-NOW coexists with STA, but scanning
    // temporarily retunes across channels; the caller should reapply the stored channel after.
    WiFi.mode(WIFI_STA);
    int16_t res = WiFi.scanNetworks(true, true);
    if (res >= 0 || res == WIFI_SCAN_RUNNING) {
        surveyState_ = SurveyState::Running;
        candidates_.clear();
        return true;
    }
    surveyState_ = SurveyState::Failed;
    return false;
}

bool RemoteChannelManager::pollSurvey() {
    if (surveyState_ != SurveyState::Running) {
        return false;
    }
    int16_t status = WiFi.scanComplete();
    if (status == WIFI_SCAN_RUNNING) {
        return false; // still scanning
    }
    if (status < 0) {
        surveyState_ = SurveyState::Failed;
        WiFi.scanDelete();
        return true;
    }

    struct ChannelScore {
        uint16_t count = 0;
        uint32_t sumAbsRssi = 0;
    };
    ChannelScore scores[kMaxChannel + 1] = {};
    for (int i = 0; i < status; ++i) {
        int ch = WiFi.channel(i);
        int rssi = WiFi.RSSI(i);
        if (ch < kMinChannel || ch > kMaxChannel) {
            continue;
        }
        scores[ch].count++;
        scores[ch].sumAbsRssi += static_cast<uint32_t>(abs(rssi));
    }

    candidates_.clear();
    candidates_.reserve(kMaxChannel - kMinChannel + 1);
    for (uint8_t ch = kMinChannel; ch <= kMaxChannel; ++ch) {
        candidates_.push_back({ch, scores[ch].count, scores[ch].sumAbsRssi});
    }
    std::sort(candidates_.begin(), candidates_.end(), [](const Candidate& a, const Candidate& b) {
        if (a.apCount != b.apCount) return a.apCount < b.apCount;
        if (a.sumAbsRssi != b.sumAbsRssi) return a.sumAbsRssi < b.sumAbsRssi;
        return a.channel < b.channel;
    });

    surveyState_ = SurveyState::Complete;
    WiFi.scanDelete();
    applyStoredChannel();
    return true;
}

void RemoteChannelManager::clearSurvey() {
    candidates_.clear();
    surveyState_ = SurveyState::Idle;
}

void RemoteChannelManager::writeStorage() {
    EEPROM.put(ADDR_MAGIC, MAGIC);
    EEPROM.put(ADDR_VER, VERSION);
    EEPROM.put(ADDR_VALUE, storedChannel_);
    EEPROM.commit();
}

void RemoteChannelManager::loadFromStorage() {
    storageValid_ = false;
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
    storageValid_ = true;
}

void RemoteChannelManager::runFactoryReset() {
    if (factoryResetCb_) {
        factoryResetCb_();
    }
    storedChannel_ = Defaults::DEFAULT_CHANNEL;
    storageValid_ = true;
    activeChannel_ = storedChannel_;
    writeStorage();
}
