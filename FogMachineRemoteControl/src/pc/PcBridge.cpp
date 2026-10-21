#include "pc/PcBridge.h"

#include <esp_system.h>
#include "FogBase64.h"
#include "FogJsonUtil.h"
#include "FogUtil.h"

using namespace Fog;

namespace {
const char* kBuildStamp = __DATE__ " " __TIME__;

const char* opName(uint8_t op) {
    switch (static_cast<Op>(op)) {
        case Op::Nop: return "ping";
        case Op::SetTimes: return "setTimes";
        case Op::SetMode: return "mode";
        case Op::Toggle: return "toggle";
        case Op::Reset: return "reset";
        case Op::SetPhase: return "setPhase";
        case Op::SetName: return "name";
        case Op::SetChannel: return "channel";
        case Op::FactoryReset: return "factoryReset";
        case Op::Identify: return "identify";
        case Op::Reboot: return "reboot";
        case Op::SetStartup: return "startPhase";
        case Op::GetInfo: return "info";
    }
    return "?";
}

const char* modeName(uint8_t m) {
    return m == 1 ? "forceOn" : (m == 2 ? "paused" : "auto");
}

const char* resultName(JobResult r) {
    switch (r) {
        case JobResult::Ok: return "ok";
        case JobResult::Rejected: return "rejected";
        case JobResult::Timeout: return "timeout";
        default: return "cancelled";
    }
}
}  // namespace

void PcBridge::begin(const Context& ctx) {
    c_ = ctx;
    json_.begin(Serial, [this](JsonDocument& doc) { handle(doc, millis()); });
    c_.fleet->tap = [this](bool tx, const uint8_t* mac, int8_t rssi, const uint8_t* data, size_t len) { tapFrame(tx, mac, rssi, data, len); };
    c_.fleet->setSink(this);
}

void PcBridge::loop(uint32_t nowMs) {
    json_.poll(nowMs);
    drainLog(nowMs);
    if (rebootAt_ && static_cast<int32_t>(nowMs - rebootAt_) >= 0) ESP.restart();
    if (factoryDeadline_ && (static_cast<int32_t>(nowMs - factoryDeadline_) >= 0 || c_.fleet->link().queuedJobs() == 0)) {
        factoryDeadline_ = 0;
        c_.fleet->wipeLocalData();
        c_.settings->resetToDefaults();
        rebootAt_ = nowMs + 300;
    }
    if (!json_.pcPresent(nowMs)) { surveyWasActive_ = false; return; }

    // survey progress -> results
    const bool surveyActive = c_.fleet->survey().active();
    if (surveyWasActive_ && !surveyActive) sendSurveyResults();
    surveyWasActive_ = surveyActive;

    if (fleetDirty_ && nowMs - lastFleetSent_ >= 150) sendFleet(nowMs);
    else if (nowMs - lastFleetMs_ >= 500) sendFleet(nowMs);
    if (nowMs - lastRemoteMs_ >= 1000) { lastRemoteMs_ = nowMs; sendRemote(nowMs); }
    if (statsEveryMs_) {
        pollInfo(nowMs);
        if (nowMs - lastStatsMs_ >= statsEveryMs_) { lastStatsMs_ = nowMs; sendStats(nowMs); sendDevStats(nowMs); }
    }
    expireStats(nowMs);
}

// ------------------------------------------------------------------ builders
void PcBridge::fillSettings(JsonObject o) const {
    const RemoteSettingsData& s = c_.settings->get();
    o["txPower"] = s.txPowerQdbm;
    o["brightness"] = s.oledBrightness;
    o["autoOff"] = s.autoOffSeconds;
    o["rssiLow"] = s.rssiLowDbm;
    o["rssiHigh"] = s.rssiHighDbm;
    JsonArray cal = o["batCal"].to<JsonArray>();
    for (int i = 0; i < 3; ++i) cal.add(s.batCalMv[i]);
    o["net"] = s.net;
    o["ch"] = s.channel;
}

