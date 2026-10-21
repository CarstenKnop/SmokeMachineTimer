#include "menu/MenuSystem.h"

#include <math.h>
#include "FogLog.h"

namespace {
enum Idx : int {
    I_PAIR, I_RENAME, I_ACTIVE, I_EDIT_TIMERS, I_LINK_STATUS, I_FIND, I_CHANNEL, I_LINK_TEST, I_AUTO_OFF, I_BRIGHTNESS,
    I_TX_POWER, I_RSSI_CAL, I_BAT_CAL, I_RESET_TIMER, I_RESET_REMOTE, I_REBOOT, I_SLEEP
};
const char kCharset[] = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-";
}  // namespace

void MenuSystem::begin(Fleet* fleet, RemoteSettings* settings, BatteryMonitor* battery) {
    fleet_ = fleet;
    settings_ = settings;
    battery_ = battery;
    const RemoteSettingsData& s = settings_->get();
    autoOffIdx = 3;
    for (int i = 0; i < 7; ++i) if (kAutoOffOptions[i] == s.autoOffSeconds) autoOffIdx = i;
    editTxPowerQdbm = s.txPowerQdbm;
    editBrightness = s.oledBrightness;
    editRssiLow = s.rssiLowDbm;
    editRssiHigh = s.rssiHighDbm;
}

const char* MenuSystem::modeName() const {
    if (!inMenu) return "main";
    switch (mode) {
        case Mode::ROOT: return "menu";
        case Mode::EDIT_AUTO_OFF: return "autoOff";
        case Mode::EDIT_TXPOWER: return "txPower";
        case Mode::EDIT_BRIGHTNESS: return "brightness";
        case Mode::PAIRING: return "pairing";
        case Mode::SELECT_ACTIVE: return "select";
        case Mode::LINK_STATUS: return "linkStatus";
        case Mode::BATTERY_CALIB: return "batteryCal";
        case Mode::EDIT_RSSI_CALIB: return "rssiCal";
        case Mode::EDIT_TIMERS: return "editTimers";
        case Mode::EDIT_NAME: return "rename";
        case Mode::CONFIRM: return "confirm";
        case Mode::CHANNEL: return "channel";
        case Mode::FIND_TIMERS: return "find";
        case Mode::LINK_TEST: return "linkTest";
    }
    return "menu";
}

void MenuSystem::enterMenu() {
    inMenu = true;
    mode = Mode::ROOT;
    prevSelected = selected;
    lastSelChange = millis();
    animScroll = scrollOffset;
}

void MenuSystem::exitMenu() {
    if (mode == Mode::PAIRING && fleet_) fleet_->stopDiscovery();
    inMenu = false;
    mode = Mode::ROOT;
    menuExitTime_ = millis();
}

void MenuSystem::enterSelectActive(bool returnToMain) {
    inMenu = true;
    mode = Mode::SELECT_ACTIVE;
    selectReturnsToMain_ = returnToMain;
    activeSel = fleet_ && fleet_->getActiveIndex() >= 0 ? fleet_->getActiveIndex() : 0;
}

void MenuSystem::enterEditTimers() {
    const SlaveDevice* a = fleet_ ? fleet_->getActiveDevice() : nullptr;
    if (!a) { if (fleet_) fleet_->showToast("No timer selected", millis()); return; }
    inMenu = true;
    mode = Mode::EDIT_TIMERS;
    editTonTenths = constrain((int)roundf(a->ton * 10.0f), 0, 99999);
    editToffTenths = constrain((int)roundf(a->toff * 10.0f), 0, 99999);
    editDigitIndex = 0;
}

void MenuSystem::enterRename() {
    const SlaveDevice* a = fleet_ ? fleet_->getActiveDevice() : nullptr;
    if (!a) { fleet_->showToast("No timer selected", millis()); return; }
    mode = Mode::EDIT_NAME;
    memset(nameBuf, 0, sizeof(nameBuf));
    strncpy(nameBuf, a->name[0] ? a->name : "Timer", sizeof(nameBuf) - 1);
    for (size_t i = strlen(nameBuf); i < sizeof(nameBuf) - 1; ++i) nameBuf[i] = ' ';     // always 9 editable cells
    namePos = 0;
}

