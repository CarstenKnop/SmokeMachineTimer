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
};

struct __attribute__((packed)) ProtocolMsg {
    uint8_t cmd;          // ProtocolCmd
    float ton;            // seconds (ON duration)
    float toff;           // seconds (OFF duration)
    char name[16];
    bool outputOverride;  // status: current output state / command: desired override
    bool resetState;      // request to reset internal timing cycle
    uint16_t calibAdc[3]; // battery calibration ADC points
};
