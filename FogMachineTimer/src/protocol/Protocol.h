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
    CALIBRATE_BATTERY = 8,
    TOGGLE_STATE = 9,
    FACTORY_RESET = 10,
    SET_CHANNEL = 11,
    ACK = 12,
    NAK = 13
};

enum class ProtocolStatus : uint8_t {
    OK = 0,
    INVALID_PARAM = 1,
    UNSUPPORTED = 2,
    BUSY = 3,
    UNKNOWN_CMD = 4
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
    uint8_t channel;      // preferred ESP-NOW channel for coordination
    uint8_t seq;          // sequence id for ACK/NAK correlation (0 => no ack expected)
    uint8_t refCmd;       // echoed command when responding with ACK/NAK
    uint8_t status;       // ProtocolStatus (meaningful for ACK/NAK/STATUS)
};
