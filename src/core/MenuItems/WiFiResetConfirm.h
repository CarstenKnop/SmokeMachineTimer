// Menu item: Reset stored station credentials and settings (factory network reset)
#pragma once
#include <Arduino.h>
class Config;
namespace MenuItem { struct WiFiResetConfirm { static constexpr const char* NAME = "WiFi Rst"; static void apply(Config& cfg); }; }