void PcBridge::sendFleet(uint32_t nowMs) {
    fleetDirty_ = false;
    lastFleetMs_ = lastFleetSent_ = nowMs;
    JsonDocument d;
    d["t"] = "fleet";
    d["seq"] = ++seq_;
    d["active"] = c_.fleet->getActiveIndex();
    d["ch"] = c_.fleet->homeChannel();
    JsonArray arr = d["devs"].to<JsonArray>();
    for (int i = 0; i < c_.fleet->getPairedCount(); ++i) {
        const SlaveDevice& v = c_.fleet->getPaired(i);
        const Device& dev = c_.fleet->link().device(i);
        JsonObject o = arr.add<JsonObject>();
        o["i"] = i;
        jsonMac(o, "mac", v.mac);
        o["name"] = v.name;
        o["online"] = v.online ? 1 : 0;
        o["on"] = v.outputState ? 1 : 0;
        o["ph"] = v.phaseOn ? "on" : "off";
        o["mode"] = modeName(v.mode);
        o["ton"] = v.ton;
        o["toff"] = v.toff;
        o["el"] = v.elapsed;
        o["rssi"] = v.rssiRemote;
        o["rssiT"] = v.rssiSlave;
        o["age"] = v.ageMs == 0xFFFFFFFFu ? -1 : static_cast<int32_t>(v.ageMs);
        o["loss"] = v.lossPct;
        o["ch"] = v.channel;
        o["boot"] = dev.statusValid ? dev.st.bootCount : 0;
        o["up"] = dev.statusValid ? dev.st.uptimeS + (nowMs - dev.statusMs) / 1000 : 0;
        char fw[8];
        snprintf(fw, sizeof(fw), "%u.%u", dev.st.fwMajor, dev.st.fwMinor);
        o["fw"] = dev.statusValid ? fw : "";
        o["pend"] = v.pending;
        o["id"] = i;
    }
    json_.send(d, false);
}

void PcBridge::sendRemote(uint32_t nowMs) {
    JsonDocument d;
    d["t"] = "remote";
    d["bat"] = c_.battery->percent();
    d["mv"] = c_.battery->millivolts();
    d["chg"] = c_.battery->charging() ? 1 : 0;
    d["usb"] = c_.battery->usbPower() ? 1 : 0;
    d["ch"] = c_.fleet->homeChannel();
    d["net"] = c_.settings->get().net;
    d["up"] = nowMs / 1000;
    d["heap"] = ESP.getFreeHeap();
    d["menu"] = menu_;
    d["blank"] = blank_ ? 1 : 0;
    d["scan"] = (c_.fleet->isDiscovering() || c_.fleet->survey().active()) ? 1 : 0;
    d["txq"] = c_.fleet->link().queuedJobs();
    json_.send(d, false);
}

void PcBridge::fillSelfStats(JsonObject self, uint32_t nowMs) {
    RemoteLink& link = c_.fleet->link();
    const RemoteCounters& rc = link.counters();
    const FogRadio::Stats& rs = c_.fleet->radio().stats();
    self["kind"] = "remote";
    self["up"] = nowMs / 1000;
    self["boots"] = c_.life->boots;
    self["heap"] = ESP.getFreeHeap();
    self["heapMin"] = ESP.getMinFreeHeap();
    self["loopMaxUs"] = c_.meter->maxUs();
    self["loopsPerS"] = c_.meter->loopsPerSecond();
    self["rstReason"] = resetReasonName(c_.resetReason);
    jsonRst(self["rst"].to<JsonObject>(), *c_.life);
    JsonObject radio = self["radio"].to<JsonObject>();
    radio["ch"] = c_.fleet->radio().channel();
    radio["tx"] = rc.tx;
    radio["txOk"] = rc.txOk;
    radio["txFail"] = rc.txFail;
    radio["txWd"] = rs.txWd;
    radio["txDrop"] = rs.txDrop;
    radio["rx"] = rc.rx;
    radio["rxBad"] = rc.rxBad;
    radio["rxForeign"] = rc.rxForeign;
    radio["rxDrop"] = rs.rxDrop;
    JsonArray chan = self["chan"].to<JsonArray>();
    for (uint8_t ch = kMinChannel; ch <= kMaxChannel; ++ch) {
        if (!rc.chTx[ch] && !rc.chRx[ch]) continue;
        JsonObject o = chan.add<JsonObject>();
        o["ch"] = ch;
        o["tx"] = rc.chTx[ch];
        o["fail"] = rc.chFail[ch];
        o["rx"] = rc.chRx[ch];
    }
    JsonObject lk = self["link"].to<JsonObject>();
    lk["cmds"] = rc.cmds;
    lk["ok"] = rc.cmdsOk;
    lk["timeout"] = rc.cmdsTimeout;
    lk["rejected"] = rc.cmdsRejected;
    lk["retries"] = rc.retries;
    lk["rttAvg"] = rc.rttCount ? static_cast<double>(rc.rttSumMs) / rc.rttCount : 0.0;
    lk["rttMax"] = rc.rttMax;
    lk["sweeps"] = rc.sweeps;
    lk["recovers"] = rc.recovers;
    lk["found"] = rc.found;
    lk["migrations"] = rc.migrations;
    JsonObject pc = self["pc"].to<JsonObject>();
    pc["linesRx"] = json_.linesRx;
    pc["linesTx"] = json_.linesTx;
    pc["badJson"] = json_.badJson;
    pc["txDrop"] = json_.txDrop;
}

