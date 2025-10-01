#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiUdp.h>
#include <CaptivePortal.h>

class WiFiService {
public:
  void begin(const char* apSsid, const char* apPass, uint16_t port=80);
  void loop();
  bool isStarted() const { return started; }
  IPAddress ip() const { return apIP; }
  uint16_t port() const { return serverPort; }
  const char* getSSID() const { return ssid.c_str(); }
  const char* getPass() const { return pass.c_str(); }
  String qrContent() const; // returns URL for QR encoding
private:
  void setupRoutes();
  bool started=false;
  WebServer server{80};
  uint16_t serverPort=80;
  IPAddress apIP{192,168,4,1};
  String ssid;
  String pass;
  CaptivePortalDNS dns;
};
