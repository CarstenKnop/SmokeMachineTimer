// Menu item: Forget only STA credentials (leave enable flag & AP settings)
#pragma once
#include <Arduino.h>
class Config;
namespace MenuItem { struct WiFiForgetConfirm { static constexpr const char* NAME = "WiFi Fgt"; static void apply(Config& cfg); }; }
