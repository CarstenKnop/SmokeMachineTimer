#include "WiFiForgetConfirm.h"
#include "../Config.h"
namespace MenuItem { void WiFiForgetConfirm::apply(Config& cfg) { cfg.forgetSta(); } }
