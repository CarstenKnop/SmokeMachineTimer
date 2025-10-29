// Protocol.h
// Defines the ESP-NOW message structure and command types.
#pragma once
#include <Arduino.h>

enum class ProtocolCmd : uint8_t {
    PAIR = 1,
    STATUS = 2,
    SET_TIMER = 3,
    OVERRIDE_OUTPUT = 4,
    RESET_STATE = 5,
    SET_NAME = 6,
    GET_RSSI = 7,
    CALIBRATE_BATTERY = 8,
    TOGGLE_STATE = 9,
    FACTORY_RESET = 10,
    SET_CHANNEL = 11
};

enum class ProtocolStatus : uint8_t {
    OK = 0,
    INVALID_PARAM = 1,
    UNSUPPORTED = 2,
    BUSY = 3,
    UNKNOWN_CMD = 4
};

struct __attribute__((packed)) ProtocolMsg {
    uint8_t cmd; // ProtocolCmd
    float ton;
    float toff;
    float elapsed;        // seconds elapsed in current state (for TIME row)
        char name[10];        // 9 chars + NUL
    bool outputOverride;
    bool resetState;
    int8_t rssiAtTimer;   // RSSI measured at timer for last packet from remote
    uint16_t calibAdc[3]; // For battery calibration
    uint8_t channel;      // preferred ESP-NOW channel
    uint8_t reserved[3];  // align to 4-byte boundary for future use
};
