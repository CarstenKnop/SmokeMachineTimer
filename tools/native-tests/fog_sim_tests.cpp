// Host-side tests for the FogLink protocol stack.
//
// The real TimerNode and RemoteLink code runs here against a simulated 2.4 GHz medium with packet loss,
// MAC-level acknowledgements, power cuts and channel changes. Build/run with tools/native-tests/run.ps1.
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <queue>
#include <random>
#include <string>
#include <vector>

#include "FogCycle.h"
#include "FogLinkStats.h"
#include "FogProto.h"
#include "FogRemoteLink.h"
#include "FogTimerNode.h"
#include "FogUtil.h"

using namespace Fog;

static int g_checks = 0, g_fail = 0;
#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        ++g_checks;                                                             \
        if (!(cond)) {                                                          \
            ++g_fail;                                                           \
            std::printf("  FAIL %s:%d  %s  ", __FILE__, __LINE__, #cond);       \
            std::printf(__VA_ARGS__);                                           \
            std::printf("\n");                                                  \
        }                                                                       \
    } while (0)

// --------------------------------------------------------------------------- simulated medium
struct SimNode;

struct SimNet {
    uint32_t now = 0;
    double loss = 0.0;                 // per-frame (data and ack) loss probability
    double burst = 0.0;                // probability that a whole unicast exchange (all MAC retries) is wiped out
    std::mt19937 rng{12345};
    std::vector<SimNode*> nodes;
    struct Ev {
        uint32_t t; uint64_t order; std::function<void()> fn;
        bool operator<(const Ev& o) const { return t != o.t ? t > o.t : order > o.order; }
    };
    std::priority_queue<Ev> q;
    uint64_t order = 0;
    uint64_t framesSent = 0;

    double rnd() { return std::uniform_real_distribution<double>(0.0, 1.0)(rng); }
    void at(uint32_t t, std::function<void()> fn) { q.push({t, order++, std::move(fn)}); }
    void send(SimNode& from, const uint8_t* mac, const uint8_t* data, size_t len, std::function<void(bool)> txDone);
    void advanceTo(uint32_t t);
};

struct SimNode {
    SimNet& net;
    uint8_t mac[6];
    uint8_t channel = kDefaultChannel;
    bool powered = false;
    bool txBusy = false;
    std::function<void(const uint8_t* src, int8_t rssi, const uint8_t* data, size_t len)> onRx;
    SimNode(SimNet& n, uint8_t id) : net(n) {
        const uint8_t m[6] = {0x24, 0x6F, 0x28, 0x00, 0x00, id};
        memcpy(mac, m, 6);
        net.nodes.push_back(this);
    }
};

void SimNet::send(SimNode& from, const uint8_t* mac, const uint8_t* data, size_t len, std::function<void(bool)> txDone) {
    ++framesSent;
    std::vector<uint8_t> copy(data, data + len);
    const uint8_t ch = from.channel;
    const uint32_t t0 = now;
    const uint8_t srcMac[6] = {from.mac[0], from.mac[1], from.mac[2], from.mac[3], from.mac[4], from.mac[5]};
    auto deliver = [this, copy, srcMac, ch](SimNode* to, uint32_t t) {
        at(t, [this, to, copy, srcMac, ch]() {
            if (to->powered && to->channel == ch && to->onRx) {
                const int8_t rssi = static_cast<int8_t>(-55 - static_cast<int>(rnd() * 12));
                to->onRx(srcMac, rssi, copy.data(), copy.size());
            }
        });
    };
    if (!mac) {   // broadcast: no ack, no retry
        for (SimNode* n : nodes) {
            if (n == &from || !n->powered || n->channel != ch) continue;
            if (rnd() >= loss && rnd() >= burst) deliver(n, t0 + 1 + static_cast<uint32_t>(rnd() * 2));
        }
        if (txDone) at(t0 + 1, [txDone]() { txDone(true); });
        return;
    }
    SimNode* target = nullptr;
    for (SimNode* n : nodes) if (n != &from && memcmp(n->mac, mac, 6) == 0) target = n;
    bool delivered = false, acked = false;
    if (target && target->powered && target->channel == ch && rnd() >= burst) {
        for (int attempt = 0; attempt < 4 && !acked; ++attempt) {
            if (rnd() >= loss) {
                if (!delivered) { delivered = true; deliver(target, t0 + 1 + attempt); }
                if (rnd() >= loss) acked = true;
            }
        }
    }
    if (txDone) at(t0 + (acked ? 2 : 22), [txDone, acked]() { txDone(acked); });
}

void SimNet::advanceTo(uint32_t t) {
    while (!q.empty() && static_cast<int32_t>(q.top().t - t) <= 0) {
        Ev e = q.top();
        q.pop();
        now = e.t;
        e.fn();
    }
    now = t;
}

// --------------------------------------------------------------------------- simulated timer
struct SimTimer : TimerPlatform {
    SimNet& net;
    SimNode node;
    TimerSettings saved;
    uint32_t boots = 0;
    std::unique_ptr<TimerNode> logic;
    std::mt19937 rng;
    int outputEdges = 0;
    bool lastOut = false;
    uint32_t identifyMs = 0;
    int rebootRequests = 0, erases = 0;

