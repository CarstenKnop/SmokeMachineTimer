#pragma once
#include <Arduino.h>
#include <Adafruit_SSD1306.h>
#include "Defaults.h"
#include "Buttons.h" // for ButtonState

class Screensaver {
public:
  void begin(Adafruit_SSD1306* d);
  void configure(uint16_t delaySec);
  void noteActivity(unsigned long now);
  void loop(unsigned long now);
  bool handleWake(const ButtonState& bs, unsigned long now);
  bool shouldConsume() const { return consume; }
  void clearConsume() { consume=false; }
  bool isBlanked() const { return blanked; }
  uint16_t getDelay() const { return delaySeconds; }
  unsigned long remainingMs(unsigned long now) const;
  uint16_t remainingSeconds(unsigned long now) const;
private:
  void recompute();
  Adafruit_SSD1306* display=nullptr; uint16_t delaySeconds=0; unsigned long lastActivity=0; unsigned long nextBlankAt=0; bool blanked=false; bool consume=false;
};
