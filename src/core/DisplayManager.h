#pragma once
#include <Adafruit_SSD1306.h>
#include "Defaults.h"
#include "TimerController.h"
#include "MenuSystem.h"
#include "Config.h"
#include "Screensaver.h"
#include "WiFiService.h"
// Replaces previous placeholder QRCode with real (trimmed) generator
#include "qrcodegen.h"

class DisplayManager {
public:
  void begin();
  Adafruit_SSD1306* get() { return &display; }
  void render(const TimerController& timerCtl, const MenuSystem& menu, const Config& config,
              bool blinkState, bool relayOn, uint32_t currentTimerTenths);
  void attachScreensaver(Screensaver* s) { screensaver = s; }
private:
  void splash();
  void printTimerValue(uint32_t value, int y, const char* label, int editDigit, bool editMode, bool blinkState, bool showDecimal, int startX=26);
  void drawProgress(const MenuSystem& menu);
  void drawMenu(const MenuSystem& menu);
  void drawResult(const MenuSystem& menu);
  void drawSaverEdit(const MenuSystem& menu, bool blinkState);
  void drawHelp(const MenuSystem& menu);
  void drawWiFiInfo(const MenuSystem& menu);
  void drawDynQR(const MenuSystem& menu);
  void drawRick(const MenuSystem& menu);
  Adafruit_SSD1306 display{128,64,&Wire,-1}; Screensaver* screensaver=nullptr; WiFiService* wifi=nullptr;
  // Helpers for WiFi QR
  void buildWifiQrString(char *out, size_t cap) const; // WIFI:T:WPA;S:...;P:...;;
  static void escapeAppend(char c, char *&w, size_t &remain);
  mutable char lastQrPayload[96] = {0};
  mutable uint8_t qrBuffer[QRCODEGEN_QR_BUFFER_LEN];
  mutable uint8_t qrTemp[QRCODEGEN_TEMP_BUFFER_LEN];
  mutable int lastQrSize = 0;
  mutable bool qrValid = false;
  mutable int lastScale = 0;
public:
  void attachWiFi(WiFiService* w) { wifi = w; }
};
