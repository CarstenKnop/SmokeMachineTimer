// main.cpp - FogMachineRemoteControl entry point
#include <Arduino.h>
#include "Pins.h"
#include <EEPROM.h>
#include "Defaults.h"

// main.cpp
// Entry point for FogMachineRemoteControl (master). Sets up UI, device management, comms, battery, and menu system.
#include "ui/DisplayManager.h"
#include "ui/ButtonInput.h"
#include "ui/InputInterpreter.h"
#include "menu/MenuSystem.h"
#include "debug/DebugMetrics.h"
#include "device/DeviceManager.h"
#include "comm/CommManager.h"
#include "battery/BatteryMonitor.h"
#include "calibration/CalibrationManager.h"
#include "protocol/Protocol.h"
#include "core/RemoteConfig.h"
#include <esp_wifi.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>


// Forward declare deep sleep helper used at end of loop
static void maybeEnterDeepSleep(const DisplayManager& disp);

DisplayManager displayMgr;
ButtonInput buttons(BUTTON_UP_GPIO, BUTTON_DOWN_GPIO, BUTTON_HASH_GPIO, BUTTON_STAR_GPIO);
MenuSystem menu;
DeviceManager deviceMgr;
CalibrationManager calibMgr;
BatteryMonitor battery(BAT_ADC_PIN, calibMgr);
CommManager comm(deviceMgr);
InputInterpreter inputInterp;
RemoteConfig rconfig;

void setup() {
  Serial.begin(115200);
  pinMode(COMM_OUT_GPIO, OUTPUT);
  digitalWrite(COMM_OUT_GPIO, LOW);
  // Initialize display first and draw splash so we can show boot progress
  displayMgr.begin(); // handles Wire + splash internally
  displayMgr.drawBootStatus("Boot: display OK");
  buttons.begin();
  displayMgr.drawBootStatus("Boot: buttons OK");
  // Detect deep sleep wake and optionally skip splash on resume
  esp_sleep_wakeup_cause_t wake = esp_sleep_get_wakeup_cause();
  bool wokeFromDeepSleep = (wake == ESP_SLEEP_WAKEUP_EXT0 || wake == ESP_SLEEP_WAKEUP_EXT1 || wake == ESP_SLEEP_WAKEUP_TIMER || wake == ESP_SLEEP_WAKEUP_GPIO);
  if (wokeFromDeepSleep) {
    displayMgr.setSkipSplash(true);
    displayMgr.drawBootStatus("Boot: woke from deep sleep");
  } else {
    displayMgr.drawBootStatus("Boot: cold start");
  }
  // Boot-time failsafe: hold UP to enter update window; show a 2s prompt before proceeding
  {
    // Debounce/settle a few cycles
    for (int i = 0; i < 4; ++i) { buttons.update(); delay(5); }
  displayMgr.drawBootStatus("Hold UP for update (2s)");
    unsigned long promptUntil = millis() + 2000UL;
    bool upDown = false;
    while (millis() < promptUntil) {
      buttons.update();
      if (buttons.upHeld() || buttons.upPressed()) { upDown = true; }
      delay(10);
    }
    if (upDown) {
      // Start update mode with a full-screen update panel (clears any prior text)
      uint32_t until = millis() + 60000UL; // 60s window
      displayMgr.drawUpdateCountdown((uint8_t)((until - millis() + 999) / 1000));
      // Keep a fixed 60s window regardless of release once detected
      while (millis() < until) {
        uint8_t remaining = (uint8_t)((until - millis() + 999) / 1000);
        displayMgr.drawUpdateCountdown(remaining);
        buttons.update();
        delay(50);
      }
      displayMgr.drawBootStatus("Update: window closed");
    }
  }
  menu.begin();
  displayMgr.drawBootStatus("Boot: menu OK");
  EEPROM.begin(512); // ensure EEPROM is available for DeviceManager persistence
  displayMgr.drawBootStatus("Boot: EEPROM OK");
  deviceMgr.begin();
  displayMgr.drawBootStatus("Boot: devices OK");
  rconfig.begin(512);
  displayMgr.drawBootStatus("Boot: config OK");
  calibMgr.begin();
  displayMgr.drawBootStatus("Boot: calib OK");
  battery.begin();
  displayMgr.drawBootStatus("Boot: battery OK");
  comm.begin();
  displayMgr.drawBootStatus("Boot: comm OK");

  // Apply stored TX power and OLED brightness
  menu.setAppliedTxPowerQdbm(rconfig.getTxPowerQdbm());
  {
    uint8_t lvl = rconfig.getOledBrightness();
    if (lvl < 5) lvl = 5;
    menu.setAppliedOledBrightness(lvl);
  }
  esp_wifi_set_max_tx_power(rconfig.getTxPowerQdbm());
  // Apply display contrast when display is ready
  // Note: Adafruit_SSD1306 uses setContrast(0..255) and dim(). We'll adjust in render using menu applied value.

  delay(3000); // Give the serial monitor a chance to connect

  Serial.println("FogMachineRemoteControl started.");
}

