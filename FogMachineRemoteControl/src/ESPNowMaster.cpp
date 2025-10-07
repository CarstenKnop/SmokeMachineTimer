#include "ESPNowMaster.h"
#include <WiFi.h>
#include <esp_now.h>
#include <EEPROM.h>

static ESPNowMaster* g_master = nullptr;

void onRecv(const uint8_t *mac, const uint8_t *incomingData, int len) {
  if (!g_master) return;
  if (len < (int)sizeof(ESPNowMsg)) return;
  ESPNowMsg msg; memcpy(&msg, incomingData, sizeof(msg));
  PeerInfo p = {};
  memcpy(p.mac, mac, 6);
  p.rssi = WiFi.RSSI();
  p.offTime = msg.offTime; p.onTime = msg.onTime; strncpy(p.name, msg.name, sizeof(p.name)-1);
  p.battery = msg.batteryPercent; p.lastSeen = millis();
  memcpy(p.calibAdc, msg.calibAdc, sizeof(p.calibAdc));
  for (auto &e : g_master->peerList) { if (memcmp(e.mac, p.mac, 6)==0) { e = p; return; } }
  g_master->peerList.push_back(p);
}

void onSend(const uint8_t *mac_addr, esp_now_send_status_t status) { (void)mac_addr; (void)status; }

void ESPNowMaster::begin() {
  g_master = this;
  EEPROM.begin(512);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  if (esp_now_init() != ESP_OK) { Serial.println("esp-now init failed"); return; }
  esp_now_register_recv_cb(onRecv);
  esp_now_register_send_cb(onSend);
  loadPeers();
}

void ESPNowMaster::scanAndPing() {
  for (auto &p : peerList) {
    ESPNowMsg m = {};
    m.type = (uint8_t)MsgType::PING;
    esp_now_send(p.mac, (uint8_t*)&m, sizeof(m));
  }
}

void ESPNowMaster::pairWith(const uint8_t mac[6], const char* name) {
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, mac, 6);
  peerInfo.channel = 0; peerInfo.encrypt = false;
  esp_err_t res = esp_now_add_peer(&peerInfo);
  Serial.printf("add_peer res=%d\n", res);
  ESPNowMsg m = {};
  m.type = (uint8_t)MsgType::PAIR; strncpy(m.name, name, sizeof(m.name)-1);
  esp_now_send(mac, (uint8_t*)&m, sizeof(m));
}

void ESPNowMaster::sendSetParams(const uint8_t mac[6], uint32_t off, uint32_t on) {
  ESPNowMsg m = {};
  m.type = (uint8_t)MsgType::SET_PARAMS; m.offTime = off; m.onTime = on;
  esp_now_send(mac, (uint8_t*)&m, sizeof(m));
}

void ESPNowMaster::sendCalib(const uint8_t mac[6], const uint16_t calib[3]) {
  ESPNowMsg m = {};
  m.type = (uint8_t)MsgType::CALIB;
  m.calibAdc[0] = calib[0]; m.calibAdc[1] = calib[1]; m.calibAdc[2] = calib[2];
  esp_now_send(mac, (uint8_t*)&m, sizeof(m));
}

void ESPNowMaster::sendSave(const uint8_t mac[6]) {
  ESPNowMsg m = {};
  m.type = (uint8_t)MsgType::SAVE;
  esp_now_send(mac, (uint8_t*)&m, sizeof(m));
}

void ESPNowMaster::persistPeers() {
  uint8_t count = peerList.size() > 8 ? 8 : peerList.size();
  EEPROM.put(0, count);
  int base = 1;
  for (int i=0;i<count;i++) {
    EEPROM.put(base, peerList[i].mac); base += 6;
    EEPROM.put(base, peerList[i].name); base += 24;
  }
  EEPROM.commit();
}

void ESPNowMaster::loadPeers() {
  uint8_t count=0; EEPROM.get(0,count);
  int base=1; for (int i=0;i<count;i++) {
  PeerInfo p={}; EEPROM.get(base, p.mac); base+=6; EEPROM.get(base, p.name); base+=24; p.lastSeen=0; peerList.push_back(p);
  esp_now_peer_info_t peerInfo = {}; memcpy(peerInfo.peer_addr, p.mac, 6); peerInfo.channel=0; esp_now_add_peer(&peerInfo);
  }
}