    SimTimer(SimNet& n, uint8_t id) : net(n), node(n, id), rng(1000 + id) {
        snprintf(saved.name, sizeof(saved.name), "T%u", id);
    }
    void powerOn() {
        node.powered = true;
        ++boots;
        node.channel = saved.channel;
        logic.reset(new TimerNode(*this));
        logic->begin(net.now, saved, boots);
        node.onRx = [this](const uint8_t* src, int8_t rssi, const uint8_t* d, size_t l) {
            if (logic) logic->onFrame(src, rssi, d, l, net.now);
        };
        lastOut = false;
    }
    void powerOff() { node.powered = false; logic.reset(); }
    void tick() {
        if (!node.powered || !logic) return;
        logic->tick(net.now);
        const bool out = logic->outputOn(net.now);
        if (out != lastOut) { ++outputEdges; lastOut = out; }
    }
    // TimerPlatform
    bool send(const uint8_t* mac, const uint8_t* data, size_t len) override {
        net.send(node, mac, data, len, [this](bool ok) { if (logic) logic->noteTxResult(ok); });
        return true;
    }
    void setChannel(uint8_t ch) override { node.channel = ch; }
    void saveSettings(const TimerSettings& s) override { saved = s; }
    void eraseSettings() override { ++erases; saved = TimerSettings(); }
    void reboot() override { ++rebootRequests; }
    void identify(uint16_t ms) override { identifyMs = ms; }
    void fillInfo(InfoPayload& info) override { info.uptimeS = 1; info.freeHeap = 100000; }
    uint32_t random(uint32_t n) override { return n ? rng() % n : 0; }
};

// --------------------------------------------------------------------------- remote harness
struct Harness : RemotePort, RemoteEvents {
    SimNet net;
    SimNode remoteNode;
    std::vector<std::unique_ptr<SimTimer>> timers;
    std::unique_ptr<RemoteLink> link;
    std::mt19937 rng{777};
    uint8_t homeStored = kDefaultChannel;
    std::vector<JobDone> done;
    std::vector<int> online, offline, rebooted;
    std::vector<std::string> migrateLog;
    LinkTestResult lastLt = {};
    bool ltDone = false;
    bool remoteAwake = true;
    int migCommitted = -1;

    explicit Harness(uint32_t startMs = 100000) : net(), remoteNode(net, 0xE0) {
        net.now = startMs;
        remoteNode.powered = true;
        link.reset(new RemoteLink(*this, this));
        remoteNode.onRx = [this](const uint8_t* src, int8_t rssi, const uint8_t* d, size_t l) {
            if (remoteAwake) link->onFrame(src, rssi, d, l, net.now);
        };
    }
    SimTimer& addTimer(bool power = true, bool pair = true) {
        const uint8_t id = static_cast<uint8_t>(timers.size() + 1);
        timers.emplace_back(new SimTimer(net, id));
        SimTimer& t = *timers.back();
        if (power) t.powerOn();
        if (pair) link->addDevice(t.node.mac, t.saved.name, t.saved.channel);
        return t;
    }
    void begin() { link->begin(remoteNode.mac, kDefaultNet, homeStored, net.now); }

    // RemotePort
    std::map<std::string, int> sentByKind;
    bool refuseSend = false;           // simulate a radio that rejects every frame
    int lastCmdIdSent = -1;
    bool send(const uint8_t* mac, const uint8_t* data, size_t len) override {
        if (remoteNode.txBusy || refuseSend) return false;
        if (data[3] == static_cast<uint8_t>(Type::Cmd)) lastCmdIdSent = data[kHeaderLen];
        ++sentByKind[std::string(typeName(data[3])) + (mac ? "/uni/ch" : "/bcast/ch") + std::to_string(remoteNode.channel)];
        remoteNode.txBusy = true;
        net.send(remoteNode, mac, data, len, [this](bool ok) { remoteNode.txBusy = false; link->onTxResult(ok, net.now); });
        return true;
    }
    void setChannel(uint8_t ch) override { remoteNode.channel = ch; }
    uint8_t channel() const override { return remoteNode.channel; }
    uint32_t random(uint32_t n) override { return n ? const_cast<Harness*>(this)->rng() % n : 0; }
    // RemoteEvents
    void onJobDone(const JobDone& d) override { done.push_back(d); }
    void onOnlineChanged(int dev, bool on) override { (on ? online : offline).push_back(dev); }
    void onRebooted(int dev) override { rebooted.push_back(dev); }
    void onHomeChannel(uint8_t ch) override { homeStored = ch; }
    void onMigrate(uint8_t stage, int dev, bool ok, uint8_t ch, uint8_t okc, uint8_t failc, bool committed) override {
        char b[96]; snprintf(b, sizeof(b), "stage=%u dev=%d ok=%d ch=%u okc=%u fail=%u commit=%d", stage, dev, ok, ch, okc, failc, committed);
        migrateLog.push_back(b);
        if (stage == 2) migCommitted = committed ? 1 : 0;
    }
    void onLinkTest(const LinkTestResult& r) override { lastLt = r; if (r.done) ltDone = true; }

    void run(uint32_t ms) {
        const uint32_t end = net.now + ms;
        while (static_cast<int32_t>(end - net.now) > 0) {
            net.advanceTo(net.now + 1);
            for (auto& t : timers) t->tick();
            if (remoteAwake) link->tick(net.now);
        }
    }
    const JobDone* lastDone(int dev, Op op) const {
        for (auto it = done.rbegin(); it != done.rend(); ++it) if (it->dev == dev && it->op == static_cast<uint8_t>(op)) return &*it;
        return nullptr;
    }
};

