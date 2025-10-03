#include "Help.h"

namespace HelpContent {
  const char* const LINES[LINES_COUNT] = {
    "Help: up/down",
    "#/* exit",
    "# hold: Menu",
    "# tap: Reset",
    "UP/DN: Edit",
    "*: Toggle",
    "Edit: # next",
    "*: Cancel edit",
    "# hold exit",
    "WiFi En toggle",
    "WiFi Rst clears",
    "POST /control",
    "/api/timers",
    "NET=remote set",
    "OTA /update"
  };

  void Controller::enter() {
    scrollInt = 0; scrollPosF = 0.0f; scrollTarget = 0; lastAnimMs = millis();
    if (Serial) Serial.println(F("Entering HELP"));
  }

  bool Controller::handleInput(const ButtonState& bs) {
    if (bs.upEdge) { if (scrollTarget>0) scrollTarget--; }
    if (bs.downEdge) { if (scrollTarget < maxStart()) scrollTarget++; }
    if (bs.hashEdge || bs.starEdge) { if (Serial) Serial.println(F("Exit HELP")); return true; }
    return false;
  }

  void Controller::update(unsigned long now) {
    unsigned long dt = now - lastAnimMs; if (!dt) return; lastAnimMs = now;
    const float speed = 8.0f; // lines/sec
    float diff = (float)scrollTarget - scrollPosF;
    float step = speed * (dt / 1000.0f);
    if (fabs(diff) <= step) { scrollPosF = (float)scrollTarget; scrollInt = scrollTarget; }
    else { scrollPosF += (diff>0? step : -step); scrollInt = (int)floor(scrollPosF + 0.001f); }
  }
}
