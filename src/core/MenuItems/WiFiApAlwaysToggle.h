// Menu item: Toggle AP always-on flag
#pragma once
#include <Arduino.h>
class Config;
namespace MenuItem { struct WiFiApAlwaysToggle { static constexpr const char* NAME = "AP Always"; static bool apply(Config& cfg); }; }
