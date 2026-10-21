#include "link/Fleet.h"

#include <algorithm>
#include "Defaults.h"
#include "FogLog.h"
#include "FogUtil.h"

using namespace Fog;

Fleet* Fleet::self_ = nullptr;

Fleet::Fleet(RemoteSettings& settings) : settings_(settings), link_(*this, this) {
    self_ = this;
}

bool Fleet::begin(uint32_t nowMs) {
    now_ = nowMs;
    const RemoteSettingsData& s = settings_.get();
    if (!radio_.begin(s.channel, s.txPowerQdbm)) {
        FLOGE("radio", "ESP-NOW init failed");
        return false;
    }
    radio_.macAddress(mac_);
    link_.begin(mac_, s.net, s.channel, nowMs);
    loadList();
    FLOGI("link", "ch %u, %d timers paired", s.channel, count_);
    return true;
}

// ------------------------------------------------------------------ persistence
void Fleet::loadList() {
    StoredList list = {};
    count_ = 0;
    active_ = 0;
    if (!store_.load(list) || list.count > kMaxTimers) return;
    for (int i = 0; i < list.count; ++i) {
        link_.addDevice(list.dev[i].mac, list.dev[i].name, list.dev[i].ch);
    }
    count_ = link_.deviceCount();
    active_ = list.active < count_ ? list.active : 0;
    saved_ = list;
}

void Fleet::saveList() {
    StoredList list = {};
    list.count = static_cast<uint8_t>(count_);
    list.active = static_cast<uint8_t>(active_ < 0 ? 0 : active_);
    for (int i = 0; i < count_; ++i) {
        const Device& d = link_.device(i);
        memcpy(list.dev[i].mac, d.mac, 6);
        memcpy(list.dev[i].name, d.name, kNameLen);
        list.dev[i].ch = d.chHint;
    }
    store_.save(list);        // skips the flash write when nothing changed
    saved_ = list;
}

// ------------------------------------------------------------------ main pump
bool Fleet::send(const uint8_t* mac, const uint8_t* data, size_t len) {
    if (!radio_.send(mac, data, len)) return false;
    if (tap) tap(true, mac, 0, data, len);
    return true;
}

void Fleet::pumpRadio(uint32_t nowMs) {
    radio_.service(nowMs);
    bool ok;
    while (radio_.pollTxResult(ok)) link_.onTxResult(ok, nowMs);
    FogRadio::Rx rx;
    for (int i = 0; i < 12 && radio_.pollRx(rx); ++i) {
        if (tap) tap(false, rx.mac, rx.rssi, rx.data, rx.len);
        if (survey_.active()) continue;          // the radio is being used for measurements: ignore stray frames
        link_.onFrame(rx.mac, rx.rssi, rx.data, rx.len, nowMs, rx.channel);
    }
}

void Fleet::loop(uint32_t nowMs) {
    now_ = nowMs;
    link_.setNow(nowMs);                     // submit() during a survey must not use a stale clock
    pumpRadio(nowMs);
    if (survey_.active()) {
        survey_.tick(nowMs);
        if (!survey_.active()) {
            radio_.setChannel(link_.homeChannel());
            link_.resyncLoss();
            link_.probeNow();
        }
    } else {
        link_.tick(nowMs);
    }
    count_ = link_.deviceCount();
    if (active_ >= count_) active_ = count_ ? count_ - 1 : 0;
    refreshViews(nowMs);
    if (link_.discovering() && nowMs - lastDiscRefresh_ > 200) { lastDiscRefresh_ = nowMs; refreshDiscovered(nowMs); }
}

void Fleet::refreshViews(uint32_t nowMs) {
    for (int i = 0; i < count_; ++i) {
        const Device& d = link_.device(i);
        SlaveDevice& v = views_[i];
        memcpy(v.mac, d.mac, 6);
        memcpy(v.name, d.name, kNameLen);
        v.online = link_.isOnline(i, nowMs);
        v.rssiRemote = d.rssi.value();
        v.rssiSlave = d.rssiT;
        v.ton = d.statusValid ? d.st.onTenths / 10.0f : 0.f;
        v.toff = d.statusValid ? d.st.offTenths / 10.0f : 0.f;
        bool phaseOn;
        uint32_t el;
        link_.predict(i, nowMs, phaseOn, el);
        v.elapsed = el / 1000.0f;
        v.phaseOn = phaseOn;
        v.outputState = d.statusValid && (d.st.flags & ST_OUTPUT_ON);
        v.mode = d.statusValid ? d.st.mode : 0;
        v.lossPct = d.loss.lossPercent();
        v.pending = static_cast<uint8_t>(link_.pendingFor(i));
        v.channel = d.chHint;
        v.rebooted = d.rebootedMs && (nowMs - d.rebootedMs) < 30000;
        v.lastStatusMs = d.statusValid ? d.statusMs : 0;
        v.ageMs = d.statusValid ? nowMs - d.lastRxMs : 0xFFFFFFFFu;
    }
}

