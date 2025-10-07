#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include "Defaults.h"
#include "Config.h"
#include "TimerController.h"
#include "ESPNowProtocol.h"

static uint8_t getBatteryPercent() {
  // Simple ADC -> voltage -> percentage mapping.
  // Assumes a single-cell Li-ion (approx 3.0V..4.2V) and default ADC 12-bit (0..4095 -> 0..3.3V).
  int raw = analogRead(BAT_ADC_PIN);
  float voltage = raw * (3.3f / 4095.0f);
  const float minV = 3.0f;
  const float maxV = 4.2f;
  int pct = (int)((voltage - minV) / (maxV - minV) * 100.0f + 0.5f);
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  return (uint8_t)pct;
}

static Config config;
static TimerController timerCtl;

void onDataRecv(const uint8_t *mac, const uint8_t *incomingData, int len) {
  if (len < (int)sizeof(ESPNowMsg)) return;
  ESPNowMsg msg; memcpy(&msg, incomingData, sizeof(msg));
  if (msg.type == (uint8_t)MsgType::PAIR) {
    config.saveName(msg.name);
    ESPNowMsg reply = {};
    reply.type = (uint8_t)MsgType::STATUS;
    reply.offTime = config.get().offTime; reply.onTime = config.get().onTime;
    strncpy(reply.name, config.get().deviceName, sizeof(reply.name)-1);
  reply.batteryPercent = getBatteryPercent();
    esp_now_send(mac, (uint8_t*)&reply, sizeof(reply));
  } else if (msg.type == (uint8_t)MsgType::SET_PARAMS) {
    config.get().offTime = msg.offTime; config.get().onTime = msg.onTime;
    timerCtl.setTimes(msg.offTime, msg.onTime);
    ESPNowMsg reply = {};
    reply.type = (uint8_t)MsgType::STATUS; reply.offTime = config.get().offTime; reply.onTime = config.get().onTime; strncpy(reply.name, config.get().deviceName, sizeof(reply.name)-1);
  reply.batteryPercent = getBatteryPercent();
    esp_now_send(mac, (uint8_t*)&reply, sizeof(reply));
  } else if (msg.type == (uint8_t)MsgType::SAVE) {
    config.saveTimersIfChanged(config.get().offTime, config.get().onTime, true);
    ESPNowMsg reply = {};
    reply.type = (uint8_t)MsgType::STATUS; reply.offTime = config.get().offTime; reply.onTime = config.get().onTime; strncpy(reply.name, config.get().deviceName, sizeof(reply.name)-1);
  reply.batteryPercent = getBatteryPercent();
  esp_now_send(mac, (uint8_t*)&reply, sizeof(reply));
  } else if (msg.type == (uint8_t)MsgType::PING) {
    ESPNowMsg reply = {};
    reply.type = (uint8_t)MsgType::PONG;
    reply.offTime = config.get().offTime; reply.onTime = config.get().onTime;
    strncpy(reply.name, config.get().deviceName, sizeof(reply.name)-1);
  reply.batteryPercent = getBatteryPercent();
  esp_now_send(mac, (uint8_t*)&reply, sizeof(reply));
  } else if (msg.type == (uint8_t)MsgType::CALIB) {
    // Save calibration ADC points sent from remote (raw ADC values)
    if (msg.calibAdc[0] || msg.calibAdc[1] || msg.calibAdc[2]) {
      config.saveCalibration(msg.calibAdc);
    }
    ESPNowMsg reply = {};
    reply.type = (uint8_t)MsgType::STATUS;
    reply.offTime = config.get().offTime; reply.onTime = config.get().onTime;
    strncpy(reply.name, config.get().deviceName, sizeof(reply.name)-1);
    reply.batteryPercent = getBatteryPercent();
    memcpy(reply.calibAdc, config.get().calibAdc, sizeof(reply.calibAdc));
    esp_now_send(mac, (uint8_t*)&reply, sizeof(reply));
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(RELAY_PIN, OUTPUT);
  config.begin();
  timerCtl.begin(&config);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  if (esp_now_init() != ESP_OK) { Serial.println("ESP-NOW init failed"); }
  esp_now_register_recv_cb(onDataRecv);
  Serial.printf("FogMachineTimer started name=%s off=%u on=%u\n", config.get().deviceName, config.get().offTime, config.get().onTime);
}

void loop() {
  unsigned long now = millis();
  timerCtl.tick(now);
  digitalWrite(RELAY_PIN, timerCtl.isRelayOn()?HIGH:LOW);
  delay(10);
}
