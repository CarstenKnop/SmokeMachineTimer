#pragma once

// ============================================================================
// Defaults.h
// ----------------------------------------------------------------------------
// Central authority for ALL non-persisted constants and tunable timings.
// Only user-configured, persisted values live in Config (offTime/onTime in tenths,
// screensaverDelaySec in seconds). Everything else (pins, UI timings, limits,
// animation speeds, version tag) is defined here so behavior can be audited or
// modified without hunting through modules.
//
// Usage pattern:
//   #include "Defaults.h" in any module needing these values.
//   Never duplicate literals ("magic numbers") elsewhere; prefer a Defaults::
//   constant. This keeps device behavior consistent and simplifies tuning.
//
// If a new constant is introduced during development, add it here with a
// concise comment. Persisted defaults (factory settings) belong instead in
// Config::Values initialization.
// ============================================================================

namespace Defaults {
  // Hardware Pins
  static constexpr int RELAY_PIN = 2;
  static constexpr int BTN_UP = 3;
  static constexpr int BTN_DOWN = 4;
  static constexpr int BTN_HASH = 9;
  static constexpr int BTN_STAR = 10;
  static constexpr int OLED_SDA = 6;
  static constexpr int OLED_SCL = 7;

  // Timer constraints (stored as tenths of seconds)
  static constexpr unsigned long TIMER_MIN = 1;      // 0000.1s
  static constexpr unsigned long TIMER_MAX = 99999;  // 9999.9s
  static constexpr int DIGITS = 5;

  // Menu timing
  static constexpr unsigned long MENU_PROGRESS_START_MS = 500;  // show bar after 0.5s
  static constexpr unsigned long MENU_PROGRESS_FULL_MS  = 3000; // full at 3s
  static constexpr float MENU_SCROLL_SPEED = 5.0f;              // rows per second

  // Editing repeat timings
  static constexpr unsigned long EDIT_INITIAL_DELAY_MS = 400;
  static constexpr unsigned long EDIT_REPEAT_INTERVAL_MS = 120;
  static constexpr unsigned long EDIT_BLINK_INTERVAL_MS = 350;
  static constexpr unsigned long MENU_FULL_BLINK_INTERVAL_MS = 400;

  // Menu result timeout
  static constexpr unsigned long MENU_RESULT_TIMEOUT_MS = 5000;

  // Screensaver blink / general delays
  static constexpr unsigned long LOOP_DELAY_MS = 10;

  // Version string
  inline const char* VERSION() { return "FogMachineTimer v1.0"; }
}
