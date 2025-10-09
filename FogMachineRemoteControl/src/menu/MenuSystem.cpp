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
        {"Pair Device"},
        {"Manage Devices"},
        {"Rename Device"},
        {"Select Active"},
        {"Edit Timers"},
        {"Show RSSI"},
        {"Battery Calibration"},
        {"Display Blanking"}
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
            if (strcmp(label, "Display Blanking") == 0) {
                startBlankingEdit();
            } else if (strcmp(label, "Pair Device") == 0) {
                enterPairing();
            } else if (strcmp(label, "Manage Devices") == 0) {
                enterManageDevices();
            } else if (strcmp(label, "Rename Device") == 0) {
                enterRename();
            } else if (strcmp(label, "Select Active") == 0) {
                enterSelectActive();
            } else if (strcmp(label, "Edit Timers") == 0) {
                auto *comm = CommManager::get();
                float ton=1.0f, toff=1.0f;
                if (comm) {
                    const SlaveDevice* act = comm->getActiveDevice();
                    if (act) { ton = act->ton; toff = act->toff; }
                }
                enterEditTimers(ton, toff);
            } else if (strcmp(label, "Show RSSI") == 0) {
                enterShowRssi();
            } else if (strcmp(label, "Battery Calibration") == 0) {
                enterBatteryCal();
            } else {
                // unknown item -> stay
            }
            return;
        }
    if (starPressed) { exitMenu(); return; }
    // Long hash does not exit while inside menu; star is used for back
    } else if (mode == Mode::EDIT_BLANKING) {
        // EDITING DISPLAY BLANKING
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
    } else if (mode == Mode::PAIRING) {
        auto *comm = CommManager::get();
        bool discovering = comm && comm->isDiscovering();
        // Hash pressed: toggle scanning or pair with selected when not scanning
        if (hashPressed) {
            if (discovering) {
                comm->stopDiscovery();
            } else {
                if (comm->getDiscoveredCount() > 0) {
                    comm->pairWithIndex(pairingSelIndex);
                    // After pairing, exit menu (could instead go to Select Active later)
                    exitMenu();
                } else {
                    comm->startDiscovery();
                }
            }
            return;
        }
        if (!discovering) {
            // Navigate list only when not actively scanning
            if (upPressed && comm && comm->getDiscoveredCount()>0) {
                if (pairingSelIndex > 0) pairingSelIndex--; else pairingSelIndex = comm->getDiscoveredCount()-1;
            }
            if (downPressed && comm && comm->getDiscoveredCount()>0) {
                if (pairingSelIndex < comm->getDiscoveredCount()-1) pairingSelIndex++; else pairingSelIndex = 0;
            }
            // Clamp in case list shrank
            if (comm && pairingSelIndex >= comm->getDiscoveredCount()) pairingSelIndex = comm->getDiscoveredCount()>0 ? comm->getDiscoveredCount()-1 : 0;
        }
        if (starPressed) { if (discovering && comm) comm->stopDiscovery(); mode = Mode::ROOT; return; }
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
    } else if (mode == Mode::RENAME_DEVICE) {
        static char tempName[16] = "";
        if (!renameInEdit) {
            // Enter edit on first hash
            if (hashPressed) { renameInEdit = true; strncpy(tempName, "NAME", sizeof(tempName)-1); return; }
            if (starPressed) { mode = Mode::ROOT; return; }
        } else {
            // Simple demo: edit first character cycling A-Z
            if (upPressed) { if (tempName[0]==0) tempName[0]='A'; else { if (tempName[0]=='Z') tempName[0]='A'; else tempName[0]++; } }
            if (downPressed){ if (tempName[0]==0) tempName[0]='Z'; else { if (tempName[0]=='A') tempName[0]='Z'; else tempName[0]--; } }
            if (hashPressed) { // commit
                auto *comm = CommManager::get(); if (comm) comm->setActiveName(tempName);
                renameInEdit = false; mode = Mode::ROOT; return; }
            if (starPressed) { renameInEdit = false; mode = Mode::ROOT; return; }
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
            exitMenu();
            return;
        }
        if (starPressed) { mode = Mode::ROOT; return; }
    } else if (mode == Mode::SHOW_RSSI) {
        if (starPressed) { mode = Mode::ROOT; return; }
    } else if (mode == Mode::BATTERY_CALIB) {
        if (hashPressed) { calibInProgress = !calibInProgress; return; }
        if (starPressed) { calibInProgress = false; mode = Mode::ROOT; return; }
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
            // Move to next digit; if beyond last, save and exit
            editDigitIndex++;
            if (editDigitIndex >= 2*Defaults::DIGITS) {
                // Send to active device
                auto *comm = CommManager::get();
                if (comm) comm->setActiveTimer((float)editTonTenths/10.0f, (float)editToffTenths/10.0f);
                // Exit edit mode completely
                exitMenu();
            }
            return;
        }
        if (starPressed) { exitMenu(); return; }
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
    // Future: persist to EEPROM & notify power manager
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
void MenuSystem::enterPairing() { mode = Mode::PAIRING; pairingScanning = false; }
void MenuSystem::enterManageDevices() { mode = Mode::MANAGE_DEVICES; }
void MenuSystem::enterRename() { mode = Mode::RENAME_DEVICE; renameInEdit = false; }
void MenuSystem::enterSelectActive() { mode = Mode::SELECT_ACTIVE; }
void MenuSystem::enterShowRssi() { mode = Mode::SHOW_RSSI; }
void MenuSystem::enterBatteryCal() { mode = Mode::BATTERY_CALIB; calibInProgress = false; }
void MenuSystem::enterEditTimers(float tonSecInit, float toffSecInit) {
    inMenu = true; // treat as a modal to capture input reliably
    mode = Mode::EDIT_TIMERS;
    // Convert to tenths and clamp 0..99999
    editTonTenths = (int)roundf(tonSecInit * 10.0f); if (editTonTenths<0) editTonTenths=0; if (editTonTenths>99999) editTonTenths=99999;
    editToffTenths = (int)roundf(toffSecInit * 10.0f); if (editToffTenths<0) editToffTenths=0; if (editToffTenths>99999) editToffTenths=99999;
    editDigitIndex = 0;
}
