// Protocol.h - Unified shared protocol between Remote (master) and Timer (slave)
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
    CALIBRATE_BATTERY = 8
    , TOGGLE_STATE = 9
    , FACTORY_RESET = 10
};

struct __attribute__((packed)) ProtocolMsg {
    uint8_t cmd;          // ProtocolCmd
    float ton;            // seconds (ON duration)
    float toff;           // seconds (OFF duration)
    float elapsed;        // seconds elapsed in current state (for TIME row)
    char name[10];      // 9 chars + NUL
    bool outputOverride;  // status: current output state / command: desired override
    bool resetState;      // request to reset internal timing cycle
    int8_t rssiAtTimer;   // RSSI measured at timer for last packet from remote
    uint16_t calibAdc[3]; // battery calibration ADC points
};