void PcBridge::sendStats(uint32_t nowMs) {
    JsonDocument d;
    d["t"] = "stats";
    d["ms"] = nowMs;
    fillSelfStats(d["self"].to<JsonObject>(), nowMs);
    json_.send(d, false);
}

void PcBridge::fillDevStat(JsonObject o, int dev, uint32_t nowMs) {
    const Device& dv = c_.fleet->link().device(dev);
    o["ms"] = nowMs;
    o["i"] = dev;
    jsonMac(o, "mac", dv.mac);
    const DevLinkStats& s = dv.stats;
    JsonObject l = o["link"].to<JsonObject>();
    l["rx"] = dv.loss.rxTotal();
    l["expected"] = dv.loss.rxTotal() + dv.loss.lostTotal();
    l["lost"] = dv.loss.lostTotal();
    l["loss"] = dv.loss.lossPercent();
    l["dups"] = dv.loss.dupTotal();
    l["macOk"] = s.macOk;
    l["macFail"] = s.macFail;
    l["cmds"] = s.cmds;
    l["cmdsOk"] = s.cmdsOk;
    l["cmdsTimeout"] = s.cmdsTimeout;
    l["retries"] = s.retries;
    l["rttAvg"] = s.rttCount ? static_cast<double>(s.rttSumMs) / s.rttCount : 0.0;
    l["rttMax"] = s.rttMax;
    l["rssiMin"] = s.rssiMin;
    l["rssiAvg"] = s.rssiCount ? static_cast<int>(s.rssiSum / static_cast<int32_t>(s.rssiCount)) : 0;
    l["rssiMax"] = s.rssiMax;
    l["offlineEvents"] = s.offlineEvents;
    uint32_t offlineMs = s.offlineMs;
    if (!dv.wasOnline && dv.offlineSince) offlineMs += nowMs - dv.offlineSince;     // include the current outage
    l["offlineMs"] = offlineMs;
    l["reboots"] = s.reboots;
    if (dv.infoValid) {
        JsonObject t = o["timer"].to<JsonObject>();
        t["age"] = nowMs - dv.infoMs;
        jsonTimerInfo(t, dv.info);
    } else {
        o["timer"] = nullptr;
    }
}

void PcBridge::sendDevStats(uint32_t nowMs, int only) {
    for (int i = 0; i < c_.fleet->getPairedCount(); ++i) {
        if (only >= 0 && i != only) continue;
        JsonDocument d;
        d["t"] = "devStat";
        fillDevStat(d.as<JsonObject>(), i, nowMs);
        json_.send(d, false);
    }
}

void PcBridge::pollInfo(uint32_t nowMs) {
    if (nowMs - lastInfoMs_ < 1000) return;
    lastInfoMs_ = nowMs;
    const int n = c_.fleet->getPairedCount();
    for (int k = 0; k < n; ++k) {
        const int i = (infoRr_ + k) % n;
        if (!c_.fleet->link().isOnline(i, nowMs)) continue;
        c_.fleet->link().submitGetInfo(i);
        infoRr_ = i + 1;
        return;
    }
}

void PcBridge::expireStats(uint32_t nowMs) {
    for (PendingStat& p : pendStat_) {
        if (!p.used || static_cast<int32_t>(nowMs - p.deadline) < 0) continue;
        p.used = false;
        JsonDocument r;
        jsonResult(r, p.req, true);
        if (p.dev >= 0 && p.dev < c_.fleet->getPairedCount()) fillDevStat(r["devStat"].to<JsonObject>(), p.dev, nowMs);
        json_.send(r, true);
    }
}

void PcBridge::onInfo(int dev) {
    fleetDirty_ = fleetDirty_;     // info does not change the fleet picture
    const uint32_t now = millis();
    for (PendingStat& p : pendStat_) {
        if (!p.used || p.dev != dev) continue;
        p.used = false;
        JsonDocument r;
        jsonResult(r, p.req, true);
        fillDevStat(r["devStat"].to<JsonObject>(), dev, now);
        json_.send(r, true);
    }
}

