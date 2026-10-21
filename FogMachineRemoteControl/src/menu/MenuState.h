// MenuState.h - everything the screens need to know about the menu (pure data, no hardware).
#pragma once

#include <stdint.h>
#include <string.h>
#include "FogProto.h"

struct MenuState {
    enum class Mode : uint8_t {
        ROOT, EDIT_AUTO_OFF, EDIT_TXPOWER, EDIT_BRIGHTNESS, PAIRING, SELECT_ACTIVE, LINK_STATUS, BATTERY_CALIB,
        EDIT_RSSI_CALIB, EDIT_TIMERS, EDIT_NAME, CONFIRM, CHANNEL, FIND_TIMERS, LINK_TEST
    };
    enum class ConfirmAction : uint8_t { NONE, RESET_TIMER, RESET_REMOTE, REBOOT };
    enum class ChannelStage : uint8_t { Scanning, Choose, Moving, Result, Failed };

    static constexpr int kItemCount = 17;
    static constexpr int kVisibleLines = 5;
    static constexpr uint32_t SELECTION_ANIM_MS = 140;
    static constexpr uint32_t SCROLL_ANIM_MS = 140;
    static const char* const kItemLabels[kItemCount];
    static const int kAutoOffOptions[7];

    Mode mode = Mode::ROOT;
    bool inMenu = false;

    // root list
    int selected = 0, scrollOffset = 0, prevSelected = 0, prevScroll = 0, scrollDir = 0;
    bool scrollAnim = false;
    uint32_t lastSelChange = 0, scrollAnimStart = 0;
    int animScroll = 0;

    // editors
    int autoOffIdx = 3;
    int8_t editTxPowerQdbm = 80;
    uint8_t editBrightness = 255;
    int editDigitIndex = 0;                    // 0..9: first 5 = Toff digits, next 5 = Ton digits
    int editToffTenths = 0, editTonTenths = 0;
    char nameBuf[Fog::kNameLen] = {0};
    int namePos = 0;
    int8_t editRssiLow = -100, editRssiHigh = -80;
    int rssiField = 0;                         // 0 = low, 1 = high
    bool calibStarted = false;
    uint16_t editCal[3] = {3300, 3750, 4150};
    int calField = 0;
    ConfirmAction confirmAction = ConfirmAction::NONE;

    // lists
    int pairSel = 0;
    int activeSel = 0;
    int linkFirst = 0;

    // channel screen
    ChannelStage chStage = ChannelStage::Scanning;
    uint8_t chOrder[Fog::kMaxChannel] = {0};   // quietest first
    int chCount = 0;
    int chSel = 0;
    uint8_t chTarget = 0;

    int autoOffSeconds() const { return kAutoOffOptions[autoOffIdx]; }
    int visibleStart() const { return scrollOffset; }
    int visibleCount() const { const int r = kItemCount - scrollOffset; return r < kVisibleLines ? r : kVisibleLines; }
    bool scrollAnimating(uint32_t now) const { return scrollAnim && (now - scrollAnimStart < SCROLL_ANIM_MS); }
    bool selectionAnimating(uint32_t now) const { return mode == Mode::ROOT && (now - lastSelChange < SELECTION_ANIM_MS) && animScroll == scrollOffset; }
};
