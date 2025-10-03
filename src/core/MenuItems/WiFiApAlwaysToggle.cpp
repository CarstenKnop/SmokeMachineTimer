#include "WiFiApAlwaysToggle.h"
#include "../Config.h"
namespace MenuItem { bool WiFiApAlwaysToggle::apply(Config& cfg) { auto c = cfg.get(); c.apAlwaysOn = !c.apAlwaysOn; cfg.saveApAlwaysOn(c.apAlwaysOn); return c.apAlwaysOn; } }
