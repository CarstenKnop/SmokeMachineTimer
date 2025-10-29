// main.cpp - FogMachineRemoteControl entry point
#include <Arduino.h>
#include "Pins.h"
#include <EEPROM.h>
#include "Defaults.h"
#include <vector>

// main.cpp
// Entry point for FogMachineRemoteControl (master). Sets up UI, device management, comms, battery, and menu system.
#include "ui/DisplayManager.h"
#include "ui/ButtonInput.h"
#include "ui/InputInterpreter.h"
#include "menu/MenuSystem.h"
#include "debug/DebugMetrics.h"
#include "debug/DebugSerialBridge.h"
#include "device/DeviceManager.h"
#include "comm/CommManager.h"
#include "battery/BatteryMonitor.h"
#include "calibration/CalibrationManager.h"
#include "protocol/Protocol.h"
#include "core/RemoteConfig.h"
#include "channel/RemoteChannelManager.h"
#include <esp_wifi.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>


// Forward declare deep sleep helpers used at end of loop
static void maybeEnterDeepSleep(const DisplayManager& disp);
static void enterDeepSleepNow();
static bool runUpdateCountdown(ButtonInput& buttons, DisplayManager& display);

DisplayManager displayMgr;
ButtonInput buttons(BUTTON_UP_GPIO, BUTTON_DOWN_GPIO, BUTTON_HASH_GPIO, BUTTON_STAR_GPIO);
MenuSystem menu;
DeviceManager deviceMgr;
CalibrationManager calibMgr;
BatteryMonitor battery(BAT_ADC_PIN, calibMgr);
RemoteChannelManager channelMgr;
CommManager comm(deviceMgr, channelMgr);
InputInterpreter inputInterp;
RemoteConfig rconfig;
DebugSerialBridge debugBridge(comm, deviceMgr, channelMgr);

static void wipeRemoteEeprom() {
  for (int i = 0; i < 512; ++i) {
    EEPROM.write(i, 0);
  }
  EEPROM.commit();
}