void PcBridge::sendSurveyResults() {
    ChannelSurvey& sv = c_.fleet->survey();
    if (sv.state() != ChannelSurvey::State::Done) {
        JsonDocument f;
        f["t"] = "surveyDone";
        f["failed"] = true;
        f["cur"] = c_.fleet->homeChannel();
        f["best"].to<JsonArray>();
        json_.send(f, true);
        return;
    }
    for (uint8_t ch = 1; ch <= 13; ++ch) {
        const ChannelSurvey::Channel& c = sv.channel(ch);
        JsonDocument d;
        d["t"] = "surveyCh";
        d["ch"] = ch;
        d["aps"] = c.aps;
        d["apRssi"] = c.apRssi;
        d["frames"] = c.frames;
        d["avgRssi"] = c.avgRssi;
        d["score"] = c.score;
        json_.send(d, true);
    }
    JsonDocument done;
    done["t"] = "surveyDone";
    JsonArray best = done["best"].to<JsonArray>();
    for (uint8_t i = 0; i < sv.bestCount(); ++i) best.add(sv.best(i));
    done["cur"] = c_.fleet->homeChannel();
    json_.send(done, true);
}

// ------------------------------------------------------------------ log / sniff
void PcBridge::drainLog(uint32_t nowMs) {
    LogRing::Entry e;
    while (logRing().pop(e)) {
        if (!json_.pcPresent(nowMs)) {
            if (e.level <= LogLevel::Info) Serial.printf("[%lu] %s: %s\n", (unsigned long)e.ms, e.src, e.msg);
            continue;
        }
        if (logLevel_ == LogLevel::Off || e.level > logLevel_) continue;
        JsonDocument d;
        d["t"] = "log";
        d["lvl"] = logLevelName(e.level);
        d["ms"] = e.ms;
        d["src"] = e.src;
        d["msg"] = e.msg;
        json_.send(d, false);
    }
}

void PcBridge::tapFrame(bool tx, const uint8_t* mac, int8_t rssi, const uint8_t* data, size_t len) {
    const uint32_t now = millis();
    if (!sniff_ || !json_.pcPresent(now)) return;
    if (now - sniffWindow_ >= 1000) { sniffWindow_ = now; sniffBudget_ = 30; }
    if (!sniffBudget_) return;
    --sniffBudget_;
    JsonDocument d;
    d["t"] = "frame";
    d["dir"] = tx ? "tx" : "rx";
    d["ms"] = now;
    char m[18] = "FF:FF:FF:FF:FF:FF";
    if (mac) macToStr(mac, m);
    d["mac"] = m;
    FrameView f;
    const bool ok = decodeFrame(data, len, f) == Decode::Ok;
    d["kind"] = ok ? typeName(static_cast<uint8_t>(f.type)) : "?";
    d["len"] = len;
    d["rssi"] = rssi;
    d["seq"] = ok ? f.seq : 0;
    String hex;
    hex.reserve(len * 3);
    for (size_t i = 0; i < len && i < 64; ++i) { char b[4]; snprintf(b, sizeof(b), "%02X ", data[i]); hex += b; }
    hex.trim();
    d["hex"] = hex;
    json_.send(d, false);
}

// ------------------------------------------------------------------ link events -> PC
void PcBridge::track(int job, uint32_t req, int dev, uint8_t op) {
    if (job <= 0) return;
    for (PendingCmd& p : pend_) {
        if (p.used) continue;
        p.used = true; p.job = static_cast<uint16_t>(job); p.req = req; p.dev = static_cast<int8_t>(dev); p.op = op;
        return;
    }
}

void PcBridge::onJobDone(const JobDone& d) {
    for (PendingCmd& p : pend_) {
        if (!p.used || p.job != d.jobId) continue;
        p.used = false;
        if (!json_.pcPresent(millis())) return;
        JsonDocument e;
        e["t"] = "cmdResult";
        e["id"] = p.req;
        e["dev"] = p.dev;
        if (p.dev >= 0 && p.dev < c_.fleet->getPairedCount()) jsonMac(e.as<JsonObject>(), "mac", c_.fleet->getPaired(p.dev).mac);
        e["op"] = opName(p.op);
        e["result"] = resultName(d.result);
        e["attempts"] = d.attempts;
        e["ms"] = d.ms;
        json_.send(e, true);
        fleetDirty_ = true;
        return;
    }
}

// ---- firmware update bridge (docs/OTA_UPDATE.md, section 5) ----
void PcBridge::handleOtaTx(JsonDocument& doc, uint32_t id) {
    JsonDocument res;
    const int dev = resolveDev(doc);
    uint8_t payload[kMaxPayload];
    const char* d = doc["d"] | "";
    const bool hasTarget = doc["mac"].is<const char*>() || !doc["dev"].isNull();
    if (!hasTarget) {
        jsonResult(res, id, false, "badArg", "dev or mac required");
    } else if (dev < 0) {                                            // unknown index / MAC, or "all"
        jsonResult(res, id, false, "noDevice", "not a paired timer");
    } else {
        const int n = base64Decode(d, strlen(d), payload, sizeof(payload));
        if (n <= 0) {
            jsonResult(res, id, false, "badArg", "d must be base64 of 1..242 bytes");
        } else {
            const RemoteLink::OtaSubmit r = c_.fleet->link().submitOta(dev, payload, static_cast<size_t>(n), id);
            if (r == RemoteLink::OtaSubmit::Ok) return;           // the answer follows when the radio is done with the frame (onOtaTx)
            if (r == RemoteLink::OtaSubmit::Full) jsonResult(res, id, false, "busy", "transmit queue full");
            else jsonResult(res, id, false, "badArg", "cannot queue this frame");
        }
    }
    json_.send(res, true);
}