void MenuSystem::clampScroll() {
    if (selected < scrollOffset) scrollOffset = selected;
    else if (selected >= scrollOffset + kVisibleLines) scrollOffset = selected - kVisibleLines + 1;
    if (scrollOffset < 0) scrollOffset = 0;
}

// Up/Down with initial delay + repeat, accelerating while held. Returns -1, 0 or +1 per call.
int MenuSystem::holdSteps(const ButtonInput& b, uint32_t now, Hold& h) {
    if (b.upPressed())   { h.upStart = now; h.downStart = 0; h.last = now; return +1; }
    if (b.downPressed()) { h.downStart = now; h.upStart = 0; h.last = now; return -1; }
    const bool up = b.upHeld(), down = b.downHeld();
    if (!up) h.upStart = 0;
    if (!down) h.downStart = 0;
    if (!up && !down) return 0;
    const uint32_t start = up ? h.upStart : h.downStart;
    if (!start) return 0;
    const uint32_t held = now - start;
    if (held < Defaults::EDIT_INITIAL_DELAY_MS) return 0;
    const uint32_t interval = held > 2000 ? Defaults::EDIT_REPEAT_INTERVAL_MS / 3 : (held > 1000 ? Defaults::EDIT_REPEAT_INTERVAL_MS / 2 : Defaults::EDIT_REPEAT_INTERVAL_MS);
    if (now - h.last < interval) return 0;
    h.last = now;
    return up ? +1 : -1;
}

void MenuSystem::update(const ButtonInput& b, uint32_t now) {
    if (!inMenu) return;
    ignoreHash_ = swallowHash_;
    if (b.hashReleased()) swallowHash_ = false;                 // the swallowed press is over after this cycle
    switch (mode) {
        case Mode::ROOT: updateRoot(b, now); break;
        case Mode::EDIT_AUTO_OFF:
        case Mode::EDIT_TXPOWER:
        case Mode::EDIT_BRIGHTNESS: updateSimpleEdit(b, now); break;
        case Mode::PAIRING: updatePairing(b); break;
        case Mode::SELECT_ACTIVE: {
            const int n = fleet_->getPairedCount();
            if (n > 0) {
                if (b.upPressed()) activeSel = (activeSel + n - 1) % n;
                if (b.downPressed()) activeSel = (activeSel + 1) % n;
                if (activeSel >= n) activeSel = n - 1;
            }
            if (b.hashPressed() && n > 0) {
                fleet_->setActive(activeSel);
                if (selectReturnsToMain_) exitMenu(); else toRoot();
            } else if (b.starPressed()) {
                if (selectReturnsToMain_) exitMenu(); else toRoot();
            }
            break;
        }
        case Mode::LINK_STATUS: {
            const int n = fleet_->getPairedCount();
            if (b.upPressed() && linkFirst > 0) --linkFirst;
            if (b.downPressed() && linkFirst < (n > 0 ? n - 1 : 0)) ++linkFirst;
            if (b.hashPressed()) fleet_->link().probeNow();
            if (b.starPressed()) toRoot();
            break;
        }
        case Mode::EDIT_TIMERS: updateEditTimers(b, now); break;
        case Mode::EDIT_NAME: updateEditName(b, now); break;
        case Mode::BATTERY_CALIB: updateCalibration(b, now); break;
        case Mode::EDIT_RSSI_CALIB: updateRssiCalib(b, now); break;
        case Mode::CHANNEL: updateChannel(b, now); break;
        case Mode::FIND_TIMERS:
            if (b.hashPressed()) fleet_->findLostTimers();
            if (b.starPressed()) toRoot();
            break;
        case Mode::LINK_TEST:
            if (b.hashPressed() && !fleet_->link().linkTesting()) fleet_->startLinkTest(40, 40);
            if (b.starPressed()) toRoot();
            break;
        case Mode::CONFIRM:
            if (b.hashPressed()) {
                if (confirmAction == ConfirmAction::RESET_TIMER) fleet_->factoryResetActive();
                else if (confirmAction == ConfirmAction::RESET_REMOTE) remoteResetPending_ = true;
                else if (confirmAction == ConfirmAction::REBOOT) rebootPending_ = true;
                exitMenu();
            } else if (b.starPressed()) {
                toRoot();
            }
            break;
    }
    if (scrollAnim && now - scrollAnimStart >= SCROLL_ANIM_MS) scrollAnim = false;
}

