#include "ESPNowMaster.h"
#include <WiFi.h>
#include <esp_now.h>
#include <esp_idf_version.h>
#include <EEPROM.h>

static ESPNowMaster* g_master = nullptr;

#if ESP_IDF_VERSION_MAJOR >= 5
void onRecv(const esp_now_recv_info_t *info, const uint8_t *incomingData, int len) {
  if (!g_master || !info) return;
  if (len < (int)sizeof(ESPNowMsg)) return;
  ESPNowMsg msg; memcpy(&msg, incomingData, sizeof(msg));
  const uint8_t *mac = info->src_addr;
  int rssi = info->rx_ctrl.rssi; // per-packet RSSI
  Serial.printf("ESPNow RX type=%u rssi=%d from %02X:%02X:%02X:%02X:%02X:%02X\n", msg.type, rssi, mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
  PeerInfo p = {};
  memcpy(p.mac, mac, 6);
  p.rssi = rssi;
  p.offTime = msg.offTime; p.onTime = msg.onTime; strncpy(p.name, msg.name, sizeof(p.name)-1);
  p.battery = msg.batteryPercent; p.lastSeen = millis();
  memcpy(p.calibAdc, msg.calibAdc, sizeof(p.calibAdc));
  for (auto &e : g_master->peerList) { if (memcmp(e.mac, p.mac, 6)==0) { e = p; return; } }
  for (auto &e : g_master->discoveredPeers) { if (memcmp(e.mac, p.mac, 6)==0) { e = p; return; } }
  g_master->discoveredPeers.push_back(p);
}
#else
void onRecv(const uint8_t *mac, const uint8_t *incomingData, int len) {
  if (!g_master) return;
  if (len < (int)sizeof(ESPNowMsg)) return;
  ESPNowMsg msg; memcpy(&msg, incomingData, sizeof(msg));
  Serial.printf("ESPNow RX type=%u from %02X:%02X:%02X:%02X:%02X:%02X\n", msg.type, mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
  PeerInfo p = {};
  memcpy(p.mac, mac, 6);
  // Fallback: no per-packet RSSI available; keep previous if exists
  int rssi = -70;
  for (auto &e : g_master->peerList) { if (memcmp(e.mac, mac, 6)==0) { rssi = e.rssi; break; } }
  p.rssi = rssi;
  p.offTime = msg.offTime; p.onTime = msg.onTime; strncpy(p.name, msg.name, sizeof(p.name)-1);
  p.battery = msg.batteryPercent; p.lastSeen = millis();
  memcpy(p.calibAdc, msg.calibAdc, sizeof(p.calibAdc));
  for (auto &e : g_master->peerList) { if (memcmp(e.mac, p.mac, 6)==0) { e = p; return; } }
  for (auto &e : g_master->discoveredPeers) { if (memcmp(e.mac, p.mac, 6)==0) { e = p; return; } }
  g_master->discoveredPeers.push_back(p);
}
#endif

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

void ESPNowMaster::broadcastPing() {
  // Broadcast discovery ping (FF:FF:FF:FF:FF:FF)
  uint8_t bcast[6]; memset(bcast, 0xFF, 6);
  esp_now_peer_info_t info = {};
  memcpy(info.peer_addr, bcast, 6);
  info.channel = 0; info.encrypt = false;
  if (!esp_now_is_peer_exist(bcast)) esp_now_add_peer(&info);
  ESPNowMsg m = {}; m.type = (uint8_t)MsgType::PING;
  esp_now_send(bcast, (uint8_t*)&m, sizeof(m));
}

void ESPNowMaster::startDiscovery(uint32_t durationMs) {
  discovering = true;
  discoveryEnd = millis() + durationMs;
  lastDiscoveryPing = 0; // force immediate ping
  clearDiscovered();
  Serial.printf("Discovery started for %lu ms\n", (unsigned long)durationMs);
}

void ESPNowMaster::tick() {
  if (!discovering) return;
  uint32_t now = millis();
  if (now - lastDiscoveryPing > 1000) { // ping every second
    broadcastPing();
    lastDiscoveryPing = now;
  }
  if (now >= discoveryEnd) {
    discovering = false;
    sortDiscoveredByRssi();
    Serial.printf("Discovery finished. Found %u peers.\n", (unsigned)discoveredPeers.size());
  }
}

void ESPNowMaster::clearDiscovered() { discoveredPeers.clear(); }

void ESPNowMaster::sortDiscoveredByRssi() {
  std::sort(discoveredPeers.begin(), discoveredPeers.end(), [](const PeerInfo&a, const PeerInfo&b){ return a.rssi > b.rssi; });
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
  addOrUpdatePeer(mac, name);
  removeFromDiscovered(mac);
}

void ESPNowMaster::addOrUpdatePeer(const uint8_t mac[6], const char* name) {
  for (auto &e : peerList) {
    if (memcmp(e.mac, mac, 6)==0) {
      if (name && *name) { strncpy(e.name, name, sizeof(e.name)-1); }
      e.lastSeen = millis();
      return;
    }
  }
  PeerInfo p = {};
  memcpy(p.mac, mac, 6);
  if (name && *name) strncpy(p.name, name, sizeof(p.name)-1);
  p.lastSeen = millis();
  peerList.push_back(p);
}

void ESPNowMaster::removeFromDiscovered(const uint8_t mac[6]) {
  discoveredPeers.erase(std::remove_if(discoveredPeers.begin(), discoveredPeers.end(), [&](const PeerInfo& e){ return memcmp(e.mac, mac, 6)==0; }), discoveredPeers.end());
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
