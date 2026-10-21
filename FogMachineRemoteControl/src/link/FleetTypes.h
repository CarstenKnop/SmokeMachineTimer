// FleetTypes.h - plain data the UI reads (no hardware dependencies, also used by the host-side screen renderer).
#pragma once

#include <stdint.h>
#include "FogProto.h"

// Read-only snapshot of one paired timer.
struct SlaveDevice {
    uint8_t mac[6] = {0};
    char name[Fog::kNameLen] = {0};
    int8_t rssiRemote = 0;        // signal at the remote (0 = unknown)
    int8_t rssiSlave = 0;         // signal at the timer
    float ton = 0.f, toff = 0.f;  // seconds
    float elapsed = 0.f;          // seconds into the current phase, predicted to "now"
    bool outputState = false;     // relay energised
    bool phaseOn = false;
    bool online = false;
    uint8_t mode = 0;             // Fog::Mode
    int lossPct = -1;
    uint8_t pending = 0;          // commands still queued
    uint8_t channel = 0;
    bool rebooted = false;        // power cycle noticed within the last 30 s
    uint32_t lastStatusMs = 0;
    uint32_t ageMs = 0;
};

struct DiscoveredDevice {
    uint8_t mac[6];
    char name[Fog::kNameLen];
    int8_t rssi;
    float ton, toff;
    uint32_t lastSeen;
    uint8_t channel;
    bool paired;
};
