#include "WiFiResetConfirm.h"
#include "../Config.h"
namespace MenuItem { void WiFiResetConfirm::apply(Config& cfg) { cfg.resetWiFi(); } }
