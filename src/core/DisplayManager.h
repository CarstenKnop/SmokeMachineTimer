#pragma once
#include <Adafruit_SSD1306.h>
#include "Defaults.h"
#include "TimerController.h"
#include "MenuSystem.h"
#include "Config.h"
#include "Screensaver.h"

class DisplayManager {
public:
  void begin();
  Adafruit_SSD1306* get() { return &display; }
  void render(const TimerController& timerCtl, const MenuSystem& menu, const Config& config,
              bool blinkState, bool relayOn, uint32_t currentTimerTenths);
  void attachScreensaver(Screensaver* s) { screensaver = s; }
private:
  void splash();
  void printTimerValue(uint32_t value, int y, const char* label, int editDigit, bool editMode, bool blinkState, bool showDecimal, int startX=26);
  void drawProgress(const MenuSystem& menu);
  void drawMenu(const MenuSystem& menu);
  void drawResult(const MenuSystem& menu);
  void drawSaverEdit(const MenuSystem& menu, bool blinkState);
  Adafruit_SSD1306 display{128,64,&Wire,-1}; Screensaver* screensaver=nullptr;
};
