#pragma once
#include "ESPNowMaster.h"
#include "Buttons.h"

class DisplayManager; // Forward declaration

class UI {
public:
  enum class State { LIST, PAIRING, EDIT_TIMES, EDIT_NAME };
  void begin(ESPNowMaster* m, DisplayManager* d);
  void loop();
  State getState() const { return state; }
  int getSelectedIndex() const { return selectedIndex; }
  ButtonState getLastButtons() const { return lastButtons; }
  const char* getEditName() const { return editName; }
private:
  ESPNowMaster* master=nullptr;
  DisplayManager* disp=nullptr;
  Buttons buttons;
  State state = State::LIST;
  enum class ServiceState { NONE, CALIB } serviceState = ServiceState::NONE;
  int selectedIndex = 0;
  // editing buffers
  uint32_t editOff=0, editOn=0;
  char editName[24] = {0};
  uint16_t editCalib[3] = {2000, 3000, 3500};
  int editCalibIndex = 0;
  ButtonState lastButtons{};
  uint8_t pendingMac[6] = {0};
  bool hasPendingMac = false;
};
