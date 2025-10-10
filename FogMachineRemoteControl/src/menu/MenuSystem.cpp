// MenuSystem.cpp
// Handles menu navigation and animated transitions.
#include "MenuSystem.h"
#include "comm/CommManager.h"
#include "Defaults.h"
#include <math.h>

// Basic parameters for UI behavior
static constexpr int VISIBLE_LINES = 5;      // number of menu lines shown
static constexpr unsigned long NAV_REPEAT_DELAY_MS = 350; // initial hold repeat
static constexpr unsigned long NAV_REPEAT_INTERVAL_MS = 140; // subsequent repeats

MenuSystem::MenuSystem() : selectedIndex(0), inMenu(false), menuEnterTime(0), scrollOffset(0), lastNavTime(0), lastSelectTime(0) {
    items = {
        {"Pair Timer"},
        {"Rename Device"},
        {"Active Timer"},
        {"Edit Timers"},
        {"OLED Brightness"},
        {"WiFi TX Power"},
        {"Show RSSI"},
        {"RSSI Calibration"},
        {"Battery Calibration"},
        {"Reset Timer"},
        {"Reset Remote"},
        {"Auto Off"},
        {"Reset"}
    };
}

void MenuSystem::begin() {
    selectedIndex = 0; inMenu = false; scrollOffset = 0; lastNavTime = 0; lastSelectTime = 0; lastActionLabel = nullptr;}