void PcBridge::onOtaTx(uint32_t tag, bool sent, bool acked) {
    if (!json_.pcPresent(millis())) return;
    JsonDocument res;
    if (sent) { jsonResult(res, tag, true); res["acked"] = acked; }
    else jsonResult(res, tag, false, "noDevice", "the timer was removed");
    json_.send(res, true);
}

void PcBridge::onOtaFrame(int dev, int8_t rssi, const uint8_t* payload, size_t len) {
    if (!json_.pcPresent(millis()) || dev < 0 || dev >= c_.fleet->getPairedCount()) return;
    char text[base64EncodedLen(kMaxPayload) + 1];
    if (!base64Encode(payload, len, text, sizeof(text))) return;
    JsonDocument d;
    d["t"] = "otaRx";
    d["dev"] = dev;
    jsonMac(d.as<JsonObject>(), "mac", c_.fleet->getPaired(dev).mac);
    d["rssi"] = rssi;
    d["d"] = text;
    json_.send(d, false);                                       // a lost report only costs the sender a retransmission
}

// While a firmware update runs the radio must not leave the home channel: say so instead of starting the activity.
bool PcBridge::refuseWhileUpdating(JsonDocument& res, uint32_t id) {
    if (!c_.fleet->updateRunning()) return false;
    jsonResult(res, id, false, "busy", "firmware update running");
    return true;
}

void PcBridge::onRemoved(int dev) {
    fleetDirty_ = true;
    for (PendingCmd& p : pend_) { if (!p.used) continue; if (p.dev == dev) p.dev = -1; else if (p.dev > dev) --p.dev; }
    for (PendingStat& p : pendStat_) { if (!p.used) continue; if (p.dev == dev) p.dev = -1; else if (p.dev > dev) --p.dev; }
}

void PcBridge::onHeard(const Heard& h) {
    if (!json_.pcPresent(millis())) return;
    JsonDocument d;
    d["t"] = "found";
    jsonMac(d.as<JsonObject>(), "mac", h.mac);
    d["name"] = h.name;
    d["rssi"] = h.rssi;
    d["ch"] = h.channel;
    d["paired"] = c_.fleet->link().findDevice(h.mac);
    d["ton"] = h.onTenths / 10.0;
    d["toff"] = h.offTenths / 10.0;
    d["on"] = h.outputOn ? 1 : 0;
    d["boot"] = h.boot;
    json_.send(d, false);
}

void PcBridge::onMigrate(uint8_t stage, int dev, bool ok, uint8_t ch, uint8_t okCount, uint8_t failCount, bool committed) {
    if (!json_.pcPresent(millis())) return;
    JsonDocument d;
    d["t"] = "migrate";
    d["ch"] = ch;
    if (stage == 0) { d["stage"] = "start"; d["targets"] = okCount; }
    else if (stage == 1) { d["stage"] = "dev"; d["dev"] = dev; d["ok"] = ok; }
    else { d["stage"] = "done"; d["ok"] = okCount; d["failed"] = failCount; d["committed"] = committed; }
    json_.send(d, true);
}

void PcBridge::onLinkTest(const LinkTestResult& r) {
    if (!json_.pcPresent(millis())) return;
    JsonDocument d;
    d["t"] = "linkTest";
    d["id"] = linkTestReq_;
    d["dev"] = r.dev;
    d["sent"] = r.sent;
    d["macAck"] = r.macAck;
    d["echo"] = r.echo;
    d["rssi"] = r.rssiAvg;
    d["rttMs"] = r.echo ? static_cast<double>(r.rttSumMs) / r.echo : 0.0;
    d["rttMax"] = r.rttMax;
    d["done"] = r.done;
    json_.send(d, true);
}

// ------------------------------------------------------------------ requests
int PcBridge::resolveDev(JsonDocument& doc) const {
    if (doc["mac"].is<const char*>()) {
        uint8_t mac[6];
        if (!parseMac(doc["mac"], mac)) return -1;
        return c_.fleet->link().findDevice(mac);
    }
    if (doc["dev"].is<const char*>()) return !strcmp(doc["dev"], "all") ? -2 : -1;
    if (doc["dev"].is<int>()) {
        const int i = doc["dev"];
        return (i >= 0 && i < c_.fleet->getPairedCount()) ? i : -1;
    }
    return -1;
}

