#include "FogRemoteLink.h"

#include <stdio.h>


namespace Fog {

namespace {
constexpr uint32_t kDiscoverDwellMs = 130;
constexpr uint32_t kFindStageGuardMs = 400;
constexpr uint32_t kRecoverFirstMs = 8000;
constexpr uint32_t kRecoverMaxMs = 120000;
constexpr uint32_t kEchoGraceForLinkTest = 250;

inline bool seqBefore(uint16_t a, uint16_t b) { return static_cast<int16_t>(a - b) < 0; }
}  // namespace

// ------------------------------------------------------------------ setup / fleet
void RemoteLink::begin(const uint8_t ownMac[6], uint8_t net, uint8_t homeChannel, uint32_t nowMs) {
    memcpy(own_, ownMac, 6);
    ownTag_ = macTag(ownMac);
    net_ = net;
    home_ = validChannel(homeChannel) ? homeChannel : kDefaultChannel;
    now_ = nowMs;
    bootMs_ = nowMs;
    port_.setChannel(home_);
    cmdSeq_ = static_cast<uint8_t>(port_.random(255));
    probePending_ = true;                    // find out who is around right away
    nextOnlineCheck_ = nowMs + 200;
    for (int i = 0; i < count_; ++i) {      // give known timers time to announce themselves before searching for them
        dev_[i].nextRecoverAt = nowMs + 15000;
        dev_[i].recoverBackoffMs = kRecoverFirstMs;
    }
}

int RemoteLink::findDevice(const uint8_t mac[6]) const {
    for (int i = 0; i < count_; ++i) if (memcmp(dev_[i].mac, mac, 6) == 0) return i;
    return -1;
}

int RemoteLink::addDevice(const uint8_t mac[6], const char* name, uint8_t chHint) {
    int idx = findDevice(mac);
    if (idx >= 0) return idx;
    if (count_ >= kMaxTimers) return -1;
    idx = count_++;
    Device& d = dev_[idx];
    d = Device();
    d.used = true;
    memcpy(d.mac, mac, 6);
    sanitizeName(d.name, name ? name : "", name ? strlen(name) : 0);
    d.chHint = validChannel(chHint) ? chHint : home_;
    d.nextRecoverAt = now_ + 15000;
    // Seed from what we already heard of it.
    for (const Heard& h : heard_) {
        if (h.used && memcmp(h.mac, mac, 6) == 0 && !d.name[0]) sanitizeName(d.name, h.name, kNameLen);
    }
    return idx;
}

void RemoteLink::removeDevice(int idx) {
    if (idx < 0 || idx >= count_) return;
    for (int j = 0; j < kMaxJobs; ++j) {
        if (!jobs_[j].used) continue;
        if (jobs_[j].dev == idx) { jobs_[j].inFlight = false; completeJob(j, JobResult::Cancelled, now_); }
    }
    for (int j = 0; j < kMaxJobs; ++j) if (jobs_[j].used && jobs_[j].dev > idx) --jobs_[j].dev;
    if (lt_.active && lt_.dev == idx) {
        lt_.active = false;
        if (ev_) { LinkTestResult r; r.dev = lt_.dev; r.sent = lt_.sent; r.macAck = lt_.macAck; r.echo = lt_.echo; r.rttSumMs = lt_.rttSum; r.rttMax = lt_.rttMax; r.rssiAvg = 0; r.done = true; ev_->onLinkTest(r); }
    }
    if (lt_.active && lt_.dev > idx) --lt_.dev;
    // Anything that holds device indices must not point at the wrong timer after the list shifted.
    if (exc_.kind != Exc::None) endExcursion(now_);
    recoverReq_ = false;
    if (mig_.active) {
        for (uint8_t k = 0; k < mig_.total; ++k) {
            if (mig_.dev[k] == idx) mig_.dev[k] = -1;
            else if (mig_.dev[k] > idx) --mig_.dev[k];
        }
    }
    if (txDev_ == idx) txDev_ = -1; else if (txDev_ > idx) --txDev_;
    for (int k = otaCount_ - 1; k >= 0; --k) {            // queued update frames follow the list; those for the removed timer are dropped
        if (ota_[k].dev == idx) { if (ota_[k].inFlight) ota_[k].dev = -1; else dropOta(k, false, false); }
        else if (ota_[k].dev > idx) --ota_[k].dev;
    }
    for (int i = idx; i + 1 < count_; ++i) dev_[i] = dev_[i + 1];
    dev_[count_ - 1] = Device();
    --count_;
    if (ev_) ev_->onRemoved(idx);
}

bool RemoteLink::isOnline(int i, uint32_t nowMs) const {
    if (i < 0 || i >= count_) return false;
    const Device& d = dev_[i];
    return d.statusValid && static_cast<uint32_t>(nowMs - d.lastRxMs) < kOnlineWindowMs;
}

int RemoteLink::onlineCount(uint32_t nowMs) const {
    int n = 0;
    for (int i = 0; i < count_; ++i) if (isOnline(i, nowMs)) ++n;
    return n;
}

void RemoteLink::predict(int i, uint32_t nowMs, bool& phaseOn, uint32_t& elapsedMs) const {
    phaseOn = false;
    elapsedMs = 0;
    if (i < 0 || i >= count_ || !dev_[i].statusValid) return;
    const Device& d = dev_[i];
    phaseOn = (d.st.flags & ST_PHASE_ON) != 0;
    elapsedMs = d.st.elapsedMs;
    if (d.st.mode != static_cast<uint8_t>(Mode::Auto) || !isOnline(i, nowMs)) return;
    uint32_t el = elapsedMs + static_cast<uint32_t>(nowMs - d.statusMs);
    for (int k = 0; k < 8; ++k) {
        const uint32_t dur = (phaseOn ? d.st.onTenths : d.st.offTenths) * 100u;
        if (dur == 0 || el < dur) break;
        el -= dur;
        phaseOn = !phaseOn;
    }
    const uint32_t dur = (phaseOn ? d.st.onTenths : d.st.offTenths) * 100u;
    if (dur && el >= dur) el = dur - 1;
    elapsedMs = el;
}

// ------------------------------------------------------------------ jobs
uint8_t RemoteLink::nextCmdId() {
    ++cmdSeq_;
    if (cmdSeq_ == 0) ++cmdSeq_;
    return cmdSeq_;
}

int RemoteLink::queuedJobs() const {
    int n = 0;
    for (const Job& j : jobs_) if (j.used) ++n;
    return n;
}

int RemoteLink::pendingFor(int dev) const {
    int n = 0;
    for (const Job& j : jobs_) if (j.used && j.dev == dev) ++n;
    return n;
}

void RemoteLink::cancelDevice(int dev) {
    for (int j = 0; j < kMaxJobs; ++j) {
        if (jobs_[j].used && jobs_[j].dev == dev) { jobs_[j].inFlight = false; completeJob(j, JobResult::Cancelled, now_); }
    }
}

int RemoteLink::headJob(int dev) const {
    int best = -1;
    for (int j = 0; j < kMaxJobs; ++j) {
        if (!jobs_[j].used || jobs_[j].dev != dev) continue;
        if (best < 0 || seqBefore(jobs_[j].id, jobs_[best].id)) best = j;
    }
    return best;
}

int RemoteLink::submit(int dev, Op op, const void* args, size_t argLen, uint32_t ttlMs, uint32_t tag, bool confirm) {
    if (dev < 0 || dev >= count_ || argLen > kMaxCmdArgs) return -1;
    const bool coalesce = op == Op::SetTimes || op == Op::SetName || op == Op::SetStartup || op == Op::SetMode || op == Op::GetInfo;
    if (coalesce) {
        // The newest value wins: replace an older, not yet transmitted job of the same kind.
        for (int j = 0; j < kMaxJobs; ++j) {
            Job& o = jobs_[j];
            if (o.used && o.dev == dev && o.op == static_cast<uint8_t>(op) && !o.inFlight && o.attempts == 0 && o.tag == tag && o.channel == 0) {
                completeJob(j, JobResult::Cancelled, now_);
            }
        }
    }
    int slot = -1;
    for (int j = 0; j < kMaxJobs; ++j) if (!jobs_[j].used) { slot = j; break; }
    if (slot < 0) return -1;
    Job& job = jobs_[slot];
    job = Job();
    job.used = true;
    job.id = ++jobSeq_;
    if (job.id == 0) job.id = ++jobSeq_;
    job.dev = static_cast<int8_t>(dev);
    job.cmdId = nextCmdId();
    job.op = static_cast<uint8_t>(op);
    job.argLen = static_cast<uint8_t>(argLen);
    if (argLen && args) memcpy(job.args, args, argLen);
    job.confirm = confirm;
    job.tag = tag;
    if (dev_[dev].statusValid && opBoundToBoot(op, argLen ? job.args[0] : 0)) job.epoch = epochOf(dev_[dev].st.bootCount);
    job.created = now_;
    job.deadline = now_ + ttlMs;
    job.nextSend = now_;
    ++cnt_.cmds;
    ++dev_[dev].stats.cmds;
    return job.id;
}

int RemoteLink::submitSetTimes(int dev, uint32_t onTenths, uint32_t offTenths, uint32_t tag) {
    uint8_t a[8];
    putU32(a, onTenths);
    putU32(a + 4, offTenths);
    return submit(dev, Op::SetTimes, a, sizeof(a), kDefaultTtlMs, tag);
}

int RemoteLink::submitMode(int dev, Mode mode, uint16_t leaseMs, bool confirm) {
    uint8_t a[3] = {static_cast<uint8_t>(mode), 0, 0};
    putU16(a + 1, leaseMs);
    // A lease refresh must be cheap and short lived; a mode *change* gets the full budget.
    return submit(dev, Op::SetMode, a, sizeof(a), confirm ? kDefaultTtlMs : 350, TagNone, confirm);
}

int RemoteLink::submitName(int dev, const char* name) {
    char n[kNameLen];
    sanitizeName(n, name ? name : "", name ? strlen(name) : 0);
    if (!n[0]) return -1;
    return submit(dev, Op::SetName, n, kNameLen);
}

int RemoteLink::submitPhase(int dev, bool on, uint32_t elapsedMs) {
    uint8_t a[5] = {static_cast<uint8_t>(on ? 1 : 0), 0, 0, 0, 0};
    putU32(a + 1, elapsedMs);
    return submit(dev, Op::SetPhase, a, sizeof(a));
}

int RemoteLink::submitChannel(int dev, uint8_t ch, uint32_t tag) {
    return submit(dev, Op::SetChannel, &ch, 1, kDefaultTtlMs, tag);
}

int RemoteLink::submitIdentify(int dev, uint16_t ms) {
    uint8_t a[2];
    putU16(a, ms);
    return submit(dev, Op::Identify, a, sizeof(a));
}

int RemoteLink::submitFactoryReset(int dev) {
    const uint8_t m = kFactoryMagic;
    return submit(dev, Op::FactoryReset, &m, 1);
}

int RemoteLink::submitReboot(int dev) {
    const uint8_t m = kRebootMagic;
    return submit(dev, Op::Reboot, &m, 1);
}

int RemoteLink::submitStartup(int dev, bool startOn) {
    const uint8_t v = startOn ? 1 : 0;
    return submit(dev, Op::SetStartup, &v, 1);
}

void RemoteLink::completeJob(int j, JobResult r, uint32_t now) {
    Job job = jobs_[j];
    jobs_[j] = Job();
    if (job.dev >= 0 && job.dev < count_) {
        DevLinkStats& s = dev_[job.dev].stats;
        switch (r) {
            case JobResult::Ok: ++s.cmdsOk; ++cnt_.cmdsOk; break;
            case JobResult::Rejected: ++s.cmdsRejected; ++cnt_.cmdsRejected; break;
            case JobResult::Timeout: ++s.cmdsTimeout; ++cnt_.cmdsTimeout; break;
            case JobResult::Cancelled: break;
        }
        if (r == JobResult::Ok && job.attempts > 0 && job.confirm) {
            const uint32_t rtt = now - job.firstSent;
            s.rttSumMs += rtt; ++s.rttCount; if (rtt > s.rttMax) s.rttMax = rtt;
            cnt_.rttSumMs += rtt; ++cnt_.rttCount; if (rtt > cnt_.rttMax) cnt_.rttMax = rtt;
        }
    }
    JobDone d;
    d.jobId = job.id;
    d.dev = job.dev;
    d.op = job.op;
    d.result = r;
    d.attempts = job.attempts;
    d.ms = now - job.created;
    d.tag = job.tag;
    if ((job.tag & 0xFFFF0000u) == TagMigrate) migrateJobDone(d, now);
    if ((job.tag & 0xFFFF0000u) == TagRecover && exc_.kind == Exc::Find && job.id == exc_.cmdJob) exc_.cmdDone = true;
    if (ev_) ev_->onJobDone(d);
}

uint32_t RemoteLink::retryDelay(const Device& d, bool macOk, uint8_t attempts) {
    if (macOk) return attempts > 4 ? 160 : kEchoWaitMs;
    const uint32_t base = d.failStreak < 3 ? 12 : (d.failStreak < 6 ? 100 : 250);
    return base + port_.random(base * 3 / 4 + 1);
}

// ------------------------------------------------------------------ tx plumbing
bool RemoteLink::sendRaw(const uint8_t* mac, Type type, const void* payload, size_t len, bool tracked) {
    (void)tracked;
    uint8_t buf[kMaxFrame];
    const size_t n = encodeFrame(buf, sizeof(buf), net_, type, txSeq_, payload, len);
    if (!n) return false;
    if (!port_.send(mac, buf, n)) return false;
    ++txSeq_;
    ++cnt_.tx;
    txUnicast_ = mac != nullptr;
    txCh_ = port_.channel();
    return true;
}

void RemoteLink::onTxResult(bool ok, uint32_t nowMs) {
    now_ = nowMs;
    if (!txBusy_) return;
    txBusy_ = false;
    lastTxDone_ = nowMs;
    if (txUnicast_) {
        if (ok) ++cnt_.txOk; else ++cnt_.txFail;
        if (txKind_ != TxKind::Find && txCh_ <= kMaxChannel) {   // search probes are expected to fail: keep them out of the channel statistics
            ++cnt_.chTx[txCh_];
            if (!ok) ++cnt_.chFail[txCh_];
        }
        if (txDev_ >= 0 && txDev_ < count_) {
            Device& d = dev_[txDev_];
            if (ok) { ++d.stats.macOk; d.failStreak = 0; }
            else { ++d.stats.macFail; if (d.failStreak < 250) ++d.failStreak; }
        }
    }
    switch (txKind_) {
        case TxKind::Job:
            for (int j = 0; j < kMaxJobs; ++j) {
                if (jobs_[j].used && jobs_[j].id == txJob_ && jobs_[j].inFlight) { afterTxForJob(j, ok, nowMs); break; }
            }
            break;
        case TxKind::Find:
            if (exc_.kind == Exc::Find && exc_.stage == 2) {
                if (ok) { exc_.foundCh = port_.channel(); exc_.stage = 3; }
                else { ++exc_.idx; exc_.stage = 0; }
            }
            break;
        case TxKind::LinkTest:
            lt_.txPending = false;
            if (ok) ++lt_.macAck;
            break;
        case TxKind::Ota:
            if (otaCount_ > 0 && ota_[0].inFlight) {
                if (ok) ++cnt_.otaTxAcked;
                dropOta(0, true, ok);
            }
            break;
        default:
            break;
    }
    txKind_ = TxKind::None;
    txJob_ = 0;
    txDev_ = -1;
}

void RemoteLink::afterTxForJob(int j, bool ok, uint32_t now) {
    Job& job = jobs_[j];
    job.inFlight = false;
    if (job.dev < 0 || job.dev >= count_) { completeJob(j, JobResult::Cancelled, now); return; }
    Device& d = dev_[job.dev];
    if (!job.confirm) {
        if (ok) { completeJob(j, JobResult::Ok, now); return; }
        if (job.attempts < 3 && static_cast<int32_t>(job.deadline - now) > 0) { job.nextSend = now + retryDelay(d, false, job.attempts); return; }
        completeJob(j, JobResult::Timeout, now);
        return;
    }
    job.nextSend = now + retryDelay(d, ok, job.attempts);
}

bool RemoteLink::trySendJob(uint32_t now) {
    if (count_ == 0) return false;
    int pick = -1;
    if (exc_.kind == Exc::Find) {
        if (exc_.stage != 4) return false;
        for (int j = 0; j < kMaxJobs; ++j) if (jobs_[j].used && jobs_[j].id == exc_.cmdJob) { pick = j; break; }
        if (pick < 0 || static_cast<int32_t>(now - jobs_[pick].nextSend) < 0) return false;
    } else if (exc_.kind != Exc::None) {
        return false;
    } else {
        for (int k = 0; k < count_; ++k) {
            const int d = (rr_ + k) % count_;
            const int j = headJob(d);
            if (j < 0 || jobs_[j].channel != 0) continue;
            if (static_cast<int32_t>(now - jobs_[j].nextSend) < 0) continue;
            pick = j;
            rr_ = d + 1;
            break;
        }
    }
    if (pick < 0) return false;
    Job& job = jobs_[pick];
    if (job.attempts >= kMaxAttempts) return false;      // wait for the deadline
    uint8_t payload[kCmdFixedLen + kMaxCmdArgs];
    payload[0] = job.cmdId;
    payload[1] = job.op;
    payload[2] = job.epoch;
    if (job.argLen) memcpy(payload + kCmdFixedLen, job.args, job.argLen);
    if (!sendRaw(dev_[job.dev].mac, Type::Cmd, payload, kCmdFixedLen + job.argLen, true)) {
        job.nextSend = now + 5;
        return false;
    }
    txBusy_ = true;
    txStart_ = now;
    txKind_ = TxKind::Job;
    txJob_ = job.id;
    txDev_ = job.dev;
    job.inFlight = true;
    ++job.attempts;
    if (job.attempts == 1) job.firstSent = now;
    else { ++cnt_.retries; ++dev_[job.dev].stats.retries; }
    return true;
}

// ------------------------------------------------------------------ rx
void RemoteLink::onFrame(const uint8_t* mac, int8_t rssi, const uint8_t* data, size_t len, uint32_t nowMs, uint8_t rxChannel) {
    now_ = nowMs;
    FrameView f;
    const Decode d = decodeFrame(data, len, f);
    if (d != Decode::Ok) {
        if (d == Decode::BadMagic) ++cnt_.rxForeign; else ++cnt_.rxBad;
        return;
    }
    if (f.net != net_) { ++cnt_.rxForeign; return; }
    ++cnt_.rx;
    const uint8_t ch = rxChannel ? rxChannel : port_.channel();
    if (ch <= kMaxChannel) ++cnt_.chRx[ch];

    if (f.type == Type::Status) {
        StatusPayload st;
        if (!readPayload(f, st)) { ++cnt_.rxBad; return; }
        st.name[kNameLen - 1] = 0;
        handleStatus(mac, rssi, f.seq, st, nowMs, ch);
    } else if (f.type == Type::Info) {
        InfoPayload info;
        if (!readPayload(f, info)) { ++cnt_.rxBad; return; }
        handleInfo(mac, info, nowMs);
    } else if (f.type == Type::Ota) {
        const int idx = findDevice(mac);
        if (idx >= 0) {
            ++cnt_.otaRx;
            if (ev_) ev_->onOtaFrame(idx, rssi, f.payload, f.len);
        }
    }
}

void RemoteLink::touchHeard(const uint8_t* mac, int8_t rssi, const StatusPayload& st, uint32_t now) {
    Heard* slot = nullptr;
    for (Heard& h : heard_) if (h.used && memcmp(h.mac, mac, 6) == 0) { slot = &h; break; }
    if (!slot) {
        for (Heard& h : heard_) if (!h.used) { slot = &h; break; }
        if (!slot) {
            slot = &heard_[0];
            for (Heard& h : heard_) if (static_cast<uint32_t>(now - h.lastMs) > static_cast<uint32_t>(now - slot->lastMs)) slot = &h;
        }
        *slot = Heard();
        slot->used = true;
        memcpy(slot->mac, mac, 6);
    }
    memcpy(slot->name, st.name, kNameLen);
    slot->name[kNameLen - 1] = 0;
    if (rssi < 0 && rssi > -120) slot->rssi = slot->rssi ? static_cast<int8_t>((slot->rssi * 3 + rssi) / 4) : rssi;
    slot->channel = st.channel;
    slot->lastMs = now;
    slot->onTenths = st.onTenths;
    slot->offTenths = st.offTenths;
    slot->outputOn = (st.flags & ST_OUTPUT_ON) != 0;
    slot->boot = st.bootCount;
    slot->fwMajor = st.fwMajor;
    slot->fwMinor = st.fwMinor;
    if (discovery_ && ev_ && static_cast<uint32_t>(now - slot->emitMs) >= 1000) {
        slot->emitMs = now;
        ev_->onHeard(*slot);
    }
}

void RemoteLink::handleStatus(const uint8_t* mac, int8_t rssi, uint8_t seq, const StatusPayload& st, uint32_t now, uint8_t rxCh) {
    touchHeard(mac, rssi, st, now);
    const int idx = findDevice(mac);
    if (idx < 0) return;
    Device& d = dev_[idx];

    // A frame from the target while parked on an excursion channel: found it.
    if (exc_.kind == Exc::Find && exc_.dev == idx && (exc_.stage == 1 || exc_.stage == 2) && rxCh == port_.channel()) {
        exc_.foundCh = rxCh;       // heard on the channel we are parked on (not a queued frame from the home channel)
        exc_.stage = 3;
    }

    const bool wasOnline = d.wasOnline;
    d.loss.onFrame(seq, now);
    d.rssi.add(rssi);
    if (rssi < 0 && rssi > -120) {
        DevLinkStats& s = d.stats;
        if (!s.rssiCount || rssi < s.rssiMin) s.rssiMin = rssi;
        if (!s.rssiCount || rssi > s.rssiMax) s.rssiMax = rssi;
        s.rssiSum += rssi;
        if (++s.rssiCount >= 100000) { s.rssiSum /= 2; s.rssiCount /= 2; }
    }
    if (st.rssiAtTimer < 0) d.rssiT = st.rssiAtTimer;
    d.lastRxMs = now;
    d.statusMs = now;
    d.st = st;
    d.statusValid = true;
    d.failStreak = 0;
    if (validChannel(st.channel)) d.chHint = st.channel;
    bool changed = true;

    if (d.bootKnown && d.lastBoot != st.bootCount) {
        ++d.stats.reboots;
        d.rebootedMs = now;
        if (ev_) ev_->onRebooted(idx);
    }
    d.lastBoot = st.bootCount;
    d.bootKnown = true;

    if (st.name[0] && memcmp(d.name, st.name, kNameLen) != 0) {
        bool renaming = false;
        for (const Job& j : jobs_) if (j.used && j.dev == idx && j.op == static_cast<uint8_t>(Op::SetName)) renaming = true;
        if (!renaming) { memcpy(d.name, st.name, kNameLen); d.name[kNameLen - 1] = 0; }
    }

    // Command echo -> confirm the head job.
    if ((st.flags & ST_REPLY) && st.lastCmdId != 0 && st.lastCmdSrc == ownTag_) {      // heartbeats repeat the last echo: only direct replies count
        for (int j = 0; j < kMaxJobs; ++j) {
            if (jobs_[j].used && jobs_[j].dev == idx && jobs_[j].cmdId == st.lastCmdId && jobs_[j].attempts > 0 && jobs_[j].confirm) {
                completeJob(j, st.lastCmdResult == static_cast<uint8_t>(CmdResult::Ok) ? JobResult::Ok : JobResult::Rejected, now);
                break;
            }
        }
    }

    // Link test echo.
    if (lt_.active && lt_.waiting && lt_.dev == idx && (st.flags & ST_REPLY) && !lt_.echoed) {
        lt_.echoed = true;
        ++lt_.echo;
        const uint32_t rtt = now - lt_.sentAt;
        lt_.rttSum += rtt;
        if (rtt > lt_.rttMax) lt_.rttMax = rtt;
        if (rssi < 0) { lt_.rssiSum += rssi; ++lt_.rssiN; }
    }

    if (!wasOnline) {
        if (d.offlineSince) d.stats.offlineMs += now - d.offlineSince;
        d.offlineSince = 0;
        d.wasOnline = true;
        d.recoverBackoffMs = kRecoverFirstMs;
        d.nextRecoverAt = now + kRecoverFirstMs;
        if (ev_) ev_->onOnlineChanged(idx, true);
    }
    if (changed && ev_) ev_->onDeviceChanged(idx);
}

void RemoteLink::handleInfo(const uint8_t* mac, const InfoPayload& info, uint32_t now) {
    const int idx = findDevice(mac);
    if (idx < 0) return;
    dev_[idx].info = info;
    dev_[idx].infoValid = true;
    dev_[idx].infoMs = now;
    if (ev_) ev_->onInfo(idx);
}

// ------------------------------------------------------------------ periodic work
void RemoteLink::checkOnline(uint32_t now) {
    for (int i = 0; i < count_; ++i) {
        Device& d = dev_[i];
        const bool on = isOnline(i, now);
        if (d.wasOnline && !on) {
            d.wasOnline = false;
            d.offlineSince = d.lastRxMs ? d.lastRxMs + kOnlineWindowMs : now;
            ++d.stats.offlineEvents;
            d.nextRecoverAt = now + kRecoverFirstMs;
            d.recoverBackoffMs = kRecoverFirstMs;
            if (ev_) ev_->onOnlineChanged(i, false);
        }
    }
}

void RemoteLink::probeNow() { probePending_ = true; }

bool RemoteLink::startDiscovery(bool sweepChannels) {
    if (otaHeld()) return false;
    discovery_ = true;
    discoverySweep_ = sweepChannels;
    nextDiscoveryProbe_ = now_;
    nextSweepAt_ = now_ + 1500;
    for (Heard& h : heard_) h.emitMs = 0;
    return true;
}

void RemoteLink::stopDiscovery() {
    discovery_ = false;
    if (exc_.kind == Exc::Discover) endExcursion(now_);
}

bool RemoteLink::startRecover(int dev) {
    if (otaHeld()) return false;
    recoverReq_ = true;
    recoverDev_ = static_cast<int8_t>(dev);
    return true;
}

void RemoteLink::setHome(uint8_t ch) {
    home_ = ch;
    port_.setChannel(ch);
    if (ev_) ev_->onHomeChannel(ch);
}

void RemoteLink::resetCounters() {
    cnt_ = RemoteCounters();
    for (int i = 0; i < count_; ++i) {
        dev_[i].stats = DevLinkStats();
        dev_[i].loss.resetTotals();
    }
}

void RemoteLink::tick(uint32_t now) {
    now_ = now;

    // radio send callback missing -> don't wedge
    if (txBusy_ && static_cast<uint32_t>(now - txStart_) > kTxWatchdogMs) onTxResult(false, now);

    // expire jobs
    for (int j = 0; j < kMaxJobs; ++j) {
        Job& job = jobs_[j];
        if (job.used && !job.inFlight && static_cast<int32_t>(now - job.deadline) >= 0) completeJob(j, JobResult::Timeout, now);
    }

    if (static_cast<int32_t>(now - nextOnlineCheck_) >= 0) { nextOnlineCheck_ = now + 200; checkOnline(now); }

    if (exc_.kind != Exc::None) {
        // user commands pre-empt a discovery sweep; a recovery runs to completion (it is short)
        if (exc_.kind == Exc::Discover && queuedJobs() > 0 && !txBusy_) endExcursion(now);
        else { tickExcursion(now); if (exc_.kind != Exc::None) { if (!txBusy_) trySendJob(now); return; } }
    }

    if (!txBusy_) trySendJob(now);
    if (!txBusy_) trySendOta(now);

    if (!txBusy_ && probePending_) {
        ProbePayload p = {static_cast<uint8_t>(port_.random(256)), kProbeJitter};
        if (sendRaw(nullptr, Type::Probe, &p, sizeof(p), false)) { probePending_ = false; txBusy_ = true; txStart_ = now; txKind_ = TxKind::Other; txDev_ = -1; }
    }

    tickDiscovery(now);
    tickLinkTest(now);

    if (!txBusy_ && recoverReq_ && exc_.kind == Exc::None && !mig_.active && !otaHeld()) {
        recoverReq_ = false;
        exc_ = Excursion();
        exc_.qLen = 0;
        if (recoverDev_ >= 0 && recoverDev_ < count_) {
            exc_.queue[exc_.qLen++] = recoverDev_;
        } else {
            for (int i = 0; i < count_; ++i) if (!isOnline(i, now)) exc_.queue[exc_.qLen++] = static_cast<int8_t>(i);
        }
        if (exc_.qLen) { ++cnt_.recovers; beginExcursion(Exc::Find, now); }
    }

    tickAutoRecover(now);
}

void RemoteLink::tickDiscovery(uint32_t now) {
    if (!discovery_ || txBusy_ || exc_.kind != Exc::None) return;
    if (static_cast<int32_t>(now - nextDiscoveryProbe_) >= 0) {
        nextDiscoveryProbe_ = now + 1000;
        ProbePayload p = {static_cast<uint8_t>(port_.random(256)), kProbeJitter};
        if (sendRaw(nullptr, Type::Probe, &p, sizeof(p), false)) { txBusy_ = true; txStart_ = now; txKind_ = TxKind::Other; txDev_ = -1; }
        return;
    }
    if (discoverySweep_ && queuedJobs() == 0 && !otaHeld() && static_cast<int32_t>(now - nextSweepAt_) >= 0) {
        nextSweepAt_ = now + 4500;
        beginExcursion(Exc::Discover, now);
    }
}

void RemoteLink::tickAutoRecover(uint32_t now) {
    if (!autoRecover_ || exc_.kind != Exc::None || txBusy_ || mig_.active || lt_.active || discovery_ || recoverReq_ || otaHeld()) return;
    if (queuedJobs() > 0) return;
    for (int i = 0; i < count_; ++i) {
        Device& d = dev_[i];
        if (d.wasOnline || isOnline(i, now)) continue;      // only timers already declared offline (flag set by checkOnline)
        if (static_cast<int32_t>(now - d.nextRecoverAt) < 0) continue;
        d.nextRecoverAt = now + d.recoverBackoffMs;
        d.recoverBackoffMs = d.recoverBackoffMs * 2 > kRecoverMaxMs ? kRecoverMaxMs : d.recoverBackoffMs * 2;
        startRecover(i);
        return;
    }
}

// ------------------------------------------------------------------ excursions (temporarily leave the home channel)
void RemoteLink::buildChannelList(int dev) {
    exc_.count = 0;
    exc_.idx = 0;
    auto add = [&](uint8_t ch) {
        if (!validChannel(ch) || ch == home_) return;
        for (uint8_t i = 0; i < exc_.count; ++i) if (exc_.list[i] == ch) return;
        exc_.list[exc_.count++] = ch;
    };
    if (dev >= 0 && dev < count_) add(dev_[dev].chHint);
    for (uint8_t ch = kMinChannel; ch <= kMaxChannel; ++ch) add(ch);
}

void RemoteLink::beginExcursion(Exc kind, uint32_t now) {
    if (txBusy_ || exc_.kind != Exc::None) return;
    ++cnt_.sweeps;
    exc_.kind = kind;
    exc_.stage = 0;
    exc_.stageStart = now;
    exc_.began = now;
    exc_.sendFails = 0;
    if (kind == Exc::Discover) {
        exc_.dev = -1;
        buildChannelList(-1);
    } else {
        exc_.dev = exc_.queue[exc_.qIdx];
        buildChannelList(exc_.dev);
    }
}

void RemoteLink::endExcursion(uint32_t now) {
    (void)now;
    exc_ = Excursion();
    port_.setChannel(home_);
    resyncLoss();                   // we could not hear the heartbeats meanwhile: that is not packet loss
    probePending_ = true;           // refresh the fleet picture right after coming home
}

void RemoteLink::tickExcursion(uint32_t now) {
    // Never stay away from the home channel for long, whatever goes wrong inside a state machine.
    if (exc_.kind != Exc::None && static_cast<uint32_t>(now - exc_.began) > 15000) { endExcursion(now); return; }
    switch (exc_.kind) {
        case Exc::None:
            return;
        case Exc::Discover:
            if (exc_.stage == 0) {
                if (txBusy_) return;
                if (exc_.idx >= exc_.count) { endExcursion(now); return; }
                port_.setChannel(exc_.list[exc_.idx]);
                ProbePayload p = {static_cast<uint8_t>(port_.random(256)), kProbeJitter};
                if (sendRaw(nullptr, Type::Probe, &p, sizeof(p), false)) { txBusy_ = true; txStart_ = now; txKind_ = TxKind::Other; txDev_ = -1; }
                exc_.stageStart = now;
                exc_.stage = 1;
            } else if (static_cast<uint32_t>(now - exc_.stageStart) >= kDiscoverDwellMs) {
                ++exc_.idx;
                exc_.stage = 0;
            }
            return;
        case Exc::Find: {
            switch (exc_.stage) {
                case 0:   // hop to the next candidate channel
                    if (txBusy_) return;
                    if (exc_.idx >= exc_.count) {   // exhausted: next device or finish
                        ++exc_.qIdx;
                        if (exc_.qIdx < exc_.qLen) {
                            exc_.dev = exc_.queue[exc_.qIdx];
                            buildChannelList(exc_.dev);
                            exc_.stage = 0;
                            return;
                        }
                        endExcursion(now);
                        return;
                    }
                    port_.setChannel(exc_.list[exc_.idx]);
                    exc_.stage = 1;
                    exc_.stageStart = now;
                    return;
                case 1:   // unicast probe: the MAC ack tells us instantly whether the timer listens here
                    if (txBusy_) return;
                    if (exc_.dev < 0 || exc_.dev >= count_) { endExcursion(now); return; }
                    {
                        ProbePayload p = {static_cast<uint8_t>(port_.random(256)), 0};
                        if (sendRaw(dev_[exc_.dev].mac, Type::Probe, &p, sizeof(p), false)) {
                            txBusy_ = true; txStart_ = now; txKind_ = TxKind::Find; txDev_ = exc_.dev;
                            exc_.stage = 2;
                            exc_.stageStart = now;
                            exc_.sendFails = 0;
                        } else if (++exc_.sendFails >= 5) {      // the radio refuses frames on this channel: try the next one
                            exc_.sendFails = 0;
                            ++exc_.idx;
                            exc_.stage = 0;
                        }
                    }
                    return;
                case 2:   // waiting for the tx result (handled in onTxResult)
                    if (!txBusy_ && static_cast<uint32_t>(now - exc_.stageStart) > kFindStageGuardMs) { ++exc_.idx; exc_.stage = 0; }
                    return;
                case 3: { // found: tell it to come home
                    ++cnt_.found;
                    uint8_t ch = home_;
                    exc_.cmdDone = false;
                    const int id = submit(exc_.dev, Op::SetChannel, &ch, 1, 900, TagRecover);
                    if (id <= 0) { endExcursion(now); return; }
                    for (int j = 0; j < kMaxJobs; ++j) if (jobs_[j].used && jobs_[j].id == id) jobs_[j].channel = exc_.foundCh;
                    exc_.cmdJob = static_cast<uint16_t>(id);
                    exc_.stage = 4;
                    exc_.stageStart = now;
                    return;
                }
                case 4:
                    if (exc_.cmdDone) {
                        ++exc_.qIdx;
                        if (exc_.qIdx < exc_.qLen) {
                            exc_.dev = exc_.queue[exc_.qIdx];
                            buildChannelList(exc_.dev);
                            exc_.stage = 0;
                            port_.setChannel(home_);
                        } else {
                            endExcursion(now);
                        }
                    }
                    return;
            }
            return;
        }
    }
}

// ------------------------------------------------------------------ fleet channel migration
bool RemoteLink::startMigrate(uint8_t ch, bool force) {
    if (otaHeld() || mig_.active || exc_.kind != Exc::None || !validChannel(ch) || ch == home_) return false;
    mig_ = Migrate();
    mig_.active = true;
    mig_.force = force;
    mig_.ch = ch;
    mig_.oldCh = home_;
    for (int i = 0; i < count_; ++i) {
        if (!isOnline(i, now_)) continue;
        const int id = submitChannel(i, ch, TagMigrate | static_cast<uint32_t>(i));
        if (id > 0) mig_.dev[mig_.total++] = static_cast<int8_t>(i);
    }
    if (ev_) ev_->onMigrate(0, -1, true, ch, mig_.total, 0, false);
    if (mig_.total == 0) finishMigrate(now_);
    return true;
}

void RemoteLink::migrateJobDone(const JobDone& d, uint32_t now) {
    if (!mig_.active) return;
    const int idx = static_cast<int>(d.tag & 0xFFu);
    const bool ok = d.result == JobResult::Ok;
    if (ok) { ++mig_.ok; for (uint8_t k = 0; k < mig_.total; ++k) if (mig_.dev[k] == idx) mig_.devOk[k] = true; }
    else ++mig_.failed;
    ++mig_.done;
    if (ev_) ev_->onMigrate(1, idx, ok, mig_.ch, mig_.ok, mig_.failed, false);
    if (mig_.done >= mig_.total) finishMigrate(now);
}

void RemoteLink::finishMigrate(uint32_t now) {
    (void)now;
    const bool commit = mig_.ok > 0 || mig_.force || count_ == 0;
    if (commit) {
        for (uint8_t k = 0; k < mig_.total; ++k) {
            const int i = mig_.dev[k];
            if (i >= 0 && i < count_ && mig_.devOk[k]) dev_[i].chHint = mig_.ch;
        }
        setHome(mig_.ch);
        ++cnt_.migrations;
        probePending_ = true;
    }
    const uint8_t ch = mig_.ch, ok = mig_.ok, failed = mig_.failed;
    mig_.active = false;
    if (ev_) ev_->onMigrate(2, -1, commit, ch, ok, failed, commit);
}

// ------------------------------------------------------------------ link test
bool RemoteLink::startLinkTest(int dev, uint16_t count, uint16_t gapMs) {
    if (otaHeld() || lt_.active || dev < 0 || dev >= count_) return false;
    lt_ = LinkTest();
    lt_.active = true;
    lt_.dev = static_cast<int8_t>(dev);
    lt_.count = count < 1 ? 1 : (count > 200 ? 200 : count);
    lt_.gap = gapMs < 20 ? 20 : (gapMs > 1000 ? 1000 : gapMs);
    lt_.nextAt = now_;
    return true;
}

void RemoteLink::tickLinkTest(uint32_t now) {
    if (!lt_.active) return;
    auto report = [&](bool done) {
        if (!ev_) return;
        LinkTestResult r;
        r.dev = lt_.dev;
        r.sent = lt_.sent; r.macAck = lt_.macAck; r.echo = lt_.echo;
        r.rttSumMs = lt_.rttSum; r.rttMax = lt_.rttMax;
        r.rssiAvg = lt_.rssiN ? lt_.rssiSum / lt_.rssiN : 0;
        r.done = done;
        ev_->onLinkTest(r);
    };
    if (lt_.waiting) {
        if (lt_.txPending) return;
        if (lt_.echoed || static_cast<uint32_t>(now - lt_.sentAt) >= kEchoGraceForLinkTest) {
            lt_.waiting = false;
            lt_.nextAt = lt_.sentAt + lt_.gap;
            if (lt_.sent % 10 == 0 && lt_.sent < lt_.count) report(false);
        }
        return;
    }
    if (lt_.sent >= lt_.count) {
        report(true);
        lt_.active = false;
        return;
    }
    if (txBusy_ || exc_.kind != Exc::None || static_cast<int32_t>(now - lt_.nextAt) < 0) return;
    ProbePayload p = {static_cast<uint8_t>(lt_.sent), 0};
    if (!sendRaw(dev_[lt_.dev].mac, Type::Probe, &p, sizeof(p), false)) return;
    txBusy_ = true; txStart_ = now; txKind_ = TxKind::LinkTest; txDev_ = lt_.dev;
    ++lt_.sent;
    lt_.sentAt = now;
    lt_.waiting = true;
    lt_.txPending = true;
    lt_.echoed = false;
}

// ------------------------------------------------------------------ firmware update bridge
RemoteLink::OtaSubmit RemoteLink::submitOta(int dev, const uint8_t* payload, size_t len, uint32_t tag) {
    if (dev < 0 || dev >= count_) return OtaSubmit::BadDevice;
    if (!payload || len == 0 || len > kMaxPayload) return OtaSubmit::TooLong;
    setOtaHold(true);                                  // the PC is talking: keep the radio free of side activities
    if (otaCount_ >= kOtaQueue) { ++cnt_.otaQueueFull; return OtaSubmit::Full; }
    OtaItem& it = ota_[otaCount_++];
    it.inFlight = false;
    it.dev = static_cast<int8_t>(dev);
    it.len = static_cast<uint8_t>(len);
    memcpy(it.data, payload, len);
    it.tag = tag;
    return OtaSubmit::Ok;
}

void RemoteLink::setOtaHold(bool on) {
    if (!on) { otaHoldUntil_ = 0; return; }
    otaHoldUntil_ = now_ + kOtaHoldMs;
    if (!otaHoldUntil_) otaHoldUntil_ = 1;
}

// Sends the oldest queued frame. Not while the radio is away from the home channel or a channel move is under way.
bool RemoteLink::trySendOta(uint32_t now) {
    if (otaCount_ == 0 || exc_.kind != Exc::None || mig_.active) return false;
    OtaItem& it = ota_[0];
    if (it.inFlight) return false;
    if (it.dev < 0 || it.dev >= count_) { dropOta(0, false, false); return false; }
    if (!sendRaw(dev_[it.dev].mac, Type::Ota, it.data, it.len, true)) return false;    // radio busy: try again next pass
    txBusy_ = true;
    txStart_ = now;
    txKind_ = TxKind::Ota;
    txDev_ = it.dev;
    it.inFlight = true;
    ++cnt_.otaTx;
    return true;
}

void RemoteLink::dropOta(int index, bool sent, bool acked) {
    const uint32_t tag = ota_[index].tag;
    for (int k = index; k + 1 < otaCount_; ++k) ota_[k] = ota_[k + 1];
    ota_[--otaCount_] = OtaItem();
    if (ev_) ev_->onOtaTx(tag, sent, acked);
}

}  // namespace Fog
