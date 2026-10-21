// FogJsonUtil.h - JSON builders shared by the remote's PC bridge and the timer's USB console.
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include "FogProto.h"
#include "FogStore.h"
#include "FogSys.h"
#include "FogUtil.h"

namespace Fog {

inline void jsonMac(JsonObject o, const char* key, const uint8_t mac[6]) {
    char s[18];
    macToStr(mac, s);
    o[key] = s;
}

inline void jsonRst(JsonObject o, const LifeCounters& c) {
    o["poweron"] = c.rstPower;
    o["brownout"] = c.rstBrownout;
    o["wdt"] = c.rstWdt;
    o["panic"] = c.rstPanic;
    o["sw"] = c.rstSw;
    o["deepsleep"] = c.rstDeepSleep;
    o["other"] = c.rstOther;
}

// Timer-side counters as sent in an INFO frame (-> "timer" object of the stats event).
inline void jsonTimerInfo(JsonObject o, const InfoPayload& i) {
    char fw[12];
    snprintf(fw, sizeof(fw), "%u.%u.%u", i.fwMajor, i.fwMinor, i.fwPatch);
    o["fw"] = fw;
    o["up"] = i.uptimeS;
    o["boots"] = i.bootCount;
    o["rstReason"] = resetReasonName(i.resetReason);
    o["heap"] = i.freeHeap;
    o["heapMin"] = i.minFreeHeap;
    o["loopMaxUs"] = i.loopMaxUs;
    o["ch"] = i.channel;
    o["net"] = i.net;
    JsonObject rst = o["rst"].to<JsonObject>();
    rst["poweron"] = i.rstPower;
    rst["brownout"] = i.rstBrownout;
    rst["wdt"] = i.rstWdt;
    rst["panic"] = i.rstPanic;
    rst["sw"] = i.rstSw;
    rst["other"] = i.rstOther;
    o["rx"] = i.rxFrames;
    o["rxBad"] = i.rxBad;
    o["rxForeign"] = i.rxForeign;
    o["rxDrop"] = i.rxDropped;
    o["rxProbes"] = i.rxProbes;
    o["rxCmds"] = i.rxCmds;
    o["tx"] = i.txFrames;
    o["txFail"] = i.txFail;
    o["txDrop"] = i.txDropped;
    o["beats"] = i.txBeats;
    o["replies"] = i.txReplies;
    o["cmdsRun"] = i.cmdsRun;
    o["cmdsDup"] = i.cmdsDup;
    o["cmdsRejected"] = i.cmdsRejected;
    o["rssiMin"] = i.rssiMin;
    o["rssiAvg"] = i.rssiAvg;
    o["rssiMax"] = i.rssiMax;
    o["rssiLast"] = i.rssiLast;
}

inline void jsonResult(JsonDocument& res, uint32_t id, bool ok, const char* err = nullptr, const char* msg = nullptr) {
    res["t"] = "res";
    res["id"] = id;
    res["ok"] = ok;
    if (err) res["err"] = err;
    if (msg) res["msg"] = msg;
}

}  // namespace Fog
