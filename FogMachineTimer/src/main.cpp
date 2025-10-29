#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <EEPROM.h>
#include "Defaults.h"

// main.cpp
// Entry point for FogMachineTimer (slave). Sets up timer, config, and ESP-NOW communication.
#include <Arduino.h>
#include "timer/TimerController.h"
#include "config/DeviceConfig.h"
#include "comm/EspNowComm.h"
#include "config/TimerChannelSettings.h"

#define FOG_OUTPUT_PIN D3

TimerController timer(FOG_OUTPUT_PIN);
DeviceConfig config;
TimerChannelSettings channelSettings;
EspNowComm comm(timer, config, channelSettings);

static void wipeTimerEeprom() {
  for (int i = 0; i < 256; ++i) {
    EEPROM.write(i, 0);
  }
  EEPROM.commit();
}

void setup() {
  Serial.begin(115200);
  EEPROM.begin(256);
  channelSettings.begin(&wipeTimerEeprom);
  config.begin();
  pinMode(FOG_OUTPUT_PIN, OUTPUT);
  // Ensure output is OFF on startup (do not blink, this pin controls the fog relay)
  digitalWrite(FOG_OUTPUT_PIN, LOW);
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
