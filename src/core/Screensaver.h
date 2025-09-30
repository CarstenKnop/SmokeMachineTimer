#pragma once
#include <Arduino.h>
#include <Adafruit_SSD1306.h>
#include "Defaults.h"
#include "Buttons.h" // for ButtonState

class Screensaver {
public:
  void begin(Adafruit_SSD1306* d) { display=d; }
  void configure(uint16_t delaySec) { delaySeconds=delaySec; recompute(); }
  void noteActivity(unsigned long now) {
    lastActivity = now;
    if (delaySeconds>0) nextBlankAt = lastActivity + (unsigned long)delaySeconds*1000UL; else nextBlankAt=0;
  }
  void loop(unsigned long now) {
    if (!blanked && nextBlankAt!=0 && (long)(now - nextBlankAt) >= 0) {
      display->ssd1306_command(SSD1306_DISPLAYOFF); blanked=true; }
  }
  bool handleWake(const ButtonState& bs, unsigned long now) {
    if (!blanked) return false;
    if (bs.up || bs.down || bs.hash || bs.star) {
      display->ssd1306_command(SSD1306_DISPLAYON);
      blanked=false; noteActivity(now); consume=true; return true;
    }
    return false;
  }
  bool shouldConsume() const { return consume; }
  void clearConsume() { consume=false; }
  bool isBlanked() const { return blanked; }
  uint16_t getDelay() const { return delaySeconds; }
  // Remaining milliseconds until blank; returns 0 if disabled or already blanked
  unsigned long remainingMs(unsigned long now) const {
    if (blanked) return 0;
    if (delaySeconds==0 || nextBlankAt==0) return 0;
    long diff = (long)(nextBlankAt - now);
    if (diff <= 0) return 0;
    return (unsigned long)diff;
  }
  // Convenience: remaining whole seconds (ceil) until blank; 0 if none
  uint16_t remainingSeconds(unsigned long now) const {
    unsigned long ms = remainingMs(now);
    if (ms==0) return 0;
    return (uint16_t)((ms + 999UL)/1000UL);
  }
private:
  void recompute() {
    if (delaySeconds>0) nextBlankAt = lastActivity + (unsigned long)delaySeconds*1000UL; else nextBlankAt=0;
  }
  Adafruit_SSD1306* display=nullptr; uint16_t delaySeconds=0; unsigned long lastActivity=0; unsigned long nextBlankAt=0; bool blanked=false; bool consume=false;
};
