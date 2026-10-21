// FogTimerNode.h - protocol brain of a timer: settings, command execution, status/heartbeat scheduling.
//
// All hardware access goes through TimerPlatform, so the exact same logic runs on the ESP32 and in the
// PC simulation that tests it against the remote's link engine.
#pragma once

#include <stdint.h>
#include "FogCycle.h"
#include "FogLinkStats.h"
#include "FogProto.h"

namespace Fog {

struct TimerSettings {
    uint32_t onTenths = kDefaultOnTenths;
    uint32_t offTenths = kDefaultOffTenths;
    char name[kNameLen] = {'F', 'o', 'g', 'T', 'i', 'm', 'e', 'r', 0, 0};
    uint8_t channel = kDefaultChannel;
    uint8_t startPhaseOn = 0;     // 0: start with the OFF phase after power-up
    uint8_t led = 1;
    uint8_t net = kDefaultNet;
};

class TimerPlatform {
public:
    virtual ~TimerPlatform() {}
    virtual bool send(const uint8_t* mac /* nullptr = broadcast */, const uint8_t* data, size_t len) = 0;
    virtual void setChannel(uint8_t ch) = 0;
    virtual void saveSettings(const TimerSettings& s) = 0;
    virtual void eraseSettings() = 0;
    virtual void reboot() = 0;
    virtual void identify(uint16_t ms) = 0;
    virtual void fillInfo(InfoPayload& info) = 0;       // platform-owned fields (heap, counters, reset reason)
    virtual uint32_t random(uint32_t exclusiveMax) = 0;
    virtual void log(const char* msg) { (void)msg; }
};

class TimerNode {
public:
    TimerNode(TimerPlatform& platform) : plat_(platform) {}

    // Call once after the radio is up. bootCount comes from persistent storage.
    void begin(uint32_t nowMs, const TimerSettings& settings, uint32_t bootCount);

    // Radio input. rssi may be 0 when unknown.
    void onFrame(const uint8_t* srcMac, int8_t rssi, const uint8_t* data, size_t len, uint32_t nowMs);

    // Must be called every loop iteration.
    void tick(uint32_t nowMs);

    bool outputOn(uint32_t nowMs) const { return engine_.outputOn(nowMs); }
    const CycleEngine& engine() const { return engine_; }
    CycleEngine& engine() { return engine_; }
    const TimerSettings& settings() const { return s_; }
    TimerSettings& mutableSettings() { return s_; }
    void applyLocalChange(uint32_t nowMs);              // USB console changed engine/settings directly
    uint8_t runLocal(Op op, const uint8_t* args, size_t argLen, uint32_t nowMs);   // same semantics as a radio command (USB console)
    void buildStatus(StatusPayload& out, uint32_t nowMs, uint8_t extraFlags) const;
    void buildInfo(InfoPayload& out) const;
    void sendStatusNow(const uint8_t* mac, uint32_t nowMs, uint8_t flags);
    uint32_t bootCount() const { return boot_; }

    // Counters (read by the platform for the INFO payload).
    uint32_t rxFrames = 0, rxBad = 0, rxForeign = 0, rxProbes = 0, rxCmds = 0;
    uint32_t txFrames = 0, txFail = 0, txBeats = 0, txReplies = 0;
    uint32_t cmdsRun = 0, cmdsDup = 0, cmdsRejected = 0;
    void noteTxResult(bool ok) { if (!ok) ++txFail; }   // platform: unicast frame not acknowledged by the MAC
    bool identifying = false;

private:
    struct Pending { bool used = false; uint8_t mac[6] = {0}; uint32_t due = 0; uint8_t kind = 0; };   // kind: 0 status, 1 info
    void handleCmd(const uint8_t* src, const FrameView& f, uint32_t nowMs);
    uint8_t applyOp(uint8_t op, const uint8_t* args, size_t argLen, uint32_t nowMs);
    void queueReply(const uint8_t* mac, uint32_t due, uint8_t kind);
    bool sendFrame(const uint8_t* mac, Type type, const void* payload, size_t len);
    void sendInfo(const uint8_t* mac);
    void execute(uint8_t op, const uint8_t* a, size_t n, uint8_t& result, uint16_t& fx, uint32_t nowMs);

    TimerPlatform& plat_;
    TimerSettings s_;
    CycleEngine engine_;
    CmdDedup dedup_;
    uint32_t boot_ = 0;
    uint32_t startMs_ = 0;
    uint8_t txSeq_ = 0;
    uint32_t nextBeat_ = 0;
    uint8_t bootBeats_ = 0;
    uint32_t bootBeatAt_ = 0;
    uint32_t lastEventMs_ = 0;
    bool statusDirty_ = false;
    bool infoWanted_ = false;
    int8_t lastRssi_ = 0;
    int8_t rssiMin_ = 0, rssiMax_ = 0;
    int32_t rssiSum_ = 0;
    uint32_t rssiCount_ = 0;
    uint8_t lastCmdId_ = 0, lastCmdResult_ = 0, lastCmdSrc_ = 0;
    Pending pending_[4];
    bool chPending_ = false; uint8_t chValue_ = 0; uint32_t chDue_ = 0;
    bool rebootPending_ = false; uint32_t rebootDue_ = 0;
    uint8_t prevOutput_ = 0;
};

}  // namespace Fog
