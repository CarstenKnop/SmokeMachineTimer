#pragma once
#include <Arduino.h>
#include "Defaults.h"
#include "Config.h"
#include "Buttons.h"

class TimerController {
public:
  enum class AppState : uint8_t { RUN, EDIT };

  void begin(Config::Values* cfgVals);
  void tick(unsigned long now);
  void resetCycle();
  void toggleRelayManual();

  uint32_t currentTimer() const { return timer; }
  bool isRelayOn() const { return relayState; }

  // Editing logic (digit-by-digit) reused from original code (simplified interface)
  void enterEdit();
  bool inEdit() const { return state == AppState::EDIT; }
  AppState getState() const { return state; }

  bool handleEdit(const ButtonState& bs, unsigned long now, bool& valuesChanged, bool& exited);

  uint8_t getEditDigit() const { return editDigit; }
  bool wasCancelled() const { return cancelled; }

private:
  void loadDigits();
  void exitEdit(bool changed);

public:
  bool timersDirty=false;

private:
  Config::Values* cfg=nullptr; bool cancelled=false; uint32_t snapshotOff=0; uint32_t snapshotOn=0;
  bool relayState=false; uint32_t timer=0; AppState state=AppState::RUN; uint8_t editDigit=0; bool digitsInit=false; uint8_t offDigits[Defaults::DIGITS]; uint8_t onDigits[Defaults::DIGITS];
  unsigned long lastTickMs=0;
};