// --------------------------------------------------------------------------- tests
static void testProto() {
    std::printf("[proto]\n");
    StatusPayload st = {};
    st.onTenths = 15; st.offTenths = 100; st.channel = 6;
    memcpy(st.name, "Fog1", 4);
    uint8_t buf[kMaxFrame];
    const size_t n = encodeFrame(buf, sizeof(buf), 1, Type::Status, 9, &st, sizeof(st));
    CHECK(n == sizeof(st) + kHeaderLen + kCrcLen, "n=%zu", n);
    FrameView v;
    CHECK(decodeFrame(buf, n, v) == Decode::Ok, "decode");
    CHECK(v.type == Type::Status && v.seq == 9 && v.len == sizeof(st), "fields");
    StatusPayload back;
    CHECK(readPayload(v, back), "readPayload");
    CHECK(back.onTenths == 15 && back.channel == 6 && strcmp(back.name, "Fog1") == 0, "payload");
    // single bit flips must always be caught
    int missed = 0;
    for (size_t i = 0; i < n; ++i) for (int b = 0; b < 8; ++b) {
        uint8_t c[kMaxFrame]; memcpy(c, buf, n); c[i] ^= (1 << b);
        FrameView x; if (decodeFrame(c, n, x) == Decode::Ok) ++missed;
    }
    CHECK(missed == 0, "undetected corruptions: %d", missed);
    CHECK(decodeFrame(buf, n - 1, v) != Decode::Ok, "truncated");
    CHECK(crc16(reinterpret_cast<const uint8_t*>("123456789"), 9) == 0x29B1, "crc16 known answer (CCITT-FALSE)");
    // golden vector for the C# side / docs
    const uint8_t gold[] = {0xF6, 0x02, 0x01, 0x03, 0x2A, 0x04, 0x07, 0x01, 0x00, 0x00};
    uint8_t gf[32];
    const size_t gn = encodeFrame(gf, sizeof(gf), 1, Type::Cmd, 0x2A, gold + 6, 4);
    CHECK(gn == 12, "golden len");
    std::printf("  golden CMD frame:");
    for (size_t i = 0; i < gn; ++i) std::printf(" %02X", gf[i]);
    std::printf("\n");
    char nm[kNameLen];
    sanitizeName(nm, "  Fog\x01 Machine-123456  ", 22);
    CHECK(strcmp(nm, "Fog Machi") == 0, "sanitize '%s'", nm);
}

static void testCycle() {
    std::printf("[cycle]\n");
    CycleEngine e;
    uint32_t t = 0xFFFFF000u;                  // straddle the millis() wrap
    e.begin(t, 10, 20, false);                 // ON 1.0 s, OFF 2.0 s
    int transitions = 0;
    uint32_t firstOn = 0, secondOn = 0;
    bool out = false;
    for (uint32_t i = 0; i < 20000; ++i, ++t) {
        e.update(t);
        const bool o = e.outputOn(t);
        if (o != out) {
            ++transitions;
            if (o) { if (!firstOn) firstOn = i; else if (!secondOn) secondOn = i; }
            out = o;
        }
    }
    CHECK(firstOn == 2000 && secondOn == 5000, "on edges at %u/%u (want 2000/5000 - no drift)", firstOn, secondOn);
    CHECK(transitions >= 12, "transitions=%d", transitions);
    // stalled loop must not flip-flop wildly or lose sync
    e.begin(1000, 10, 20, false);
    e.update(1000 + 60000);
    CHECK(e.elapsedMs(1000 + 60000) <= 3000, "stall resync");
    // force-on lease
    e.begin(0, 10, 20, false);
    CHECK(e.setMode(Mode::ForceOn, 100, 500), "force changes");
    CHECK(!e.setMode(Mode::ForceOn, 300, 500), "refresh is not a change");
    e.update(700); CHECK(e.outputOn(700), "still on (lease refreshed to 800)");
    e.update(900); CHECK(!e.outputOn(900) && e.mode() == Mode::Auto, "lease expired -> auto, output off");
    CHECK(e.elapsedMs(900) < 50, "fresh OFF phase after force");
    // pause freezes and resumes
    e.begin(0, 10, 20, false);
    e.update(500); e.setMode(Mode::Paused, 500, 0);
    CHECK(!e.outputOn(5000), "paused output off");
    e.setMode(Mode::Auto, 5000, 0); e.update(5000);
    CHECK(e.elapsedMs(5000) == 500, "resumed at elapsed %u", e.elapsedMs(5000));
    // toggle
    e.begin(0, 10, 20, false);
    e.toggle(10); CHECK(e.outputOn(10), "toggle -> on");
    e.toggle(20); CHECK(!e.outputOn(20), "toggle -> off");
    e.setMode(Mode::ForceOn, 30, 1000); e.toggle(40); CHECK(!e.outputOn(40), "toggle from force -> off");
    e.setPhase(true, 400, 100); CHECK(e.outputOn(100) && e.elapsedMs(100) == 400, "setPhase");
    // stale now (older than phase start) must not flip the phase
    e.begin(5000, 10, 20, false);
    e.update(4990); CHECK(!e.outputOn(4990), "stale now ok");
}