void PcBridge::handleCmd(JsonDocument& doc, uint32_t id) {
    JsonDocument res;
    const char* op = doc["op"] | "";
    const int target = resolveDev(doc);
    if (target == -1) { jsonResult(res, id, false, "noDevice", "unknown timer"); json_.send(res, true); return; }
    if (c_.fleet->getPairedCount() == 0) { jsonResult(res, id, false, "noDevice", "no timers paired"); json_.send(res, true); return; }
    const bool confirm = doc["confirm"] | true;
    RemoteLink& link = c_.fleet->link();

    auto submitOne = [&](int dev) -> int {
        if (!strcmp(op, "ping")) return link.submit(dev, Op::Nop, nullptr, 0);
        if (!strcmp(op, "setTimes")) {
            const double on = doc["on"] | -1.0, off = doc["off"] | -1.0;
            const uint32_t a = secondsToTenths(on), b = secondsToTenths(off);
            if (!validTenths(a) || !validTenths(b)) return -3;
            return link.submitSetTimes(dev, a, b);
        }
        if (!strcmp(op, "mode")) {
            const char* m = doc["mode"] | "";
            Mode mode;
            if (!strcmp(m, "auto")) mode = Mode::Auto;
            else if (!strcmp(m, "forceOn")) mode = Mode::ForceOn;
            else if (!strcmp(m, "paused")) mode = Mode::Paused;
            else return -3;
            return link.submitMode(dev, mode, static_cast<uint16_t>(constrain(static_cast<int>(doc["leaseMs"] | 1000), 200, 5000)), confirm);
        }
        if (!strcmp(op, "toggle")) return link.submitToggle(dev);
        if (!strcmp(op, "reset")) return link.submitReset(dev);
        if (!strcmp(op, "setPhase")) return link.submitPhase(dev, !strcmp(doc["phase"] | "off", "on"), static_cast<uint32_t>((doc["elapsed"] | 0.0) * 1000.0));
        if (!strcmp(op, "name")) {
            const char* nm = doc["name"] | "";
            char clean[kNameLen];
            sanitizeName(clean, nm, strlen(nm));
            if (!clean[0]) return -3;
            return c_.fleet->renameByIndex(dev, clean);
        }
        if (!strcmp(op, "channel")) {
            const int ch = doc["ch"] | 0;
            if (!validChannel(static_cast<uint8_t>(ch))) return -3;
            return link.submitChannel(dev, static_cast<uint8_t>(ch));
        }
        if (!strcmp(op, "startPhase")) return link.submitStartup(dev, !strcmp(doc["phase"] | "off", "on"));
        if (!strcmp(op, "identify")) return link.submitIdentify(dev, doc["ms"] | 5000);
        if (!strcmp(op, "reboot")) return link.submitReboot(dev);
        if (!strcmp(op, "factoryReset")) return link.submitFactoryReset(dev);
        return -3;
    };
    auto opCode = [&]() -> uint8_t {
        static const struct { const char* n; Op o; } map[] = {
            {"ping", Op::Nop}, {"setTimes", Op::SetTimes}, {"mode", Op::SetMode}, {"toggle", Op::Toggle}, {"reset", Op::Reset},
            {"setPhase", Op::SetPhase}, {"name", Op::SetName}, {"channel", Op::SetChannel}, {"startPhase", Op::SetStartup},
            {"identify", Op::Identify}, {"reboot", Op::Reboot}, {"factoryReset", Op::FactoryReset}};
        for (const auto& m : map) if (!strcmp(m.n, op)) return static_cast<uint8_t>(m.o);
        return 0;
    };

    int queued = 0;
    bool badArg = false;
    const bool wantResult = confirm || strcmp(op, "mode") != 0;
    for (int dev = 0; dev < c_.fleet->getPairedCount(); ++dev) {
        if (target >= 0 && dev != target) continue;
        const int job = submitOne(dev);
        if (job == -3) { badArg = true; break; }
        if (job > 0) { ++queued; if (wantResult) track(job, id, dev, opCode()); }
    }
    if (badArg) jsonResult(res, id, false, "badArg", "bad or missing argument");
    else if (!queued) jsonResult(res, id, false, "full", "command queue full");
    else { jsonResult(res, id, true); res["queued"] = queued; }
    json_.send(res, true);
}

