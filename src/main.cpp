// Main application entry (clean rewrite to remove hidden characters)
#include <Arduino.h>
#include <Wire.h>
#include "core/Defaults.h"
#include "core/Config.h"
#include "core/Buttons.h"
#include "core/TimerController.h"
#include "core/Screensaver.h"
#include "core/MenuSystem.h"
#include "core/DisplayManager.h"
#include "core/WiFiService.h"
#include "core/AsyncPortalService.h"
#include "core/ConnectivityStatus.h"
#include <ESPmDNS.h>

static Config config;
static Buttons buttons;
static TimerController timerCtl;
static Screensaver screensaver;
static MenuSystem menu;
static DisplayManager displayMgr;
static WiFiService wifiService;
static AsyncPortalService asyncPortal;
static bool wifiStarted=false;
static unsigned long portalLastActiveMenuMs = 0; // last time we were in a WiFi related menu
static bool portalManuallyStarted = false;
static constexpr unsigned long PORTAL_IDLE_STOP_MS = 30000; // stop portal 30s after leaving menu
unsigned long netSetFlashUntil = 0; // show NET indicator until millis (extern in DisplayManager)
unsigned long staFlashUntil = 0; // show STA indicator when station connects

static unsigned long lastBlink = 0;
static bool blinkState = false;
static unsigned long hashHoldStartRun = 0; // track # hold for distinguishing short press vs menu hold
// Metrics exposed via /health (must have external linkage)
unsigned long loopsPerSec = 0;
unsigned long remoteUpdateCount = 0;
static unsigned long loopCounter = 0; static unsigned long lastLoopMeasureMs=0;