static void testLossTracker() {
    std::printf("[loss tracker]\n");
    LossTracker lt;
    std::mt19937 rng(5);
    uint8_t seq = 0;
    uint32_t t = 1000;
    for (int i = 0; i < 400; ++i) {
        ++seq; t += 1000;
        if ((rng() % 100) >= 20) lt.onFrame(seq, t);
    }
    const int p = lt.lossPercent();
    CHECK(p >= 5 && p <= 40, "loss%% est=%d (true ~20)", p);
    CHECK(lt.lostTotal() > 40 && lt.lostTotal() < 140, "lost total=%u", lt.lostTotal());
    LossTracker idle;
    idle.onFrame(1, 1000); idle.onFrame(2, 2000);
    idle.onFrame(50, 60000);          // long silence: no phantom loss
    CHECK(idle.lostTotal() == 0, "silence is not loss (%u)", idle.lostTotal());
    RssiFilter f; f.add(-60); f.add(-80); f.add(0); f.add(-127);
    CHECK(f.value() <= -60 && f.value() >= -80, "rssi filter %d", f.value());
}

static void testBasicFleet(double loss, double burst) {
    std::printf("[fleet of 10, loss %.0f%%]\n", loss * 100);
    Harness h;
    h.net.loss = loss;
    h.net.burst = burst;
    for (int i = 0; i < 10; ++i) h.addTimer();
    h.begin();
    h.run(loss > 0.2 ? 4000 : 1500);
    int online = 0;
    for (int i = 0; i < 10; ++i) if (h.link->isOnline(i, h.net.now)) ++online;
    CHECK(online >= (loss > 0.2 ? 9 : 10), "timers seen quickly (got %d)", online);

    for (int i = 0; i < 10; ++i) h.link->submitSetTimes(i, 5 + i, 50 + i);
    h.run(3000);
    int ok = 0, bad = 0;
    for (const JobDone& d : h.done) (d.result == JobResult::Ok ? ok : bad)++;
    CHECK(ok == 10, "all SetTimes confirmed (ok=%d bad=%d)", ok, bad);
    for (int i = 0; i < 10; ++i) {
        const auto& s = h.timers[i]->logic->settings();
        CHECK(s.onTenths == (uint32_t)(5 + i) && s.offTenths == (uint32_t)(50 + i), "timer %d applied (%u/%u)", i, s.onTenths, s.offTenths);
        CHECK(h.timers[i]->saved.onTenths == (uint32_t)(5 + i), "timer %d persisted", i);
    }
    uint32_t maxAtt = 0; double avgAtt = 0;
    for (const JobDone& d : h.done) { maxAtt = std::max<uint32_t>(maxAtt, d.attempts); avgAtt += d.attempts; }
    std::printf("  avg attempts %.2f, max %u, remote tx %u (txFail %u), retries %u\n", avgAtt / h.done.size(), maxAtt,
                h.link->counters().tx, h.link->counters().txFail, h.link->counters().retries);
    // steady-state traffic: heartbeats only, nothing from the remote
    const uint32_t tx0 = h.link->counters().tx;
    h.run(10000);
    CHECK(h.link->counters().tx - tx0 <= 2, "idle remote is silent (sent %u frames in 10 s)", h.link->counters().tx - tx0);
    if (h.link->counters().tx - tx0 > 2) for (auto& kv : h.sentByKind) std::printf("    sent %-24s %d\n", kv.first.c_str(), kv.second);
}

static void testNoDoubleExecute() {
    std::printf("[idempotency: toggle under 40%% loss]\n");
    Harness h;
    h.net.loss = 0.40;
    h.addTimer();
    h.begin();
    h.run(2500);
    int submitted = 60;
    for (int i = 0; i < submitted; ++i) { h.link->submitToggle(0); h.run(3500); }
    int ok = 0, timeout = 0;
    for (const JobDone& d : h.done) { if (d.result == JobResult::Ok) ++ok; else if (d.result == JobResult::Timeout) ++timeout; }
    const TimerNode& n = *h.timers[0]->logic;
    std::printf("  jobs ok=%d timeout=%d  timer: run=%u dup=%u rxCmds=%u\n", ok, timeout, n.cmdsRun, n.cmdsDup, n.rxCmds);
    CHECK(n.cmdsRun <= (uint32_t)submitted, "never executes more commands than were submitted (%u)", n.cmdsRun);
    CHECK(n.cmdsRun >= (uint32_t)ok, "every confirmed toggle ran (%u >= %d)", n.cmdsRun, ok);
    CHECK(n.cmdsRun + n.cmdsDup == n.rxCmds, "run+dup == received");
    CHECK(n.cmdsDup > 0, "duplicates did occur and were suppressed");
}

