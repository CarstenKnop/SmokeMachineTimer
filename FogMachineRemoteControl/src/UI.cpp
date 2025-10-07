#include "UI.h"
#include "DisplayManager.h"
#include <Arduino.h>

void UI::begin(ESPNowMaster* m, DisplayManager* d) {
  master = m; disp = d; buttons.begin();
}

void UI::loop() {
  auto bs = buttons.poll();
  lastButtons = bs;
  if (bs.upEdge || bs.downEdge || bs.hashEdge || bs.starEdge) {
    Serial.printf("BTN edges: U%u D%u # %u * %u\n", bs.upEdge, bs.downEdge, bs.hashEdge, bs.starEdge);
  }
  // pump master periodic tasks (discovery pings)
  master->tick();
  if (state == State::LIST && serviceState == ServiceState::NONE) {
    // navigate list
    if (bs.upEdge) { if (selectedIndex>0) selectedIndex--; }
    if (bs.downEdge) { if (selectedIndex < (int)master->peerList.size()-1) selectedIndex++; }
    if (bs.hashEdge) {
      // enter edit times
      if ((int)master->peerList.size() > selectedIndex) {
        auto &p = master->peerList[selectedIndex]; editOff = p.offTime; editOn = p.onTime; strncpy(editName, p.name, sizeof(editName)-1); state = State::EDIT_TIMES;
      }
    }
    if (bs.starEdge) {
      // enter pairing mode (discovery scan)
      state = State::PAIRING;
      master->startDiscovery(12000);
      selectedIndex = 0;
      Serial.println("UI: Enter PAIRING mode");
    }
    // long-press star enters service CALIB mode
    static unsigned long starHoldStart = 0;
    if (bs.star) { if (starHoldStart==0) starHoldStart = millis(); else if (millis()-starHoldStart>1200) {
        if ((int)master->peerList.size() > selectedIndex) { // enter calib
          state = State::LIST; serviceState = ServiceState::CALIB; if (master->peerList.size()>selectedIndex) memcpy(editCalib, master->peerList[selectedIndex].calibAdc, sizeof(editCalib));
        }
        starHoldStart = 0;
    } }
    if (!bs.star) { starHoldStart=0; }
  } else if (state == State::EDIT_TIMES) {
    // up/down adjust off time by 10 tenths (1s)
    if (bs.upEdge) { editOff += 10; if (editOff > 60000) editOff = 60000; }
    if (bs.downEdge) { if (editOff >= 10) editOff -= 10; }
    // hash to switch to edit name, star to send live params, double hash to save
    if (bs.starEdge) {
      // send live params to device
      if ((int)master->peerList.size() > selectedIndex) {
        master->sendSetParams(master->peerList[selectedIndex].mac, editOff, editOn);
      }
    }
    if (bs.hashEdge) { state = State::EDIT_NAME; }
    if (bs.hash && bs.hashEdge) { /* noop */ }
    if (bs.up && bs.down && bs.star) { /* placeholder for multi-button actions */ }
    // long-hold hash to save
    static unsigned long hashHoldStart = 0;
    if (bs.hash) { if (hashHoldStart==0) hashHoldStart = millis(); else if (millis()-hashHoldStart>1500) {
        // Save requested
        if ((int)master->peerList.size() > selectedIndex) {
          master->sendSetParams(master->peerList[selectedIndex].mac, editOff, editOn);
          master->sendSave(master->peerList[selectedIndex].mac);
        }
        hashHoldStart = 0; state = State::LIST;
    } }
    if (!bs.hash) { hashHoldStart=0; }
  } else if (state == State::PAIRING) {
    // show discovered peers, select one to pair with
    if (bs.upEdge) { if (selectedIndex > 0) selectedIndex--; }
    if (bs.downEdge) { if (selectedIndex < (int)master->discoveredPeers.size() - 1) selectedIndex++; }
    if (bs.hashEdge) {
      // choose discovered peer -> go to EDIT_NAME
      if (!master->discoveredPeers.empty() && selectedIndex < (int)master->discoveredPeers.size()) {
        auto &p = master->discoveredPeers[selectedIndex];
        memcpy(pendingMac, p.mac, 6); hasPendingMac = true;
        snprintf(editName, sizeof(editName), "Timer-%02X%02X", p.mac[4], p.mac[5]);
        state = State::EDIT_NAME;
        Serial.printf("UI: Selected for pairing %02X:%02X:%02X:%02X:%02X:%02X\n", p.mac[0],p.mac[1],p.mac[2],p.mac[3],p.mac[4],p.mac[5]);
      }
    }
    if (bs.starEdge) { // exit pairing mode
      state = State::LIST;
      Serial.println("UI: Exit PAIRING mode");
    }
  } else if (state == State::EDIT_NAME) {
    // simple name editing: up/down cycle ASCII 32..90 for first char; hash to accept
    if (bs.upEdge) { char &c = editName[0]; if (c < 'Z') c++; else c='A'; }
    if (bs.downEdge) { char &c = editName[0]; if (c > 'A') c--; else c='Z'; }
    if (bs.hashEdge) {
      // Confirm name -> PAIR
      if (hasPendingMac) {
        master->pairWith(pendingMac, editName);
        master->addOrUpdatePeer(pendingMac, editName);
        master->persistPeers();
        hasPendingMac = false;
        state = State::LIST;
        Serial.println("UI: Pair+Name committed");
      }
    }
  }
  // Service mode: CALIB editor
  if (serviceState == ServiceState::CALIB) {
    // up/down adjust current calib point
    if (bs.upEdge) { int v = editCalib[editCalibIndex]; v += 16; if (v>4095) v=4095; editCalib[editCalibIndex]=v; }
    if (bs.downEdge) { int v = editCalib[editCalibIndex]; v -= 16; if (v<0) v=0; editCalib[editCalibIndex]=v; }
    // star/hash change index
    if (bs.starEdge) { editCalibIndex = (editCalibIndex+1)%3; }
    if (bs.hashEdge) {
      // confirm and send calib to device
      if ((int)master->peerList.size() > selectedIndex) {
        master->sendCalib(master->peerList[selectedIndex].mac, editCalib);
        master->persistPeers();
      }
      serviceState = ServiceState::NONE;
    }
  }
}
