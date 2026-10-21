// PcBridge.h - FogSerial/2 endpoint of the remote (docs/SERIAL_PROTOCOL.md): lets the Fog Console act as a
// remote control, configure the system and debug the radio link.
#pragma once

#include <Arduino.h>
#include <functional>
#include "FogJsonLine.h"
#include "FogLog.h"
#include "FogRemoteLink.h"
#include "FogSys.h"
#include "battery/BatteryMonitor.h"
#include "config/RemoteSettings.h"
#include "link/Fleet.h"

class PcBridge : public Fog::RemoteEvents {
public:
    struct Context {
        Fleet* fleet = nullptr;
        RemoteSettings* settings = nullptr;
        BatteryMonitor* battery = nullptr;
        const Fog::LifeCounters* life = nullptr;
        Fog::LoopMeter* meter = nullptr;
        uint8_t resetReason = 0;
    };

    void begin(const Context& ctx);
    void loop(uint32_t nowMs);
    bool pcPresent(uint32_t nowMs) const { return json_.pcPresent(nowMs); }
    void setMenuName(const char* name) { menu_ = name; }
    void setBlank(bool b) { blank_ = b; }
    // Called after a settings change that needs applying (tx power, brightness...).
    std::function<void()> onSettingsChanged;

    // Fog::RemoteEvents (forwarded by Fleet)
    void onJobDone(const Fog::JobDone& d) override;
    void onDeviceChanged(int) override { fleetDirty_ = true; }
    void onOnlineChanged(int, bool) override { fleetDirty_ = true; }
    void onRebooted(int) override { fleetDirty_ = true; }
    void onHeard(const Fog::Heard& h) override;
    void onInfo(int dev) override;
    void onMigrate(uint8_t stage, int dev, bool ok, uint8_t ch, uint8_t okCount, uint8_t failCount, bool committed) override;
    void onLinkTest(const Fog::LinkTestResult& r) override;
    void onHomeChannel(uint8_t) override { fleetDirty_ = true; }
    void onRemoved(int dev) override;

private:
    struct PendingCmd { bool used = false; uint16_t job = 0; uint32_t req = 0; int8_t dev = -1; uint8_t op = 0; };
    struct PendingStat { bool used = false; uint32_t req = 0; int8_t dev = -1; uint32_t deadline = 0; };

    void handle(JsonDocument& doc, uint32_t nowMs);
    void handleCmd(JsonDocument& doc, uint32_t id);
    void handleGet(JsonDocument& doc, uint32_t id, uint32_t nowMs);
    void handleSet(JsonDocument& doc, uint32_t id);
    void handleFactoryReset(JsonDocument& doc, uint32_t id, uint32_t nowMs);
    int resolveDev(JsonDocument& doc) const;               // >=0 index, -1 none, -2 all
    void fillSettings(JsonObject o) const;
    void sendFleet(uint32_t nowMs);
    void sendRemote(uint32_t nowMs);
    void fillSelfStats(JsonObject self, uint32_t nowMs);
    void sendStats(uint32_t nowMs);
    void fillDevStat(JsonObject o, int dev, uint32_t nowMs);
    void sendDevStats(uint32_t nowMs, int only = -1);
    void sendSurveyResults();
    void drainLog(uint32_t nowMs);
    void tapFrame(bool tx, const uint8_t* mac, int8_t rssi, const uint8_t* data, size_t len);
    void pollInfo(uint32_t nowMs);
    void expireStats(uint32_t nowMs);
    void track(int job, uint32_t req, int dev, uint8_t op);

    Context c_;
    Fog::JsonLine json_;
    Fog::LogLevel logLevel_ = Fog::LogLevel::Off;
    bool sniff_ = false;
    uint32_t statsEveryMs_ = 0;
    uint32_t lastFleetMs_ = 0, lastFleetSent_ = 0, lastRemoteMs_ = 0, lastStatsMs_ = 0, lastInfoMs_ = 0;
    uint32_t sniffWindow_ = 0, sniffBudget_ = 0;
    uint32_t seq_ = 0;
    bool fleetDirty_ = false;
    bool blank_ = false;
    const char* menu_ = "main";
    int infoRr_ = 0;
    PendingCmd pend_[48];
    PendingStat pendStat_[4];
    uint32_t surveyReq_ = 0;
    bool surveyWasActive_ = false;
    uint32_t linkTestReq_ = 0;
    uint32_t factoryDeadline_ = 0;
    bool factoryRestart_ = false;
    uint32_t rebootAt_ = 0;
};
