#pragma once
#include <Arduino.h>
#include "Defaults.h"
#include "Config.h"

class TimerController {
public:
  enum class AppState : uint8_t { RUN, EDIT };

  void begin(Config::Values* cfgVals) {
    cfg = cfgVals;
    relayState = false;
    timer = 0;
    lastTickMs = millis();
  }

  void tick(unsigned long now) {
    if (state != AppState::RUN) return;
    // Accumulate elapsed time and convert to tenths (100ms)
    unsigned long elapsed = now - lastTickMs;
    if (elapsed >= 100) {
      unsigned long steps = elapsed / 100; // number of 0.1s intervals
      lastTickMs += steps * 100; // advance baseline
      while (steps--) {
        if (relayState) {
          if (timer < cfg->onTime) timer++; else { relayState = false; timer = 0; }
        } else {
          if (timer < cfg->offTime) timer++; else { relayState = true; timer = 0; }
        }
      }
    }
  }

  void resetCycle() { relayState = false; timer = 0; }
  void toggleRelayManual() { relayState = !relayState; timer = 0; }

  uint32_t currentTimer() const { return timer; }
  bool isRelayOn() const { return relayState; }

  // Editing logic (digit-by-digit) reused from original code (simplified interface)
  void enterEdit() { state = AppState::EDIT; editDigit = 0; digitsInit=false; cancelled=false; snapshotOff=cfg->offTime; snapshotOn=cfg->onTime; }
  bool inEdit() const { return state == AppState::EDIT; }
  AppState getState() const { return state; }

  bool handleEdit(const ButtonState& bs, unsigned long now, bool& valuesChanged, bool& exited) {
    exited = false; valuesChanged = false;
    static bool requireRelease=false; static bool firstCycle=true; static unsigned long holdStart=0; static unsigned long lastStep=0; 
    // Hash long-hold for early exit (2s) like legacy behavior
    static unsigned long hashHoldStart=0; static bool hashWasHeld=false;
    if (!digitsInit) loadDigits();
    bool upHeld=bs.up, downHeld=bs.down; bool actUp=bs.upEdge, actDown=bs.downEdge;
    if (firstCycle) { requireRelease = true; firstCycle=false; }
    if (requireRelease) {
      if (!upHeld && !downHeld) { requireRelease=false; holdStart=0; }
      actUp=actDown=false;
    } else {
      if (upHeld || downHeld) {
        if (holdStart==0) { holdStart=now; lastStep=now; }
        unsigned long held=now-holdStart;
        if (held > Defaults::EDIT_INITIAL_DELAY_MS) {
          if (now - lastStep >= Defaults::EDIT_REPEAT_INTERVAL_MS) {
            if (upHeld) actUp=true; if (downHeld) actDown=true; lastStep=now; }
          else { actUp=actDown=false; }
        } else {
          if (!bs.upEdge) actUp=false; if (!bs.downEdge) actDown=false;
        }
      } else { holdStart=0; }
    }

    // Track hash hold (for early exit)
    if (bs.hash) { if (hashHoldStart==0) hashHoldStart=now; }
    else { hashHoldStart=0; hashWasHeld=false; }
    uint8_t* digits = (editDigit < Defaults::DIGITS)? offDigits : onDigits;
    uint8_t digit = editDigit % Defaults::DIGITS;
    uint8_t orig = digits[digit]; bool changed=false;
    if (actUp) { digits[digit] = (digits[digit]+1)%10; changed=true; }
    if (actDown){ digits[digit] = (digits[digit]+9)%10; changed=true; }
    if (changed) {
      uint32_t newVal=0; for(int i=0;i<Defaults::DIGITS;++i) newVal = newVal*10 + digits[i];
      if (newVal < Defaults::TIMER_MIN || newVal > Defaults::TIMER_MAX) { digits[digit]=orig; }
      else {
        uint32_t* target = (editDigit < Defaults::DIGITS)? &cfg->offTime : &cfg->onTime;
  if (*target != newVal) { *target = newVal; valuesChanged=true; timersDirty=true; }
      }
    }
    // Star now cancels (discard changes)
    if (bs.starEdge) {
      cancelled = true; // mark for discard
      // restore original values
      cfg->offTime = snapshotOff; cfg->onTime = snapshotOn; timersDirty=false; valuesChanged=false;
      exitEdit(false); exited=true; firstCycle=true; return true;
    }
    if (bs.hashEdge) {
      editDigit++; if (editDigit >= Defaults::DIGITS*2) { exitEdit(valuesChanged); exited=true; firstCycle=true; }
      else requireRelease=true;
    }
    else if (bs.hash && !hashWasHeld && hashHoldStart && (now - hashHoldStart >= 2000UL)) {
      hashWasHeld=true; exitEdit(valuesChanged); exited=true; firstCycle=true; }
    return true;
  }

  uint8_t getEditDigit() const { return editDigit; }
  bool wasCancelled() const { return cancelled; }

private:
  void loadDigits() {
    uint32_t tempOffInt = cfg->offTime / 10; uint8_t tempOffFrac = cfg->offTime % 10;
    uint32_t tempOnInt  = cfg->onTime / 10;  uint8_t tempOnFrac = cfg->onTime % 10;
    offDigits[Defaults::DIGITS-1] = tempOffFrac; onDigits[Defaults::DIGITS-1] = tempOnFrac;
    for (int i=Defaults::DIGITS-2;i>=0;--i){ offDigits[i]=tempOffInt%10; tempOffInt/=10; }
    for (int i=Defaults::DIGITS-2;i>=0;--i){ onDigits[i]=tempOnInt%10; tempOnInt/=10; }
    digitsInit=true;
  }
  void exitEdit(bool changed) { state = AppState::RUN; digitsInit=false; if (changed && !cancelled) timersDirty=true; }

public:
  bool timersDirty=false;

private:
  Config::Values* cfg=nullptr; bool cancelled=false; uint32_t snapshotOff=0; uint32_t snapshotOn=0;
  bool relayState=false; uint32_t timer=0; AppState state=AppState::RUN; uint8_t editDigit=0; bool digitsInit=false; uint8_t offDigits[Defaults::DIGITS]; uint8_t onDigits[Defaults::DIGITS];
  unsigned long lastTickMs=0;
};
