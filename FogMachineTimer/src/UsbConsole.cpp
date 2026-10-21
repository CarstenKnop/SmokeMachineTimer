#include "UsbConsole.h"

#include <esp_system.h>
#include "FogJsonUtil.h"
#include "FogUtil.h"

using namespace Fog;

namespace {
const char* kBuildStamp = __DATE__ " " __TIME__;

const char* modeName(Mode m) {
    switch (m) {
        case Mode::ForceOn: return "forceOn";
        case Mode::Paused:  return "paused";
        default:            return "auto";
    }
}
}  // namespace

void UsbConsole::begin() {
    json_.begin(Serial, [this](JsonDocument& doc) { handle(doc, millis()); });
}

void UsbConsole::loop(uint32_t nowMs) {
    json_.poll(nowMs);
    drainLog();
    if (!json_.pcPresent(nowMs)) return;
    if (nowMs - lastFleetMs_ >= 500) { lastFleetMs_ = nowMs; sendFleet(nowMs); }
    if (nowMs - lastRemoteMs_ >= 1000) { lastRemoteMs_ = nowMs; sendRemote(nowMs); }
    if (statsEveryMs_ && nowMs - lastStatsMs_ >= statsEveryMs_) { lastStatsMs_ = nowMs; sendStats(nowMs, 0, false); }
}

