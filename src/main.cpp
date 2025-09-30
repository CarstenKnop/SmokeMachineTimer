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

static Config config;
static Buttons buttons;
static TimerController timerCtl;
static Screensaver screensaver;
static MenuSystem menu;
static DisplayManager displayMgr;

static unsigned long lastBlink = 0;
static bool blinkState = false;
static unsigned long hashHoldStartRun = 0; // track # hold for distinguishing short press vs menu hold

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
}

void loop() {
  unsigned long now = millis();
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
  if (menu.inHelp()) { menu.updateHelpAnimation(now); }
  menu.updateResult(now);

  // Timing + relay
  // Pause run timing while in progress hold (after threshold) so cycle freezes
  // Always tick now (previous pause removed to keep timing continuous). If you want pause, re-instate condition.
  timerCtl.tick(now);
  digitalWrite(Defaults::RELAY_PIN, timerCtl.isRelayOn()?HIGH:LOW);

  // Render only if not blanked
  if (!screensaver.isBlanked()) {
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
