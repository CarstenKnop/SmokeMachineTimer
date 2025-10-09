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

  // Timer bounds (tenths of seconds) used when editing remote-side values
  static constexpr unsigned long TIMER_MIN = 1;       // 0000.1s (0.1s)
  static constexpr unsigned long TIMER_MAX = 99999;   // 9999.9s (~2h 46m)
  static constexpr int DIGITS = 5;                    // 5 digits (XXXX.X)

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

  // Post-action result/info timeout
  static constexpr unsigned long MENU_RESULT_TIMEOUT_MS = 5000;  // 5s message lifetime

  // General loop pacing (soft delay per loop if needed)
  static constexpr unsigned long LOOP_DELAY_MS = 10;

  // Version tag (may be overridden later per project release scheme)
  inline const char* VERSION() { return "FogMachineTimer v1.0"; }
}

