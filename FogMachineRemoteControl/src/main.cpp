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


DisplayManager displayMgr;
ButtonInput buttons(BUTTON_UP_GPIO, BUTTON_DOWN_GPIO, BUTTON_HASH_GPIO, BUTTON_STAR_GPIO);
MenuSystem menu;
DeviceManager deviceMgr;
CalibrationManager calibMgr;
BatteryMonitor battery(BAT_ADC_PIN, calibMgr);
CommManager comm(deviceMgr);
InputInterpreter inputInterp;

void setup() {
  Serial.begin(115200);
  pinMode(COMM_OUT_GPIO, OUTPUT);
  digitalWrite(COMM_OUT_GPIO, LOW);
  displayMgr.begin(); // handles Wire + splash internally
  buttons.begin();
  menu.begin();
  EEPROM.begin(512); // ensure EEPROM is available for DeviceManager persistence
  deviceMgr.begin();
  calibMgr.begin();
  battery.begin();
  comm.begin();

  sleep(3); // Give the serial monitor a chance to connect

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

  // STAR button: click vs hold
  {
    static unsigned long starDownMs = 0;
    static bool appliedHold = false;
    bool pressEdge = buttons.starPressed();
    bool held = buttons.starHeld();
    unsigned long nowMs = millis();
    if (pressEdge) { starDownMs = nowMs; }
    if (held && starDownMs && (nowMs - starDownMs >= Defaults::STAR_HOLD_THRESHOLD_MS) && !appliedHold) {
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
        comm.toggleActive();
      }
      appliedHold = false;
      starDownMs = 0;
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

  // Status polling only on main screen (not in menu) and when we have an active device
  static unsigned long lastStatusReq = 0;
  if (!menu.isInMenu()) {
    unsigned long nowMs = millis();
    static unsigned long fastPollUntil = 0;
    if (prevInMenu && !menu.isInMenu()) { fastPollUntil = millis() + 2000; }
    unsigned long interval = (millis() < fastPollUntil) ? 120 : 300; // faster
    if (nowMs - lastStatusReq > interval) {
      comm.requestStatusActive();
      lastStatusReq = nowMs;
    }
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
}
