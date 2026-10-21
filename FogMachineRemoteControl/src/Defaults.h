// Defaults.h - compile-time constants of the remote (UI timing, layout, hardware polarity).
// Persistent user settings live in config/RemoteSettings.h.
#pragma once

#include <Arduino.h>
#include <stdint.h>
#include "FogProto.h"
#define FOG_FW_STRING_REMOTE FOG_FW_STRING

namespace Defaults {
  // Display
  static constexpr uint8_t OLED_ROTATION = 0;                 // 0..3 = 0/90/180/270 degrees
  static constexpr int DIGITS = 5;                            // timer digits XXXX.X

  // Timer bounds enforced by the editor (tenths of a second): 0.1 s .. 9999.9 s
  static constexpr unsigned long SLAVE_TIMER_MIN_TENTHS = Fog::kMinTenths;
  static constexpr unsigned long SLAVE_TIMER_MAX_TENTHS = Fog::kMaxTenths;

  // A timer is "stale" on the display when its last report is older than this.
  static constexpr unsigned long RSSI_STALE_MS = Fog::kOnlineWindowMs;

  // Buttons / menu timing
  static constexpr unsigned long BUTTON_DEBOUNCE_MS = 25;
  static constexpr unsigned long BUTTON_LONG_PRESS_MS = 800;  // '#': long press opens the menu
  static constexpr unsigned long MENU_PROGRESS_START_MS = 500;
  static constexpr unsigned long MENU_PROGRESS_FULL_MS  = 3000;
  static constexpr unsigned long EDIT_INITIAL_DELAY_MS   = 400;
  static constexpr unsigned long EDIT_REPEAT_INTERVAL_MS = 120;
  static constexpr unsigned long STAR_HOLD_THRESHOLD_MS = 350;   // '*' click vs hold (hold = fog while held)
  static constexpr unsigned long STAR_DEEP_SLEEP_HOLD_MS = 3000; // '*' very long hold = sleep now
  static constexpr unsigned long MENU_RESULT_TIMEOUT_MS = 5000;
  static constexpr unsigned long FORCE_LEASE_MS = 1000;          // dead-man lease of a held '*'
  static constexpr unsigned long FORCE_REFRESH_MS = 300;

  // COMM LED (external, on D3)
  static constexpr unsigned long COMM_LED_MIN_ON_MS = 8;
  static constexpr bool COMM_LED_ACTIVE_HIGH = true;

  // Layout of the main screen
  static constexpr int UI_TIMER_START_X   = 26;
  static constexpr int UI_TIMER_ROW_Y_OFF = 0;
  static constexpr int UI_TIMER_ROW_Y_ON  = 24;
  static constexpr int UI_TIMER_ROW_Y_TIME= 48;
  static constexpr int UI_DIGIT_WIDTH     = 11;
  static constexpr int UI_LABEL_GAP_X     = 10;
  static constexpr int UI_STATE_CHAR_Y    = 48;
  static constexpr int UI_BATT_X = 0, UI_BATT_Y = 0, UI_BATT_W = 16, UI_BATT_H = 8, UI_BATT_TERM_W = 2, UI_BATT_TERM_H = 4;
  static constexpr int UI_PBAR_X = 0, UI_PBAR_Y = 48, UI_PBAR_W = 128, UI_PBAR_H = 16;

  // Charger status pins (see Pins.h)
  static constexpr bool CHARGER_CHG_ACTIVE_HIGH = true;
  static constexpr bool CHARGER_PWR_ACTIVE_HIGH = true;

  inline const char* VERSION() { return "FogRemote v" FOG_FW_STRING_REMOTE; }
}