void MenuSystem::update(bool upPressed, bool downPressed, bool hashPressed, bool hashLongPressed, bool starPressed, bool upHeld, bool downHeld) {
    // Always process when editing timers to keep keys responsive, even if we eventually render over main screen
    if (!inMenu && mode != Mode::EDIT_TIMERS) return;
    unsigned long now = millis();
    (void)hashLongPressed; // intentionally unused here
    if (mode == Mode::ROOT) {
        // ROOT MODE: list navigation
        if (upPressed) {
            prevSelectedIndex = selectedIndex;
            int oldScroll = scrollOffset;
            if (selectedIndex > 0) selectedIndex--; else selectedIndex = (int)items.size() - 1; // wrap
            clampScroll(); lastNavTime = now; lastSelectionChangeTime = now; animScrollOffsetAtChange = scrollOffset;
            if (scrollOffset != oldScroll) { // start scroll animation
                scrollAnimActive = true; scrollAnimStart = now; scrollAnimDir = -1; prevScrollOffset = oldScroll; }
        }
        if (downPressed) {
            prevSelectedIndex = selectedIndex;
            int oldScroll = scrollOffset;
            if (selectedIndex < (int)items.size() - 1) selectedIndex++; else selectedIndex = 0; // wrap
            clampScroll(); lastNavTime = now; lastSelectionChangeTime = now; animScrollOffsetAtChange = scrollOffset;
            if (scrollOffset != oldScroll) { scrollAnimActive = true; scrollAnimStart = now; scrollAnimDir = +1; prevScrollOffset = oldScroll; }
        }
        if (hashPressed) {
            lastSelectTime = now;
            lastActionLabel = items[selectedIndex].label;
            // Determine action
            const char* label = items[selectedIndex].label;
            if (strcmp(label, "Auto Off") == 0) {
                startBlankingEdit();
            } else if (strcmp(label, "Pair Timer") == 0) {
                enterPairing();
            } else if (strcmp(label, "Rename Device") == 0) {
                const char* seed = "NAME"; if (auto *comm = CommManager::get()) { const SlaveDevice* act = comm->getActiveDevice(); if (act && act->name[0]) seed = act->name; }
                enterEditName(seed);
            } else if (strcmp(label, "Active Timer") == 0) {
                enterSelectActive(false);
            } else if (strcmp(label, "Edit Timers") == 0) {
                auto *comm = CommManager::get();
                float ton=1.0f, toff=1.0f;
                if (comm) {
                    const SlaveDevice* act = comm->getActiveDevice();
                    if (act) { ton = act->ton; toff = act->toff; }
                }
                enterEditTimers(ton, toff);
            } else if (strcmp(label, "WiFi TX Power") == 0) {
                enterTxPower();
            } else if (strcmp(label, "OLED Brightness") == 0) {
                enterBrightness();
            } else if (strcmp(label, "RSSI Calibration") == 0) {
                enterRssiCalib();
            } else if (strcmp(label, "Show RSSI") == 0) {
                enterShowRssi();
            } else if (strcmp(label, "Battery Calibration") == 0) {
                enterBatteryCal();
            } else if (strcmp(label, "Reset Timer") == 0) {
                enterConfirm(ConfirmAction::RESET_SLAVE);
            } else if (strcmp(label, "Reset Remote") == 0) {
                enterConfirm(ConfirmAction::RESET_REMOTE);
            } else if (strcmp(label, "Reset") == 0) {
                enterConfirm(ConfirmAction::POWER_CYCLE);
            } else {
                // unknown item -> stay
            }
            return;
        }
    if (starPressed) { exitMenu(); return; }
    // Long hash does not exit while inside menu; star is used for back
    } else if (mode == Mode::EDIT_BLANKING) {
        // EDITING AUTO OFF
        if (upPressed) {
            if (blankingIndex < BLANKING_OPTION_COUNT - 1) blankingIndex++;
        }
        if (downPressed) {
            if (blankingIndex > 0) blankingIndex--;
        }
        if (starPressed) { // cancel -> revert index to applied value and go back to root
            cancelBlankingEdit();
            return;
        }
        if (hashPressed) { // confirm -> apply and go back to root
            confirmBlankingEdit(false); // stay in menu
            return;
        }
        // Long hash no-op while editing (selection done on short press)
    } else if (mode == Mode::EDIT_TXPOWER) {
        // Up/Down adjust in steps of 1 qdbm (0.25 dBm). Clamp 0..84.
        if (upPressed)   { if (editTxPowerQdbm < 84) editTxPowerQdbm++; }
        if (downPressed) { if (editTxPowerQdbm > 0)  editTxPowerQdbm--; }
        if (starPressed) { // cancel -> revert to applied and back to root
            editTxPowerQdbm = appliedTxPowerQdbm; mode = Mode::ROOT; return;
        }
        if (hashPressed) { // save
            txSavePending = true; appliedTxPowerQdbm = editTxPowerQdbm; mode = Mode::ROOT; return;
        }
    } else if (mode == Mode::EDIT_BRIGHTNESS) {
        // Up/Down adjust brightness (0..255)
        if (upPressed)   { if (editOledBrightness < 255) editOledBrightness = (uint8_t)min<int>(255, editOledBrightness + 5); }
        if (downPressed) { if (editOledBrightness > 5)   editOledBrightness = (uint8_t)max<int>(5, editOledBrightness - 5); else editOledBrightness = 5; }
        if (starPressed) { editOledBrightness = appliedOledBrightness; if (editOledBrightness < 5) editOledBrightness = 5; mode = Mode::ROOT; return; }
        if (hashPressed) { if (editOledBrightness < 5) editOledBrightness = 5; brightSavePending = true; appliedOledBrightness = editOledBrightness; mode = Mode::ROOT; return; }
    } else if (mode == Mode::PAIRING) {
        auto *comm = CommManager::get();
        if (!comm) { if (starPressed) { mode = Mode::ROOT; } return; }
        // Ensure discovery runs continuously on this screen
        if (!comm->isDiscovering()) comm->startDiscovery(0);
        int discCount = comm->getDiscoveredCount();
        // Up/Down navigate discovered list (wrap)
        if (upPressed && discCount>0) {
            if (pairingSelIndex > 0) pairingSelIndex--; else pairingSelIndex = discCount-1;
        }
        if (downPressed && discCount>0) {
            if (pairingSelIndex < discCount-1) pairingSelIndex++; else pairingSelIndex = 0;
        }
        if (pairingSelIndex >= discCount) pairingSelIndex = discCount>0 ? discCount-1 : 0;
        // '#' pairs or unpairs the selected device, stays on screen for multiple operations
        if (hashPressed && discCount>0) {
            const auto &d = comm->getDiscovered(pairingSelIndex);
            int pairedIdx = comm->findPairedIndexByMac(d.mac);
            if (pairedIdx >= 0) {
                // Unpair
                comm->unpairByMac(d.mac);
            } else {
                // Pair
                comm->pairWithIndex(pairingSelIndex);
            }
            return;
        }
        if (starPressed) { mode = Mode::ROOT; return; }
    } else if (mode == Mode::MANAGE_DEVICES) {
        // Manage Devices: Up/Down navigate paired device list
        auto *comm = CommManager::get();
        int count = comm ? comm->getPairedCount() : 0;
        if (upPressed && count>0) { if (manageSelIndex>0) manageSelIndex--; else manageSelIndex = count-1; }
        if (downPressed && count>0) { if (manageSelIndex < count-1) manageSelIndex++; else manageSelIndex = 0; }
        if (hashPressed && count>0) {
            // Short press: activate this device
            comm->activateDeviceByIndex(manageSelIndex);
            return;
        }
        // Long press hash to delete (if more than one device) – simple heuristic: detect if hashLongPressed and not just pressed
        if (hashLongPressed && !hashPressed && count>0) {
            if (count>1) {
                comm->removeDeviceByIndex(manageSelIndex);
                int newCount = comm->getPairedCount();
                if (manageSelIndex >= newCount) manageSelIndex = newCount>0?newCount-1:0;
            }
            return;
        }
        if (starPressed) { mode = Mode::ROOT; return; }
    } else if (mode == Mode::EDIT_NAME) {
        // Name editor: small font per-char editing, extended charset with hold-to-repeat
        auto *comm = CommManager::get();
        unsigned long nowMs = millis();
        static unsigned long nameHoldStartUp = 0, nameHoldStartDown = 0, nameLastRepeatMs = 0;
        // Character set order: space, 0-9, A-Z, a-z
        static const char* CHARSET = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
        static const int CHARSET_LEN = 1 + 10 + 26 + 26;
        auto nextChar=[&](char c){
            // Default to space if unknown
            int idx = 0;
            for (int i=0;i<CHARSET_LEN;i++){ if (CHARSET[i]==c){ idx=i; break; } }
            return CHARSET[(idx+1)%CHARSET_LEN];
        };
        auto prevChar=[&](char c){
            int idx = 0;
            for (int i=0;i<CHARSET_LEN;i++){ if (CHARSET[i]==c){ idx=i; break; } }
            return CHARSET[(idx-1+CHARSET_LEN)%CHARSET_LEN];
        };
        auto applyChar=[&](int dir){
            char &ch = renameBuf[renamePos];
            if (ch==0) ch=' ';
            ch = (dir>0) ? nextChar(ch) : prevChar(ch);
        };
        if (upPressed)   { applyChar(+1); nameHoldStartUp = nowMs; nameLastRepeatMs = 0; return; }
        if (downPressed) { applyChar(-1); nameHoldStartDown = nowMs; nameLastRepeatMs = 0; return; }
        bool anyHeld=false; if (upHeld) { anyHeld=true; if (!nameHoldStartUp) nameHoldStartUp=nowMs; } else nameHoldStartUp=0; if (downHeld) { anyHeld=true; if (!nameHoldStartDown) nameHoldStartDown=nowMs; } else nameHoldStartDown=0;
        if (anyHeld) {
            unsigned long start = nameHoldStartUp ? nameHoldStartUp : nameHoldStartDown;
            if (nowMs - start >= Defaults::EDIT_INITIAL_DELAY_MS) {
                if (nameLastRepeatMs==0 || (nowMs - nameLastRepeatMs) >= Defaults::EDIT_REPEAT_INTERVAL_MS) {
                    if (upHeld) applyChar(+1); if (downHeld) applyChar(-1);
                    nameLastRepeatMs = nowMs; return;
                }
            }
        } else { nameLastRepeatMs = 0; }
        // '#' short press: move right; at end -> save and exit. Long-press: move left
        if (hashPressed) {
            if (renamePos < (int)sizeof(renameBuf)-2) { renamePos++; if (renameBuf[renamePos]==0) renameBuf[renamePos]=' '; return; }
            if (comm) comm->setActiveName(renameBuf);
            mode = Mode::ROOT; return;
        }
        if (hashLongPressed && !hashPressed) { // long hold -> move left
            if (renamePos > 0) renamePos--; else {
                // wrap to last non-NUL position
                int last=0; for (int i=0; i<(int)sizeof(renameBuf)-1; ++i) { if (renameBuf[i]==0) break; last=i; }
                renamePos = last;
            }
            return;
        }
        if (starPressed) {
            // STAR cancels/returns: exit without saving
            mode = Mode::ROOT; return;
        }
    } else if (mode == Mode::SELECT_ACTIVE) {
        // Navigate paired device list (DeviceManager consulted only for count via external code / DisplayManager)
        // We can't access DeviceManager directly here without coupling; we rely on main/Display to clamp.
        if (upPressed) {
            if (activeSelIndex > 0) activeSelIndex--; else activeSelIndex = 0; // clamp; main may wrap if desired
        }
        if (downPressed) {
            activeSelIndex++; // will be clamped externally if beyond count
        }
        if (hashPressed) {
            // Trigger selection consumption in main loop
            activeSelectTriggered = true;
            activeSelectIndexPending = activeSelIndex;
            if (selectActiveReturnToMain) {
                // Exit to main screen (leave inMenu=false)
                inMenu = false; mode = Mode::ROOT; menuExitTime = millis();
            } else {
                // Return to menu root
                mode = Mode::ROOT;
            }
            return;
        }
        if (starPressed) {
            if (selectActiveReturnToMain) { inMenu = false; mode = Mode::ROOT; menuExitTime = millis(); }
            else { mode = Mode::ROOT; }
            return;
        }
    } else if (mode == Mode::SHOW_RSSI) {
        auto *comm = CommManager::get();
        int count = comm ? comm->getPairedCount() : 0;
        // Up/Down scroll list
        if (upPressed) { if (rssiFirstIndex > 0) rssiFirstIndex--; }
        if (downPressed) { if (rssiFirstIndex < (count>0?count-1:0)) rssiFirstIndex++; }
        // '#' refresh all RSSI by requesting status from each paired device
        if (hashPressed && comm) {
            for (int i=0;i<comm->getPairedCount();++i) {
                comm->requestStatus(comm->getPaired(i));
            }
        }
        if (starPressed) { mode = Mode::ROOT; return; }
    } else if (mode == Mode::BATTERY_CALIB) {
        // Battery Calibration keys with hold-to-repeat:
        // - Before start: '#' begins editing, '*' cancels/back
        // - During edit: Up/Down adjust current point (with repeat); '#' advances and saves at the end; '*' cancels without saving
        if (!calibInProgress) {
            if (hashPressed) { calibInProgress = true; editCalibIndex = 0; return; }
            if (starPressed) { mode = Mode::ROOT; return; }
            // Ignore other keys until started
        } else {
            // Edge increments
            if (upPressed)   { editCalib[editCalibIndex] = (uint16_t)min(4095, (int)editCalib[editCalibIndex] + 5); calibHoldStartUp = now; calibLastRepeatMs = 0; }
            if (downPressed){ editCalib[editCalibIndex] = (uint16_t)max(0,    (int)editCalib[editCalibIndex] - 5); calibHoldStartDown = now; calibLastRepeatMs = 0; }
            // Hold-to-repeat
            bool anyHeld = false;
            if (upHeld)   { anyHeld = true; if (calibHoldStartUp==0) calibHoldStartUp = now; }
            else calibHoldStartUp = 0;
            if (downHeld) { anyHeld = true; if (calibHoldStartDown==0) calibHoldStartDown = now; }
            else calibHoldStartDown = 0;
            if (anyHeld) {
                unsigned long start = calibHoldStartUp ? calibHoldStartUp : calibHoldStartDown;
                if (now - start >= Defaults::EDIT_INITIAL_DELAY_MS) {
                    if (calibLastRepeatMs==0 || (now - calibLastRepeatMs) >= Defaults::EDIT_REPEAT_INTERVAL_MS) {
                        if (upHeld)   editCalib[editCalibIndex] = (uint16_t)min(4095, (int)editCalib[editCalibIndex] + 5);
                        if (downHeld) editCalib[editCalibIndex] = (uint16_t)max(0,    (int)editCalib[editCalibIndex] - 5);
                        calibLastRepeatMs = now;
                        return;
                    }
                }
            } else {
                calibLastRepeatMs = 0;
            }
            if (hashPressed) {
                if (editCalibIndex < 2) { editCalibIndex++; }
                else { calibSavePending = true; mode = Mode::ROOT; calibInProgress = false; return; }
            }
            if (starPressed) { mode = Mode::ROOT; calibInProgress = false; return; }
        }
    } else if (mode == Mode::EDIT_RSSI_CALIB) {
        // Edit Low (0 bars) and High (6 bars) thresholds in dBm with hold-to-repeat
        auto clamp=[&](){
            if (editRssiHighDbm < editRssiLowDbm + 5) editRssiHighDbm = (int8_t)(editRssiLowDbm + 5);
            if (editRssiHighDbm > 0) editRssiHighDbm = 0;
            if (editRssiLowDbm < -120) editRssiLowDbm = -120;
        };
        if (upPressed)   { if (rssiEditIndex==0) editRssiLowDbm++; else editRssiHighDbm++; rssiHoldStartUp = now; rssiLastRepeatMs=0; clamp(); return; }
        if (downPressed) { if (rssiEditIndex==0) editRssiLowDbm--; else editRssiHighDbm--; rssiHoldStartDown = now; rssiLastRepeatMs=0; clamp(); return; }
        bool anyHeld=false;
        if (upHeld)   { anyHeld=true; if (!rssiHoldStartUp) rssiHoldStartUp=now; } else rssiHoldStartUp=0;
        if (downHeld) { anyHeld=true; if (!rssiHoldStartDown) rssiHoldStartDown=now; } else rssiHoldStartDown=0;
        if (anyHeld) {
            unsigned long start = rssiHoldStartUp ? rssiHoldStartUp : rssiHoldStartDown;
            if (now - start >= Defaults::EDIT_INITIAL_DELAY_MS) {
                if (rssiLastRepeatMs==0 || (now - rssiLastRepeatMs) >= Defaults::EDIT_REPEAT_INTERVAL_MS) {
                    if (upHeld)   { if (rssiEditIndex==0) editRssiLowDbm++; else editRssiHighDbm++; }
                    if (downHeld) { if (rssiEditIndex==0) editRssiLowDbm--; else editRssiHighDbm--; }
                    rssiLastRepeatMs = now; clamp(); return;
                }
            }
        } else {
            rssiLastRepeatMs = 0;
        }
        if (hashPressed) {
            if (rssiEditIndex == 0) { rssiEditIndex = 1; }
            else { appliedRssiLowDbm = editRssiLowDbm; appliedRssiHighDbm = editRssiHighDbm; rssiSavePending = true; mode = Mode::ROOT; return; }
            return;
        }
        if (starPressed) { mode = Mode::ROOT; return; }
    } else if (mode == Mode::EDIT_TIMERS) {
        // Edit two timer values in tenths: first TOFF digits [0..DIGITS-1], then TON digits [DIGITS..2*DIGITS-1]
        auto tweakDigit=[&](int &tenths, int whichDigit, int delta){
            // whichDigit 0..(Defaults::DIGITS-1) with 0=most significant
            // Change only that digit (wrap within 0..9), no carry to other digits
            int pow10 = 1;
            for (int i=0;i<Defaults::DIGITS-whichDigit-1;i++) pow10 *= 10; // digit place weight
            int digit = (tenths / pow10) % 10;
            digit = (digit + delta) % 10; if (digit < 0) digit += 10;
            // Clear that digit and set new value
            tenths = tenths - ((tenths / pow10) % 10) * pow10 + digit * pow10;
            if (tenths < 0) tenths = 0; if (tenths > 99999) tenths = 99999;
        };
        int which = editDigitIndex;
        bool editingToff = (which < Defaults::DIGITS);
        int digitIn = editingToff ? which : (which - Defaults::DIGITS);
        auto applyStep=[&](int s){ if (editingToff) tweakDigit(editToffTenths, digitIn, s); else tweakDigit(editTonTenths, digitIn, s); };
        // Edge presses
        if (upPressed)   { applyStep(+1); editHoldStartUp = now; editLastRepeatMs = 0; return; }
        if (downPressed) { applyStep(-1); editHoldStartDown = now; editLastRepeatMs = 0; return; }
        // Hold-to-repeat
        bool anyHeld = false;
        if (upHeld)   { anyHeld = true; if (editHoldStartUp==0) editHoldStartUp=now; }
        else editHoldStartUp = 0;
        if (downHeld) { anyHeld = true; if (editHoldStartDown==0) editHoldStartDown=now; }
        else editHoldStartDown = 0;
        if (anyHeld) {
            unsigned long start = editHoldStartUp ? editHoldStartUp : editHoldStartDown;
            if (now - start >= Defaults::EDIT_INITIAL_DELAY_MS) {
                if (editLastRepeatMs==0 || (now - editLastRepeatMs) >= Defaults::EDIT_REPEAT_INTERVAL_MS) {
                    if (upHeld) applyStep(+1); if (downHeld) applyStep(-1);
                    editLastRepeatMs = now;
                    return;
                }
            }
        } else {
            editLastRepeatMs = 0;
        }
        if (hashPressed) {
            // Move to next digit; if beyond last, clamp to slave bounds, save and exit
            editDigitIndex++;
            if (editDigitIndex >= 2*Defaults::DIGITS) {
                // Send to active device
                auto *comm = CommManager::get();
                // Clamp to slave min/max (tenths) before sending
                int toff = editToffTenths;
                int ton  = editTonTenths;
                if (toff < (int)Defaults::SLAVE_TIMER_MIN_TENTHS) toff = (int)Defaults::SLAVE_TIMER_MIN_TENTHS;
                if (toff > (int)Defaults::SLAVE_TIMER_MAX_TENTHS) toff = (int)Defaults::SLAVE_TIMER_MAX_TENTHS;
                if (ton  < (int)Defaults::SLAVE_TIMER_MIN_TENTHS) ton  = (int)Defaults::SLAVE_TIMER_MIN_TENTHS;
                if (ton  > (int)Defaults::SLAVE_TIMER_MAX_TENTHS) ton  = (int)Defaults::SLAVE_TIMER_MAX_TENTHS;
                if (comm) comm->setActiveTimer((float)ton/10.0f, (float)toff/10.0f);
                // Exit edit mode completely
                exitMenu();
            }
            return;
        }
        // Long-press '#' moves cursor left for symmetry
        if (hashLongPressed && !hashPressed) {
            if (editDigitIndex > 0) editDigitIndex--; else editDigitIndex = 2*Defaults::DIGITS - 1;
            return;
        }
        if (starPressed) { exitMenu(); return; }
    } else if (mode == Mode::CONFIRM) {
        if (hashPressed) {
            if (confirmAction == ConfirmAction::RESET_SLAVE) {
                if (auto *comm = CommManager::get()) comm->factoryResetActive();
                exitMenu();
                return;
            } else if (confirmAction == ConfirmAction::RESET_REMOTE) {
                remoteResetPending = true;
                exitMenu();
                return;
            } else if (confirmAction == ConfirmAction::POWER_CYCLE) {
                // Request a power cycle (software restart) of the remote
                powerCyclePending = true;
                exitMenu();
                return;
            }
        }
        if (starPressed) { mode = Mode::ROOT; return; }
    }
    // Housekeeping: turn off scroll animation if elapsed
    if (scrollAnimActive && (millis() - scrollAnimStart >= SCROLL_ANIM_MS)) scrollAnimActive = false;
}

