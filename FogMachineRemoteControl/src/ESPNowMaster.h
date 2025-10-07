#pragma once
#include <Arduino.h>
#include <vector>
#include "ESPNowProtocol.h"

struct PeerInfo { uint8_t mac[6]; char name[24]; int rssi; uint32_t offTime; uint32_t onTime; uint8_t battery; uint16_t calibAdc[3]; unsigned long lastSeen; };

class ESPNowMaster {
public:
  void begin();
  void scanAndPing();
  void pairWith(const uint8_t mac[6], const char* name);
  void sendSetParams(const uint8_t mac[6], uint32_t off, uint32_t on);
  void sendSave(const uint8_t mac[6]);
  void sendCalib(const uint8_t mac[6], const uint16_t calib[3]);
  const std::vector<PeerInfo>& peers() const { return peerList; }
  void persistPeers();
  void loadPeers();
  std::vector<PeerInfo> peerList;
};