void PcBridge::handleGet(JsonDocument& doc, uint32_t id, uint32_t nowMs) {
    JsonDocument res;
    const char* what = doc["what"] | "";
    if (!strcmp(what, "fleet")) {
        jsonResult(res, id, true);
        json_.send(res, true);
        sendFleet(nowMs);
        return;
    }
    if (!strcmp(what, "settings")) {
        jsonResult(res, id, true);
        fillSettings(res["settings"].to<JsonObject>());
    } else if (!strcmp(what, "stats")) {
        jsonResult(res, id, true);
        JsonObject st = res["stats"].to<JsonObject>();
        st["ms"] = nowMs;
        fillSelfStats(st["self"].to<JsonObject>(), nowMs);
        json_.send(res, true);
        sendDevStats(nowMs);
        return;
    } else if (!strcmp(what, "devStats")) {
        const int dev = resolveDev(doc);
        if (dev < 0) { jsonResult(res, id, false, "noDevice", "unknown timer"); json_.send(res, true); return; }
        c_.fleet->link().submitGetInfo(dev);
        for (PendingStat& p : pendStat_) {
            if (p.used) continue;
            p.used = true; p.req = id; p.dev = static_cast<int8_t>(dev); p.deadline = nowMs + 3000;
            return;                                       // answered from onInfo() / expireStats()
        }
        jsonResult(res, id, false, "busy", "too many pending requests");
    } else {
        jsonResult(res, id, false, "badArg", "unknown 'what'");
    }
    json_.send(res, true);
}

void PcBridge::handleSet(JsonDocument& doc, uint32_t id) {
    JsonDocument res;
    JsonObjectConst s = doc["settings"];
    RemoteSettingsData& d = c_.settings->mut();
    if (s["txPower"].is<int>()) d.txPowerQdbm = static_cast<int8_t>(s["txPower"].as<int>());
    if (s["brightness"].is<int>()) d.oledBrightness = static_cast<uint8_t>(constrain(s["brightness"].as<int>(), 5, 255));
    if (s["autoOff"].is<int>()) d.autoOffSeconds = static_cast<uint16_t>(constrain(s["autoOff"].as<int>(), 0, 3600));
    if (s["rssiLow"].is<int>()) d.rssiLowDbm = static_cast<int8_t>(s["rssiLow"].as<int>());
    if (s["rssiHigh"].is<int>()) d.rssiHighDbm = static_cast<int8_t>(s["rssiHigh"].as<int>());
    if (s["batCal"].is<JsonArrayConst>() && s["batCal"].size() == 3) for (int i = 0; i < 3; ++i) d.batCalMv[i] = s["batCal"][i];
    c_.settings->save();                     // clamps + stores only if changed
    if (onSettingsChanged) onSettingsChanged();
    jsonResult(res, id, true);
    fillSettings(res["settings"].to<JsonObject>());
    json_.send(res, true);
}

void PcBridge::handleFactoryReset(JsonDocument& doc, uint32_t id, uint32_t nowMs) {
    JsonDocument res;
    if (!(doc["confirm"] | false)) { jsonResult(res, id, false, "badArg", "confirm:true required"); json_.send(res, true); return; }
    const bool all = !strcmp(doc["scope"] | "remote", "all");
    if (all) for (int i = 0; i < c_.fleet->getPairedCount(); ++i) c_.fleet->link().submitFactoryReset(i);
    factoryDeadline_ = nowMs + (all ? 4000 : 50);
    jsonResult(res, id, true);
    json_.send(res, true);
}