static void testPowerCut() {
    std::printf("[power cut while heating]\n");
    Harness h;
    h.net.loss = 0.05;
    h.addTimer(); h.addTimer();
    h.begin();
    h.run(2000);
    CHECK(h.link->isOnline(0, h.net.now) && h.link->isOnline(1, h.net.now), "both online");
    h.timers[0]->powerOff();
    h.run(4500);
    CHECK(!h.link->isOnline(0, h.net.now), "offline after power cut");
    CHECK(!h.offline.empty() && h.offline.back() == 0, "offline event");
    // A command sent now must expire, and must NOT run when the timer comes back.
    const uint32_t before = h.timers[0]->saved.onTenths;
    h.link->submitSetTimes(0, 99, 99);
    h.link->submitToggle(0);
    h.run(3000);
    const JobDone* d = h.lastDone(0, Op::Toggle);
    CHECK(d && d->result == JobResult::Timeout, "toggle to dead timer times out");
    CHECK(h.link->queuedJobs() == 0, "queue drained (stale commands discarded)");
    h.timers[0]->powerOn();
    h.run(2000);
    CHECK(h.link->isOnline(0, h.net.now), "back online within 2 s of power-up");
    CHECK(!h.rebooted.empty(), "reboot noticed");
    CHECK(h.timers[0]->logic->settings().onTenths == before, "stale SetTimes did not run late");
    CHECK(h.timers[0]->logic->cmdsRun == 0, "stale commands did not run late");
    CHECK(h.link->device(0).st.bootCount == 2, "boot counter visible (%u)", h.link->device(0).st.bootCount);
    // many power cycles in a row (thermostat chattering)
    for (int i = 0; i < 20; ++i) { h.timers[1]->powerOff(); h.run(300); h.timers[1]->powerOn(); h.run(700); }
    CHECK(h.link->isOnline(1, h.net.now), "survives rapid power cycling");
}

static void testForceLease() {
    std::printf("[dead-man lease]\n");
    Harness h;
    h.net.loss = 0.30;
    SimTimer& t = h.addTimer();
    h.begin();
    h.run(2500);
    h.link->submitMode(0, Mode::ForceOn, 1000, true);
    h.run(500);
    CHECK(t.logic->outputOn(h.net.now), "forced on");
    int offSamples = 0;
    for (int i = 0; i < 20; ++i) {   // button held: refresh every 300 ms
        h.link->submitMode(0, Mode::ForceOn, 1000, false);
        for (int k = 0; k < 3; ++k) { h.run(100); if (!t.logic->outputOn(h.net.now)) ++offSamples; }
    }
    CHECK(offSamples == 0, "held output never drops under 30%% loss (%d dropouts)", offSamples);
    // remote vanishes (battery died / asleep): output must fall back by itself
    h.remoteAwake = false;
    h.run(1300);
    CHECK(!t.logic->outputOn(h.net.now), "output off after lease expiry without remote");
    CHECK(t.logic->engine().mode() == Mode::Auto, "back to auto");
}

static void testWakeAndDiscovery() {
    std::printf("[remote wake-up]\n");
    Harness h;
    h.net.loss = 0.1;
    for (int i = 0; i < 6; ++i) h.addTimer();
    h.remoteAwake = false;
    h.run(60000);           // remote asleep for a minute
    h.remoteAwake = true;
    h.begin();
    h.run(500);
    int online = 0;
    for (int i = 0; i < 6; ++i) if (h.link->isOnline(i, h.net.now)) ++online;
    CHECK(online >= 5, "fleet picture within 0.5 s of waking (%d/6)", online);
}

static void testMigrationAndRecovery() {
    std::printf("[channel migration + recovery]\n");
    Harness h;
    h.net.loss = 0.05;
    for (int i = 0; i < 4; ++i) h.addTimer();
    h.begin();
    h.run(2000);
    h.timers[3]->powerOff();                       // one machine is heating
    h.run(4000);
    CHECK(h.link->startMigrate(11, false), "migrate started");
    h.run(6000);
    CHECK(h.migCommitted == 1, "migration committed (%s)", h.migrateLog.empty() ? "no log" : h.migrateLog.back().c_str());
    CHECK(h.homeStored == 11 && h.remoteNode.channel == 11, "remote followed to ch 11");
    for (int i = 0; i < 3; ++i) CHECK(h.timers[i]->node.channel == 11 && h.timers[i]->saved.channel == 11, "timer %d on ch 11 and persisted", i);
    for (int i = 0; i < 3; ++i) CHECK(h.link->isOnline(i, h.net.now), "timer %d online on new channel", i);
    // the late one powers up on the OLD channel and must be fetched home automatically
    h.timers[3]->powerOn();
    CHECK(h.timers[3]->node.channel == 1, "late timer boots on old channel");
    h.run(30000);
    CHECK(h.timers[3]->node.channel == 11, "recovery brought it to ch 11 (is %u)", h.timers[3]->node.channel);
    CHECK(h.link->isOnline(3, h.net.now), "late timer online again");
    CHECK(h.timers[3]->saved.channel == 11, "and persisted there");
    CHECK(h.link->counters().found >= 1, "found counter");

    // migration to an unreachable fleet must not strand the remote
    Harness g;
    g.addTimer();
    g.begin();
    g.run(2000);
    g.timers[0]->powerOff();
    g.run(5000);
    CHECK(g.link->startMigrate(9, false), "migrate accepted");
    g.run(2000);
    CHECK(g.migCommitted == 0 && g.remoteNode.channel == 1, "no timer confirmed -> stay on channel 1 (ch=%u commit=%d)", g.remoteNode.channel, g.migCommitted);
}