void MenuSystem::activate(int index, uint32_t now) {
    switch (index) {
        case I_PAIR:
            mode = Mode::PAIRING;
            pairSel = 0;
            fleet_->startDiscovery(true);
            break;
        case I_RENAME: enterRename(); break;
        case I_ACTIVE: enterSelectActive(false); break;
        case I_EDIT_TIMERS: enterEditTimers(); break;
        case I_LINK_STATUS: mode = Mode::LINK_STATUS; linkFirst = 0; fleet_->link().probeNow(); break;
        case I_FIND: mode = Mode::FIND_TIMERS; fleet_->findLostTimers(); break;
        case I_CHANNEL: mode = Mode::CHANNEL; startChannelScan(now); break;
        case I_LINK_TEST:
            if (!fleet_->getActiveDevice()) { fleet_->showToast("No timer selected", now); break; }
            mode = Mode::LINK_TEST;
            fleet_->startLinkTest(40, 40);
            break;
        case I_AUTO_OFF: mode = Mode::EDIT_AUTO_OFF; break;
        case I_BRIGHTNESS: mode = Mode::EDIT_BRIGHTNESS; editBrightness = settings_->get().oledBrightness; break;
        case I_TX_POWER: mode = Mode::EDIT_TXPOWER; editTxPowerQdbm = settings_->get().txPowerQdbm; break;
        case I_RSSI_CAL:
            mode = Mode::EDIT_RSSI_CALIB;
            rssiField = 0;
            editRssiLow = settings_->get().rssiLowDbm;
            editRssiHigh = settings_->get().rssiHighDbm;
            break;
        case I_BAT_CAL:
            mode = Mode::BATTERY_CALIB;
            calibStarted = false;
            calField = 0;
            for (int i = 0; i < 3; ++i) editCal[i] = settings_->get().batCalMv[i];
            break;
        case I_RESET_TIMER:
            if (!fleet_->getActiveDevice()) { fleet_->showToast("No timer selected", now); break; }
            mode = Mode::CONFIRM; confirmAction = ConfirmAction::RESET_TIMER; break;
        case I_RESET_REMOTE: mode = Mode::CONFIRM; confirmAction = ConfirmAction::RESET_REMOTE; break;
        case I_REBOOT: mode = Mode::CONFIRM; confirmAction = ConfirmAction::REBOOT; break;
        case I_SLEEP: sleepPending_ = true; exitMenu(); break;
    }
}

void MenuSystem::updateRoot(const ButtonInput& b, uint32_t now) {
    auto move = [&](int dir) {
        prevSelected = selected;
        const int oldScroll = scrollOffset;
        selected = (selected + dir + kItemCount) % kItemCount;
        clampScroll();
        lastSelChange = now;
        animScroll = scrollOffset;
        if (scrollOffset != oldScroll) { scrollAnim = true; scrollAnimStart = now; scrollDir = dir; prevScroll = oldScroll; }
    };
    if (b.upPressed()) move(-1);
    if (b.downPressed()) move(+1);
    if (b.hashPressed()) { activate(selected, now); swallowHash_ = true; ignoreHash_ = true; }
    else if (b.starPressed()) exitMenu();
}

void MenuSystem::updateSimpleEdit(const ButtonInput& b, uint32_t now) {
    const int step = holdSteps(b, now, hold_);
    RemoteSettingsData& s = settings_->mut();
    if (mode == Mode::EDIT_AUTO_OFF) {
        if (step > 0 && autoOffIdx < 6) ++autoOffIdx;
        if (step < 0 && autoOffIdx > 0) --autoOffIdx;
        if (b.hashPressed()) { s.autoOffSeconds = kAutoOffOptions[autoOffIdx]; settings_->save(); settingsApplied_ = true; toRoot(); }
        else if (b.starPressed()) {
            autoOffIdx = 3;
            for (int i = 0; i < 7; ++i) if (kAutoOffOptions[i] == s.autoOffSeconds) autoOffIdx = i;
            toRoot();
        }
    } else if (mode == Mode::EDIT_TXPOWER) {
        editTxPowerQdbm = (int8_t)constrain(editTxPowerQdbm + step, 8, 84);
        if (b.hashPressed()) { s.txPowerQdbm = editTxPowerQdbm; settings_->save(); settingsApplied_ = true; toRoot(); }
        else if (b.starPressed()) toRoot();
    } else {   // brightness: live preview is applied by the display while editing
        editBrightness = (uint8_t)constrain((int)editBrightness + step * 5, 5, 255);
        if (b.hashPressed()) { s.oledBrightness = editBrightness; settings_->save(); settingsApplied_ = true; toRoot(); }
        else if (b.starPressed()) { editBrightness = s.oledBrightness; toRoot(); }
    }
}

