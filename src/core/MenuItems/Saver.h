#pragma once
#include <Arduino.h>

class Config; // fwd
class Screensaver; // fwd
struct ButtonState; // fwd

namespace SaverMenu {
  static constexpr const char* LABEL = "Saver"; // legacy usage for edit screen
  static constexpr const char* NAME = "Saver";   // menu display name

  struct EditController {
    void begin(uint16_t current);
    bool handle(const ButtonState& bs, unsigned long now, Config& config, Screensaver& saver); // returns true if state exit
    uint16_t value() const { return editingValue; }
    void setDirtyCallback(void (*cb)()) { dirtyCb = cb; }
  private:
    void repeatHandler(const ButtonState& bs, unsigned long now);
    void resetRepeat();
    uint16_t editingValue=0;
    bool repeatInited=false; unsigned long holdStart=0; unsigned long lastStep=0; bool actUp=false; bool actDown=false;
    bool ignoreFirstHashEdge=false;
    void (*dirtyCb)() = nullptr; // optional hook
  };
}
