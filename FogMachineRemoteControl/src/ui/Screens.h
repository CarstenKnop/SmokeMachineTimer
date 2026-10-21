// Screens.h - all OLED screens as pure functions of a UiModel (128x64, monochrome).
// They draw into any Adafruit_GFX, so the firmware and the PC preview tool (tools/oled-preview) share them.
#pragma once

#include <Adafruit_GFX.h>
#include "menu/MenuState.h"
#include "ui/UiModel.h"

namespace screens {

constexpr uint16_t kWhite = 1;
constexpr uint16_t kBlack = 0;

void drawMain(Adafruit_GFX& g, const UiModel& m);
void drawMenu(Adafruit_GFX& g, const UiModel& m, const MenuState& s);   // every menu mode, incl. the root list
void drawSplash(Adafruit_GFX& g);
void drawBootStatus(Adafruit_GFX& g, const char* msg);
void drawUpdateCountdown(Adafruit_GFX& g, uint8_t secondsRemaining);
void drawErrorScreen(Adafruit_GFX& g);

// building blocks (also used by the preview tool)
void drawBattery(Adafruit_GFX& g, uint8_t percent, bool charging, bool usbPower, uint32_t now);
void drawSignalBars(Adafruit_GFX& g, int8_t rssi, int x, int y, int8_t low, int8_t high);

}  // namespace screens