void MenuSystem::updatePairing(const ButtonInput& b) {
    const int n = fleet_->getDiscoveredCount();
    if (n > 0) {
        if (b.upPressed()) pairSel = (pairSel + n - 1) % n;
        if (b.downPressed()) pairSel = (pairSel + 1) % n;
        if (pairSel >= n) pairSel = n - 1;
    }
    if (b.hashPressed() && n > 0) {
        const DiscoveredDevice d = fleet_->getDiscovered(pairSel);
        const int idx = fleet_->findPairedIndexByMac(d.mac);
        if (idx >= 0) fleet_->removeDeviceByIndex(idx);
        else fleet_->pairWithIndex(pairSel);
    }
    if (b.starPressed()) { fleet_->stopDiscovery(); toRoot(); }
}

void MenuSystem::updateEditTimers(const ButtonInput& b, uint32_t now) {
    auto tweak = [&](int& tenths, int digit, int delta) {
        int pow10 = 1;
        for (int i = 0; i < Defaults::DIGITS - digit - 1; ++i) pow10 *= 10;
        int d = (tenths / pow10) % 10;
        d = (d + delta) % 10;
        if (d < 0) d += 10;
        tenths = tenths - ((tenths / pow10) % 10) * pow10 + d * pow10;
    };
    const int step = holdSteps(b, now, hold_);
    if (step != 0) {
        if (editDigitIndex < Defaults::DIGITS) tweak(editToffTenths, editDigitIndex, step);
        else tweak(editTonTenths, editDigitIndex - Defaults::DIGITS, step);
    }
    if (hashLong(b)) { editDigitIndex = editDigitIndex > 0 ? editDigitIndex - 1 : 2 * Defaults::DIGITS - 1; return; }
    if (hashClick(b)) {
        ++editDigitIndex;
        if (editDigitIndex >= 2 * Defaults::DIGITS) {
            const int toff = constrain(editToffTenths, (int)Defaults::SLAVE_TIMER_MIN_TENTHS, (int)Defaults::SLAVE_TIMER_MAX_TENTHS);
            const int ton = constrain(editTonTenths, (int)Defaults::SLAVE_TIMER_MIN_TENTHS, (int)Defaults::SLAVE_TIMER_MAX_TENTHS);
            fleet_->setActiveTimer(ton / 10.0f, toff / 10.0f);
            exitMenu();
        }
    }
    if (b.starPressed()) exitMenu();
}

void MenuSystem::updateEditName(const ButtonInput& b, uint32_t now) {
    const int len = (int)sizeof(nameBuf) - 1;
    const int step = holdSteps(b, now, hold_);
    if (step != 0) {
        char& c = nameBuf[namePos];
        const char* p = strchr(kCharset, c);
        int idx = p ? (int)(p - kCharset) : 0;
        const int n = (int)sizeof(kCharset) - 1;
        idx = (idx + step + n) % n;
        c = kCharset[idx];
    }
    if (hashLong(b)) { namePos = namePos > 0 ? namePos - 1 : len - 1; return; }
    if (hashClick(b)) {
        if (namePos < len - 1) { ++namePos; return; }
        // last cell: save (trim trailing blanks)
        char out[Fog::kNameLen];
        strncpy(out, nameBuf, sizeof(out));
        out[sizeof(out) - 1] = 0;
        for (int i = (int)strlen(out) - 1; i >= 0 && out[i] == ' '; --i) out[i] = 0;
        if (out[0]) fleet_->setActiveName(out);
        exitMenu();
        return;
    }
    if (b.starPressed()) exitMenu();
}

