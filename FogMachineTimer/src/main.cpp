#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include "Defaults.h"

// main.cpp
// Entry point for FogMachineTimer (slave). Sets up timer, config, and ESP-NOW communication.
#include <Arduino.h>
#include "timer/TimerController.h"
#include "config/DeviceConfig.h"
#include "comm/EspNowComm.h"

#define FOG_OUTPUT_PIN D0

TimerController timer(FOG_OUTPUT_PIN);
DeviceConfig config;
EspNowComm comm(timer, config);

void setup() {
  Serial.begin(115200);
  config.begin();
  pinMode(FOG_OUTPUT_PIN, OUTPUT);
  // Blink D0 output 3 times on startup
  for (int i = 0; i < 3; ++i) {
    digitalWrite(FOG_OUTPUT_PIN, HIGH);
    delay(150);
    digitalWrite(FOG_OUTPUT_PIN, LOW);
    delay(150);
  }
  timer.begin(config.getTon(), config.getToff());
  comm.begin();
  Serial.println("FogMachineTimer started.");
}

void loop() {
  unsigned long now = millis();
  timer.update(now);
  comm.loop();
  delay(10);
}
