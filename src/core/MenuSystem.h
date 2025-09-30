#pragma once
#include <Arduino.h>
#include <cmath>
#include "Defaults.h"
#include "Buttons.h"
#include "Config.h"
#include "Screensaver.h"

class MenuSystem {
public:
  enum class State : uint8_t { INACTIVE, PROGRESS, SELECT, RESULT, SAVER_EDIT };

  void begin();

  State getState() const { return state; }
  bool showMenuHint() const { return menuHint; }
  int getMenuIndex() const { return menuIndex; }
  float getScrollPos() const { return menuScrollPos; }
  int getSelectedMenu() const { return selectedMenu; }
  unsigned long getMenuResultStart() const { return menuResultStart; }
  uint16_t getEditingSaverValue() const { return editingSaverValue; }

  bool inSaverEdit() const { return state == State::SAVER_EDIT; }
  bool inSelect() const { return state == State::SELECT; }
  bool inProgress() const { return state == State::PROGRESS; }
  bool inResult() const { return state == State::RESULT; }

  void startProgress(unsigned long now);

  // Deferred start: invoked only after initial threshold so progress bar begins empty
  void startProgressDeferred(unsigned long now);

  void updateProgress(bool hashHeld, bool hashReleased, unsigned long now);

  void cancel();
  void setMenuHint(bool v) { menuHint = v; if (!v && state==State::INACTIVE) {/*no-op*/} }

  void navigate(const ButtonState& bs, unsigned long now);

  // Returns true if consumed selection
  bool handleSelect(const ButtonState& bs, unsigned long now, Config& config);

  void updateResult(unsigned long now);

  // Saver edit handling (rollover 0<->990; increments of 10)
  bool handleSaverEdit(const ButtonState& bs, unsigned long now, Config& config, Screensaver& saver);

  void animateScroll(unsigned long now);

  float progressFraction(unsigned long now) const;

  bool progressFull(unsigned long now) const;

  static constexpr int MENU_COUNT = 10;
  // Mark MENU_NAMES as inline so no separate definition is required (prevents undefined reference during linking)
  static inline const char* const MENU_NAMES[MENU_COUNT] = {
    "Saver","Menu2","Menu3","Menu4","Menu5","Menu6","Menu7","Menu8","Menu9","Menu10"
  };

private:
  void enterSelect(unsigned long now);
  void beginSaverEdit(uint16_t current);

  // Auto repeat handler for saver edit
  void repeatHandler(const ButtonState& bs, unsigned long now);
  void resetRepeat();

  State state = State::INACTIVE;
  unsigned long hashHoldStart = 0;
  int menuIndex = 0;
  int selectedMenu = -1;
  unsigned long menuResultStart = 0;
  float menuScrollPos = 0.0f;
  unsigned long lastScrollUpdate = 0;
  // Saver edit
  uint16_t editingSaverValue = 0;
  // repeat
  bool repeatInited=false; unsigned long holdStart=0; unsigned long lastStep=0; bool actUp=false; bool actDown=false;
  bool ignoreFirstHashEdgeSaver=false;
  bool menuHint=false; // show 'M' immediately after hash press
};
