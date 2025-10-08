// MenuSystem.cpp
// Handles menu navigation and animated transitions.
#include "MenuSystem.h"
#include "comm/CommManager.h"

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
        {"Show RSSI"},
        {"Battery Calibration"},
        {"Display Blanking"}
    };
}

void MenuSystem::begin() {
    selectedIndex = 0; inMenu = false; scrollOffset = 0; lastNavTime = 0; lastSelectTime = 0; lastActionLabel = nullptr;}

void MenuSystem::update(bool upPressed, bool downPressed, bool hashPressed, bool hashLongPressed, bool starPressed) {
    if (!inMenu) return;
    unsigned long now = millis();
    // Suppress interpreting the continuing long-press that opened the menu until released
    if (enteredHoldActive) {
        if (!hashLongPressed) enteredHoldActive = false; // user released '#' after entry
    }
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
            } else if (strcmp(label, "Show RSSI") == 0) {
                enterShowRssi();
            } else if (strcmp(label, "Battery Calibration") == 0) {
                enterBatteryCal();
            } else {
                // unknown item -> stay
            }
            return;
        }
        if (starPressed && !enteredHoldActive) { exitMenu(); return; }
        // Long hash no longer exits while inside menu (reserved for selection only)
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
        if (starPressed) { mode = Mode::ROOT; return; }
    } else if (mode == Mode::RENAME_DEVICE) {
        if (hashPressed) { renameInEdit = !renameInEdit; return; }
        if (starPressed) { renameInEdit = false; mode = Mode::ROOT; return; }
    } else if (mode == Mode::SELECT_ACTIVE) {
        if (starPressed) { mode = Mode::ROOT; return; }
    } else if (mode == Mode::SELECT_ACTIVE) {
        // Active device selection list
        DeviceManager* dm = nullptr; // will be set via CommManager? (we access through singleton not yet; placeholder)
        // navigation handled here via up/down using device count only when not scanning
        // We will inject actual device list handling in DisplayManager rendering; logic minimal here.
        if (hashPressed) { /* selection handled externally in DisplayManager / main for now */ return; }
        if (starPressed) { mode = Mode::ROOT; return; }
    } else if (mode == Mode::SHOW_RSSI) {
        if (starPressed) { mode = Mode::ROOT; return; }
    } else if (mode == Mode::BATTERY_CALIB) {
        if (hashPressed) { calibInProgress = !calibInProgress; return; }
        if (starPressed) { calibInProgress = false; mode = Mode::ROOT; return; }
    }
    // Housekeeping: turn off scroll animation if elapsed
    if (scrollAnimActive && (millis() - scrollAnimStart >= SCROLL_ANIM_MS)) scrollAnimActive = false;
}

void MenuSystem::enterMenu() {
    inMenu = true;
    menuEnterTime = millis();
    lastActionLabel = nullptr;
    enteredHoldActive = true;
    mode = Mode::ROOT; // always start at root
    prevSelectedIndex = selectedIndex;
    lastSelectionChangeTime = millis();
    animScrollOffsetAtChange = scrollOffset;
}

void MenuSystem::exitMenu() {
    inMenu = false;
    mode = Mode::ROOT; // reset to root
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
