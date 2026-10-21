#include "menu/MenuState.h"

const char* const MenuState::kItemLabels[MenuState::kItemCount] = {
    "Pair Timer", "Rename Timer", "Active Timer", "Edit Timers", "Link Status", "Find Timers", "Channel", "Link Test",
    "Auto Off", "OLED Brightness", "Radio TX Power", "RSSI Calibration", "Battery Calibration", "Reset Timer",
    "Reset Remote", "Reboot Remote", "Sleep Now"};

const int MenuState::kAutoOffOptions[7] = {0, 15, 30, 60, 120, 300, 600};
