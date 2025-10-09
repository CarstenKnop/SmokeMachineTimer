// DisplayManager.h
// Handles OLED rendering, UI, and battery indicator.
#pragma once
#include <Adafruit_SSD1306.h>
#include "device/DeviceManager.h"
#include "battery/BatteryMonitor.h"
#include "menu/MenuSystem.h"
#include "ui/ButtonInput.h"

class DisplayManager {
public:
    DisplayManager();
    void begin();
    void render(const DeviceManager& deviceMgr, const BatteryMonitor& battery, const MenuSystem& menu, const ButtonInput& buttons);
    bool isOk() const { return inited; }
    bool isBlank() const { return isBlanked; }
    void setSkipSplash(bool v) { skipSplash = v; }
    bool hasError() const { return initFailed; }
    // Minimal UI to show a firmware update countdown at boot
    void drawUpdateCountdown(uint8_t secondsRemaining) const;
    // Draw a one-line boot status at the bottom of the splash (y≈54)
    void drawBootStatus(const char* msg) const;
private:
    mutable Adafruit_SSD1306 display;
    bool inited = false;
    bool initFailed = false;
    bool skipSplash = false;
    // Blanking state
    mutable bool isBlanked = false;
    mutable unsigned long lastWakeMs = 0;
    int selectedSda = -1;
    int selectedScl = -1;
    void splash();
    void drawErrorScreen() const;
    void drawBatteryIndicator(uint8_t percent) const;
    void drawMenu(const MenuSystem& menu, const DeviceManager& deviceMgr) const;
    void drawMainScreen(const DeviceManager& deviceMgr, const BatteryMonitor& battery) const;
    void drawProgressBar(unsigned long holdMs, unsigned long longPressMs) const;
    void drawTimerRow(int tenths, int y, const char* label, int startX = 10) const;
};