void MenuSystem::updateCalibration(const ButtonInput& b, uint32_t now) {
    if (!calibStarted) {
        if (b.hashPressed()) { calibStarted = true; calField = 0; }
        else if (b.starPressed()) toRoot();
        return;
    }
    const int step = holdSteps(b, now, hold_);
    if (step != 0) editCal[calField] = (uint16_t)constrain((int)editCal[calField] + step * 10, 2500, 4500);
    if (hashLong(b)) { editCal[calField] = battery_->readMillivoltsNow(); return; }     // capture the live reading
    if (hashClick(b)) {
        if (calField < 2) { ++calField; return; }
        if (editCal[0] < editCal[1] && editCal[1] < editCal[2]) {
            for (int i = 0; i < 3; ++i) settings_->mut().batCalMv[i] = editCal[i];
            settings_->save();
            toRoot();
        } else {
            fleet_->showToast("Must ascend 0<50<100", now);
            calField = 0;
        }
    }
    if (b.starPressed()) toRoot();
}

void MenuSystem::updateRssiCalib(const ButtonInput& b, uint32_t now) {
    const int step = holdSteps(b, now, hold_);
    if (step != 0) {
        if (rssiField == 0) editRssiLow = (int8_t)constrain(editRssiLow + step, -120, -10);
        else editRssiHigh = (int8_t)constrain(editRssiHigh + step, -110, 0);
        if (editRssiHigh < editRssiLow + 5) { if (rssiField == 0) editRssiLow = (int8_t)(editRssiHigh - 5); else editRssiHigh = (int8_t)(editRssiLow + 5); }
    }
    if (b.hashPressed()) {
        if (rssiField == 0) rssiField = 1;
        else {
            settings_->mut().rssiLowDbm = editRssiLow;
            settings_->mut().rssiHighDbm = editRssiHigh;
            settings_->save();
            toRoot();
        }
    } else if (b.starPressed()) toRoot();
}

void MenuSystem::startChannelScan(uint32_t now) {
    chCount = 0;
    chSel = 0;
    if (fleet_->startSurvey()) chStage = ChannelStage::Scanning;
    else { chStage = ChannelStage::Failed; }
    (void)now;
}

void MenuSystem::updateChannel(const ButtonInput& b, uint32_t now) {
    ChannelSurvey& sv = fleet_->survey();
    switch (chStage) {
        case ChannelStage::Scanning:
            if (sv.state() == ChannelSurvey::State::Done) {
                chCount = sv.bestCount();
                for (int i = 0; i < chCount; ++i) chOrder[i] = sv.best(i);
                chSel = 0;
                chStage = ChannelStage::Choose;
            } else if (sv.state() == ChannelSurvey::State::Failed || (!sv.active() && sv.state() == ChannelSurvey::State::Idle)) {
                chStage = ChannelStage::Failed;
            }
            if (b.starPressed()) { sv.cancel(); fleet_->radio().setChannel(fleet_->homeChannel()); toRoot(); }
            break;
        case ChannelStage::Choose:
            if (b.upPressed() && chSel > 0) --chSel;
            if (b.downPressed() && chSel < chCount - 1) ++chSel;
            if (hashLong(b)) startChannelScan(now);
            else if (hashClick(b)) {
                chTarget = chOrder[chSel];
                if (chTarget == fleet_->homeChannel()) { fleet_->showToast("Already on that channel", now); toRoot(); }
                else if (fleet_->migrateTo(chTarget, false)) chStage = ChannelStage::Moving;
                else fleet_->showToast("Busy, try again", now);
            }
            if (b.starPressed()) toRoot();
            break;
        case ChannelStage::Moving:
            if (fleet_->migrateResult() >= 0) chStage = ChannelStage::Result;
            break;
        case ChannelStage::Result:
            if (fleet_->migrateResult() == 0 && b.hashPressed()) {          // nobody answered: offer to force it
                if (fleet_->migrateTo(chTarget, true)) chStage = ChannelStage::Moving;
            } else if (b.starPressed() || (fleet_->migrateResult() == 1 && b.hashPressed())) {
                fleet_->clearMigrateResult();
                toRoot();
            }
            break;
        case ChannelStage::Failed:
            if (b.hashPressed()) startChannelScan(now);
            else if (b.starPressed()) toRoot();
            break;
    }
}
