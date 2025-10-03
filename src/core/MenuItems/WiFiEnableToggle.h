// Menu item: Toggle WiFi enable/disable setting
#pragma once
#include <Arduino.h>
class Config;
namespace MenuItem {
  struct WiFiEnableToggle {
    static constexpr const char* NAME = "WiFi En";
    static bool apply(Config& cfg); // returns new enabled state
  };
}