void MenuSystem::enterMenu() {
    inMenu = true;
    menuEnterTime = millis();
    lastActionLabel = nullptr;
    mode = Mode::ROOT; // always start at root
    prevSelectedIndex = selectedIndex;
    lastSelectionChangeTime = millis();
    animScrollOffsetAtChange = scrollOffset;
}

void MenuSystem::exitMenu() {
    inMenu = false;
    mode = Mode::ROOT; // reset to root
    menuExitTime = millis();
}

void MenuSystem::nextItem() {
    if (inMenu && selectedIndex < (int)items.size() - 1) { selectedIndex++; clampScroll(); }
}

void MenuSystem::prevItem() {
    if (inMenu && selectedIndex > 0) { selectedIndex--; clampScroll(); }
}

int MenuSystem::getSelectedIndex() const {
    return selectedIndex;
}

const char* MenuSystem::getCurrentMenuName() const {
    if (inMenu && selectedIndex < (int)items.size()) return items[selectedIndex].label;
    return "";
}

bool MenuSystem::isInMenu() const {
    return inMenu;
}

int MenuSystem::getVisibleStart() const { return scrollOffset; }

int MenuSystem::getVisibleCount(int maxLines) const {
    int remain = (int)items.size() - scrollOffset;
    return remain < maxLines ? remain : maxLines;
}

