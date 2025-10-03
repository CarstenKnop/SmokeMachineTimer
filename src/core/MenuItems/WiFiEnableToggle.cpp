#include "WiFiEnableToggle.h"
#include "../Config.h"
namespace MenuItem { bool WiFiEnableToggle::apply(Config& cfg) { auto c = cfg.get(); c.wifiEnabled = !c.wifiEnabled; cfg.saveWiFiEnabled(c.wifiEnabled); return c.wifiEnabled; } }