static void testLinkTestAndSweepDiscovery() {
    std::printf("[link test + sweep discovery]\n");
    Harness h;
    h.net.loss = 0.15;
    h.addTimer();
    SimTimer& stray = h.addTimer(true, false);    // unpaired, parked on channel 9
    stray.saved.channel = 9; stray.powerOff(); stray.powerOn();
    h.begin();
    h.run(2000);
    CHECK(h.link->startLinkTest(0, 40, 40), "link test started");
    h.run(4000);
    CHECK(h.ltDone, "link test finished");
    std::printf("  sent %u macAck %u echo %u rttMax %u\n", h.lastLt.sent, h.lastLt.macAck, h.lastLt.echo, h.lastLt.rttMax);
    CHECK(h.lastLt.sent == 40 && h.lastLt.macAck >= 25 && h.lastLt.echo >= 20, "plausible results under 15%% loss");
    h.link->startDiscovery(true);
    h.run(9000);
    bool sawStray = false;
    for (int i = 0; i < RemoteLink::kMaxHeard; ++i) {
        const Heard& e = h.link->heard(i);
        if (e.used && memcmp(e.mac, stray.node.mac, 6) == 0 && e.channel == 9) sawStray = true;
    }
    CHECK(sawStray, "sweep found the timer on channel 9");
    CHECK(h.remoteNode.channel == 1, "sweep returned home");
    h.link->stopDiscovery();
    h.run(500);
}

static void testClockWrap() {
    std::printf("[millis() wrap]\n");
    Harness h(0xFFFFFFFFu - 4000);
    h.net.loss = 0.1;
    h.addTimer(); h.addTimer();
    h.begin();
    h.run(2000);
    h.link->submitSetTimes(0, 7, 70);
    h.run(2000);                                   // crosses the wrap
    const JobDone* d = h.lastDone(0, Op::SetTimes);
    CHECK(d && d->result == JobResult::Ok, "command across the wrap");
    CHECK(h.link->isOnline(1, h.net.now), "still online after wrap");
    h.run(6000);
    CHECK(h.link->isOnline(0, h.net.now) && h.link->isOnline(1, h.net.now), "online 6 s after wrap");
}

static void testRejectAndFactory() {
    std::printf("[rejects / factory reset / names]\n");
    Harness h;
    SimTimer& t = h.addTimer();
    h.begin();
    h.run(1500);
    h.link->submitSetTimes(0, 0, 100);             // 0 tenths is invalid
    h.run(500);
    const JobDone* d = h.lastDone(0, Op::SetTimes);
    CHECK(d && d->result == JobResult::Rejected, "invalid times rejected");
    CHECK(t.saved.onTenths == kDefaultOnTenths, "nothing stored");
    h.link->submitName(0, "  Stage Left  ");
    h.run(500);
    CHECK(strcmp(t.logic->settings().name, "Stage Lef") == 0, "name sanitised/truncated '%s'", t.logic->settings().name);
    CHECK(strcmp(h.link->device(0).name, "Stage Lef") == 0, "remote list adopts the name '%s'", h.link->device(0).name);
    h.link->submitFactoryReset(0);
    h.run(600);
    CHECK(t.erases == 1 && strcmp(t.logic->settings().name, "FogTimer") == 0, "factory reset");
    h.link->submitReboot(0);
    h.run(800);
    CHECK(t.rebootRequests == 1, "reboot requested once");
    h.link->submit(0, Op::Reboot, "x", 1);          // wrong magic
    h.run(600);
    CHECK(t.rebootRequests == 1, "reboot without magic ignored");
}

static void testManyJobsOrdering() {
    std::printf("[per-device ordering + coalescing]\n");
    Harness h;
    h.net.loss = 0.2;
    h.addTimer();
    h.begin();
    h.run(2000);
    h.link->submitMode(0, Mode::Paused, 0, true);
    h.link->submitMode(0, Mode::Auto, 0, true);          // newer mode replaces the not-yet-sent pause
    h.link->submitSetTimes(0, 11, 22);
    h.link->submitSetTimes(0, 33, 44);                   // newer replaces older
    h.run(3000);
    CHECK(h.timers[0]->logic->settings().onTenths == 33, "newest SetTimes wins (%u)", h.timers[0]->logic->settings().onTenths);
    bool cancelled = false;
    for (const JobDone& d : h.done) if (d.result == JobResult::Cancelled) cancelled = true;
    CHECK(cancelled, "superseded jobs reported as cancelled");
}


