// MenuSystem.h - the OLED menu logic: navigation, editors and the actions behind the menu entries.
//
// The visible state lives in MenuState (so screens can be rendered without hardware); this class changes it.
// Persistent changes go through RemoteSettings, radio actions through Fleet.
#pragma once

#include <Arduino.h>
#include "Defaults.h"
#include "battery/BatteryMonitor.h"
#include "config/RemoteSettings.h"
#include "link/Fleet.h"
#include "menu/MenuState.h"
#include "ui/ButtonInput.h"

class MenuSystem : public MenuState {
public:
    void begin(Fleet* fleet, RemoteSettings* settings, BatteryMonitor* battery);
    void update(const ButtonInput& b, uint32_t nowMs);

    bool isInMenu() const { return inMenu; }
    void enterMenu();
    void exitMenu();
    unsigned long menuExitTime() const { return menuExitTime_; }
    void enterSelectActive(bool returnToMain);
    void enterEditTimers();                    // from the main screen or the menu; uses the active timer's values
    const char* modeName() const;

    // actions the main loop must perform (consumed once)
    bool consumeRemoteReset() { const bool v = remoteResetPending_; remoteResetPending_ = false; return v; }
    bool consumeReboot() { const bool v = rebootPending_; rebootPending_ = false; return v; }
    bool consumeSleep() { const bool v = sleepPending_; sleepPending_ = false; return v; }
    bool consumeSettingsApplied() { const bool v = settingsApplied_; settingsApplied_ = false; return v; }

private:
    struct Hold { uint32_t upStart = 0, downStart = 0, last = 0; };
    int holdSteps(const ButtonInput& b, uint32_t now, Hold& h);
    void toRoot() { mode = Mode::ROOT; }
    void activate(int index, uint32_t now);
    void clampScroll();
    void updateRoot(const ButtonInput& b, uint32_t now);
    void updateEditTimers(const ButtonInput& b, uint32_t now);
    void updateEditName(const ButtonInput& b, uint32_t now);
    void updateCalibration(const ButtonInput& b, uint32_t now);
    void updateRssiCalib(const ButtonInput& b, uint32_t now);
    void updateChannel(const ButtonInput& b, uint32_t now);
    void updatePairing(const ButtonInput& b);
    void updateSimpleEdit(const ButtonInput& b, uint32_t now);
    void startChannelScan(uint32_t now);
    void enterRename();
    // The '#' press that opened an editor is still going on when the editor starts: its release (click) or
    // long-press event must not be read as input to the editor.
    bool hashClick(const ButtonInput& b) const { return b.hashClick() && !ignoreHash_; }
    bool hashLong(const ButtonInput& b) const { return b.hashLongEvent() && !ignoreHash_; }

    Fleet* fleet_ = nullptr;
    RemoteSettings* settings_ = nullptr;
    BatteryMonitor* battery_ = nullptr;
    uint32_t menuExitTime_ = 0;
    bool selectReturnsToMain_ = false;
    bool swallowHash_ = false, ignoreHash_ = false;
    Hold hold_;
    bool remoteResetPending_ = false, rebootPending_ = false, settingsApplied_ = false, sleepPending_ = false;
};