bool MenuSystem::isAnimatingEnter() const {
    if (!inMenu) return false;
    return millis() - menuEnterTime < 200; // simple 200ms enter phase
}

void MenuSystem::clampScroll() {
    if (selectedIndex < scrollOffset) scrollOffset = selectedIndex;
    else if (selectedIndex >= scrollOffset + VISIBLE_LINES) scrollOffset = selectedIndex - VISIBLE_LINES + 1;
    if (scrollOffset < 0) scrollOffset = 0;
    if (scrollOffset > (int)items.size() - 1) scrollOffset = (int)items.size() - 1;
}

// --- Blanking edit helpers ---
void MenuSystem::startBlankingEdit() {
    mode = Mode::EDIT_BLANKING;
    // Set index to current applied value so editing starts from active setting
    blankingIndex = findBlankingIndexFor(appliedBlankingSeconds);
}

void MenuSystem::cancelBlankingEdit() {
    // Restore index to applied (already) and return to root
    blankingIndex = findBlankingIndexFor(appliedBlankingSeconds);
    mode = Mode::ROOT;
}

void MenuSystem::confirmBlankingEdit(bool exitMenuAfter) {
    appliedBlankingSeconds = blankingOptions[blankingIndex];
    blankSavePending = true; // signal persistence
    mode = Mode::ROOT;
    if (exitMenuAfter) exitMenu();
}