void Fleet::refreshDiscovered(uint32_t nowMs) {
    DiscoveredDevice tmp[RemoteLink::kMaxHeard];
    int n = 0;
    for (int i = 0; i < RemoteLink::kMaxHeard; ++i) {
        const Heard& h = link_.heard(i);
        if (!h.used || nowMs - h.lastMs > 12000) continue;
        DiscoveredDevice& d = tmp[n++];
        memcpy(d.mac, h.mac, 6);
        memcpy(d.name, h.name, kNameLen);
        d.rssi = h.rssi;
        d.ton = h.onTenths / 10.0f;
        d.toff = h.offTenths / 10.0f;
        d.lastSeen = h.lastMs;
        d.channel = h.channel;
        d.paired = link_.findDevice(h.mac) >= 0;
    }
    // Strongest first, but in 10 dB steps and then by address, so the list does not shuffle under the cursor
    // while the signal values jitter.
    std::sort(tmp, tmp + n, [](const DiscoveredDevice& a, const DiscoveredDevice& b) {
        const int ba = (a.rssi + 120) / 10, bb = (b.rssi + 120) / 10;
        if (ba != bb) return ba > bb;
        return memcmp(a.mac, b.mac, 6) < 0;
    });
    discCount_ = n;
    for (int i = 0; i < n; ++i) disc_[i] = tmp[i];
}

// ------------------------------------------------------------------ list management
void Fleet::setActive(int idx) {
    if (idx < 0 || idx >= count_) return;
    active_ = idx;
    saveList();
}

void Fleet::removeDeviceByIndex(int idx) {
    if (idx < 0 || idx >= count_) return;
    if (holdDev_ == idx) { holding_ = false; holdDev_ = -1; } else if (holdDev_ > idx) --holdDev_;
    link_.removeDevice(idx);
    count_ = link_.deviceCount();
    if (active_ > idx) --active_;
    if (active_ >= count_) active_ = count_ ? count_ - 1 : 0;
    saveList();
    refreshViews(now_);
}

bool Fleet::pairMac(const uint8_t mac[6], const char* name, uint8_t channel) {
    const int existing = link_.findDevice(mac);
    if (existing >= 0) return true;
    const int idx = link_.addDevice(mac, name, channel);
    if (idx < 0) { showToast("List full (10)", now_); return false; }
    count_ = link_.deviceCount();
    if (count_ == 1) active_ = 0;
    // A name chosen while pairing (PC tool) is written to the timer, otherwise the timer's own name would win.
    // (Only when it is on our channel; one that has to be fetched first keeps its own name until renamed.)
    char wanted[kNameLen];
    sanitizeName(wanted, name ? name : "", name ? strlen(name) : 0);
    const bool onHome = !validChannel(channel) || channel == link_.homeChannel();
    char heardName[kNameLen] = {0};
    for (int i = 0; i < RemoteLink::kMaxHeard; ++i) {
        const Heard& h = link_.heard(i);
        if (h.used && memcmp(h.mac, mac, 6) == 0) memcpy(heardName, h.name, kNameLen);
    }
    if (wanted[0] && onHome && strcmp(wanted, heardName) != 0) renameByIndex(idx, wanted);
    saveList();
    if (!onHome) link_.startRecover(idx);                    // fetch it onto our channel
    else link_.probeNow();
    return true;
}

bool Fleet::pairWithIndex(int idx) {
    if (idx < 0 || idx >= discCount_) return false;
    const DiscoveredDevice d = disc_[idx];
    return pairMac(d.mac, d.name, d.channel);
}

void Fleet::startDiscovery(bool sweep) {
    link_.startDiscovery(sweep);
    lastDiscRefresh_ = 0;
}

void Fleet::stopDiscovery() { link_.stopDiscovery(); }

// ------------------------------------------------------------------ commands
void Fleet::toggleActive() {
    if (active_ < 0 || active_ >= count_) return;
    link_.submitToggle(active_);
}

void Fleet::holdFog(bool held, uint32_t nowMs) {
    if (held && !holding_) {
        if (active_ < 0 || active_ >= count_) return;
        holding_ = true;
        holdDev_ = active_;
        link_.submitMode(holdDev_, Mode::ForceOn, Defaults::FORCE_LEASE_MS, true);
        nextRefresh_ = nowMs + Defaults::FORCE_REFRESH_MS;
    } else if (held && holding_) {
        if (static_cast<int32_t>(nowMs - nextRefresh_) >= 0) {
            link_.submitMode(holdDev_, Mode::ForceOn, Defaults::FORCE_LEASE_MS, false);
            nextRefresh_ = nowMs + Defaults::FORCE_REFRESH_MS;
        }
    } else if (!held && holding_) {
        holding_ = false;
        if (holdDev_ >= 0 && holdDev_ < count_) link_.submitMode(holdDev_, Mode::Auto, 0, true);
        holdDev_ = -1;
    }
}

