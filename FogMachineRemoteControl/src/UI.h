#pragma once
#include "ESPNowMaster.h"
#include "DisplayManager.h"
#include "Buttons.h"

class UI {
public:
  void begin(ESPNowMaster* m, DisplayManager* d);
  void loop();
private:
  ESPNowMaster* master=nullptr;
  DisplayManager* disp=nullptr;
  Buttons buttons;
  enum class State { LIST, EDIT_TIMES, EDIT_NAME } state = State::LIST;
  enum class ServiceState { NONE, CALIB } serviceState = ServiceState::NONE;
  int selectedIndex = 0;
  // editing buffers
  uint32_t editOff=0, editOn=0;
  char editName[24] = {0};
  uint16_t editCalib[3] = {2000, 3000, 3500};
  int editCalibIndex = 0;
};
