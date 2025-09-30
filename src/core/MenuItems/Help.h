#pragma once
#include <Arduino.h>
#include "../Buttons.h"

namespace HelpContent {
  static constexpr int LINES_COUNT = 10;
  static constexpr const char* NAME = "Help";
  extern const char* const LINES[LINES_COUNT];
  inline const char* line(int i){ return (i>=0 && i<LINES_COUNT) ? LINES[i] : ""; }

  struct Controller {
    // Public interface used by MenuSystem
    void enter();
    // returns true if exit requested
  bool handleInput(const ButtonState& bs);
    void update(unsigned long now);
    float scrollPos() const { return scrollPosF; }
    int target() const { return scrollTarget; }
    int visibleLines() const { return VISIBLE_LINES; }
    int maxStart() const { return (LINES_COUNT>VISIBLE_LINES)? (LINES_COUNT - VISIBLE_LINES) : 0; }
    int currentStart() const { return scrollInt; }
  private:
    static constexpr int VISIBLE_LINES = 4; // 64px / 16px per line
    int scrollInt = 0; // integer anchor
    float scrollPosF = 0.0f; // animated position
    int scrollTarget = 0;
    unsigned long lastAnimMs = 0;
  };
}