void Fleet::setActiveTimer(float tonSec, float toffSec) {
    if (active_ < 0 || active_ >= count_) return;
    link_.submitSetTimes(active_, secondsToTenths(tonSec), secondsToTenths(toffSec));
}

void Fleet::setActiveName(const char* name) { renameByIndex(active_, name); }

int Fleet::renameByIndex(int idx, const char* name) {
    if (idx < 0 || idx >= count_ || !name) return -1;
    const int job = link_.submitName(idx, name);
    if (job > 0) {
        sanitizeName(link_.mutableDevice(idx).name, name, strlen(name));      // show the new name immediately
        saveList();
    }
    return job;
}

void Fleet::wipeLocalData() {
    store_.erase();
}

void Fleet::factoryResetActive() {
    if (active_ < 0 || active_ >= count_) return;
    link_.submitFactoryReset(active_);
}

bool Fleet::startSurvey() {
    if (survey_.active() || link_.migrating()) return false;
    stopDiscovery();
    return survey_.start(link_.homeChannel(), now_);
}

bool Fleet::migrateTo(uint8_t channel, bool force) {
    migOk_ = migFail_ = migTotal_ = 0;
    migResult_ = -1;
    return link_.startMigrate(channel, force);
}

void Fleet::showToast(const char* msg, uint32_t nowMs, uint32_t ms) {
    strncpy(toast_, msg, sizeof(toast_) - 1);
    toast_[sizeof(toast_) - 1] = 0;
    toastUntil_ = nowMs + ms;
    if (!toastUntil_) toastUntil_ = 1;
}

// ------------------------------------------------------------------ link events
void Fleet::onJobDone(const JobDone& d) {
    if (d.dev >= 0 && d.dev < count_ && d.tag == RemoteLink::TagNone) {
        const char* name = link_.device(d.dev).name;
        char msg[32];
        if (d.result == JobResult::Timeout) {
            // forced-on refreshes are fire-and-forget; only report real commands
            if (d.op != static_cast<uint8_t>(Op::SetMode) || d.attempts > 1) {
                snprintf(msg, sizeof(msg), "%.9s: no answer", name);
                showToast(msg, now_);
            }
        } else if (d.result == JobResult::Rejected) {
            snprintf(msg, sizeof(msg), "%.9s: rejected", name);
            showToast(msg, now_);
        }
    }
    if (sink_) sink_->onJobDone(d);
}

void Fleet::onDeviceChanged(int dev) {
    // The timer is authoritative for its name: keep the stored list in step (rarely changes -> rarely writes).
    if (dev >= 0 && dev < count_) {
        const Device& d = link_.device(dev);
        if (dev < saved_.count && (memcmp(saved_.dev[dev].name, d.name, kNameLen) != 0 || saved_.dev[dev].ch != d.chHint)) saveList();
    }
    if (sink_) sink_->onDeviceChanged(dev);
}

void Fleet::onOnlineChanged(int dev, bool online) {
    FLOGI("link", "%s %s", dev >= 0 && dev < count_ ? link_.device(dev).name : "?", online ? "online" : "OFFLINE");
    if (sink_) sink_->onOnlineChanged(dev, online);
}

void Fleet::onRebooted(int dev) {
    if (dev >= 0 && dev < count_) {
        char msg[32];
        snprintf(msg, sizeof(msg), "%.9s power-cycled", link_.device(dev).name);
        showToast(msg, now_, 3000);
        FLOGW("link", "%s rebooted (power cycle)", link_.device(dev).name);
    }
    if (sink_) sink_->onRebooted(dev);
}

void Fleet::onHeard(const Heard& h) { if (sink_) sink_->onHeard(h); }
void Fleet::onInfo(int dev) { if (sink_) sink_->onInfo(dev); }

void Fleet::onMigrate(uint8_t stage, int dev, bool ok, uint8_t ch, uint8_t okCount, uint8_t failCount, bool committed) {
    if (stage == 0) { migTotal_ = okCount; migOk_ = migFail_ = 0; migResult_ = -1; }
    else if (stage == 1) { migOk_ = okCount; migFail_ = failCount; }
    else if (stage == 2) {
        migOk_ = okCount; migFail_ = failCount;
        migResult_ = committed ? 1 : 0;
        char msg[32];
        if (committed) snprintf(msg, sizeof(msg), "Channel %u set", ch);
        else snprintf(msg, sizeof(msg), "No timer answered");
        showToast(msg, now_, 3500);
    }
    if (sink_) sink_->onMigrate(stage, dev, ok, ch, okCount, failCount, committed);
}

void Fleet::onLinkTest(const LinkTestResult& r) { ltResult_ = r; if (sink_) sink_->onLinkTest(r); }

void Fleet::onHomeChannel(uint8_t ch) {
    settings_.mut().channel = ch;
    settings_.save();
    saveList();                    // channel hints changed with it
    if (sink_) sink_->onHomeChannel(ch);
}
