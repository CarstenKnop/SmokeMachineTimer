// FogRemoteLink.h - the remote's link engine: fleet tracking, reliable command delivery, discovery,
// lost-timer recovery, channel migration and link tests.
//
// Pure C++ (hardware reached through RemotePort) so it is exercised on a PC against simulated
// timers with packet loss and power cuts. Single-threaded: call everything from the main loop.
//
// Firmware update (docs/OTA_UPDATE.md): the link only bridges Type::Ota frames between the PC and a paired timer. submitOta() queues
// one frame (8 at most, sent in order, one at a time, after any due command), onOtaFrame() hands back what a timer sent. While the PC
// keeps updating, the radio is "held": nothing that leaves the home channel or floods it is started (discovery, recovery, channel
// move, link test); the hold ends by itself 15 s after the last submitOta()/setOtaHold(true).
//
// Delivery model (see docs/RADIO_PROTOCOL.md):
//   * a command is a short-lived *job*; it is (re)sent until the timer echoes its command id in a STATUS,
//     or its time budget runs out - after that it is discarded and never runs late;
//   * the timer de-duplicates retransmissions, so retries are always safe;
//   * state comes from timer heartbeats, so there is no polling traffic.
#pragma once

#include <stdint.h>
#include "FogLinkStats.h"
#include "FogProto.h"

namespace Fog {

class RemotePort {
public:
    virtual ~RemotePort() {}
    // Queue one frame. mac==nullptr means broadcast. Returns false if the radio cannot take it right now.
    // The result of a unicast frame is reported later through RemoteLink::onTxResult().
    virtual bool send(const uint8_t* mac, const uint8_t* data, size_t len) = 0;
    virtual void setChannel(uint8_t ch) = 0;
    virtual uint8_t channel() const = 0;
    virtual uint32_t random(uint32_t exclusiveMax) = 0;
};

enum class JobResult : uint8_t { Ok, Rejected, Timeout, Cancelled };

struct JobDone {
    uint16_t jobId;
    int8_t dev;
    uint8_t op;
    JobResult result;
    uint8_t attempts;
    uint32_t ms;       // time from submit to completion
    uint32_t tag;
};

struct DevLinkStats {
    uint32_t macOk = 0, macFail = 0;
    uint32_t cmds = 0, cmdsOk = 0, cmdsTimeout = 0, cmdsRejected = 0, retries = 0;
    uint32_t rttSumMs = 0, rttCount = 0, rttMax = 0;
    int8_t rssiMin = 0, rssiMax = 0;
    int32_t rssiSum = 0;
    uint32_t rssiCount = 0;
    uint32_t offlineEvents = 0, offlineMs = 0, reboots = 0;
};

struct Device {
    bool used = false;
    uint8_t mac[6] = {0};
    char name[kNameLen] = {0};
    uint8_t chHint = kDefaultChannel;      // channel it was last seen on (persisted by the caller)
    // runtime
    bool statusValid = false;
    StatusPayload st = {};
    uint32_t statusMs = 0;                 // when st was received
    uint32_t lastRxMs = 0;                 // any frame
    RssiFilter rssi;
    int8_t rssiT = 0;                      // RSSI at the timer
    LossTracker loss;
    bool wasOnline = false;
    uint32_t offlineSince = 0;
    uint16_t lastBoot = 0;
    bool bootKnown = false;
    uint32_t rebootedMs = 0;               // when a power cycle was last noticed
    uint8_t failStreak = 0;                // consecutive unicast frames not acknowledged
    uint32_t nextRecoverAt = 0;
    uint32_t recoverBackoffMs = 8000;
    bool infoValid = false;
    InfoPayload info = {};
    uint32_t infoMs = 0;
    DevLinkStats stats;
};

struct Heard {
    bool used = false;
    uint8_t mac[6] = {0};
    char name[kNameLen] = {0};
    int8_t rssi = 0;
    uint8_t channel = 0;
    uint32_t lastMs = 0;
    uint32_t emitMs = 0;
    uint32_t onTenths = 0, offTenths = 0;
    bool outputOn = false;
    uint16_t boot = 0;
    uint8_t fwMajor = 0, fwMinor = 0;
};

struct RemoteCounters {
    uint32_t tx = 0, txOk = 0, txFail = 0, rx = 0, rxBad = 0, rxForeign = 0;
    uint32_t cmds = 0, cmdsOk = 0, cmdsTimeout = 0, cmdsRejected = 0, retries = 0;
    uint32_t rttSumMs = 0, rttCount = 0, rttMax = 0;
    uint32_t sweeps = 0, recovers = 0, found = 0, migrations = 0;
    uint32_t otaTx = 0, otaTxAcked = 0, otaRx = 0, otaQueueFull = 0;
    uint32_t chTx[kMaxChannel + 1] = {0}, chFail[kMaxChannel + 1] = {0}, chRx[kMaxChannel + 1] = {0};
};

struct LinkTestResult {
    int8_t dev;
    uint16_t sent, macAck, echo;
    uint32_t rttSumMs, rttMax;
    int rssiAvg;
    bool done;
};

class RemoteEvents {
public:
    virtual ~RemoteEvents() {}
    virtual void onJobDone(const JobDone&) {}
    virtual void onDeviceChanged(int /*dev*/) {}
    virtual void onOnlineChanged(int /*dev*/, bool /*online*/) {}
    virtual void onRebooted(int /*dev*/) {}
    virtual void onHeard(const Heard&) {}
    virtual void onInfo(int /*dev*/) {}
    // stage: 0 start, 1 per-device result, 2 done
    virtual void onMigrate(uint8_t /*stage*/, int /*dev*/, bool /*ok*/, uint8_t /*ch*/, uint8_t /*okCount*/, uint8_t /*failCount*/, bool /*committed*/) {}
    virtual void onLinkTest(const LinkTestResult&) {}
    virtual void onHomeChannel(uint8_t /*ch*/) {}      // caller should persist
    virtual void onLog(const char* /*msg*/) {}
    virtual void onRemoved(int /*dev*/) {}              // a timer was removed; higher indices shifted down
    // Firmware update frames. onOtaTx: the frame queued with submitOta(tag) was sent (sent) and the timer's radio acknowledged it (acked);
    // sent=false: it was dropped without being sent (the timer was removed). onOtaFrame: a Type::Ota payload from paired timer dev.
    virtual void onOtaTx(uint32_t /*tag*/, bool /*sent*/, bool /*acked*/) {}
    virtual void onOtaFrame(int /*dev*/, int8_t /*rssi*/, const uint8_t* /*payload*/, size_t /*len*/) {}
};

class RemoteLink {
public:
    static constexpr int kMaxJobs = 16;
    static constexpr int kMaxHeard = 16;
    static constexpr uint32_t kEchoWaitMs = 100;
    static constexpr uint32_t kTxWatchdogMs = 150;
    static constexpr uint32_t kDefaultTtlMs = 2500;
    static constexpr uint8_t kMaxAttempts = 16;
    static constexpr int kOtaQueue = 8;
    static constexpr uint32_t kOtaHoldMs = 15000;