int MenuSystem::findBlankingIndexFor(int seconds) const {
    for (int i=0;i<BLANKING_OPTION_COUNT;++i) if (blankingOptions[i]==seconds) return i;
    // If not found, clamp to nearest
    if (seconds < blankingOptions[0]) return 0;
    if (seconds > blankingOptions[BLANKING_OPTION_COUNT-1]) return BLANKING_OPTION_COUNT-1;
    // fallback
    return 0;
}

// --- Other mode helpers ---
void MenuSystem::enterPairing() {
    inMenu = true;
    mode = Mode::PAIRING;
    pairingScanning = false;
    pairingSelIndex = 0;
    // Auto-start discovery for better UX
    if (auto *comm = CommManager::get()) {
        if (!comm->isDiscovering()) comm->startDiscovery(0); // continuous while on screen
    }
}
void MenuSystem::enterManageDevices() { inMenu = true; mode = Mode::MANAGE_DEVICES; }
void MenuSystem::enterRename() { mode = Mode::RENAME_DEVICE; renameInEdit = false; }
void MenuSystem::enterEditName(const char* initialName) {
    inMenu = true; mode = Mode::EDIT_NAME; strncpy(renameBuf, initialName, sizeof(renameBuf)-1); renameBuf[sizeof(renameBuf)-1]=0; renamePos = 0;
}
void MenuSystem::enterSelectActive(bool returnToMain) { inMenu = true; mode = Mode::SELECT_ACTIVE; selectActiveReturnToMain = returnToMain; }
void MenuSystem::enterShowRssi() {
    mode = Mode::SHOW_RSSI;
    if (auto *comm = CommManager::get()) comm->requestStatusActive();
}
void MenuSystem::enterBatteryCal() { mode = Mode::BATTERY_CALIB; calibInProgress = false; }
void MenuSystem::enterRssiCalib() {
    inMenu = true;
    mode = Mode::EDIT_RSSI_CALIB;
    // Reset edit focus to Low and seed edit values from applied on entry
    rssiEditIndex = 0; // start editing Low
    editRssiLowDbm = appliedRssiLowDbm;
    editRssiHighDbm = appliedRssiHighDbm;
}
void MenuSystem::enterEditTimers(float tonSecInit, float toffSecInit) {
    inMenu = true; // treat as a modal to capture input reliably
    mode = Mode::EDIT_TIMERS;
    // Convert to tenths and clamp 0..99999
    editTonTenths = (int)roundf(tonSecInit * 10.0f); if (editTonTenths<0) editTonTenths=0; if (editTonTenths>99999) editTonTenths=99999;
    editToffTenths = (int)roundf(toffSecInit * 10.0f); if (editToffTenths<0) editToffTenths=0; if (editToffTenths>99999) editToffTenths=99999;
    editDigitIndex = 0;
}

void MenuSystem::enterTxPower() {
    mode = Mode::EDIT_TXPOWER;
    // Seed edit value from applied so the UI starts at the current setting
    editTxPowerQdbm = appliedTxPowerQdbm;
}

void MenuSystem::enterBrightness() {
    mode = Mode::EDIT_BRIGHTNESS;
    // Seed edit value from applied so the UI starts at the current setting
    editOledBrightness = appliedOledBrightness;
}
