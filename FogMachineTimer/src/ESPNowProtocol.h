#pragma once
#include <Arduino.h>

enum class MsgType : uint8_t { PAIR=1, STATUS=2, SET_PARAMS=3, SAVE=4, PING=5, PONG=6, CALIB=7 };

struct __attribute__((packed)) ESPNowMsg {
  uint8_t type; // MsgType
  int8_t rssi; // optional
  uint32_t offTime; // tenths
  uint32_t onTime;  // tenths
  char name[24];
  uint8_t batteryPercent; // 0..100
  uint16_t calibAdc[3]; // optional: raw ADC calibration points (0..4095)
};
