#include "FogRadio.h"

#include <WiFi.h>
#include <esp_now.h>
#include <esp_mac.h>
#include <esp_wifi.h>

namespace Fog {

FogRadio* FogRadio::self_ = nullptr;

namespace {
constexpr uint8_t kBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
constexpr uint32_t kTxWatchdogMs = 120;
constexpr int kRxQueueDepth = 32;     // a burst after wake-up can arrive while the OLED is being flushed (~25 ms)
}  // namespace

bool FogRadio::begin(uint8_t channel, int8_t txPowerQdbm) {
    self_ = this;
    if (!rxQueue_) rxQueue_ = xQueueCreate(kRxQueueDepth, sizeof(Rx));
    if (!rxQueue_) return false;

    WiFi.persistent(false);            // never write Wi-Fi settings to flash
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    WiFi.setSleep(false);              // radio must always listen (modem sleep would drop frames)
    esp_wifi_set_ps(WIFI_PS_NONE);

    // Channels 12 and 13 are legal in Europe; tell the driver so it will transmit there.
    wifi_country_t country = {};
    memcpy(country.cc, "DK", 2);
    country.schan = 1;
    country.nchan = 13;
    country.max_tx_power = 20;
    country.policy = WIFI_COUNTRY_POLICY_MANUAL;
    esp_wifi_set_country(&country);

    if (esp_now_init() != ESP_OK) return false;
    esp_now_register_recv_cb(&FogRadio::onRecv);
    esp_now_register_send_cb(&FogRadio::onSend);
    setTxPower(txPowerQdbm);
    up_ = true;
    peerCount_ = 0;
    ensurePeer(kBroadcast);
    return setChannel(channel);
}

void FogRadio::end() {
    if (!up_) return;
    esp_now_unregister_recv_cb();
    esp_now_unregister_send_cb();
    esp_now_deinit();
    up_ = false;
}

bool FogRadio::setChannel(uint8_t channel) {
    if (!validChannel(channel)) return false;
    // esp_wifi_set_channel only sticks while the radio is not associated; we never associate.
    const esp_err_t err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) return false;
    channel_ = channel;
    return true;
}

void FogRadio::setTxPower(int8_t qdbm) {
    if (qdbm < 8) qdbm = 8;
    if (qdbm > 84) qdbm = 84;
    esp_wifi_set_max_tx_power(qdbm);
}

void FogRadio::macAddress(uint8_t out[6]) const {
    esp_read_mac(out, ESP_MAC_WIFI_STA);
}

bool FogRadio::ensurePeer(const uint8_t* mac) {
    if (esp_now_is_peer_exist(mac)) return true;
    // The peer table is small (20). Keep the newest 12 of ours and recycle the oldest slot.
    if (peerCount_ >= 12) {
        const uint8_t victim = peerNext_ % 12;
        esp_now_del_peer(peers_[victim]);
        memcpy(peers_[victim], mac, 6);
        peerNext_ = static_cast<uint8_t>((victim + 1) % 12);
    } else {
        memcpy(peers_[peerCount_++], mac, 6);
    }
    esp_now_peer_info_t info = {};
    memcpy(info.peer_addr, mac, 6);
    info.channel = 0;                  // 0 = whatever channel the radio is on right now
    info.ifidx = WIFI_IF_STA;
    info.encrypt = false;
    if (esp_now_add_peer(&info) != ESP_OK) { ++stats_.peerErrors; return false; }
    return true;
}

bool FogRadio::send(const uint8_t* mac, const uint8_t* data, size_t len) {
    if (!up_ || txBusy_ || len == 0 || len > kMaxFrame) return false;
    const uint8_t* dst = mac ? mac : kBroadcast;
    if (!ensurePeer(dst)) return false;
    txDone_ = false;
    txBusy_ = true;
    txStartMs_ = millis();
    if (esp_now_send(dst, data, len) != ESP_OK) {
        txBusy_ = false;
        ++stats_.txDrop;
        return false;
    }
    ++stats_.tx;
    return true;
}

bool FogRadio::pollTxResult(bool& ok) {
    if (!txDone_) return false;
    ok = txOk_;
    txDone_ = false;
    if (ok) ++stats_.txOk; else ++stats_.txFail;
    return true;
}

bool FogRadio::pollRx(Rx& out) {
    if (!rxQueue_) return false;
    return xQueueReceive(rxQueue_, &out, 0) == pdTRUE;
}

void FogRadio::service(uint32_t nowMs) {
    if (txBusy_ && !txDone_ && static_cast<uint32_t>(nowMs - txStartMs_) > kTxWatchdogMs) {
        ++stats_.txWd;
        txOk_ = false;
        txBusy_ = false;
        txDone_ = true;                // report as failed so the link layer keeps going
    }
}

// ---- Wi-Fi task context from here down: copy and return ------------------------------------------------
void FogRadio::onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    FogRadio* r = self_;
    if (!r || !info || !data || len <= 0 || len > static_cast<int>(kMaxFrame)) return;
    Rx item;
    memcpy(item.mac, info->src_addr, 6);
    item.rssi = info->rx_ctrl ? static_cast<int8_t>(info->rx_ctrl->rssi) : 0;
    item.channel = info->rx_ctrl ? static_cast<uint8_t>(info->rx_ctrl->channel) : 0;
    item.len = static_cast<uint8_t>(len);
    memcpy(item.data, data, len);
    if (xQueueSend(r->rxQueue_, &item, 0) != pdTRUE) ++r->stats_.rxDrop;
    else ++r->stats_.rx;
}

void FogRadio::onSend(const esp_now_send_info_t* info, esp_now_send_status_t status) {
    (void)info;
    FogRadio* r = self_;
    if (!r) return;
    r->txOk_ = (status == ESP_NOW_SEND_SUCCESS);
    r->txBusy_ = false;
    r->txDone_ = true;
}

}  // namespace Fog
