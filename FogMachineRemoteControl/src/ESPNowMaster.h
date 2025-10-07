#pragma once
#include <Arduino.h>
#include <vector>
#include "ESPNowProtocol.h"

struct PeerInfo { uint8_t mac[6]; char name[24]; int rssi; uint32_t offTime; uint32_t onTime; uint8_t battery; uint16_t calibAdc[3]; unsigned long lastSeen; };

class ESPNowMaster {
public:
  void begin();
  // known peers ping (status refresh)
  void scanAndPing();
  // discovery lifecycle
  void startDiscovery(uint32_t durationMs = 12000);
  void tick(); // call regularly from loop
  bool isDiscovering() const { return discovering; }
  uint32_t discoveryMsLeft() const { return discovering && millis() < discoveryEnd ? (discoveryEnd - millis()) : 0; }
  void clearDiscovered();
  void sortDiscoveredByRssi();
  // pairing and control
  void pairWith(const uint8_t mac[6], const char* name);
  void sendSetParams(const uint8_t mac[6], uint32_t off, uint32_t on);
  void sendSave(const uint8_t mac[6]);
  void sendCalib(const uint8_t mac[6], const uint16_t calib[3]);
  const std::vector<PeerInfo>& peers() const { return peerList; }
  std::vector<PeerInfo> discoveredPeers;
  void persistPeers();
  void loadPeers();
  std::vector<PeerInfo> peerList;
  void addOrUpdatePeer(const uint8_t mac[6], const char* name);
  void removeFromDiscovered(const uint8_t mac[6]);
private:
  void broadcastPing();
  bool discovering = false;
  uint32_t discoveryEnd = 0;
  uint32_t lastDiscoveryPing = 0;
};