void setup() {
  Serial.begin(9600);
  while(!Serial && millis() < 1500) { }
  pinMode(Defaults::RELAY_PIN, OUTPUT);
  Wire.begin(Defaults::OLED_SDA, Defaults::OLED_SCL);
  config.begin();
  buttons.begin();
  timerCtl.begin(&config.get());
  displayMgr.begin();
  screensaver.begin(displayMgr.get());
  screensaver.configure(config.get().screensaverDelaySec);
  screensaver.noteActivity(millis());
  displayMgr.attachScreensaver(&screensaver);
  displayMgr.attachWiFi(&wifiService); // legacy for QR payload building
  displayMgr.attachPortal(&asyncPortal); // for refined connectivity glyphs
  asyncPortal.setAuth("admin","admin"); // legacy fallback
  asyncPortal.setControlAuth("admin","admin");
  asyncPortal.setOtaAuth("admin","admin");
  // Initialize mirror flags via helper
  asyncPortal.initConfigMirror(config.get().wifiEnabled, config.get().apAlwaysOn);
  // Setters to mutate config from web API
  asyncPortal.setWifiEnableSetter([&](bool en){
    config.saveWiFiEnabled(en?1:0);
    asyncPortal.setWifiEnabledMirror(en);
    Serial.print(F("[API] wifiEnabled set to ")); Serial.println(en?1:0);
    if (!en && asyncPortal.isStarted()) { asyncPortal.stop(); }
  });
  asyncPortal.setApAlwaysSetter([&](bool en){
    config.saveApAlwaysOn(en?1:0);
    asyncPortal.setApAlwaysMirror(en);
    Serial.print(F("[API] apAlwaysOn set to ")); Serial.println(en?1:0);
  });
  // Relay toggle callback for dashboard API
  asyncPortal.setRelayToggleCallback([&](bool &newState){
    // Toggle manual relay (simulate user star press) respecting edit state
    if (timerCtl.inEdit()) return false;
    timerCtl.toggleRelayManual();
    newState = timerCtl.isRelayOn();
    return true;
  });
  // Dynamic JSON status provider for /values
  asyncPortal.setStatusCallback([&](String &out){
    // Build JSON manually to avoid dynamic allocations beyond String
    // Data: off,on,currentElapsed,relay,onPhase,saverRemaining,version
    unsigned long off = config.get().offTime; // tenths
    unsigned long on = config.get().onTime;
    bool relayOn = timerCtl.isRelayOn();
    unsigned long currentElapsed = timerCtl.currentTimer(); // tenths elapsed
    unsigned long saverRemain = screensaver.remainingSeconds(millis());
    uint8_t wifiEn = config.get().wifiEnabled;
    IPAddress apip = asyncPortal.isStarted()? asyncPortal.ip() : IPAddress(0,0,0,0);
  out.reserve(196);
    out += '{';
    out += F("\"off\":"); out += off;
    out += F(",\"on\":"); out += on;
    out += F(",\"currentElapsed\":"); out += currentElapsed;
    out += F(",\"relay\":"); out += (relayOn?1:0);
    out += F(",\"phase\":\""); out += (relayOn?"ON":"OFF"); out += '\"';
    out += F(",\"saverRemain\":"); out += saverRemain;
    out += F(",\"wifiEnabled\":"); out += wifiEn;
    out += F(",\"apIp\":\""); out += apip.toString(); out += '\"';
  out += F(",\"apActive\":"); out += (asyncPortal.isApActive()?1:0);
  out += F(",\"apSuppressed\":"); out += (asyncPortal.isApSuppressed()?1:0);
      out += F(",\"apAlwaysOn\":"); out += (config.get().apAlwaysOn?1:0);
    // STA fields
    AsyncPortalService::StaState ss = asyncPortal.getStaState();
    out += F(",\"staStatus\":\"");
    switch(ss){
      case AsyncPortalService::StaState::IDLE: out += F("IDLE"); break;
      case AsyncPortalService::StaState::SCANNING: out += F("SCANNING"); break;
      case AsyncPortalService::StaState::CONNECTING: out += F("CONNECTING"); break;
      case AsyncPortalService::StaState::CONNECTED: out += F("CONNECTED"); break;
      case AsyncPortalService::StaState::FAILED: out += F("FAILED"); break;
    }
    out += '\"';
    if (ss == AsyncPortalService::StaState::CONNECTED) {
      out += F(",\"staIp\":\""); out += asyncPortal.getStaIp().toString(); out += '\"';
      out += F(",\"staRssi\":"); out += asyncPortal.getStaRssi();
    }
    out += F(",\"staConnected\":"); out += (ss==AsyncPortalService::StaState::CONNECTED?1:0);
    out += F(",\"version\":\""); out += Defaults::VERSION(); out += '\"';
  // Add placeholders for additional diagnostics appended at portal layer (avoid trailing comma issues)
  out += '}';
  });
  asyncPortal.setTimerUpdateCallback([&](uint32_t offTenths, uint32_t onTenths, String &err){
    // Validate
    static unsigned long lastRemoteApply=0;
    const unsigned long DEBOUNCE_MS = 2000; // 2s debounce for EEPROM wear protection
    unsigned long nowMs = millis();
    if (nowMs - lastRemoteApply < DEBOUNCE_MS) { err = F("Too soon"); return false; }
    if (offTenths < Defaults::TIMER_MIN || offTenths > Defaults::TIMER_MAX) { err = F("Bad off"); return false; }
    if (onTenths  < Defaults::TIMER_MIN || onTenths  > Defaults::TIMER_MAX) { err = F("Bad on"); return false; }
    if (timerCtl.inEdit()) { err = F("Local edit active"); return false; }
    bool changed = (config.get().offTime != offTenths) || (config.get().onTime != onTenths);
    if (!changed) { err = F("No change"); return true; }
    // Apply
    config.saveTimersIfChanged(offTenths, onTenths, true);
    timerCtl.reloadFromConfig(config.get());
    netSetFlashUntil = millis() + 1500; // 1.5s flash
    lastRemoteApply = nowMs;
    remoteUpdateCount++;
    return true;
  });
  // Portal now gated; will be started on-demand when user opens WiFi/QR menu
  if (!MDNS.begin("fogtimer")) {
    Serial.println(F("[mDNS] Failed to start"));
  } else {
    MDNS.addService("http", "tcp", 80);
  Serial.println(F("[mDNS] Advertised fogtimer.local"));
  }
}