static void testSoak() {
    std::printf("[soak: 10 timers, 10 minutes, random power cuts + commands + a channel move]\n");
    Harness h;
    h.net.loss = 0.12;
    h.net.burst = 0.12;
    for (int i = 0; i < 10; ++i) h.addTimer();
    h.begin();
    std::mt19937 rng(99);
    std::vector<uint32_t> nextEvent(10), lastPowerEvent(10, 0);
    for (int i = 0; i < 10; ++i) nextEvent[i] = h.net.now + 15000 + rng() % 60000;
    struct Meta { int dev; uint32_t at; };
    std::map<uint16_t, Meta> jobs;
    int eligibleOk = 0, eligibleBad = 0, maxQueue = 0;
    uint32_t nextCmd = h.net.now + 2000;
    bool migrated = false;
    const uint32_t start = h.net.now;
    for (uint32_t step = 0; step < 600; ++step) {                 // 600 x 1 s
        for (int i = 0; i < 10; ++i) {
            if (static_cast<int32_t>(h.net.now - nextEvent[i]) < 0) continue;
            SimTimer& t = *h.timers[i];
            if (t.node.powered) { t.powerOff(); nextEvent[i] = h.net.now + 10000 + rng() % 30000; }
            else { t.powerOn(); nextEvent[i] = h.net.now + 20000 + rng() % 70000; }
            lastPowerEvent[i] = h.net.now;
        }
        if (!migrated && h.net.now - start > 300000) { migrated = true; h.link->startMigrate(11, false); }
        if (static_cast<int32_t>(h.net.now - nextCmd) >= 0) {
            const int dev = rng() % 10;
            int id = -1;
            switch (rng() % 4) {
                case 0: id = h.link->submitToggle(dev); break;
                case 1: id = h.link->submitSetTimes(dev, 5 + rng() % 50, 20 + rng() % 200); break;
                case 2: id = h.link->submitMode(dev, Mode::Auto, 0, true); break;
                default: id = h.link->submitName(dev, "Soak"); break;
            }
            if (id > 0) jobs[static_cast<uint16_t>(id)] = {dev, h.net.now};
            nextCmd = h.net.now + 500 + rng() % 2500;
        }
        const size_t before = h.done.size();
        h.run(1000);
        maxQueue = std::max(maxQueue, h.link->queuedJobs());
        for (size_t k = before; k < h.done.size(); ++k) {
            const JobDone& d = h.done[k];
            auto it = jobs.find(d.jobId);
            if (it == jobs.end() || d.tag != 0 || h.migCommitted == 0) continue;
            const Meta m = it->second;
            const bool eligible = h.timers[m.dev]->node.powered && lastPowerEvent[m.dev] + 1000 < m.at && h.net.now - m.at > 0 &&
                                  h.remoteNode.channel == h.timers[m.dev]->node.channel;
            if (eligible && d.result == JobResult::Ok) ++eligibleOk;
            else if (eligible && d.result == JobResult::Timeout) ++eligibleBad;
        }
    }
    std::printf("  commands to healthy timers: %d ok, %d timed out; max queue %d; migration committed=%d; remote on ch %u\n",
                eligibleOk, eligibleBad, maxQueue, h.migCommitted, h.remoteNode.channel);
    CHECK(eligibleOk > 100, "enough samples");
    CHECK(eligibleBad * 100 <= (eligibleOk + eligibleBad) * 6, "<= 6%% of commands to healthy timers fail under 12%%+12%% loss (%d/%d)", eligibleBad, eligibleOk + eligibleBad);
    CHECK(maxQueue <= RemoteLink::kMaxJobs, "queue bounded");
    // end of show: everything powered, remote stays: all timers must come back on the (possibly new) home channel
    for (auto& t : h.timers) if (!t->node.powered) t->powerOn();
    h.net.loss = 0.05; h.net.burst = 0.02;
    h.run(60000);
    int online = 0;
    for (int i = 0; i < 10; ++i) if (h.link->isOnline(i, h.net.now)) ++online;
    CHECK(online == 10, "all timers back after the soak (online %d/10, remote ch %u)", online, h.remoteNode.channel);
    for (int i = 0; i < 10; ++i) {
        const TimerNode& n = *h.timers[i]->logic;
        CHECK(n.cmdsRun + n.cmdsDup == n.rxCmds, "timer %d: run+dup == received", i);
        CHECK(h.timers[i]->node.channel == h.remoteNode.channel, "timer %d on the home channel (%u vs %u)", i, h.timers[i]->node.channel, h.remoteNode.channel);
    }
    CHECK(h.link->queuedJobs() == 0, "nothing left in the queue");
}

static void testUtil() {
    std::printf("[util]\n");
    uint8_t mac[6];
    CHECK(parseMac("24:6F:28:00:01:fF", mac) && mac[0] == 0x24 && mac[5] == 0xFF, "parseMac");
    CHECK(!parseMac("24:6F:28:00:01", mac) && !parseMac("24:6F:28:00:01:ZZ", mac), "bad mac rejected");
    char s[18];
    macToStr(mac, s);
    CHECK(strcmp(s, "24:6F:28:00:01:FF") == 0, "macToStr %s", s);
    CHECK(secondsToTenths(1.5) == 15 && secondsToTenths(0.1) == 1 && secondsToTenths(9999.9) == 99999 && secondsToTenths(-1) == 0, "seconds to tenths");
}

static void testReplayAfterPowerCut() {
    std::puts("[replay of a retried Toggle after a power cut]");
    Harness h;
    SimTimer& t = h.addTimer();
    h.begin();
    h.run(2000);
    CHECK(!t.logic->outputOn(h.net.now), "starts with the output off");
    h.link->submitToggle(0);
    // The timer executes the toggle, then loses power before its echo leaves; power returns within the job budget.
    auto orig = t.node.onRx;
    bool killed = false;
    uint32_t restoreAt = 0;
    t.node.onRx = [&](const uint8_t* src, int8_t rssi, const uint8_t* d, size_t l) {
        orig(src, rssi, d, l);
        if (!killed && t.logic && t.logic->cmdsRun >= 1) { killed = true; t.powerOff(); restoreAt = h.net.now + 1200; }
    };
    for (int i = 0; i < 40; ++i) {
        h.run(100);
        if (!t.node.powered && restoreAt && h.net.now >= restoreAt) t.powerOn();
    }
    CHECK(killed, "the toggle reached the timer first");
    CHECK(t.node.powered && t.logic, "timer is back");
    CHECK(t.logic->cmdsRun == 0, "the replayed Toggle was NOT executed after the reboot (cmdsRun=%u)", t.logic->cmdsRun);
    CHECK(!t.logic->outputOn(h.net.now), "output stayed OFF after power-up");
    const JobDone* d = h.lastDone(0, Op::Toggle);
    CHECK(d && d->result == JobResult::Rejected, "remote is told the command is stale (result %d)", d ? static_cast<int>(d->result) : -1);
}

