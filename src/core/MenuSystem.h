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

  void begin() {
    state = State::INACTIVE;
    menuIndex = 0;
    menuScrollPos = 0.0f;
  }

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

  void startProgress(unsigned long now) {
    if (state == State::INACTIVE) {
      state = State::PROGRESS;
      hashHoldStart = now;
    }
  }

  // Deferred start: invoked only after initial threshold so progress bar begins empty
  void startProgressDeferred(unsigned long now) {
    if (state == State::INACTIVE) {
      state = State::PROGRESS;
      // Backdate so total required hold time is MENU_PROGRESS_FULL_MS (not + start threshold)
      hashHoldStart = now - Defaults::MENU_PROGRESS_START_MS;
    }
  }

  void updateProgress(bool hashHeld, bool hashReleased, unsigned long now) {
    if (state != State::PROGRESS) return;
    if (hashHeld) { return; }
    unsigned long held = now - hashHoldStart;
    if (held >= Defaults::MENU_PROGRESS_FULL_MS) enterSelect(now); else cancel();
  }

  void cancel() { state = State::INACTIVE; menuHint=false; }
  void setMenuHint(bool v) { menuHint = v; if (!v && state==State::INACTIVE) {/*no-op*/} }

  void navigate(const ButtonState& bs, unsigned long now) {
    if (state != State::SELECT) return;
    if (bs.upEdge) { menuIndex = (menuIndex - 1 + MENU_COUNT) % MENU_COUNT; }
    if (bs.downEdge) { menuIndex = (menuIndex + 1) % MENU_COUNT; }
    animateScroll(now);
  }

  // Returns true if consumed selection
  bool handleSelect(const ButtonState& bs, unsigned long now, Config& config) {
    if (state != State::SELECT) return false;
    if (bs.starEdge) { state = State::INACTIVE; return true; }
    if (bs.hashEdge) {
      selectedMenu = menuIndex;
      if (selectedMenu == 0) { // Saver
        beginSaverEdit(config.get().screensaverDelaySec);
      } else {
        state = State::RESULT;
        menuResultStart = now;
      }
      return true;
    }
    return false;
  }

  void updateResult(unsigned long now) {
    if (state == State::RESULT && (now - menuResultStart) >= Defaults::MENU_RESULT_TIMEOUT_MS) {
      state = State::INACTIVE;
    }
  }

  // Saver edit handling (rollover 0<->990; increments of 10)
  bool handleSaverEdit(const ButtonState& bs, unsigned long now, Config& config, Screensaver& saver) {
    if (state != State::SAVER_EDIT) return false;
    ButtonState local = bs;
    if (ignoreFirstHashEdgeSaver && bs.hashEdge) { local.hashEdge=false; ignoreFirstHashEdgeSaver=false; }
    repeatHandler(local, now);
    bool changed = false;
    if (actUp) {
      if (editingSaverValue == 0) editingSaverValue = 10;
      else if (editingSaverValue == 990) editingSaverValue = 0;
      else editingSaverValue += 10;
      changed = true;
    }
    if (actDown) {
      if (editingSaverValue == 0) editingSaverValue = 990;
      else if (editingSaverValue == 10) editingSaverValue = 0;
      else editingSaverValue -= 10;
      changed = true;
    }
    if (changed) saver.noteActivity(now); // treat edit as activity
    if (local.starEdge) { // cancel
      state = State::SELECT;
      resetRepeat();
      return true;
    }
    if (local.hashEdge) { // save
      config.saveScreensaverIfChanged(editingSaverValue);
      saver.configure(config.get().screensaverDelaySec = editingSaverValue);
      saver.noteActivity(now);
      state = State::SELECT;
      resetRepeat();
      return true;
    }
    return true;
  }

  void animateScroll(unsigned long now) {
    unsigned long dtMs = now - lastScrollUpdate;
    if (dtMs == 0) return;
    lastScrollUpdate = now;
    float dt = dtMs / 1000.0f;
    float target = (float)menuIndex;
    float diff = target - menuScrollPos;
    if (diff > (MENU_COUNT / 2)) diff -= MENU_COUNT; else if (diff < -(MENU_COUNT / 2)) diff += MENU_COUNT;
    float step = Defaults::MENU_SCROLL_SPEED * dt;
    if (fabs(diff) <= step) menuScrollPos = target; else {
      menuScrollPos += (diff > 0 ? step : -step);
      if (menuScrollPos < 0) menuScrollPos += MENU_COUNT; if (menuScrollPos >= MENU_COUNT) menuScrollPos -= MENU_COUNT;
    }
  }

  float progressFraction(unsigned long now) const {
    if (state != State::PROGRESS) return 0.0f;
    unsigned long held = now - hashHoldStart;
    if (held <= Defaults::MENU_PROGRESS_START_MS) return 0.0f; // keep bar empty until threshold passes
    unsigned long span = held - Defaults::MENU_PROGRESS_START_MS;
    unsigned long total = Defaults::MENU_PROGRESS_FULL_MS - Defaults::MENU_PROGRESS_START_MS;
    if (span > total) span = total;
    return (float)span / (float)total;
  }

  bool progressFull(unsigned long now) const {
    return state == State::PROGRESS && (now - hashHoldStart) >= Defaults::MENU_PROGRESS_FULL_MS;
  }

  static constexpr int MENU_COUNT = 10;
  // Mark MENU_NAMES as inline so no separate definition is required (prevents undefined reference during linking)
  static inline const char* const MENU_NAMES[MENU_COUNT] = {
    "Saver","Menu2","Menu3","Menu4","Menu5","Menu6","Menu7","Menu8","Menu9","Menu10"
  };

private:
  void enterSelect(unsigned long now) {
    state = State::SELECT;
    menuScrollPos = (float)menuIndex;
    lastScrollUpdate = now;
    menuHint=false;
  }
  void beginSaverEdit(uint16_t current) {
    editingSaverValue = current - (current % 10);
    state = State::SAVER_EDIT;
    resetRepeat();
    ignoreFirstHashEdgeSaver = true;
  }

  // Auto repeat handler for saver edit
  void repeatHandler(const ButtonState& bs, unsigned long now) {
    if (!repeatInited) { repeatInited=true; holdStart=0; }
    actUp = bs.upEdge; actDown = bs.downEdge;
    bool heldAny = bs.up || bs.down;
    if (heldAny) {
      if (holdStart==0) { holdStart=now; lastStep=now; }
      unsigned long held = now - holdStart;
      if (held > Defaults::EDIT_INITIAL_DELAY_MS) {
        if (now - lastStep >= Defaults::EDIT_REPEAT_INTERVAL_MS) {
          if (bs.up) actUp = true; if (bs.down) actDown = true; lastStep = now;
        } else { actUp = actDown = false; }
      } else {
        if (!bs.upEdge) actUp=false; if (!bs.downEdge) actDown=false;
      }
    } else {
      holdStart=0;
    }
  }
  void resetRepeat() { holdStart=0; lastStep=0; repeatInited=false; actUp=actDown=false; }

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