void loop() {
  unsigned long now = millis();
  // Loop frequency measurement
  loopCounter++;
  if (now - lastLoopMeasureMs >= 1000) { loopsPerSec = loopCounter; loopCounter=0; lastLoopMeasureMs = now; }
  ButtonState bs = buttons.poll();

  // Blink for edit states
  if (now - lastBlink > Defaults::EDIT_BLINK_INTERVAL_MS) { blinkState = !blinkState; lastBlink = now; }

  // Screensaver handling: first allow wake detection, then process blanking/consumption
  if (screensaver.isBlanked()) {
    // Attempt to wake on any button press; consume that press
    if (screensaver.handleWake(bs, now)) {
      bs.upEdge = bs.downEdge = bs.hashEdge = bs.starEdge = false; // ignore edges after wake
    }
  } else {
    if (bs.up || bs.down || bs.hash || bs.star) { screensaver.noteActivity(now); }
    screensaver.loop(now); // may blank during this call
  }

  // # button handling in RUN (short reset vs hold for menu). Progress state starts only after threshold.
  if (!timerCtl.inEdit()) {
    if (menu.getState()==MenuSystem::State::INACTIVE) {
      if (bs.hashEdge) { hashHoldStartRun = now; }
      if (bs.hashEdge) { menu.setMenuHint(true); }
      if (hashHoldStartRun && bs.hash && (now - hashHoldStartRun) >= Defaults::MENU_PROGRESS_START_MS) {
        menu.startProgressDeferred(now); // begins progress state
      }
      if (!bs.hash && hashHoldStartRun) {
        unsigned long held = now - hashHoldStartRun;
        if (held < Defaults::MENU_PROGRESS_START_MS) {
          timerCtl.resetCycle(); // short tap reset
          Serial.println(F("Short # reset (cycle restarted)"));
          menu.setMenuHint(false); // short tap, remove hint
        }
        hashHoldStartRun = 0;
      }
    }
    if (menu.inProgress()) {
      // Release inside progress decides menu entry or cancel
      if (!bs.hash) {
        menu.updateProgress(false,true, now); // release
        menu.setMenuHint(false); // progress ended
      } else {
        menu.updateProgress(true,false, now); // still holding
      }
    }
  }

  // Run mode interactions
  if (timerCtl.getState()==TimerController::AppState::RUN && menu.getState()==MenuSystem::State::INACTIVE) {
    if (bs.starEdge) timerCtl.toggleRelayManual();
    if (bs.upEdge || bs.downEdge) timerCtl.enterEdit();
  }

  // Edit handling
  if (timerCtl.inEdit()) {
    bool changed=false, exited=false; timerCtl.handleEdit(bs, now, changed, exited);
    if (exited) {
      if (!timerCtl.wasCancelled() && timerCtl.timersDirty) {
        Serial.println(F("Edit exit: saving timers"));
        config.saveTimersIfChanged(config.get().offTime, config.get().onTime, true);
        timerCtl.timersDirty=false;
      } else if (timerCtl.wasCancelled()) {
        Serial.println(F("Edit cancelled: changes discarded"));
      }
    }
  }

  // Menu logic
  if (menu.inSelect()) { menu.navigate(bs, now); }
  menu.processInput(bs, now, config, screensaver);
  // Handle special WiFi menu transient states (toggle / reset) on first entry
  if (menu.getState()==MenuSystem::State::WIFI_ENABLE_TOGGLE) {
    static bool applied=false; static unsigned long stamp=0; if (!applied) {
      applied=true; stamp=now;
      uint8_t newEn = menu.wifiEnableTempValue();
      config.saveWiFiEnabled(newEn);
      Serial.print(F("[WiFi] WiFi Enable saved=")); Serial.println(newEn);
      if (!newEn && asyncPortal.isStarted()) { asyncPortal.stop(); Serial.println(F("[WiFi] Portal stopped (disabled)")); }
    }
    if (now - stamp > 1000) { applied=false; }
  } else { static bool applied=false; applied=false; }
  if (menu.getState()==MenuSystem::State::WIFI_RESET_CONFIRM) {
    static bool handledReset=false; static unsigned long resetStamp=0; if (!handledReset) {
      handledReset=true; resetStamp=now;
      config.resetWiFi();
      Serial.println(F("[WiFi] Credentials reset"));
    }
    if (now - resetStamp > 1200) { handledReset=false; }
  } else {
    static bool handledReset=false; handledReset=false;
  }
  if (menu.getState()==MenuSystem::State::WIFI_AP_ALWAYS_TOGGLE) {
    static bool applied=false; static unsigned long stamp=0; if (!applied) {
      applied=true; stamp=now;
      uint8_t newVal = menu.apAlwaysTempValue(); config.saveApAlwaysOn(newVal);
      Serial.print(F("[WiFi] AP Always saved=")); Serial.println(newVal);
      if (newVal) {
        if (!asyncPortal.isStarted() && config.get().wifiEnabled) {
          if (asyncPortal.begin("FogTimerAP","",80)) Serial.println(F("[Portal] Started (AP always)"));
        }
      } else {
        // If turning off and AP is suppressed+STA disconnected later, logic will stop after idle.
      }
    }
    if (now - stamp > 1000) { applied=false; }
  } else { static bool applied=false; applied=false; }
  // Legacy WiFiService no longer auto-started; async portal handles AP + DNS.
  // Still loop legacy if ever started manually (future compatibility)
  if (wifiStarted) wifiService.loop();
  // Portal gating logic: start when entering WiFi info, dynamic QR, or Rick screens
  MenuSystem::State st = menu.getState();
  bool inPortalMenu = (st==MenuSystem::State::WIFI_INFO || st==MenuSystem::State::QR_DYN || st==MenuSystem::State::RICK);
  if ((inPortalMenu || config.get().apAlwaysOn) && config.get().wifiEnabled) {
    portalLastActiveMenuMs = now;
    if (!asyncPortal.isStarted()) {
  if (asyncPortal.begin("FogTimerAP","",80)) {
        Serial.println(F("[Portal] Started on-demand"));
      } else {
        Serial.println(F("[Portal] Failed to start"));
      }
    }
  } else {
    // If portal running and idle for threshold, stop to reduce RF noise / power
    if (!config.get().apAlwaysOn && asyncPortal.isStarted() && (now - portalLastActiveMenuMs) > PORTAL_IDLE_STOP_MS) {
      asyncPortal.stop();
      Serial.println(F("[Portal] Stopped after idle"));
    }
  }
  asyncPortal.loop();
  // Auto-reconnect & AP suppression
  static unsigned long lastReconnectAttempt=0;
  if (config.get().staSsid[0] != '\0') {
    AsyncPortalService::StaState sst = asyncPortal.getStaState();
    if ((sst == AsyncPortalService::StaState::FAILED || sst == AsyncPortalService::StaState::IDLE) && millis() - lastReconnectAttempt > 10000) {
      asyncPortal.beginJoin(String(config.get().staSsid), String());
      lastReconnectAttempt = millis();
      Serial.println(F("[WiFi] Auto reconnect attempt"));
    }
  }
  if (asyncPortal.maybeDisableApOnSta(5000)) {
    Serial.println(F("[WiFi] AP disabled (STA stable)"));
  }
  asyncPortal.ensureApIfSuppressed();
  // Persist STA creds if a pending connection just succeeded
  if (asyncPortal.connectionSucceeded() && asyncPortal.hasPendingCreds()) {
    // If stored creds differ from pending, save
    if (strcmp(config.get().staSsid, asyncPortal.pendingSsidName().c_str()) != 0) {
      config.saveStaCreds(asyncPortal.pendingSsidName().c_str(), ""); // password not retrievable here (would need capture); future: store pass on beginJoin callback
      Serial.print(F("[WiFi] Saved STA SSID: ")); Serial.println(asyncPortal.pendingSsidName());
    }
    if (staFlashUntil < millis()) { staFlashUntil = millis() + 1500; }
  }
  if (menu.getState()==MenuSystem::State::WIFI_FORGET_CONFIRM) {
    static bool handledForget=false; static unsigned long forgetStamp=0; if (!handledForget) {
      handledForget=true; forgetStamp=now; config.forgetSta(); Serial.println(F("[WiFi] Station credentials forgotten"));
    }
    if (now - forgetStamp > 1200) { handledForget=false; }
  } else { static bool handledForget=false; handledForget=false; }
  if (menu.inHelp()) { menu.updateHelpAnimation(now); }
  menu.updateResult(now);

  // Timing + relay
  // Pause run timing while in progress hold (after threshold) so cycle freezes
  // Always tick now (previous pause removed to keep timing continuous). If you want pause, re-instate condition.
  timerCtl.tick(now);
  digitalWrite(Defaults::RELAY_PIN, timerCtl.isRelayOn()?HIGH:LOW);

  // Render only if not blanked
  if (!screensaver.isBlanked()) {
    // Build connectivity snapshot for glyph logic
  ConnectivityStatus cs;
  cs.wifiEnabled = config.get().wifiEnabled;
  cs.apActive = asyncPortal.isApActive();
  cs.apSuppressed = asyncPortal.isApSuppressed();
  cs.staConnected = (asyncPortal.getStaState() == AsyncPortalService::StaState::CONNECTED);
  cs.apClients = asyncPortal.apClients();
  // Consider auth recent if within last 8 seconds
  unsigned long lastAuth = asyncPortal.lastAuthMs();
  cs.recentAuth = (lastAuth!=0) && (millis() - lastAuth < 8000UL);
  cs.staRssi = asyncPortal.getStaRssi();
    displayMgr.setConnectivityStatus(cs);
    displayMgr.render(timerCtl, menu, config, blinkState, timerCtl.isRelayOn(), timerCtl.currentTimer());
  }
  // Debug: state summary every second (even while blanked)
  static unsigned long lastDbg=0; if (now - lastDbg >= 1000) {
    lastDbg = now;
    uint16_t remaining = screensaver.remainingSeconds(now);
    Serial.print(F("State:")); Serial.print(timerCtl.inEdit()?"EDIT":"RUN"); Serial.print(' ');
    Serial.print(F("Relay:")); Serial.print(timerCtl.isRelayOn()?"ON":"OFF"); Serial.print(' ');
    Serial.print(F("OffTime:")); Serial.print(config.get().offTime); Serial.print(' ');
    Serial.print(F("OnTime:")); Serial.print(config.get().onTime); Serial.print(' ');
    Serial.print(F("RemainingSaver:")); Serial.print(remaining); Serial.print(' ');
    Serial.print(F("Menu:")); Serial.print((int)menu.getState()); Serial.print(' ');
    Serial.print(F("Timer:")); Serial.print(timerCtl.currentTimer()); Serial.print(' ');
    Serial.print(F("Blanked:")); Serial.println(screensaver.isBlanked()?"Y":"N");
  }
  delay(Defaults::LOOP_DELAY_MS);
}

// End of file
