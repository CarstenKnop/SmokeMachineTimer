// Minimal decoupled connectivity status interface to avoid heavy include coupling.
#pragma once
struct ConnectivityStatus {
    bool wifiEnabled=false;      // user setting
    bool apActive=false;         // SoftAP currently running
    bool apSuppressed=false;     // SoftAP intentionally suppressed after STA connect
    bool staConnected=false;     // Station interface has valid connection
    uint8_t apClients=0;         // number of associated stations
    bool recentAuth=false;       // true if an authenticated HTTP request occurred recently
    int16_t staRssi=0;           // RSSI of STA connection if connected
};
