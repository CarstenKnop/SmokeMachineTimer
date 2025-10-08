// MenuSystem.h
// Handles menu navigation, progress bar, and animated transitions.
#pragma once
#include <Arduino.h>
#include <vector>

struct MenuItem {
    const char* label;
    // Future: callback id / function pointer
};

class MenuSystem {
public:
    MenuSystem();
    void begin();
    void update(bool upPressed, bool downPressed, bool hashPressed, bool hashLongPressed, bool starPressed);
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
    enum class Mode { ROOT, EDIT_BLANKING, PAIRING, MANAGE_DEVICES, RENAME_DEVICE, SELECT_ACTIVE, SHOW_RSSI, BATTERY_CALIB };
    bool isEditing() const { return mode != Mode::ROOT; }
    bool isEditingBlanking() const { return mode == Mode::EDIT_BLANKING; }
    int getEditingBlankingSeconds() const { return blankingOptions[blankingIndex]; }
    int getAppliedBlankingSeconds() const { return appliedBlankingSeconds; }
    // Placeholder mode state queries
    // Access current mode
    Mode getMode() const { return mode; }
    bool pairingActive() const { return mode == Mode::PAIRING && pairingScanning; }
    bool renameEditing() const { return mode == Mode::RENAME_DEVICE && renameInEdit; }
    bool batteryCalActive() const { return mode == Mode::BATTERY_CALIB && calibInProgress; }
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
    int scrollOffset; // index of top visible item
    unsigned long lastNavTime;
    unsigned long lastSelectTime;
    const char* lastActionLabel = nullptr;
    bool enteredHoldActive = false; // true while the initial long press that opened the menu remains held
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
public:
    int getPairingSelection() const { return pairingSelIndex; }
    void setPairingSelection(int v) { pairingSelIndex = v; }
    bool renameInEdit = false;
    bool calibInProgress = false;
    // Mode entry helpers for other items
    void enterPairing();
    void enterManageDevices();
    void enterRename();
    void enterSelectActive();
    void enterShowRssi();
    void enterBatteryCal();
    void startBlankingEdit();
    void cancelBlankingEdit();
    void confirmBlankingEdit(bool exitMenuAfter = false);
    int findBlankingIndexFor(int seconds) const;
    void clampScroll();
};
