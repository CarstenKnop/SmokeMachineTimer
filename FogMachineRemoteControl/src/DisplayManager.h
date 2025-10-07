#pragma once
#include <Adafruit_SSD1306.h>
#include "ESPNowMaster.h"

class UI; // Forward declaration

class DisplayManager {
public:
  void begin();
  void render(const ESPNowMaster& master, const UI& ui);
private:
  Adafruit_SSD1306 display{128,64,&Wire,-1};
  void drawAntenna(int x,int y,int strength);
  void drawBattery(int x,int y,int pct);
};
