#include "TimerController.h"

void TimerController::begin(Config* cfgPtr) {
  cfg = cfgPtr;
  offTime = cfg->get().offTime;
  onTime = cfg->get().onTime;
  relayState = false;
  timer = 0;
  lastTickMs = millis();
}

void TimerController::tick(unsigned long now) {
  unsigned long elapsed = now - lastTickMs;
  if (elapsed >= 100) {
    unsigned long steps = elapsed / 100;
    lastTickMs += steps * 100;
    while (steps--) {
      if (relayState) {
        if (timer < onTime) timer++; else { relayState = false; timer = 0; }
      } else {
        if (timer < offTime) timer++; else { relayState = true; timer = 0; }
      }
    }
  }
}

void TimerController::resetCycle() { relayState = false; timer = 0; }
void TimerController::toggleRelayManual() { relayState = !relayState; timer = 0; }
