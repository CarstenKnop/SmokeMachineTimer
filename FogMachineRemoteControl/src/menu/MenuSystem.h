// MenuSystem.h
// Handles menu navigation, progress bar, and animated transitions.
#pragma once
#include <Arduino.h>
#include <vector>
#include "Defaults.h"

struct MenuItem {
    const char* label;
    // Future: callback id / function pointer
};

class MenuSystem {
public:
    MenuSystem();
    void begin();
    // Note: hashLongPressed is no longer used for menu entry gating; kept for internal edit flows if needed.
    void update(bool upPressed, bool downPressed, bool hashPressed, bool hashLongPressed, bool starPressed, bool upHeld, bool downHeld);
    void enterMenu();
    void exitMenu();
    void nextItem(); // legacy
    void prevItem(); // legacy
    int getSelectedIndex() const;
    const char* getCurrentMenuName() const;
    bool isInMenu() const;
    // Rendering helpers
    int getVisibleStart() const; // first visible item index
    int getVisibleCount(int maxLines) const; // number of items to draw
    const MenuItem& getItem(int index) const { return items[index]; }
    int getItemCount() const { return (int)items.size(); }
    unsigned long getMenuEnterTime() const { return menuEnterTime; }
    bool isAnimatingEnter() const; // simple fade/scroll placeholder
    bool justSelected() const { return lastSelectTime && (millis() - lastSelectTime < 400); }
    const char* getLastActionLabel() const { return lastActionLabel; }
    // Editing helpers / modes
    enum class Mode { ROOT, EDIT_BLANKING, EDIT_TXPOWER, EDIT_BRIGHTNESS, PAIRING, MANAGE_DEVICES, RENAME_DEVICE, SELECT_ACTIVE, SHOW_RSSI, BATTERY_CALIB, EDIT_RSSI_CALIB, EDIT_TIMERS, EDIT_NAME, CONFIRM, CHANNEL_SETTINGS };
    struct ChannelOption {
        uint8_t channel;
        uint16_t apCount;
        uint32_t sumAbsRssi;
    };
    enum class ConfirmAction { NONE, RESET_SLAVE, RESET_REMOTE, POWER_CYCLE };
    bool isEditing() const { return mode != Mode::ROOT; }
    bool isEditingBlanking() const { return mode == Mode::EDIT_BLANKING; }
    bool isEditingTxPower() const { return mode == Mode::EDIT_TXPOWER; }
    bool isEditingBrightness() const { return mode == Mode::EDIT_BRIGHTNESS; }
    int getEditingBlankingSeconds() const { return blankingOptions[blankingIndex]; }
    int getAppliedBlankingSeconds() const { return appliedBlankingSeconds; }
    int8_t getAppliedTxPowerQdbm() const { return appliedTxPowerQdbm; }
    uint8_t getAppliedOledBrightness() const { return appliedOledBrightness; }
    // Applied RSSI calibration bounds (dBm)
    int8_t getAppliedRssiLowDbm() const { return appliedRssiLowDbm; }
    int8_t getAppliedRssiHighDbm() const { return appliedRssiHighDbm; }
    int8_t getEditingTxPowerQdbm() const { return editTxPowerQdbm; }
    uint8_t getEditingOledBrightness() const { return editOledBrightness; }
    void setAppliedTxPowerQdbm(int8_t v) { appliedTxPowerQdbm = v; }
    void setAppliedOledBrightness(uint8_t v) { appliedOledBrightness = v; }
    void setAppliedRssiLowDbm(int8_t v) { appliedRssiLowDbm = v; }
    void setAppliedRssiHighDbm(int8_t v) { appliedRssiHighDbm = v; }
    // Placeholder mode state queries
    // Access current mode
    Mode getMode() const { return mode; }
    bool pairingActive() const { return mode == Mode::PAIRING && pairingScanning; }
    bool renameEditing() const { return mode == Mode::RENAME_DEVICE && renameInEdit; }
    // Rename buffer access for rendering
    const char* getRenameBuffer() const { return renameBuf; }
    int getRenamePos() const { return renamePos; }
    bool batteryCalActive() const { return mode == Mode::BATTERY_CALIB && calibInProgress; }
    bool editingTimers() const { return mode == Mode::EDIT_TIMERS; }
    bool editingName() const { return mode == Mode::EDIT_NAME; }
    bool editingChannels() const { return mode == Mode::CHANNEL_SETTINGS; }
    // RSSI list scroll state
    int getRssiFirst() const { return rssiFirstIndex; }
    void setRssiFirst(int v) { rssiFirstIndex = v; }
    // Selection animation helpers
    int getPrevSelectedIndex() const { return prevSelectedIndex; }
    unsigned long getLastSelectionChangeTime() const { return lastSelectionChangeTime; }
    bool isSelectionAnimating() const { return (mode==Mode::ROOT) && (millis() - lastSelectionChangeTime < SELECTION_ANIM_MS) && (animScrollOffsetAtChange == scrollOffset); }
    static constexpr unsigned long SELECTION_ANIM_MS = 140; // duration of highlight slide
    // Scroll animation helpers
    bool isScrollAnimating() const { return scrollAnimActive && (millis() - scrollAnimStart < SCROLL_ANIM_MS); }
    unsigned long getScrollAnimStart() const { return scrollAnimStart; }
    int getScrollAnimDir() const { return scrollAnimDir; } // +1 = moving down, -1 = moving up
    int getPrevScrollOffset() const { return prevScrollOffset; }
    static constexpr unsigned long SCROLL_ANIM_MS = 140;
private:
    std::vector<MenuItem> items;
    int selectedIndex;
    bool inMenu;
    unsigned long menuEnterTime;
    unsigned long menuExitTime = 0;
public:
    unsigned long getMenuExitTime() const { return menuExitTime; }
private:
    int scrollOffset; // index of top visible item
    unsigned long lastNavTime;
    unsigned long lastSelectTime;
    const char* lastActionLabel = nullptr;
    int prevSelectedIndex = 0; // for highlight animation
    unsigned long lastSelectionChangeTime = 0;
    int animScrollOffsetAtChange = 0; // disable animation if scroll offset changed mid-way
    // Scroll animation state
    bool scrollAnimActive = false;
    unsigned long scrollAnimStart = 0;
    int scrollAnimDir = 0; // +1 down, -1 up
    int prevScrollOffset = 0; // previous offset before animation
    // Mode / editing state
    Mode mode = Mode::ROOT;
    static constexpr int BLANKING_OPTION_COUNT = 7;
    const int blankingOptions[BLANKING_OPTION_COUNT] = {0,15,30,60,120,300,600}; // seconds (0 = off)
    int blankingIndex = 3; // default 60s
    int appliedBlankingSeconds = 60; // currently active (persist later)
    // Placeholder additional mode state
    bool pairingScanning = false;
    int pairingSelIndex = 0; // selection index in discovered list
    // Active device selection state (for SELECT_ACTIVE mode)
    int activeSelIndex = 0; // index navigating paired devices
    bool activeSelectTriggered = false; // set true when user confirms selection
    int activeSelectIndexPending = -1;  // which index was chosen
    bool selectActiveReturnToMain = false; // when true, exiting SELECT_ACTIVE returns to main, otherwise returns to menu root
    int manageSelIndex = 0; // selection inside Manage Devices
public:
    int getPairingSelection() const { return pairingSelIndex; }
    void setPairingSelection(int v) { pairingSelIndex = v; }
    int getActiveSelectIndex() const { return activeSelIndex; }
    bool consumeActiveSelect(int &outIndex) { if (!activeSelectTriggered) return false; activeSelectTriggered=false; outIndex=activeSelectIndexPending; return true; }
    // Remote reset request (handled by main loop)
    bool consumeRemoteReset() { bool v = remoteResetPending; remoteResetPending = false; return v; }
    // Confirmation helpers
    void enterConfirm(ConfirmAction a) { confirmAction = a; mode = Mode::CONFIRM; }
    ConfirmAction getConfirmAction() const { return confirmAction; }
    // Rename editing state
    bool renameInEdit = false;
    char renameBuf[10] = {0}; // 9 + NUL
    int renamePos = 0; // index within renameBuf being edited
    // moved to consolidated calibration state section below
    int getManageSelection() const { return manageSelIndex; }
    void setManageSelection(int v) { manageSelIndex = v; }
    // Mode entry helpers for other items
    void enterPairing();
    void enterManageDevices(); // deprecated (no longer used)
    void enterRename();
    void enterEditName(const char* initialName);
    // Enter Active Timer selection; if returnToMain=true, exit returns to main screen, else to menu
    void enterSelectActive(bool returnToMain = false);
    void enterShowRssi();
    void enterTxPower();
    void enterBrightness();
    void enterBatteryCal();
    void enterRssiCalib();
    void enterEditTimers(float tonSecInit, float toffSecInit);
    void enterChannelSettings();
    // Battery calibration helpers (UI state only)
    void initBatteryCal(uint16_t a0, uint16_t a50, uint16_t a100) { editCalib[0]=a0; editCalib[1]=a50; editCalib[2]=a100; calibInitialized=true; editCalibIndex=0; }
    bool consumeCalibSave(uint16_t out[3]) { if (!calibSavePending) return false; calibSavePending=false; out[0]=editCalib[0]; out[1]=editCalib[1]; out[2]=editCalib[2]; return true; }
    uint16_t getEditCalib(int i) const { return (i>=0&&i<3)?editCalib[i]:0; }
    int getEditCalibIndex() const { return editCalibIndex; }
    void startBlankingEdit();
    void cancelBlankingEdit();
    void confirmBlankingEdit(bool exitMenuAfter = false);
    void setAppliedBlankingSeconds(int seconds) { appliedBlankingSeconds = seconds; blankingIndex = findBlankingIndexFor(seconds); }
    bool consumeBlankingSave(int &outSecs) { if (!blankSavePending) return false; blankSavePending=false; outSecs = appliedBlankingSeconds; return true; }
    bool consumeTxPowerSave(int8_t &outQdbm) { if (!txSavePending) return false; txSavePending=false; outQdbm = editTxPowerQdbm; return true; }
    bool consumeBrightnessSave(uint8_t &outLvl) { if (!brightSavePending) return false; brightSavePending=false; outLvl = editOledBrightness; return true; }
    bool consumeRssiCalibSave(int8_t &outLow, int8_t &outHigh) { if (!rssiSavePending) return false; rssiSavePending=false; outLow = appliedRssiLowDbm; outHigh = appliedRssiHighDbm; return true; }
    bool consumeChannelSave(uint8_t &outChannel) { if (!channelSavePending) return false; channelSavePending = false; outChannel = channelSaveValue; channelCurrent = channelSaveValue; return true; }
    bool consumeChannelScanRequest() { if (!channelScanPending) return false; channelScanPending = false; return true; }
    void setChannelScanResult(const std::vector<ChannelOption>& options, uint8_t currentChannel);
    void setChannelScanFailed();
    bool isChannelScanActive() const { return channelScanActive; }
    bool isChannelScanFailed() const { return channelScanFailed; }
    int getChannelSelection() const { return channelSelection; }
    int getChannelOptionCount() const { return (int)channelOptions.size(); }
    const ChannelOption& getChannelOption(int index) const { return channelOptions[index]; }
    uint8_t getChannelCurrent() const { return channelCurrent; }
    // WiFi TX power (qdbm, 0.25 dBm units)
    int8_t editTxPowerQdbm = 84;
    int8_t appliedTxPowerQdbm = 84;
    bool txSavePending = false;
    // OLED brightness (0..255)
    uint8_t editOledBrightness = 255;
    uint8_t appliedOledBrightness = 255;
    bool brightSavePending = false;
    bool blankSavePending = false;
    // RSSI calibration (dBm)
    int8_t editRssiLowDbm = -100;
    int8_t editRssiHighDbm = -80;
    int8_t appliedRssiLowDbm = -100;
    int8_t appliedRssiHighDbm = -80;
    bool rssiSavePending = false;
    int rssiEditIndex = 0; // 0 = Low, 1 = High
    // Hold state for RSSI edit
    unsigned long rssiHoldStartUp = 0;
    unsigned long rssiHoldStartDown = 0;
    unsigned long rssiLastRepeatMs = 0;
    // Channel selection state
    std::vector<ChannelOption> channelOptions;
    bool channelScanPending = false;
    bool channelScanActive = false;
    bool channelScanFailed = false;
    int channelSelection = 0;
    uint8_t channelCurrent = Defaults::DEFAULT_CHANNEL;
    bool channelSavePending = false;
    uint8_t channelSaveValue = Defaults::DEFAULT_CHANNEL;
    int findBlankingIndexFor(int seconds) const;
    void clampScroll();
    // --- Edit timers state ---
    int editDigitIndex = 0; // 0..(2*Defaults::DIGITS-1); first Defaults::DIGITS are Toff, next are Ton
    int editToffTenths = 0; // tenths of seconds
    int editTonTenths = 0;
    // Repeat state for edit mode
    unsigned long editHoldStartUp = 0;
    unsigned long editHoldStartDown = 0;
    unsigned long editLastRepeatMs = 0;
public:
    int getEditDigitIndex() const { return editDigitIndex; }
    int getEditToffTenths() const { return editToffTenths; }
    int getEditTonTenths() const { return editTonTenths; }
    // Battery calibration UI state
    bool calibInProgress = false;
    bool calibInitialized = false;
    bool calibSavePending = false;
    uint16_t editCalib[3] = {0,0,0};
    int editCalibIndex = 0; // 0..2
    // Hold-to-repeat state for calibration editing
    unsigned long calibHoldStartUp = 0;
    unsigned long calibHoldStartDown = 0;
    unsigned long calibLastRepeatMs = 0;
    // RSSI list scroll pos
    int rssiFirstIndex = 0;
    // Remote reset pending flag
    bool remoteResetPending = false;
    // Power cycle (software restart) pending flag
    bool powerCyclePending = false;
    bool consumePowerCycle() { bool v = powerCyclePending; powerCyclePending = false; return v; }
    // Confirmation state
    ConfirmAction confirmAction = ConfirmAction::NONE;
};
