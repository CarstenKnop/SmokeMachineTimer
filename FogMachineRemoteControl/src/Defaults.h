// ---------------------------------------------------------------------------
// Defaults.h (Remote Control)
// ---------------------------------------------------------------------------
// Central non‑persisted constants for the FogMachineRemoteControl firmware.
// Mirrors (and intentionally duplicates) the core defaults so this project can
// be built standalone by PlatformIO without needing to reference the parent
// core directory include paths. If you change a value in the core defaults,
// replicate it here (or refactor later to a shared include path).
// ---------------------------------------------------------------------------
#pragma once

namespace Defaults {
  // Display rotation: 0,1,2,3 => 0/90/180/270 degrees
  static constexpr uint8_t OLED_ROTATION = 0;

  // Hardware Pins (ESP32-C3 Seeed XIAO)
  static constexpr int RELAY_PIN = 2;      // (May be unused on remote, kept for parity)
  static constexpr int BTN_UP = 3;
  static constexpr int BTN_DOWN = 4;
  static constexpr int BTN_HASH = 9;       // '#'
  static constexpr int BTN_STAR = 10;      // '*'
  static constexpr int OLED_SDA = 6;       // Primary I2C SDA (alt pair handled elsewhere if needed)
  static constexpr int OLED_SCL = 7;       // Primary I2C SCL

  static constexpr int DIGITS = 5;                    // 5 digits (XXXX.X)

  // Bounds for the timer device (slave) as enforced by its Config/Defaults
  // FogMachineTimer/src/Defaults.h: TIMER_MIN=10 (1.0s), TIMER_MAX=60000 (6000.0s)
  // Use these to cap values in the remote's timer editor so we never send out-of-range values.
  static constexpr unsigned long SLAVE_TIMER_MIN_TENTHS = 1;      // 0.1s minimum (requested)
  static constexpr unsigned long SLAVE_TIMER_MAX_TENTHS = 99999;  // 9999.9s maximum (2.77h)
  // RSSI staleness window (ms)
  static constexpr unsigned long RSSI_STALE_MS = 3000;

  // Menu / UI timing
  // Time user must hold '#' to trigger menu entry (long-press)
  static constexpr unsigned long BUTTON_LONG_PRESS_MS = 800;
  // Grace period before showing hold progress bar (prevents flicker for short taps)
  static constexpr unsigned long MENU_HOLD_GRACE_MS = 250;
  static constexpr unsigned long MENU_PROGRESS_START_MS = 500;   // show hold progress after 0.5s
  static constexpr unsigned long MENU_PROGRESS_FULL_MS  = 3000;  // reach full bar at 3s
  static constexpr float MENU_SCROLL_SPEED = 5.0f;               // rows per second velocity

  // Editing repeat timings
  static constexpr unsigned long EDIT_INITIAL_DELAY_MS   = 400;  // first repeat after hold
  static constexpr unsigned long EDIT_REPEAT_INTERVAL_MS = 120;  // subsequent repeat cadence
  static constexpr unsigned long EDIT_BLINK_INTERVAL_MS  = 350;  // cursor blink
  static constexpr unsigned long MENU_FULL_BLINK_INTERVAL_MS = 400; // full-screen blink feedback
  // Star button behavior: threshold to distinguish click vs hold (ms)
  // STAR: threshold to distinguish click vs hold (ms)
  // Choose a value that avoids accidental holds but still feels responsive
  static constexpr unsigned long STAR_HOLD_THRESHOLD_MS = 350;

  // Post-action result/info timeout
  static constexpr unsigned long MENU_RESULT_TIMEOUT_MS = 5000;  // 5s message lifetime

  // General loop pacing (soft delay per loop if needed)
  static constexpr unsigned long LOOP_DELAY_MS = 1;

  // COMM LED: minimum on-time for visibility (ms)
  static constexpr unsigned long COMM_LED_MIN_ON_MS = 2; // make this longer (e.g., 120) if still not visible
  // COMM LED polarity (true = active HIGH, false = active LOW)
  static constexpr bool COMM_LED_ACTIVE_HIGH = true;

  // UI layout (remote)
  // Timer rows and digits
  static constexpr int UI_TIMER_START_X   = 26;  // left offset for timer digits
  static constexpr int UI_TIMER_ROW_Y_OFF = 0;   // OFF row Y
  static constexpr int UI_TIMER_ROW_Y_ON  = 24;  // ON row Y
  static constexpr int UI_TIMER_ROW_Y_TIME= 48;  // TIME row Y
  static constexpr int UI_DIGIT_WIDTH     = 11;  // pixel width per digit in text size 2
  static constexpr int UI_LABEL_GAP_X     = 10;  // gap after digits before label
  // State indicator position (the bottom '*')
  static constexpr int UI_STATE_CHAR_Y    = 48;  // keep at bottom-left; may overlap TIME label minimally
  // Battery icon position and size (top-left)
  static constexpr int UI_BATT_X          = 0;
  static constexpr int UI_BATT_Y          = 0;
  static constexpr int UI_BATT_W          = 16;  // body width (excludes terminal)
  static constexpr int UI_BATT_H          = 8;   // body height
  static constexpr int UI_BATT_TERM_W     = 2;   // terminal width
  static constexpr int UI_BATT_TERM_H     = 4;   // terminal height
  // Progress bar geometry
  static constexpr int UI_PBAR_X          = 0;
  static constexpr int UI_PBAR_Y          = 48;
  static constexpr int UI_PBAR_W          = 128;
  static constexpr int UI_PBAR_H          = 16;


  // Version tag (may be overridden later per project release scheme)
  inline const char* VERSION() { return "FogMachineTimer v1.0"; }
}

