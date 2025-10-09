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
private:
    mutable Adafruit_SSD1306 display;
    bool inited = false;
    void splash();
    void drawBatteryIndicator(uint8_t percent) const;
    void drawMenu(const MenuSystem& menu, const DeviceManager& deviceMgr) const;
    void drawMainScreen(const DeviceManager& deviceMgr, const BatteryMonitor& battery) const;
    void drawProgressBar(unsigned long holdMs, unsigned long longPressMs) const;
    void drawTimerRow(int tenths, int y, const char* label, int startX = 10) const;
};