    enum : uint32_t { TagNone = 0, TagMigrate = 0x10000, TagRecover = 0x20000, TagInfo = 0x30000 };

    RemoteLink(RemotePort& port, RemoteEvents* events = nullptr) : port_(port), ev_(events) {}

    void begin(const uint8_t ownMac[6], uint8_t net, uint8_t homeChannel, uint32_t nowMs);
    void setEvents(RemoteEvents* ev) { ev_ = ev; }
    uint8_t homeChannel() const { return home_; }
    uint8_t net() const { return net_; }

    // ---- fleet ----
    int addDevice(const uint8_t mac[6], const char* name, uint8_t chHint);
    void removeDevice(int idx);
    int deviceCount() const { return count_; }
    const Device& device(int i) const { return dev_[i]; }
    Device& mutableDevice(int i) { return dev_[i]; }
    int findDevice(const uint8_t mac[6]) const;
    bool isOnline(int i, uint32_t nowMs) const;
    // Phase / elapsed time extrapolated to "now" from the last report.
    void predict(int i, uint32_t nowMs, bool& phaseOn, uint32_t& elapsedMs) const;
    int onlineCount(uint32_t nowMs) const;

    // ---- commands ----
    // Returns the job id (>0) or -1 if the queue is full / bad device. confirm=false: send-and-forget (lease refreshes).
    int submit(int dev, Op op, const void* args, size_t argLen, uint32_t ttlMs = kDefaultTtlMs, uint32_t tag = TagNone, bool confirm = true);
    int submitSetTimes(int dev, uint32_t onTenths, uint32_t offTenths, uint32_t tag = TagNone);
    int submitMode(int dev, Mode mode, uint16_t leaseMs, bool confirm = true);
    int submitToggle(int dev) { return submit(dev, Op::Toggle, nullptr, 0); }
    int submitReset(int dev) { return submit(dev, Op::Reset, nullptr, 0); }
    int submitName(int dev, const char* name);
    int submitPhase(int dev, bool on, uint32_t elapsedMs);
    int submitChannel(int dev, uint8_t ch, uint32_t tag = TagNone);
    int submitIdentify(int dev, uint16_t ms);
    int submitGetInfo(int dev) { return submit(dev, Op::GetInfo, nullptr, 0, 3000, TagInfo); }
    int submitFactoryReset(int dev);
    int submitReboot(int dev);
    int submitStartup(int dev, bool startOn);
    int pendingFor(int dev) const;
    void cancelDevice(int dev);