void setup() {
  // Ensure RTC IO holds are released on boot so pins can be configured normally
  gpio_deep_sleep_hold_dis();
  Serial.begin(115200);
  pinMode(COMM_OUT_GPIO, OUTPUT);
  digitalWrite(COMM_OUT_GPIO, LOW);
  // Start normally (no special wake flag or splash skipping)
  esp_sleep_wakeup_cause_t wake = esp_sleep_get_wakeup_cause();
  bool wokeFromDeepSleep = (wake == ESP_SLEEP_WAKEUP_EXT0 || wake == ESP_SLEEP_WAKEUP_EXT1 || wake == ESP_SLEEP_WAKEUP_TIMER || wake == ESP_SLEEP_WAKEUP_GPIO);
  // Initialize display and show boot progress
  displayMgr.begin(); // handles Wire + splash internally (may be skipped)
  displayMgr.drawBootStatus("Boot: display OK");
  buttons.begin();
  displayMgr.drawBootStatus("Boot: buttons OK");
  // Configure charger status pins with sensible pulls
  if (CHARGER_CHG_PIN >= 0) { pinMode(CHARGER_CHG_PIN, Defaults::CHARGER_CHG_ACTIVE_HIGH ? INPUT_PULLDOWN : INPUT_PULLUP); }
  if (CHARGER_PWR_PIN >= 0) { pinMode(CHARGER_PWR_PIN, Defaults::CHARGER_PWR_ACTIVE_HIGH ? INPUT : INPUT_PULLUP); }
  // Inform user if we woke from deep sleep
  displayMgr.drawBootStatus(wokeFromDeepSleep ? "Boot: woke from deep sleep" : "Boot: cold start");
  bool updateCountdownArmed = false;
  // Boot-time failsafe: user must hold UP through boot to enter update window
  {
    // Debounce/settle a few cycles and capture initial state
    for (int i = 0; i < 4; ++i) { buttons.update(); delay(5); }
    updateCountdownArmed = buttons.upHeld();
    displayMgr.drawBootStatus("Hold UP through boot for update");
    unsigned long promptUntil = millis() + 2000UL;
    while (millis() < promptUntil) {
      buttons.update();
      if (buttons.upHeld()) { updateCountdownArmed = true; }
      delay(10);
    }
  }
  menu.begin();
  displayMgr.drawBootStatus("Boot: menu OK");
  EEPROM.begin(512); // ensure EEPROM is available for DeviceManager persistence
  displayMgr.drawBootStatus("Boot: EEPROM OK");
  channelMgr.begin(&wipeRemoteEeprom, 512);
  displayMgr.drawBootStatus("Boot: channel OK");
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
  comm.attachDebugBridge(&debugBridge);
  debugBridge.begin();
  // Kick an initial status request so main screen (RSSI/bars) populates quickly after boot
  comm.requestStatusActive();

  // Check whether UP remained held through boot to trigger update countdown
  buttons.update();
  bool updateCountdownReady = updateCountdownArmed && (buttons.upHeld() || buttons.upPressed());
  if (updateCountdownReady) {
    displayMgr.drawBootStatus("Update: countdown (*/cancel)");
    runUpdateCountdown(buttons, displayMgr);
    buttons.update();
  }

  // Apply stored TX power, OLED brightness, and display blanking
  menu.setAppliedTxPowerQdbm(rconfig.getTxPowerQdbm());
  {
    uint8_t lvl = rconfig.getOledBrightness();
    if (lvl < 5) lvl = 5;
    menu.setAppliedOledBrightness(lvl);
  }
  // Blanking seconds persisted
  menu.setAppliedBlankingSeconds(rconfig.getBlankingSeconds());
  // RSSI calibration bounds
  menu.editRssiLowDbm = rconfig.getRssiLowDbm();
  menu.editRssiHighDbm = rconfig.getRssiHighDbm();
  menu.setAppliedRssiLowDbm(menu.editRssiLowDbm);
  menu.setAppliedRssiHighDbm(menu.editRssiHighDbm);
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
    static bool deepSleepIssued = false;
    bool pressEdge = buttons.starPressed();
    bool held = buttons.starHeld();
    unsigned long nowMs = millis();
    // On press edge, capture whether this press began after the last menu exit
    unsigned long exitTime = menu.getMenuExitTime();
    if (pressEdge) {
      starDownMs = nowMs;
      pressAfterExit = (exitTime == 0) || (nowMs >= exitTime);
      appliedHold = false;
      deepSleepIssued = false;
    }
    if (pressAfterExit && held && starDownMs) {
      unsigned long heldMs = nowMs - starDownMs;
      if (!deepSleepIssued && heldMs >= Defaults::STAR_DEEP_SLEEP_HOLD_MS) {
        deepSleepIssued = true;
        displayMgr.blankNow();
        maybeEnterDeepSleep(displayMgr);
      } else if (!appliedHold && heldMs >= Defaults::STAR_HOLD_THRESHOLD_MS) {
        comm.overrideActive(true);
        appliedHold = true;
      }
    }
    if (starDownMs && !held) {
      unsigned long heldMs = nowMs - starDownMs;
      if (!deepSleepIssued && pressAfterExit) {
        if (heldMs >= Defaults::STAR_HOLD_THRESHOLD_MS) {
          comm.overrideActive(false);
        } else {
          comm.toggleActive();
        }
      }
      appliedHold = false;
      starDownMs = 0;
      pressAfterExit = false;
      deepSleepIssued = false;
    }
  }

  menu.update(buttons.upPressed(), buttons.downPressed(), buttons.hashPressed(), buttons.hashLongPressed(), buttons.starPressed(), buttons.upHeld(), buttons.downHeld());
  if (menu.consumeChannelScanRequest()) {
    if (!channelMgr.requestSurvey()) {
      menu.setChannelScanFailed();
    }
  }
  if (channelMgr.getSurveyState() == RemoteChannelManager::SurveyState::Running) {
    if (channelMgr.pollSurvey()) {
      if (channelMgr.getSurveyState() == RemoteChannelManager::SurveyState::Complete) {
        std::vector<MenuSystem::ChannelOption> options;
        const auto &candidates = channelMgr.getCandidates();
        options.reserve(candidates.size());
        for (const auto &cand : candidates) {
          MenuSystem::ChannelOption opt{cand.channel, cand.apCount, cand.sumAbsRssi};
          options.push_back(opt);
        }
        menu.setChannelScanResult(options, channelMgr.getStoredChannel());
      } else {
        menu.setChannelScanFailed();
      }
      channelMgr.clearSurvey();
    }
  }
  uint8_t pendingChannel = 0;
  if (menu.consumeChannelSave(pendingChannel)) {
    uint8_t previousChannel = channelMgr.getActiveChannel();
    if (channelMgr.storeChannel(pendingChannel)) {
      comm.onChannelChanged(previousChannel);
    } else {
      channelMgr.applyStoredChannel();
    }
  }
  // If menu just closed, ensure a new long-press requires a fresh leading edge
  if (prevInMenu && !menu.isInMenu()) {
    inputInterp.resetOnMenuExit(menu.getMenuExitTime());
    // Immediately request a fresh status for the active device to update RSSI bars promptly
    comm.requestStatusActive();
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
  debugBridge.loop();

  // Status polling only on main screen (not in menu), when display is not blank, and when we have an active device
  static unsigned long lastStatusReq = 0;
  if (!menu.isInMenu() && !displayMgr.isBlank()) {
    unsigned long nowMs = millis();
    static unsigned long fastPollUntil = 0;
    if (prevInMenu && !menu.isInMenu()) { fastPollUntil = millis() + 2000; }
    // Adapt polling rate: faster when stale, moderately fast otherwise; min with fast-poll window
    bool staleRssi = true;
    if (const SlaveDevice* act = deviceMgr.getActive()) {
      staleRssi = (act->lastStatusMs == 0) || (nowMs - act->lastStatusMs > Defaults::RSSI_STALE_MS);
    }
    unsigned long baseInterval = staleRssi ? 140UL : 220UL; // ~7 Hz when stale, ~4.5 Hz when fresh
    unsigned long interval = (millis() < fastPollUntil) ? min<unsigned long>(120UL, baseInterval) : baseInterval;
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

  // While calibrating RSSI thresholds, keep polling the active device for Timer-side RSSI updates
  static unsigned long lastRssiCalibPollMs = 0;
  if (menu.getMode() == MenuSystem::Mode::EDIT_RSSI_CALIB && !displayMgr.isBlank()) {
    unsigned long nowMs = millis();
    if (nowMs - lastRssiCalibPollMs > 500) { // 2 Hz to feel live
      comm.requestStatusActive();
      lastRssiCalibPollMs = nowMs;
    }
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
    displayMgr.setPreventBlanking(debugBridge.isPcConnected());
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

  // Handle menu saves (persist settings)
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
  }
  int secs;
  if (menu.consumeBlankingSave(secs)) {
    rconfig.setBlankingSeconds((uint16_t)max(0, secs));
    rconfig.save();
  }
  int8_t loDbm, hiDbm;
  if (menu.consumeRssiCalibSave(loDbm, hiDbm)) {
    rconfig.setRssiLowDbm(loDbm);
    rconfig.setRssiHighDbm(hiDbm);
    rconfig.save();
  }

  // Handle remote factory reset request (from menu)
  if (menu.consumeRemoteReset()) {
    Serial.println("[REMOTE] Factory reset: clearing paired devices and calibration, restarting...");
    deviceMgr.factoryReset();
    calibMgr.resetToDefaults();
    delay(200);
    ESP.restart();
  }

  // Handle power cycle request (from menu)
  if (menu.consumePowerCycle()) {
    Serial.println("[REMOTE] Power cycle requested via menu. Restarting...");
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
static void configureDeepSleepWakePins() {
  const uint64_t wake_mask = (1ULL << BUTTON_UP_GPIO) | (1ULL << BUTTON_DOWN_GPIO);
  gpio_wakeup_enable((gpio_num_t)BUTTON_UP_GPIO, GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable((gpio_num_t)BUTTON_DOWN_GPIO, GPIO_INTR_LOW_LEVEL);
  gpio_pullup_en((gpio_num_t)BUTTON_UP_GPIO);
  gpio_pullup_en((gpio_num_t)BUTTON_DOWN_GPIO);
  gpio_deep_sleep_hold_en();
  esp_deep_sleep_enable_gpio_wakeup(wake_mask, ESP_GPIO_WAKEUP_GPIO_LOW);
}

static void enterDeepSleepNow() {
  Serial.println("[REMOTE] Entering deep sleep...");
  configureDeepSleepWakePins();
  esp_deep_sleep_start();
}

static bool runUpdateCountdown(ButtonInput& buttons, DisplayManager& display) {
  const uint32_t windowMs = 60000UL;
  uint32_t endMs = millis() + windowMs;
  while (true) {
    uint32_t nowMs = millis();
    int32_t remainingMs = static_cast<int32_t>(endMs - nowMs);
    if (remainingMs <= 0) {
      break;
    }
    uint8_t remaining = static_cast<uint8_t>((static_cast<uint32_t>(remainingMs) + 999U) / 1000U);
    display.drawUpdateCountdown(remaining);
    buttons.update();
    if (buttons.starPressed()) {
      display.drawBootStatus("Update canceled");
      return false;
    }
    delay(50);
  }
  display.drawBootStatus("Update: window closed");
  return true;
}

static void maybeEnterDeepSleep(const DisplayManager& disp) {
  if (!disp.isBlank()) return;
  enterDeepSleepNow();
}
