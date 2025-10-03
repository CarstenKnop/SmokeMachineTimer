#pragma once
#include <Adafruit_SSD1306.h>
#include "Defaults.h"
#include "TimerController.h"
#include "MenuSystem.h"
#include "Config.h"
#include "Screensaver.h"
#include "WiFiService.h"
#include "ConnectivityStatus.h"
// Replaces previous placeholder QRCode with real (trimmed) generator
#include "qrcodegen.h"
// Portal service forward declared below (avoid heavy dependency here)
class AsyncPortalService;

class DisplayManager {
public:
  void begin();
  Adafruit_SSD1306* get() { return &display; }
  void render(const TimerController& timerCtl, const MenuSystem& menu, const Config& config,
              bool blinkState, bool relayOn, uint32_t currentTimerTenths);
  void attachScreensaver(Screensaver* s) { screensaver = s; }
  void attachPortal(AsyncPortalService* ) { /* portal support removed for now */ }
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
  void drawInfo(const MenuSystem& menu);
  // WiFi icons 12x8 (width x height) row-major bits (1=white pixel)
  static const uint8_t ICON_WIFI_AP[] PROGMEM;
  static const uint8_t ICON_WIFI_STA[] PROGMEM;
  static const uint8_t ICON_WIFI_DUAL[] PROGMEM;
  static const uint8_t ICON_WIFI_SUPPRESSED[] PROGMEM;
  static const uint8_t ICON_WIFI_HOSTED[] PROGMEM; // AP always on
  void drawIcon(int16_t x, int16_t y, const uint8_t *bitmap, uint8_t w, uint8_t h);
  Adafruit_SSD1306 display{128,64,&Wire,-1}; Screensaver* screensaver=nullptr; WiFiService* wifi=nullptr; // portal pointer removed
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
  void setConnectivityStatus(const ConnectivityStatus& s) { conn = s; }
private:
  ConnectivityStatus conn; // snapshot provided externally each render loop
public:
};