void loop() {
  static unsigned long lastDiag = 0;
  static unsigned long loopCount = 0;
  static bool prevInMenu = false;

  buttons.update();
  // Aggregate button events for diagnostics
  static uint32_t upPresses=0, downPresses=0, hashPresses=0, starPresses=0, hashLongEntries=0;
  if (buttons.upPressed())   upPresses++;
  if (buttons.downPressed()) downPresses++;
  if (buttons.hashPressed()) hashPresses++;
  if (buttons.starPressed()) starPresses++;

  // Interpret inputs
  auto ev = inputInterp.update(buttons, menu);
  if (ev.longHash) { menu.enterMenu(); hashLongEntries++; }
  if (ev.shortHash) {
    // Enter Edit Timers directly from main screen
    const SlaveDevice* act = deviceMgr.getActive();
    if (act) { menu.enterEditTimers(act->ton, act->toff); }
  }
  // From main screen, Up/Down opens Active Timer selection
  if (!menu.isInMenu()) {
    if (buttons.upPressed() || buttons.downPressed()) {
      // Enter selection so that exiting returns to main screen
      menu.enterSelectActive(true);
    }
  }

  // STAR button: click vs hold
  {
    static unsigned long starDownMs = 0;
    static bool appliedHold = false;
    static bool pressAfterExit = false;
    bool pressEdge = buttons.starPressed();
    bool held = buttons.starHeld();
    unsigned long nowMs = millis();
    // On press edge, capture whether this press began after the last menu exit
    unsigned long exitTime = menu.getMenuExitTime();
    if (pressEdge) {
      starDownMs = nowMs;
      pressAfterExit = (exitTime == 0) || (nowMs >= exitTime);
    }
    // Hold action only if this press started after exit
    if (pressAfterExit && held && starDownMs && (nowMs - starDownMs >= Defaults::STAR_HOLD_THRESHOLD_MS) && !appliedHold) {
      // Start hold: force ON
      comm.overrideActive(true);
      appliedHold = true;
    }
    // Release edge detection: was down (starDownMs!=0) and now not held
    if (starDownMs && !held) {
      unsigned long heldMs = nowMs - starDownMs;
      if (heldMs >= Defaults::STAR_HOLD_THRESHOLD_MS) {
        // End hold: turn OFF
        comm.overrideActive(false);
      } else {
        // Short click: toggle
        if (pressAfterExit) comm.toggleActive();
      }
      appliedHold = false;
      starDownMs = 0;
      pressAfterExit = false;
    }
  }

  menu.update(buttons.upPressed(), buttons.downPressed(), buttons.hashPressed(), buttons.hashLongPressed(), buttons.starPressed(), buttons.upHeld(), buttons.downHeld());
  // If menu just closed, ensure a new long-press requires a fresh leading edge
  if (prevInMenu && !menu.isInMenu()) {
    inputInterp.resetOnMenuExit(menu.getMenuExitTime());
  }

  // Handle active device selection commit
  int newActiveIdx = -1;
  if (menu.consumeActiveSelect(newActiveIdx)) {
    if (newActiveIdx >=0 && newActiveIdx < deviceMgr.getDeviceCount()) {
      deviceMgr.setActiveIndex(newActiveIdx);
      Serial.printf("[ACTIVE] Selected device index %d\n", newActiveIdx);
      comm.requestStatusActive(); // fast refresh
    }
  }

  comm.loop();

  // Status polling only on main screen (not in menu), when display is not blank, and when we have an active device
  static unsigned long lastStatusReq = 0;
  if (!menu.isInMenu() && !displayMgr.isBlank()) {
    unsigned long nowMs = millis();
    static unsigned long fastPollUntil = 0;
    if (prevInMenu && !menu.isInMenu()) { fastPollUntil = millis() + 2000; }
    unsigned long interval = (millis() < fastPollUntil) ? 120 : 300; // faster
    if (nowMs - lastStatusReq > interval) {
      comm.requestStatusActive();
      lastStatusReq = nowMs;
    }
  }

  // Live RSSI screen refresh: when on SHOW_RSSI, periodically poll the visible devices
  static unsigned long lastRssiRefreshMs = 0;
  if (menu.getMode() == MenuSystem::Mode::SHOW_RSSI && !displayMgr.isBlank()) {
    // Enable remote-side RSSI sniffer while on this screen
    comm.setRssiSnifferEnabled(true);
    unsigned long nowMs = millis();
    if (nowMs - lastRssiRefreshMs > 1000) { // 1s cadence for visible list
      int first = menu.getRssiFirst();
      int count = comm.getPairedCount();
      int maxRows = 4;
      for (int i = 0; i < maxRows; ++i) {
        int idx = first + i;
        if (idx >= 0 && idx < count) {
          const SlaveDevice &dev = comm.getPaired(idx);
          comm.requestStatus(dev);
        }
      }
      lastRssiRefreshMs = nowMs;
    }
  } else {
    // Turn off sniffer when leaving RSSI screen
    comm.setRssiSnifferEnabled(false);
  }

  // Pairing: keep scanning continuously while on screen, but pause when display is blank
  {
    static bool pairingWasActive = false;
    bool onPair = (menu.getMode() == MenuSystem::Mode::PAIRING);
    if (onPair) {
      if (displayMgr.isBlank()) {
        if (comm.isDiscovering()) comm.stopDiscovery();
      } else {
        if (!comm.isDiscovering()) comm.startDiscovery(0);
      }
    } else if (pairingWasActive) {
      // Leaving pairing screen: ensure discovery is stopped
      if (comm.isDiscovering()) comm.stopDiscovery();
    }
    pairingWasActive = onPair;
  }

  static unsigned long lastDisplay = 0;
  unsigned long now = millis();
  if (now - lastDisplay > 33) { // ~30Hz
    displayMgr.render(deviceMgr, battery, menu, buttons);
    lastDisplay = now;
  }

  // Handle battery calibration UI lifecycle
  if (menu.getMode() == MenuSystem::Mode::BATTERY_CALIB) {
    if (!menu.calibInitialized) {
      uint16_t a0,a50,a100; calibMgr.getCalibrationPoints(a0,a50,a100);
      menu.initBatteryCal(a0,a50,a100);
    }
    uint16_t out[3];
    if (menu.consumeCalibSave(out)) {
      calibMgr.setCalibrationPoints(out[0], out[1], out[2]);
    }
  }

  // Handle remote factory reset request (from menu)
  if (menu.consumeRemoteReset()) {
  // Handle menu saves for TX power and brightness
  int8_t qdbm;
  if (menu.consumeTxPowerSave(qdbm)) {
    rconfig.setTxPowerQdbm(qdbm);
    rconfig.save();
    esp_wifi_set_max_tx_power(qdbm);
  }
  uint8_t lvl;
  if (menu.consumeBrightnessSave(lvl)) {
    if (lvl < 5) lvl = 5;
    rconfig.setOledBrightness(lvl);
    rconfig.save();
    // No direct global setter here; DisplayManager can read applied value via menu or we apply via a small hook
  }
    Serial.println("[REMOTE] Factory reset: clearing paired devices and calibration, restarting...");
    deviceMgr.factoryReset();
    calibMgr.resetToDefaults();
    delay(200);
    ESP.restart();
  }

  loopCount++;
  if (now - lastDiag > 1000) {
    DebugMetrics &dm = DebugMetrics::instance();
    Serial.printf("[DIAG] loop/s=%lu inMenu=%d #hold=%lums BTN(U,D,#,*,#L)=%lu,%lu,%lu,%lu,%lu DISP(fr=%lu avgPrep=%lums avgFlush=%lums maxFlush=%lums slow=%lu pbarFr=%lu pbarLast=%.0f%%)\n",
          loopCount, menu.isInMenu(), buttons.hashHoldDuration(),
          (unsigned long)upPresses,(unsigned long)downPresses,(unsigned long)hashPresses,(unsigned long)starPresses,(unsigned long)hashLongEntries,
          (unsigned long)dm.getFrameCount(), (unsigned long)dm.getAvgPrep(), (unsigned long)dm.getAvgFlush(), (unsigned long)dm.getMaxFlush(), (unsigned long)dm.getSlowFlushes(),
          (unsigned long)dm.getProgressFrames(), dm.getLastProgressPct()*100.0f);
    dm.resetProgress();
    dm.resetDisplay();
    upPresses = downPresses = hashPresses = starPresses = hashLongEntries = 0;
    loopCount = 0;
    lastDiag = now;
  }
  prevInMenu = menu.isInMenu();
  // Enter deep sleep if display is currently blanked
  maybeEnterDeepSleep(displayMgr);
}