void PcBridge::handle(JsonDocument& doc, uint32_t nowMs) {
    const char* type = doc["t"] | "";
    const uint32_t id = doc["id"] | 0;
    JsonDocument res;
    Fleet& fleet = *c_.fleet;

    if (!strcmp(type, "hello")) {
        jsonResult(res, id, true);
        res["kind"] = "remote";
        res["fw"] = FOG_FW_STRING;
        res["proto"] = 2;
        jsonMac(res.as<JsonObject>(), "mac", fleet.ownMac());
        res["name"] = "Remote";
        res["ch"] = fleet.homeChannel();
        res["net"] = c_.settings->get().net;
        res["upMs"] = nowMs;
        res["rst"] = resetReasonName(c_.resetReason);
        res["boots"] = c_.life->boots;
        res["heap"] = ESP.getFreeHeap();
        res["build"] = kBuildStamp;
        res["maxDevs"] = kMaxTimers;
        res["caps"].to<JsonArray>().add("ota");            // firmware update bridge (otaTx / otaRx / otaHold)
        fleetDirty_ = true;                                // send the picture right after hello
    } else if (!strcmp(type, "ping")) {
        jsonResult(res, id, true);
        res["ms"] = nowMs;
    } else if (!strcmp(type, "get")) {
        handleGet(doc, id, nowMs);
        return;
    } else if (!strcmp(type, "set")) {
        handleSet(doc, id);
        return;
    } else if (!strcmp(type, "cmd")) {
        handleCmd(doc, id);
        return;
    } else if (!strcmp(type, "select")) {
        const int dev = resolveDev(doc);
        if (dev < 0) jsonResult(res, id, false, "noDevice", "unknown timer");
        else { fleet.setActive(dev); fleetDirty_ = true; jsonResult(res, id, true); }
    } else if (!strcmp(type, "pair")) {
        uint8_t mac[6];
        if (!parseMac(doc["mac"] | "", mac)) jsonResult(res, id, false, "badArg", "bad mac");
        else {
            uint8_t ch = fleet.homeChannel();
            for (int i = 0; i < RemoteLink::kMaxHeard; ++i) {
                const Heard& h = fleet.link().heard(i);
                if (h.used && memcmp(h.mac, mac, 6) == 0) ch = h.channel;
            }
            if (fleet.pairMac(mac, doc["name"] | "", ch)) { jsonResult(res, id, true); fleetDirty_ = true; }
            else jsonResult(res, id, false, "full", "timer list is full");
        }
    } else if (!strcmp(type, "unpair")) {
        const int dev = resolveDev(doc);
        if (dev < 0) jsonResult(res, id, false, "noDevice", "unknown timer");
        else { fleet.removeDeviceByIndex(dev); fleetDirty_ = true; jsonResult(res, id, true); }
    } else if (!strcmp(type, "otaTx")) {
        handleOtaTx(doc, id);
        return;
    } else if (!strcmp(type, "otaHold")) {
        fleet.link().setOtaHold(doc["on"] | false);
        jsonResult(res, id, true);
    } else if (!strcmp(type, "discover")) {
        if ((doc["on"] | false) && refuseWhileUpdating(res, id)) { /* answered below */ }
        else { if (doc["on"] | false) fleet.startDiscovery(doc["sweep"] | false); else fleet.stopDiscovery(); jsonResult(res, id, true); }
    } else if (!strcmp(type, "survey")) {
        if (refuseWhileUpdating(res, id)) { /* answered below */ }
        else if (fleet.startSurvey()) { surveyReq_ = id; jsonResult(res, id, true); }
        else jsonResult(res, id, false, "busy", "survey/migration running, or the Wi-Fi scan could not start");
    } else if (!strcmp(type, "channel")) {
        const int ch = doc["ch"] | 0;
        if (refuseWhileUpdating(res, id)) { /* answered below */ }
        else if (!validChannel(static_cast<uint8_t>(ch))) jsonResult(res, id, false, "badArg", "channel 1..13");
        else if (ch == fleet.homeChannel()) jsonResult(res, id, false, "badArg", "already on that channel");
        else if (fleet.migrateTo(static_cast<uint8_t>(ch), doc["force"] | false)) jsonResult(res, id, true);
        else jsonResult(res, id, false, "busy", "cannot start now");
    } else if (!strcmp(type, "linkTest")) {
        const int dev = resolveDev(doc);
        if (refuseWhileUpdating(res, id)) { /* answered below */ }
        else if (dev < 0) jsonResult(res, id, false, "noDevice", "unknown timer");
        else if (!fleet.link().startLinkTest(dev, doc["count"] | 50, doc["gapMs"] | 40)) jsonResult(res, id, false, "busy", "link test running");
        else { linkTestReq_ = id; jsonResult(res, id, true); }
    } else if (!strcmp(type, "recover")) {
        if (!refuseWhileUpdating(res, id)) { fleet.findLostTimers(); jsonResult(res, id, true); }
    } else if (!strcmp(type, "logCfg")) {
        logLevel_ = logLevelFromName(doc["level"] | "off", LogLevel::Off);
        jsonResult(res, id, true);
    } else if (!strcmp(type, "sniff")) {
        sniff_ = doc["on"] | false;
        jsonResult(res, id, true);
    } else if (!strcmp(type, "statsCfg")) {
        const uint32_t iv = doc["intervalMs"] | 0;
        statsEveryMs_ = iv == 0 ? 0 : (iv < 500 ? 500 : iv);
        jsonResult(res, id, true);
    } else if (!strcmp(type, "reboot")) {
        jsonResult(res, id, true);
        json_.send(res, true);
        rebootAt_ = nowMs + 300;
        return;
    } else if (!strcmp(type, "factoryReset")) {
        handleFactoryReset(doc, id, nowMs);
        return;
    } else {
        jsonResult(res, id, false, "unknownType", type);
    }
    json_.send(res, true);
}
