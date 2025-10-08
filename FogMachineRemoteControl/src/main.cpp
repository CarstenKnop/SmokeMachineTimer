// main.cpp - FogMachineRemoteControl entry point
#include <Arduino.h>
#include "Pins.h"
#include "ESPNowMaster.h"
#include "Defaults.h"

// main.cpp
// Entry point for FogMachineRemoteControl (master). Sets up UI, device management, comms, battery, and menu system.
#include "ui/DisplayManager.h"
#include "ui/ButtonInput.h"
#include "menu/MenuSystem.h"
#include "debug/DebugMetrics.h"
#include "device/DeviceManager.h"
#include "comm/CommManager.h"
#include "battery/BatteryMonitor.h"
#include "calibration/CalibrationManager.h"


DisplayManager displayMgr;
ButtonInput buttons(BUTTON_UP_GPIO, BUTTON_DOWN_GPIO, BUTTON_HASH_GPIO, BUTTON_STAR_GPIO);
MenuSystem menu;
DeviceManager deviceMgr;
CalibrationManager calibMgr;
BatteryMonitor battery(BAT_ADC_PIN, calibMgr);
CommManager comm(deviceMgr);

void setup() {
  Serial.begin(115200);
  pinMode(COMM_OUT_GPIO, OUTPUT);
  digitalWrite(COMM_OUT_GPIO, LOW);
  displayMgr.begin(); // handles Wire + splash internally
  buttons.begin();
  menu.begin();
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
  buttons.update();
  // Aggregate button events instead of spamming per press
  static uint32_t upPresses=0, downPresses=0, hashPresses=0, starPresses=0, hashLongEntries=0;
  if (buttons.upPressed())   upPresses++;
  if (buttons.downPressed()) downPresses++;
  if (buttons.hashPressed()) hashPresses++;
  if (buttons.starPressed()) starPresses++;
  static bool hashLongHandled = false; // ensure one menu entry per long press
  if (buttons.hashLongPressed() && !menu.isInMenu() && !hashLongHandled) {
      menu.enterMenu();
      hashLongEntries++;
      hashLongHandled = true;
  }
  if (!buttons.hashLongPressed()) hashLongHandled = false;
  menu.update(buttons.upPressed(), buttons.downPressed(), buttons.hashPressed(), buttons.hashLongPressed(), buttons.starPressed());
  comm.loop();
  // Status polling only on main screen (not in menu) and when we have an active device
  static unsigned long lastStatusReq = 0;
  if (!menu.isInMenu()) {
    unsigned long nowMs = millis();
    if (nowMs - lastStatusReq > 500) { // throttle to twice per second
      comm.requestStatusActive();
      lastStatusReq = nowMs;
    }
  }
  static unsigned long lastDisplay = 0;
  unsigned long now = millis();
  if (now - lastDisplay > 33) { // ~30Hz
  //Serial.println("[MAIN DIAG] Calling displayMgr.render()");
    displayMgr.render(deviceMgr, battery, menu, buttons);
    lastDisplay = now;
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
}