static void testHeartbeatIsNotAnEcho() {
    std::puts("[a heartbeat repeating an old echo does not confirm a command]");
    Harness h;
    SimTimer& t = h.addTimer();
    h.begin();
    h.run(1500);
    t.powerOff();
    h.link->submitSetTimes(0, 7, 70);
    h.run(300);
    CHECK(h.lastCmdIdSent > 0, "a command was transmitted");
    StatusPayload st = {};
    t.logic.reset(new TimerNode(t));
    t.logic->buildStatus(st, h.net.now, ST_HEARTBEAT);
    st.lastCmdId = static_cast<uint8_t>(h.lastCmdIdSent);
    st.lastCmdSrc = macTag(h.remoteNode.mac);
    uint8_t buf[kMaxFrame];
    size_t n = encodeFrame(buf, sizeof(buf), kDefaultNet, Type::Status, 1, &st, sizeof(st));
    h.link->onFrame(t.node.mac, -60, buf, n, h.net.now);
    CHECK(h.lastDone(0, Op::SetTimes) == nullptr, "heartbeat with a matching old id does not complete the job");
    st.flags = ST_REPLY;
    n = encodeFrame(buf, sizeof(buf), kDefaultNet, Type::Status, 2, &st, sizeof(st));
    h.link->onFrame(t.node.mac, -60, buf, n, h.net.now);
    CHECK(h.lastDone(0, Op::SetTimes) && h.lastDone(0, Op::SetTimes)->result == JobResult::Ok, "a real reply does");
}

static void testExcursionWatchdog() {
    std::puts("[an excursion can never strand the remote away from home]");
    Harness h;
    h.addTimer();
    h.begin();
    h.run(2000);
    h.timers[0]->powerOff();
    h.run(6000);
    h.link->startRecover(0);
    bool away = false;
    for (int i = 0; i < 300 && !away; ++i) { h.run(10); away = h.remoteNode.channel != 1; }
    CHECK(away, "the recovery sweep left the home channel");
    h.refuseSend = true;                       // the radio now refuses every frame while we are parked elsewhere
    h.run(20000);
    CHECK(h.remoteNode.channel == 1 && !h.link->busyExcursion(), "back on the home channel (ch %u busy=%d)", h.remoteNode.channel, h.link->busyExcursion());
    h.refuseSend = false;
    h.timers[0]->powerOn();
    h.run(4000);
    CHECK(h.link->isOnline(0, h.net.now), "and everything works again afterwards");
}

static void testRemoveDuringActivity() {
    std::puts("[removing a timer while it is being tested / searched]");
    Harness h;
    h.addTimer(); h.addTimer(); h.addTimer();
    h.begin();
    h.run(2000);
    CHECK(h.link->startLinkTest(1, 30, 40), "link test started");
    h.run(300);
    h.link->removeDevice(1);
    h.run(500);
    CHECK(h.ltDone, "the cancelled link test still reports its end");
    CHECK(h.link->deviceCount() == 2 && !h.link->linkTesting(), "list shrank, test stopped");
    h.timers[0]->powerOff();
    h.run(6000);
    h.link->startRecover(0);
    for (int i = 0; i < 300 && h.remoteNode.channel == 1; ++i) h.run(10);
    CHECK(h.remoteNode.channel != 1, "sweep running");
    h.link->removeDevice(0);
    h.run(100);
    CHECK(h.remoteNode.channel == 1 && !h.link->busyExcursion(), "removing the target ends the sweep and returns home");
}

static void testStaleClockWhileSurveying() {
    std::puts("[a command submitted while the link is not ticking (channel survey)]");
    Harness h;
    h.addTimer();
    h.begin();
    h.run(2000);
    h.remoteAwake = false;                     // the survey owns the radio for ~3 s: link.tick() is not called
    h.run(3000);
    h.link->setNow(h.net.now);                 // what Fleet::loop does every pass
    h.link->submitToggle(0);
    h.remoteAwake = true;
    h.run(600);
    const JobDone* d = h.lastDone(0, Op::Toggle);
    CHECK(d && d->result == JobResult::Ok, "the command gets its full time budget (result %d)", d ? static_cast<int>(d->result) : -1);
}

int main() {
    testProto();
    testCycle();
    testLossTracker();
    testBasicFleet(0.0, 0.0);
    testBasicFleet(0.10, 0.10);
    testBasicFleet(0.30, 0.30);
    testNoDoubleExecute();
    testPowerCut();
    testForceLease();
    testWakeAndDiscovery();
    testMigrationAndRecovery();
    testLinkTestAndSweepDiscovery();
    testClockWrap();
    testRejectAndFactory();
    testManyJobsOrdering();
    testReplayAfterPowerCut();
    testHeartbeatIsNotAnEcho();
    testExcursionWatchdog();
    testRemoveDuringActivity();
    testStaleClockWhileSurveying();
    testUtil();
    testSoak();
    std::printf("\n%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
