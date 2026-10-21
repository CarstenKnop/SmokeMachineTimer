#include "FogTimerNode.h"

#include <stdio.h>

namespace Fog {

namespace {
enum Fx : uint16_t { FX_SAVE = 1, FX_CHANNEL = 2, FX_REBOOT = 4, FX_FACTORY = 8, FX_IDENTIFY = 16, FX_INFO = 32 };
constexpr uint32_t kEventMinGapMs = 15;     // never spam events faster than this
}  // namespace

void TimerNode::begin(uint32_t nowMs, const TimerSettings& settings, uint32_t bootCount) {
    s_ = settings;
    if (!validChannel(s_.channel)) s_.channel = kDefaultChannel;
    if (!validTenths(s_.onTenths)) s_.onTenths = kDefaultOnTenths;
    if (!validTenths(s_.offTenths)) s_.offTenths = kDefaultOffTenths;
    boot_ = bootCount;
    startMs_ = nowMs;
    ota_.setTarget(plat_.otaTarget());
    engine_.begin(nowMs, s_.onTenths, s_.offTenths, s_.startPhaseOn != 0);
    // Announce ourselves right after power-up so the remote notices the reboot within a few ms.
    bootBeats_ = 3;
    bootBeatAt_ = nowMs;
    nextBeat_ = nowMs + kHeartbeatMs;
    prevOutput_ = engine_.outputOn(nowMs) ? 1 : 0;
}

void TimerNode::buildStatus(StatusPayload& out, uint32_t nowMs, uint8_t extraFlags) const {
    memset(&out, 0, sizeof(out));
    uint8_t flags = extraFlags;
    if (engine_.outputOn(nowMs)) flags |= ST_OUTPUT_ON;
    if (engine_.phaseOn()) flags |= ST_PHASE_ON;
    if (static_cast<uint32_t>(nowMs - startMs_) < kBootFlagMs) flags |= ST_JUST_BOOTED;
    if (identifying) flags |= ST_IDENTIFY;
    out.flags = flags;
    out.mode = static_cast<uint8_t>(engine_.mode());
    out.onTenths = engine_.onTenths();
    out.offTenths = engine_.offTenths();
    out.elapsedMs = engine_.elapsedMs(nowMs);
    out.channel = s_.channel;
    out.rssiAtTimer = lastRssi_;
    out.lastCmdId = lastCmdId_;
    out.lastCmdResult = lastCmdResult_;
    out.lastCmdSrc = lastCmdSrc_;
    out.fwMajor = kFwMajor;
    out.fwMinor = kFwMinor;
    out.startPhaseOn = s_.startPhaseOn;
    out.bootCount = static_cast<uint16_t>(boot_ > 0xFFFF ? 0xFFFF : boot_);
    out.uptimeS = (nowMs - startMs_) / 1000u;
    memcpy(out.name, s_.name, kNameLen);
    out.name[kNameLen - 1] = 0;
}

bool TimerNode::sendFrame(const uint8_t* mac, Type type, const void* payload, size_t len) {
    uint8_t buf[kMaxFrame];
    const size_t n = encodeFrame(buf, sizeof(buf), s_.net, type, txSeq_++, payload, len);
    if (!n) return false;
    ++txFrames;
    return plat_.send(mac, buf, n);
}

void TimerNode::sendStatusNow(const uint8_t* mac, uint32_t nowMs, uint8_t flags) {
    StatusPayload st;
    buildStatus(st, nowMs, flags);
    sendFrame(mac, Type::Status, &st, sizeof(st));
}

void TimerNode::buildInfo(InfoPayload& info) const {
    memset(&info, 0, sizeof(info));
    info.kind = 2;
    info.fwMajor = kFwMajor; info.fwMinor = kFwMinor; info.fwPatch = kFwPatch;
    info.startPhaseOn = s_.startPhaseOn;
    info.channel = s_.channel;
    info.net = s_.net;
    info.bootCount = boot_;
    info.rxFrames = rxFrames; info.rxBad = rxBad; info.rxForeign = rxForeign;
    info.rxProbes = rxProbes; info.rxCmds = rxCmds;
    info.txFrames = txFrames; info.txFail = txFail; info.txBeats = txBeats; info.txReplies = txReplies;
    info.cmdsRun = cmdsRun; info.cmdsDup = cmdsDup; info.cmdsRejected = cmdsRejected;
    info.rssiLast = lastRssi_; info.rssiMin = rssiMin_; info.rssiMax = rssiMax_;
    info.rssiAvg = rssiCount_ ? static_cast<int8_t>(rssiSum_ / static_cast<int32_t>(rssiCount_)) : 0;
    plat_.fillInfo(info);       // platform adds uptime, heap, loop time, reset reason/counts, queue drops
}

void TimerNode::sendInfo(const uint8_t* mac) {
    InfoPayload info;
    buildInfo(info);
    sendFrame(mac, Type::Info, &info, sizeof(info));
}

void TimerNode::queueReply(const uint8_t* mac, uint32_t due, uint8_t kind) {
    // Reuse a slot for the same destination/kind, otherwise a free one, otherwise the oldest.
    Pending* slot = nullptr;
    for (Pending& p : pending_) if (p.used && p.kind == kind && memcmp(p.mac, mac, 6) == 0) { slot = &p; break; }
    if (!slot) for (Pending& p : pending_) if (!p.used) { slot = &p; break; }
    if (!slot) slot = &pending_[0];
    slot->used = true;
    memcpy(slot->mac, mac, 6);
    slot->due = due;
    slot->kind = kind;
}

void TimerNode::onFrame(const uint8_t* srcMac, int8_t rssi, const uint8_t* data, size_t len, uint32_t nowMs) {
    FrameView f;
    const Decode d = decodeFrame(data, len, f);
    if (d != Decode::Ok) {
        if (d == Decode::BadMagic) { ++rxForeign; return; }    // some other ESP-NOW user
        ++rxBad;
        return;
    }
    if (f.net != s_.net) { ++rxForeign; return; }
    ++rxFrames;
    // Signal statistics describe the link to the remote, so only count frames that come from a remote
    // (other timers on the channel send Status frames that we hear too).
    const bool fromRemote = (f.type == Type::Probe || f.type == Type::Cmd || f.type == Type::Ota);
    if (fromRemote && rssi < 0 && rssi > -120) {
        lastRssi_ = rssi;
        if (!rssiCount_ || rssi < rssiMin_) rssiMin_ = rssi;
        if (!rssiCount_ || rssi > rssiMax_) rssiMax_ = rssi;
        rssiSum_ += rssi;
        if (++rssiCount_ >= 100000) { rssiSum_ /= 2; rssiCount_ /= 2; }
    }

    switch (f.type) {
        case Type::Probe: {
            ProbePayload p;
            if (!readPayload(f, p, 1)) { ++rxBad; return; }
            ++rxProbes;
            const uint32_t jitter = (p.flags & kProbeJitter) ? plat_.random(60) : 0;
            queueReply(srcMac, nowMs + jitter, 0);
            break;
        }
        case Type::Cmd:
            handleCmd(srcMac, f, nowMs);
            break;
        case Type::Ota:
            ++rxOta;
            ota_.onPayload(srcMac, f.payload, f.len, nowMs);
            break;
        default:
            break;   // STATUS/INFO from other timers: not for us
    }
}

void TimerNode::execute(uint8_t op, const uint8_t* a, size_t n, uint8_t& result, uint16_t& fx, uint32_t nowMs) {
    result = static_cast<uint8_t>(CmdResult::Ok);
    switch (static_cast<Op>(op)) {
        case Op::Nop:
            break;
        case Op::SetTimes: {
            if (n < 8) { result = static_cast<uint8_t>(CmdResult::Invalid); break; }
            const uint32_t on = getU32(a), off = getU32(a + 4);
            if (!validTenths(on) || !validTenths(off)) { result = static_cast<uint8_t>(CmdResult::Invalid); break; }
            if (on != s_.onTenths || off != s_.offTenths) {
                s_.onTenths = on; s_.offTenths = off;
                engine_.setTimes(on, off);
                fx |= FX_SAVE;
            }
            break;
        }
        case Op::SetMode: {
            if (n < 1 || a[0] > 2) { result = static_cast<uint8_t>(CmdResult::Invalid); break; }
            const uint16_t lease = (n >= 3) ? getU16(a + 1) : 1000;
            engine_.setMode(static_cast<Mode>(a[0]), nowMs, lease);
            break;
        }
        case Op::Toggle:
            engine_.toggle(nowMs);
            break;
        case Op::Reset:
            engine_.reset(nowMs);
            break;
        case Op::SetPhase: {
            if (n < 1 || a[0] > 1) { result = static_cast<uint8_t>(CmdResult::Invalid); break; }
            engine_.setPhase(a[0] != 0, (n >= 5) ? getU32(a + 1) : 0, nowMs);
            break;
        }
        case Op::SetName: {
            char name[kNameLen];
            sanitizeName(name, reinterpret_cast<const char*>(a), n < kNameLen ? n : kNameLen);
            if (!name[0]) { result = static_cast<uint8_t>(CmdResult::Invalid); break; }
            if (memcmp(name, s_.name, kNameLen) != 0) { memcpy(s_.name, name, kNameLen); fx |= FX_SAVE; }
            break;
        }
        case Op::SetChannel: {
            if (n < 1 || !validChannel(a[0])) { result = static_cast<uint8_t>(CmdResult::Invalid); break; }
            chValue_ = a[0];
            fx |= FX_CHANNEL;
            break;
        }
        case Op::FactoryReset:
            if (n < 1 || a[0] != kFactoryMagic) { result = static_cast<uint8_t>(CmdResult::Invalid); break; }
            fx |= FX_FACTORY;
            break;
        case Op::Identify:
            fx |= FX_IDENTIFY;
            break;
        case Op::Reboot:
            if (n < 1 || a[0] != kRebootMagic) { result = static_cast<uint8_t>(CmdResult::Invalid); break; }
            fx |= FX_REBOOT;
            break;
        case Op::SetStartup:
            if (n < 1 || a[0] > 1) { result = static_cast<uint8_t>(CmdResult::Invalid); break; }
            if (s_.startPhaseOn != a[0]) { s_.startPhaseOn = a[0]; fx |= FX_SAVE; }
            break;
        case Op::GetInfo:
            fx |= FX_INFO;
            break;
        default:
            result = static_cast<uint8_t>(CmdResult::Unsupported);
            break;
    }
}

uint8_t TimerNode::applyOp(uint8_t op, const uint8_t* args, size_t argLen, uint32_t nowMs) {
    uint8_t result = 0;
    uint16_t fx = 0;
    uint16_t identifyMs = 5000;
    if (op == static_cast<uint8_t>(Op::Identify)) identifyMs = argLen >= 2 ? getU16(args) : 5000;
    execute(op, args, argLen, result, fx, nowMs);
    ++cmdsRun;
    if (result != static_cast<uint8_t>(CmdResult::Ok)) { ++cmdsRejected; return result; }
    if (fx & FX_SAVE) plat_.saveSettings(s_);
    if (fx & FX_IDENTIFY) plat_.identify(identifyMs);
    if (fx & FX_FACTORY) {
        plat_.eraseSettings();
        const uint8_t keepCh = s_.channel, keepNet = s_.net;
        s_ = TimerSettings();
        s_.channel = keepCh;       // stay reachable on the current channel
        s_.net = keepNet;
        engine_.setTimes(s_.onTenths, s_.offTenths);
        engine_.reset(nowMs);
        plat_.saveSettings(s_);
    }
    if (fx & FX_CHANNEL) {
        chPending_ = true;
        chDue_ = nowMs + 200;      // let our echo (and a possible retransmission) leave on the old channel first
    }
    if (fx & FX_REBOOT) {
        rebootPending_ = true;
        rebootDue_ = nowMs + 150;
    }
    if (fx & FX_INFO) infoWanted_ = true;
    return result;
}

void TimerNode::handleCmd(const uint8_t* src, const FrameView& f, uint32_t nowMs) {
    if (f.len < kCmdFixedLen) { ++rxBad; return; }
    ++rxCmds;
    const uint8_t cmdId = f.payload[0];
    const uint8_t op = f.payload[1];
    const uint8_t epoch = f.payload[2];

    const CmdDedup::Hit hit = dedup_.check(src, cmdId, nowMs);
    if (hit.duplicate) {
        // Retransmission of something we already executed (our echo was lost): just repeat the echo.
        ++cmdsDup;
        queueReply(src, nowMs, 0);
        if (op == static_cast<uint8_t>(Op::GetInfo)) queueReply(src, nowMs, 1);
        return;
    }

    infoWanted_ = false;
    uint8_t result;
    if (epoch != 0 && epoch != epochOf(static_cast<uint16_t>(boot_ > 0xFFFF ? 0xFFFF : boot_))) {
        // Issued for an earlier boot (the remote retried across a power cut): never execute it.
        result = static_cast<uint8_t>(CmdResult::Stale);
        ++cmdsRejected;
    } else {
        result = applyOp(op, f.payload + kCmdFixedLen, f.len - kCmdFixedLen, nowMs);
    }
    dedup_.record(src, cmdId, result, nowMs);
    lastCmdId_ = cmdId;
    lastCmdResult_ = result;
    lastCmdSrc_ = macTag(src);

    queueReply(src, nowMs, 0);                       // the echo that confirms the command to the remote
    if (infoWanted_) queueReply(src, nowMs, 1);
    // The unicast echo carries the new state; keep the broadcast event machinery quiet for this tick.
    lastEventMs_ = nowMs;
    prevOutput_ = engine_.outputOn(nowMs) ? 1 : 0;
}

uint8_t TimerNode::runLocal(Op op, const uint8_t* args, size_t argLen, uint32_t nowMs) {
    const uint8_t r = applyOp(static_cast<uint8_t>(op), args, argLen, nowMs);
    statusDirty_ = true;
    return r;
}

void TimerNode::applyLocalChange(uint32_t nowMs) {
    (void)nowMs;
    statusDirty_ = true;
}

void TimerNode::tick(uint32_t nowMs) {
    if (otaQuiesced_ && ota_.state() != Ota::Activating) otaQuiesced_ = false;      // the activation did not happen: carry on
    // 1. cycle engine -> event status on every output / phase change
    const bool changed = engine_.update(nowMs);
    const uint8_t out = engine_.outputOn(nowMs) ? 1 : 0;
    if (changed || out != prevOutput_) statusDirty_ = true;
    prevOutput_ = out;

    // 2. pending replies (probe answers with jitter, command echoes)
    for (Pending& p : pending_) {
        if (!p.used || static_cast<int32_t>(nowMs - p.due) < 0) continue;
        p.used = false;
        if (p.kind == 1) sendInfo(p.mac);
        else { ++txReplies; sendStatusNow(p.mac, nowMs, ST_REPLY); }
    }

    // 3. delayed channel change / reboot
    if (chPending_ && static_cast<int32_t>(nowMs - chDue_) >= 0) {
        chPending_ = false;
        if (validChannel(chValue_) && chValue_ != s_.channel) {
            s_.channel = chValue_;
            plat_.saveSettings(s_);
            plat_.setChannel(chValue_);
            bootBeats_ = 2;                   // tell the remote we arrived
            bootBeatAt_ = nowMs + 50;
        }
    }
    if (rebootPending_ && static_cast<int32_t>(nowMs - rebootDue_) >= 0) {
        rebootPending_ = false;
        plat_.reboot();
    }

    // 4. event status (output flipped...) — broadcast so every listening remote sees it
    if (statusDirty_ && static_cast<uint32_t>(nowMs - lastEventMs_) >= kEventMinGapMs) {
        statusDirty_ = false;
        lastEventMs_ = nowMs;
        sendStatusNow(nullptr, nowMs, 0);
        nextBeat_ = nowMs + kHeartbeatMs;      // the event doubles as a heartbeat
    }

    // 5. boot announcement burst
    if (bootBeats_ && static_cast<int32_t>(nowMs - bootBeatAt_) >= 0) {
        --bootBeats_;
        bootBeatAt_ = nowMs + (bootBeats_ == 1 ? 150 : 300);
        ++txBeats;
        sendStatusNow(nullptr, nowMs, ST_HEARTBEAT);
    }

    // 6. periodic heartbeat with jitter so ten timers do not beat in lock-step
    if (static_cast<int32_t>(nowMs - nextBeat_) >= 0) {
        nextBeat_ = nowMs + kHeartbeatMs - 125 + plat_.random(250);
        ++txBeats;
        sendStatusNow(nullptr, nowMs, ST_HEARTBEAT);
    }

    // 7. firmware update work (erase one sector, hash a slice, ...) - last, so everything above was already queued for this pass
    if (!otaDeferred_) ota_.tick(nowMs);
}

bool TimerNode::otaSend(const uint8_t* mac, const uint8_t* payload, size_t len) {
    if (memcmp(mac, kUsbMac, 6) == 0) {
        if (!usbSink_) return false;
        usbSink_->onOtaReply(payload, len);
        return true;
    }
    return sendFrame(mac, Type::Ota, payload, len);
}

}  // namespace Fog