    // ---- radio plumbing ----
    // rxChannel: channel the frame was really received on (0 = assume the radio's current channel)
    void onFrame(const uint8_t* mac, int8_t rssi, const uint8_t* data, size_t len, uint32_t nowMs, uint8_t rxChannel = 0);
    void setNow(uint32_t nowMs) { now_ = nowMs; }      // keep submit() deadlines right while tick() is not running
    void onTxResult(bool ok, uint32_t nowMs);
    void tick(uint32_t nowMs);

    // ---- activities ----
    void probeNow();                                   // broadcast "send STATUS" on the home channel
    bool startDiscovery(bool sweepChannels);          // false: refused (firmware update running)
    void stopDiscovery();
    bool discovering() const { return discovery_; }
    int heardCount() const { return kMaxHeard; }
    const Heard& heard(int i) const { return heard_[i]; }
    bool startRecover(int dev);                        // dev<0: every offline timer; false: refused (firmware update running)
    bool startMigrate(uint8_t ch, bool force);
    bool migrating() const { return mig_.active; }
    bool startLinkTest(int dev, uint16_t count, uint16_t gapMs);
    bool linkTesting() const { return lt_.active; }
    void setAutoRecover(bool on) { autoRecover_ = on; }
    void setHomeChannel(uint8_t ch) { if (validChannel(ch)) setHome(ch); }
    uint8_t cmdSeq() const { return cmdSeq_; }
    void setCmdSeq(uint8_t v) { cmdSeq_ = v; }
    bool busyExcursion() const { return exc_.kind != Exc::None; }
    int queuedJobs() const;

    // ---- firmware update bridge ----
    enum class OtaSubmit : uint8_t { Ok, Full, BadDevice, TooLong };
    OtaSubmit submitOta(int dev, const uint8_t* payload, size_t len, uint32_t tag);
    void setOtaHold(bool on);                          // on: (re)start the 15 s hold, off: release it
    bool otaHeld() const { return otaHoldUntil_ != 0 && static_cast<int32_t>(now_ - otaHoldUntil_) < 0; }
    int otaQueued() const { return otaCount_; }

    const RemoteCounters& counters() const { return cnt_; }
    void resetCounters();
    void resyncLoss() { for (int i = 0; i < count_; ++i) dev_[i].loss.resync(); }   // after the radio was away from the home channel

private:
    struct Job {
        bool used = false;
        uint16_t id = 0;
        int8_t dev = -1;
        uint8_t cmdId = 0, op = 0, argLen = 0, epoch = 0;
        uint8_t args[kMaxCmdArgs] = {0};
        bool confirm = true;
        bool inFlight = false;
        uint8_t attempts = 0;
        uint8_t channel = 0;           // 0 = home channel, else only sent while parked on that channel
        uint32_t tag = 0;
        uint32_t created = 0, deadline = 0, nextSend = 0, firstSent = 0;
    };
    struct OtaItem {
        bool inFlight = false;
        int8_t dev = -1;
        uint8_t len = 0;
        uint8_t data[kMaxPayload] = {0};
        uint32_t tag = 0;
    };
    enum class Exc : uint8_t { None, Discover, Find };
    struct Excursion {
        Exc kind = Exc::None;
        uint8_t stage = 0;
        uint8_t list[kMaxChannel] = {0};
        uint8_t count = 0, idx = 0;
        int8_t dev = -1;
        int8_t queue[kMaxTimers] = {0};
        uint8_t qLen = 0, qIdx = 0;
        uint8_t foundCh = 0;
        uint32_t stageStart = 0, began = 0;
        uint8_t sendFails = 0;
        uint16_t cmdJob = 0;
        bool cmdDone = false;
        bool probeIsFind = false;
    };
    struct Migrate {
        bool active = false, force = false;
        uint8_t ch = 0, total = 0, done = 0, ok = 0, failed = 0;
        uint8_t oldCh = 0;
        int8_t dev[kMaxTimers] = {0};
        bool devOk[kMaxTimers] = {false};
    };
    struct LinkTest {
        bool active = false;
        int8_t dev = -1;
        uint16_t count = 0, gap = 40, sent = 0, macAck = 0, echo = 0;
        uint32_t rttSum = 0, rttMax = 0, nextAt = 0, sentAt = 0, endAt = 0;
        bool waiting = false, txPending = false, echoed = false;
        int32_t rssiSum = 0;
        uint16_t rssiN = 0;
    };

