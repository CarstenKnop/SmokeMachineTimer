// Fleet.h - the remote's view of its timers: radio pump, FogLink engine, paired list, hold-to-fog,
// discovery list and everything the OLED UI and the PC bridge need.
#pragma once

#include <Arduino.h>
#include <functional>
#include "FogRadio.h"
#include "FogRemoteLink.h"
#include "config/RemoteSettings.h"
#include "link/ChannelSurvey.h"
#include "link/FleetTypes.h"

class Fleet : public Fog::RemotePort, public Fog::RemoteEvents {
public:
    static Fleet* get() { return self_; }
    explicit Fleet(RemoteSettings& settings);

    bool begin(uint32_t nowMs);
    void loop(uint32_t nowMs);
    void applyTxPower(int8_t qdbm) { radio_.setTxPower(qdbm); }
    void setSink(Fog::RemoteEvents* sink) { sink_ = sink; }
    void setAwake(bool awake) { link_.setAutoRecover(awake); }

    Fog::RemoteLink& link() { return link_; }
    Fog::FogRadio& radio() { return radio_; }
    ChannelSurvey& survey() { return survey_; }
    const uint8_t* ownMac() const { return mac_; }
    uint8_t homeChannel() const { return link_.homeChannel(); }

    // ---- paired list (UI) ----
    int getPairedCount() const { return count_; }
    const SlaveDevice& getPaired(int i) const { return views_[i]; }
    int getActiveIndex() const { return active_; }
    const SlaveDevice* getActiveDevice() const { return (active_ >= 0 && active_ < count_) ? &views_[active_] : nullptr; }
    void setActive(int idx);
    void removeDeviceByIndex(int idx);
    int findPairedIndexByMac(const uint8_t mac[6]) const { return link_.findDevice(mac); }
    void unpairByMac(const uint8_t mac[6]) { removeDeviceByIndex(link_.findDevice(mac)); }

    // ---- commands on the active timer ----
    void toggleActive();
    void holdFog(bool held, uint32_t nowMs);     // '*' held: dead-man ForceOn, released: Auto
    void setActiveTimer(float tonSec, float toffSec);
    void setActiveName(const char* name);
    void factoryResetActive();
    int renameByIndex(int idx, const char* name);      // returns the job id (or -1)
    void wipeLocalData();                               // forget pairings (factory reset of the remote)

    // ---- discovery / pairing ----
    void startDiscovery(bool sweep);
    void stopDiscovery();
    bool isDiscovering() const { return link_.discovering(); }
    int getDiscoveredCount() const { return discCount_; }
    const DiscoveredDevice& getDiscovered(int i) const { return disc_[i]; }
    bool pairWithIndex(int idx);                 // index into the discovered list
    bool pairMac(const uint8_t mac[6], const char* name, uint8_t channel);

    // ---- channel tools ----
    bool startSurvey();
    bool migrateTo(uint8_t channel, bool force);
    void findLostTimers();
    bool startLinkTest(uint16_t count, uint16_t gapMs);
    bool updateRunning() const { return link_.otaHeld(); }      // the PC is updating a timer over the air: the radio must stay put
    const Fog::LinkTestResult& linkTestResult() const { return ltResult_; }

    // ---- notifications for the UI ----
    const char* toast(uint32_t nowMs) const { return (toastUntil_ && static_cast<int32_t>(toastUntil_ - nowMs) > 0) ? toast_ : nullptr; }
    void showToast(const char* msg, uint32_t nowMs, uint32_t ms = 2500);
    bool migrating() const { return link_.migrating(); }
    uint8_t migrateOk() const { return migOk_; }
    uint8_t migrateFail() const { return migFail_; }
    uint8_t migrateTotal() const { return migTotal_; }
    int8_t migrateResult() const { return migResult_; }   // -1 running/none, 0 abandoned, 1 committed
    void clearMigrateResult() { migResult_ = -1; }

    // ---- Fog::RemotePort ----
    bool send(const uint8_t* mac, const uint8_t* data, size_t len) override;
    void setChannel(uint8_t ch) override { radio_.setChannel(ch); }
    uint8_t channel() const override { return radio_.channel(); }
    uint32_t random(uint32_t n) override { return n ? esp_random() % n : 0; }

    // ---- Fog::RemoteEvents ----
    void onJobDone(const Fog::JobDone& d) override;
    void onDeviceChanged(int dev) override;
    void onOnlineChanged(int dev, bool online) override;
    void onRebooted(int dev) override;
    void onHeard(const Fog::Heard& h) override;
    void onInfo(int dev) override;
    void onRemoved(int dev) override { if (sink_) sink_->onRemoved(dev); }
    void onOtaTx(uint32_t tag, bool sent, bool acked) override { if (sink_) sink_->onOtaTx(tag, sent, acked); }
    void onOtaFrame(int dev, int8_t rssi, const uint8_t* payload, size_t len) override { if (sink_) sink_->onOtaFrame(dev, rssi, payload, len); }
    void onMigrate(uint8_t stage, int dev, bool ok, uint8_t ch, uint8_t okCount, uint8_t failCount, bool committed) override;
    void onLinkTest(const Fog::LinkTestResult& r) override;
    void onHomeChannel(uint8_t ch) override;

    // Frame tap for the PC packet inspector.
    using Tap = std::function<void(bool tx, const uint8_t* mac, int8_t rssi, const uint8_t* data, size_t len)>;
    Tap tap;
    uint32_t loopsDropped = 0;

private:
    struct StoredDevice { uint8_t mac[6]; char name[Fog::kNameLen]; uint8_t ch; uint8_t pad; };
    struct StoredList { uint8_t count; uint8_t active; uint8_t pad[2]; StoredDevice dev[Fog::kMaxTimers]; };
    void saveList();
    void loadList();
    void refreshViews(uint32_t nowMs);
    void refreshDiscovered(uint32_t nowMs);
    void pumpRadio(uint32_t nowMs);

    static Fleet* self_;
    RemoteSettings& settings_;
    Fog::FogRadio radio_;
    Fog::RemoteLink link_;
    ChannelSurvey survey_;
    Fog::RemoteEvents* sink_ = nullptr;
    uint8_t mac_[6] = {0};
    int count_ = 0;
    int active_ = 0;
    SlaveDevice views_[Fog::kMaxTimers];
    DiscoveredDevice disc_[Fog::RemoteLink::kMaxHeard];
    int discCount_ = 0;
    uint32_t lastDiscRefresh_ = 0;
    // hold-to-fog
    bool holding_ = false;
    int holdDev_ = -1;
    uint32_t nextRefresh_ = 0;
    // toast
    char toast_[32] = {0};
    uint32_t toastUntil_ = 0;
    // migration progress
    uint8_t migOk_ = 0, migFail_ = 0, migTotal_ = 0;
    int8_t migResult_ = -1;
    Fog::LinkTestResult ltResult_ = {};
    uint32_t now_ = 0;
    Fog::BlobStore<StoredList> store_{"fogremote", "devices", 1};
    StoredList saved_ = {};
};