// Helper: enter deep sleep when display is blanked; wake on button press
// Note: On ESP32-C3, deep sleep GPIO wake uses EXT1 with modes:
//  - ESP_EXT1_WAKEUP_ALL_LOW
//  - ESP_EXT1_WAKEUP_ANY_HIGH
// Our buttons are active-low with pull-ups during run. To ensure reliable compile/run on C3
// without hardware changes, we select a single wake pin (GPIO9 = '#') and use ALL_LOW.
// If you later rewire for active-high with pulldowns, you can switch to ANY_HIGH and include
// all buttons in the mask to wake on any button.
static void maybeEnterDeepSleep(const DisplayManager& disp) {
  if (!disp.isBlank()) return;
  // Reliable wake on ESP32-C3: use EXT1 wake on a single RTC-capable active-low button.
  // Use UP (GPIO3) so pressing it wakes the device.
  const uint64_t wake_mask = (1ULL << BUTTON_UP_GPIO);
  // For active-low buttons, use ALL_LOW (pin goes low when pressed)
  // Ensure RTC pull-up is enabled so the line stays HIGH during deep sleep
  rtc_gpio_init((gpio_num_t)BUTTON_UP_GPIO);
  rtc_gpio_set_direction((gpio_num_t)BUTTON_UP_GPIO, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pullup_en((gpio_num_t)BUTTON_UP_GPIO);
  rtc_gpio_pulldown_dis((gpio_num_t)BUTTON_UP_GPIO);
  esp_deep_sleep_enable_ext1_wakeup(wake_mask, ESP_EXT1_WAKEUP_ALL_LOW);
  delay(5);
  esp_deep_sleep_start();
}
