#pragma once
#include <Arduino.h>
#include "Config.h"

class TimerController {
public:
  void begin(Config* cfgPtr);
  void tick(unsigned long now);
  void resetCycle();
  void toggleRelayManual();

  uint32_t currentTimer() const { return timer; }
  bool isRelayOn() const { return relayState; }

  void setTimes(uint32_t offT, uint32_t onT) { if (offT>0) offTime=offT; if (onT>0) onTime=onT; }
  uint32_t offTime=100, onTime=100;

private:
  bool relayState=false; uint32_t timer=0; unsigned long lastTickMs=0;
  Config* cfg=nullptr;
};
