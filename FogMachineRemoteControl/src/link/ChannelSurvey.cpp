#include "link/ChannelSurvey.h"

#include <WiFi.h>
#include <esp_wifi.h>
#include <algorithm>

using namespace Fog;

namespace {
constexpr uint32_t kScanMsPerChannel = 110;
constexpr uint32_t kDwellMs = 90;
constexpr uint32_t kScanTimeoutMs = 6000;

volatile uint16_t g_frames[14];
volatile int32_t g_rssiSum[14];
volatile uint8_t g_dwellCh = 0;

// Wi-Fi task context: count only, nothing else.
void onPromisc(void* buf, wifi_promiscuous_pkt_type_t type) {
    (void)type;
    const uint8_t ch = g_dwellCh;
    if (!buf || ch == 0 || ch > 13) return;
    const wifi_promiscuous_pkt_t* pkt = static_cast<const wifi_promiscuous_pkt_t*>(buf);
    if (g_frames[ch] < 0xFFFF) { g_frames[ch] = g_frames[ch] + 1; g_rssiSum[ch] = g_rssiSum[ch] + pkt->rx_ctrl.rssi; }
}
}  // namespace

bool ChannelSurvey::start(uint8_t restoreChannel, uint32_t nowMs) {
    if (active()) return false;
    restore_ = validChannel(restoreChannel) ? restoreChannel : kDefaultChannel;
    for (Channel& c : ch_) c = Channel();
    memset(const_cast<uint16_t*>(g_frames), 0, sizeof(g_frames));
    memset(const_cast<int32_t*>(g_rssiSum), 0, sizeof(g_rssiSum));
    bestCount_ = 0;
    if (WiFi.scanNetworks(true, false, false, kScanMsPerChannel) == WIFI_SCAN_FAILED) {
        state_ = State::Failed;
        return false;
    }
    state_ = State::Scanning;
    stateStart_ = nowMs;
    return true;
}

void ChannelSurvey::cancel() {
    if (!active()) return;
    esp_wifi_set_promiscuous(false);
    esp_wifi_scan_stop();                  // the driver must not be scanning when the channel is set
    WiFi.scanDelete();
    esp_wifi_set_channel(restore_, WIFI_SECOND_CHAN_NONE);
    state_ = State::Idle;
}

uint8_t ChannelSurvey::progressPercent() const {
    switch (state_) {
        case State::Scanning: return static_cast<uint8_t>(std::min<uint32_t>(55, (millis() - stateStart_) * 55 / (kScanMsPerChannel * 14UL)));
        case State::Dwell: return static_cast<uint8_t>(55 + (dwellCh_ ? dwellCh_ : 1) * 44 / 13);
        case State::Done: return 100;
        default: return 0;
    }
}

void ChannelSurvey::tick(uint32_t nowMs) {
    if (state_ == State::Scanning) {
        const int16_t n = WiFi.scanComplete();
        if (n == WIFI_SCAN_RUNNING) {
            if (nowMs - stateStart_ > kScanTimeoutMs) { esp_wifi_scan_stop(); WiFi.scanDelete(); esp_wifi_set_channel(restore_, WIFI_SECOND_CHAN_NONE); state_ = State::Failed; }
            return;
        }
        if (n < 0) { esp_wifi_set_channel(restore_, WIFI_SECOND_CHAN_NONE); state_ = State::Failed; return; }
        finishScan();
        // start the passive traffic dwell
        const wifi_promiscuous_filter_t filter = {WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA};
        esp_wifi_set_promiscuous_filter(&filter);
        esp_wifi_set_promiscuous_rx_cb(&onPromisc);
        esp_wifi_set_promiscuous(true);
        dwellCh_ = 1;
        g_dwellCh = 1;
        esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
        stateStart_ = nowMs;
        state_ = State::Dwell;
        return;
    }
    if (state_ == State::Dwell) {
        if (nowMs - stateStart_ < kDwellMs) return;
        if (dwellCh_ >= 13) {
            g_dwellCh = 0;
            esp_wifi_set_promiscuous(false);
            esp_wifi_set_channel(restore_, WIFI_SECOND_CHAN_NONE);
            finish();
            return;
        }
        ++dwellCh_;
        g_dwellCh = dwellCh_;
        esp_wifi_set_channel(dwellCh_, WIFI_SECOND_CHAN_NONE);
        stateStart_ = nowMs;
    }
}

void ChannelSurvey::finishScan() {
    const int16_t n = WiFi.scanComplete();
    for (int i = 0; i < n; ++i) {
        const int ch = WiFi.channel(i);
        const int rssi = WiFi.RSSI(i);
        if (ch < 1 || ch > 13) continue;
        Channel& c = ch_[ch];
        if (c.aps < 255) ++c.aps;
        if (c.apRssi == 0 || rssi > c.apRssi) c.apRssi = static_cast<int8_t>(rssi);
    }
    WiFi.scanDelete();
}

void ChannelSurvey::computeScores() {
    for (int c = 1; c <= 13; ++c) {
        ch_[c].frames = g_frames[c];
        ch_[c].avgRssi = g_frames[c] ? static_cast<int8_t>(g_rssiSum[c] / g_frames[c]) : 0;
    }
    static const float overlap[5] = {1.0f, 0.8f, 0.55f, 0.3f, 0.1f};    // neighbouring channels bleed into each other
    float load[14] = {0};
    for (int ap = 1; ap <= 13; ++ap) {
        if (!ch_[ap].aps) continue;
        float w = (ch_[ap].apRssi + 95) / 45.0f;                          // -95 dBm -> 0, -50 dBm -> 1
        w = std::max(0.0f, std::min(1.0f, w));
        const float strength = 40.0f * w + 6.0f * std::min<int>(ch_[ap].aps, 4) * 0.5f;   // a few weak APs still count
        for (int c = 1; c <= 13; ++c) {
            const int d = abs(c - ap);
            if (d <= 4) load[c] += strength * overlap[d];
        }
    }
    for (int c = 1; c <= 13; ++c) {
        const float traffic = std::min(1.0f, ch_[c].frames / 80.0f) * 40.0f;
        for (int k = 1; k <= 13; ++k) {
            const int d = abs(c - k);
            if (d <= 2) load[k] += traffic * (d == 0 ? 1.0f : (d == 1 ? 0.4f : 0.15f));
        }
    }
    for (int c = 1; c <= 13; ++c) ch_[c].score = static_cast<uint8_t>(std::min(100.0f, load[c]));
}

void ChannelSurvey::finish() {
    computeScores();
    uint8_t order[13];
    for (uint8_t i = 0; i < 13; ++i) order[i] = i + 1;
    std::sort(order, order + 13, [this](uint8_t a, uint8_t b) {
        if (ch_[a].score != ch_[b].score) return ch_[a].score < ch_[b].score;
        const bool aNon = (a == 1 || a == 6 || a == 11), bNon = (b == 1 || b == 6 || b == 11);   // prefer the classic non-overlapping trio on ties
        if (aNon != bNon) return aNon;
        return a < b;
    });
    bestCount_ = 13;
    memcpy(best_, order, 13);
    state_ = State::Done;
}