void UsbConsole::drainLog() {
    LogRing::Entry e;
    while (logRing().pop(e)) {
        if (!json_.pcPresent(millis())) {                // nobody speaks FogSerial: plain text for a normal serial monitor
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

void UsbConsole::onFrame(bool tx, const uint8_t* mac, int8_t rssi, const uint8_t* data, size_t len, uint32_t nowMs) {
    if (!sniff_ || !json_.pcPresent(nowMs)) return;
    if (nowMs - sniffWindowMs_ >= 1000) { sniffWindowMs_ = nowMs; sniffBudget_ = 30; }
    if (!sniffBudget_) return;
    --sniffBudget_;
    JsonDocument d;
    d["t"] = "frame";
    d["dir"] = tx ? "tx" : "rx";
    d["ms"] = nowMs;
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

void UsbConsole::fillSettings(JsonObject o) const {
    const TimerSettings& s = node_.settings();
    o["name"] = s.name;
    o["ton"] = s.onTenths / 10.0;
    o["toff"] = s.offTenths / 10.0;
    o["startPhase"] = s.startPhaseOn ? "on" : "off";
    o["led"] = s.led != 0;
    o["ch"] = s.channel;
    o["net"] = s.net;
}

void UsbConsole::sendFleet(uint32_t nowMs) {
    JsonDocument d;
    d["t"] = "fleet";
    d["seq"] = ++seq_;
    d["active"] = 0;
    d["ch"] = node_.settings().channel;
    JsonObject o = d["devs"].to<JsonArray>().add<JsonObject>();
    uint8_t mac[6];
    plat_.radio.macAddress(mac);
    o["i"] = 0;
    jsonMac(o, "mac", mac);
    o["name"] = node_.settings().name;
    o["online"] = 1;
    o["on"] = node_.outputOn(nowMs) ? 1 : 0;
    o["ph"] = node_.engine().phaseOn() ? "on" : "off";
    o["mode"] = modeName(node_.engine().mode());
    o["ton"] = node_.engine().onTenths() / 10.0;
    o["toff"] = node_.engine().offTenths() / 10.0;
    o["el"] = node_.engine().elapsedMs(nowMs) / 1000.0;
    o["rssi"] = 0;
    o["rssiT"] = 0;
    o["age"] = 0;
    o["loss"] = -1;
    o["ch"] = node_.settings().channel;
    o["boot"] = node_.bootCount();
    o["up"] = nowMs / 1000;
    o["fw"] = "2.0";
    o["pend"] = 0;
    o["id"] = 0;
    json_.send(d, false);
}

void UsbConsole::sendRemote(uint32_t nowMs) {
    JsonDocument d;
    d["t"] = "remote";
    d["up"] = nowMs / 1000;
    d["heap"] = ESP.getFreeHeap();
    d["ch"] = node_.settings().channel;
    d["net"] = node_.settings().net;
    d["txq"] = 0;
    json_.send(d, false);
}

void UsbConsole::sendStats(uint32_t nowMs, uint32_t id, bool asResponse) {
    JsonDocument d;
    JsonObject root = d.to<JsonObject>();
    JsonObject stats = asResponse ? d["stats"].to<JsonObject>() : root;
    if (asResponse) { d["t"] = "res"; d["id"] = id; d["ok"] = true; }
    else d["t"] = "stats";
    stats["ms"] = nowMs;
    JsonObject self = stats["self"].to<JsonObject>();
    self["kind"] = "timer";
    self["up"] = nowMs / 1000;
    self["boots"] = plat_.life.boots;
    self["heap"] = ESP.getFreeHeap();
    self["heapMin"] = ESP.getMinFreeHeap();
    self["loopMaxUs"] = plat_.loop.maxUs();
    self["loopsPerS"] = plat_.loop.loopsPerSecond();
    self["rstReason"] = resetReasonName(plat_.resetReason);
    jsonRst(self["rst"].to<JsonObject>(), plat_.life);
    const FogRadio::Stats& rs = plat_.radio.stats();
    JsonObject radio = self["radio"].to<JsonObject>();
    radio["ch"] = plat_.radio.channel();
    radio["tx"] = rs.tx;
    radio["txOk"] = rs.txOk;
    radio["txFail"] = rs.txFail;
    radio["txWd"] = rs.txWd;
    radio["txDrop"] = rs.txDrop + plat_.txQueueDrops;
    radio["rx"] = node_.rxFrames;
    radio["rxBad"] = node_.rxBad;
    radio["rxForeign"] = node_.rxForeign;
    radio["rxDrop"] = rs.rxDrop;
    JsonObject pc = self["pc"].to<JsonObject>();
    pc["linesRx"] = json_.linesRx;
    pc["linesTx"] = json_.linesTx;
    pc["badJson"] = json_.badJson;
    pc["txDrop"] = json_.txDrop;
    InfoPayload info;
    node_.buildInfo(info);
    jsonTimerInfo(self["timer"].to<JsonObject>(), info);
    json_.send(d, asResponse);
}

void UsbConsole::handleCmd(JsonDocument& doc, uint32_t id, uint32_t nowMs) {
    JsonDocument res;
    const JsonVariantConst dev = doc["dev"];
    if (!dev.isNull() && !(dev.is<int>() && dev.as<int>() == 0) && !doc["mac"].is<const char*>()) {
        jsonResult(res, id, false, "noDevice", "a directly connected timer is device 0");
        json_.send(res, true);
        return;
    }
    const char* op = doc["op"] | "";
    uint8_t args[kMaxCmdArgs] = {0};
    size_t n = 0;
    Op code = Op::Nop;
    bool valid = true;
    if (!strcmp(op, "ping")) { code = Op::Nop; }
    else if (!strcmp(op, "setTimes")) {
        code = Op::SetTimes;
        putU32(args, secondsToTenths(doc["on"] | -1.0));
        putU32(args + 4, secondsToTenths(doc["off"] | -1.0));
        n = 8;
    } else if (!strcmp(op, "mode")) {
        code = Op::SetMode;
        const char* m = doc["mode"] | "";
        args[0] = !strcmp(m, "auto") ? 0 : !strcmp(m, "forceOn") ? 1 : !strcmp(m, "paused") ? 2 : 9;
        putU16(args + 1, doc["leaseMs"] | 1000);
        n = 3;
    } else if (!strcmp(op, "toggle")) code = Op::Toggle;
    else if (!strcmp(op, "reset")) code = Op::Reset;
    else if (!strcmp(op, "setPhase")) {
        code = Op::SetPhase;
        const char* p = doc["phase"] | "off";
        args[0] = !strcmp(p, "on") ? 1 : 0;
        putU32(args + 1, static_cast<uint32_t>((doc["elapsed"] | 0.0) * 1000.0));
        n = 5;
    } else if (!strcmp(op, "name")) {
        code = Op::SetName;
        const char* nm = doc["name"] | "";
        strncpy(reinterpret_cast<char*>(args), nm, kNameLen);
        n = kNameLen;
    } else if (!strcmp(op, "channel")) { code = Op::SetChannel; args[0] = doc["ch"] | 0; n = 1; }
    else if (!strcmp(op, "startPhase")) { code = Op::SetStartup; args[0] = !strcmp(doc["phase"] | "off", "on") ? 1 : 0; n = 1; }
    else if (!strcmp(op, "identify")) { code = Op::Identify; putU16(args, doc["ms"] | 5000); n = 2; }
    else if (!strcmp(op, "reboot")) { code = Op::Reboot; args[0] = kRebootMagic; n = 1; }
    else if (!strcmp(op, "factoryReset")) { code = Op::FactoryReset; args[0] = kFactoryMagic; n = 1; }
    else valid = false;
    if (!valid) {
        jsonResult(res, id, false, "badArg", "unknown op");
        json_.send(res, true);
        return;
    }
    const uint8_t r = node_.runLocal(code, args, n, nowMs);
    jsonResult(res, id, true);
    res["queued"] = 1;
    json_.send(res, true);
    JsonDocument ev;
    ev["t"] = "cmdResult";
    ev["id"] = id;
    ev["dev"] = 0;
    ev["op"] = op;
    ev["result"] = r == 0 ? "ok" : "rejected";
    ev["attempts"] = 1;
    ev["ms"] = 0;
    json_.send(ev, true);
}

void UsbConsole::handle(JsonDocument& doc, uint32_t nowMs) {
    const char* type = doc["t"] | "";
    const uint32_t id = doc["id"] | 0;
    JsonDocument res;

    if (!strcmp(type, "hello")) {
        uint8_t mac[6];
        plat_.radio.macAddress(mac);
        jsonResult(res, id, true);
        res["kind"] = "timer";
        res["fw"] = FOG_FW_STRING;
        res["proto"] = 2;
        jsonMac(res.as<JsonObject>(), "mac", mac);
        res["name"] = node_.settings().name;
        res["ch"] = node_.settings().channel;
        res["net"] = node_.settings().net;
        res["upMs"] = nowMs;
        res["rst"] = resetReasonName(plat_.resetReason);
        res["boots"] = plat_.life.boots;
        res["heap"] = ESP.getFreeHeap();
        res["build"] = kBuildStamp;
        res["maxDevs"] = 1;
    } else if (!strcmp(type, "ping")) {
        jsonResult(res, id, true);
        res["ms"] = nowMs;
    } else if (!strcmp(type, "get")) {
        const char* what = doc["what"] | "";
        if (!strcmp(what, "fleet")) { jsonResult(res, id, true); json_.send(res, true); sendFleet(nowMs); return; }
        if (!strcmp(what, "settings")) { jsonResult(res, id, true); fillSettings(res["settings"].to<JsonObject>()); }
        else if (!strcmp(what, "stats")) { sendStats(nowMs, id, true); return; }
        else jsonResult(res, id, false, "unsupported", "get what?");
    } else if (!strcmp(type, "set")) {
        JsonObjectConst s = doc["settings"];
        uint8_t a[kMaxCmdArgs] = {0};
        bool bad = false;
        if (s["ton"].is<double>() || s["toff"].is<double>()) {
            const double on = s["ton"] | node_.settings().onTenths / 10.0;
            const double off = s["toff"] | node_.settings().offTenths / 10.0;
            putU32(a, secondsToTenths(on)); putU32(a + 4, secondsToTenths(off));
            if (node_.runLocal(Op::SetTimes, a, 8, nowMs) != 0) bad = true;
        }
        if (s["name"].is<const char*>()) {
            memset(a, 0, sizeof(a));
            strncpy(reinterpret_cast<char*>(a), s["name"], kNameLen);
            if (node_.runLocal(Op::SetName, a, kNameLen, nowMs) != 0) bad = true;
        }
        if (s["startPhase"].is<const char*>()) {
            a[0] = !strcmp(s["startPhase"], "on") ? 1 : 0;
            node_.runLocal(Op::SetStartup, a, 1, nowMs);
        }
        if (s["led"].is<bool>()) {
            node_.mutableSettings().led = s["led"].as<bool>() ? 1 : 0;
            plat_.saveSettings(node_.settings());
        }
        if (bad) jsonResult(res, id, false, "badArg", "value rejected");
        else { jsonResult(res, id, true); fillSettings(res["settings"].to<JsonObject>()); }
    } else if (!strcmp(type, "cmd")) {
        handleCmd(doc, id, nowMs);
        return;
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
        node_.runLocal(Op::Reboot, reinterpret_cast<const uint8_t*>("\x5A"), 1, nowMs);
        return;
    } else if (!strcmp(type, "factoryReset")) {
        if (!(doc["confirm"] | false)) { jsonResult(res, id, false, "badArg", "confirm:true required"); }
        else { uint8_t m = kFactoryMagic; node_.runLocal(Op::FactoryReset, &m, 1, nowMs); jsonResult(res, id, true); }
    } else if (!strcmp(type, "select") || !strcmp(type, "pair") || !strcmp(type, "unpair") || !strcmp(type, "discover") ||
               !strcmp(type, "survey") || !strcmp(type, "channel") || !strcmp(type, "linkTest") || !strcmp(type, "recover")) {
        jsonResult(res, id, false, "unsupported", "fleet functions need a remote");
    } else {
        jsonResult(res, id, false, "unknownType", type);
    }
    json_.send(res, true);
}