    bool sendRaw(const uint8_t* mac, Type type, const void* payload, size_t len, bool tracked);
    void handleStatus(const uint8_t* mac, int8_t rssi, uint8_t seq, const StatusPayload& st, uint32_t now, uint8_t rxCh);
    void handleInfo(const uint8_t* mac, const InfoPayload& info, uint32_t now);
    void touchHeard(const uint8_t* mac, int8_t rssi, const StatusPayload& st, uint32_t now);
    void completeJob(int j, JobResult r, uint32_t now);
    int findJobForEcho(int dev, uint8_t cmdId) const;
    int headJob(int dev) const;
    bool trySendJob(uint32_t now);
    void afterTxForJob(int j, bool ok, uint32_t now);
    void checkOnline(uint32_t now);
    void tickExcursion(uint32_t now);
    void tickDiscovery(uint32_t now);
    void tickAutoRecover(uint32_t now);
    void tickLinkTest(uint32_t now);
    bool trySendOta(uint32_t now);
    void dropOta(int index, bool sent, bool acked);
    void beginExcursion(Exc kind, uint32_t now);
    void endExcursion(uint32_t now);
    void buildChannelList(int dev);
    void migrateJobDone(const JobDone& d, uint32_t now);
    void finishMigrate(uint32_t now);
    void setHome(uint8_t ch);
    uint32_t retryDelay(const Device& d, bool macOk, uint8_t attempts);
    uint8_t nextCmdId();
    void noteChannelTx(bool ok);

    RemotePort& port_;
    RemoteEvents* ev_;
    uint8_t own_[6] = {0};
    uint8_t ownTag_ = 0;
    uint32_t now_ = 0;
    uint8_t net_ = kDefaultNet;
    uint8_t home_ = kDefaultChannel;
    Device dev_[kMaxTimers];
    int count_ = 0;
    Job jobs_[kMaxJobs];
    uint16_t jobSeq_ = 0;
    uint8_t cmdSeq_ = 0;
    uint8_t txSeq_ = 0;
    int rr_ = 0;
    // tx tracking
    bool txBusy_ = false;
    uint32_t txStart_ = 0;
    uint16_t txJob_ = 0;                 // job id in flight (0 = none)
    bool txUnicast_ = false;
    enum class TxKind : uint8_t { None, Job, Find, LinkTest, Other, Ota } txKind_ = TxKind::None;
    uint32_t lastTxDone_ = 0;
    int8_t txDev_ = -1;
    uint8_t txCh_ = 0;
    bool recoverReq_ = false;
    int8_t recoverDev_ = -1;
    // activities
    Heard heard_[kMaxHeard];
    bool discovery_ = false, discoverySweep_ = false;
    uint32_t nextDiscoveryProbe_ = 0, nextSweepAt_ = 0;
    bool autoRecover_ = true;
    uint32_t nextOnlineCheck_ = 0;
    Excursion exc_;
    Migrate mig_;
    LinkTest lt_;
    bool probePending_ = false;
    OtaItem ota_[kOtaQueue];
    int otaCount_ = 0;
    uint32_t otaHoldUntil_ = 0;
    uint32_t bootMs_ = 0;
    RemoteCounters cnt_;
};

}  // namespace Fog
