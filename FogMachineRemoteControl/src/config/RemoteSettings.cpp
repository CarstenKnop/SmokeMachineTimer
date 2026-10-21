#include "config/RemoteSettings.h"

void RemoteSettings::begin() {
    if (!store_.load(d_)) d_ = RemoteSettingsData();
    clamp();
}

void RemoteSettings::clamp() {
    if (d_.txPowerQdbm < 8) d_.txPowerQdbm = 8;
    if (d_.txPowerQdbm > 84) d_.txPowerQdbm = 84;
    if (d_.oledBrightness < 5) d_.oledBrightness = 5;
    if (d_.autoOffSeconds > 3600) d_.autoOffSeconds = 3600;
    if (d_.rssiLowDbm < -120) d_.rssiLowDbm = -120;
    if (d_.rssiHighDbm > 0) d_.rssiHighDbm = 0;
    if (d_.rssiHighDbm < d_.rssiLowDbm + 5) d_.rssiHighDbm = static_cast<int8_t>(d_.rssiLowDbm + 5);
    if (d_.net < 1 || d_.net > 254) d_.net = Fog::kDefaultNet;
    if (!Fog::validChannel(d_.channel)) d_.channel = Fog::kDefaultChannel;
    // calibration points must be ascending and plausible for a Li-Ion cell
    auto& c = d_.batCalMv;
    if (c[0] < 2500 || c[0] > 4000 || c[1] <= c[0] || c[2] <= c[1] || c[2] > 4500) {
        c[0] = 3300; c[1] = 3750; c[2] = 4150;
    }
}

void RemoteSettings::save() {
    clamp();
    store_.save(d_);
}

void RemoteSettings::resetToDefaults() {
    d_ = RemoteSettingsData();
    store_.save(d_);
}
