// ---------------------------------------------------------------------------
// Defaults.h (Timer Unit)
// ---------------------------------------------------------------------------
// Expanded to align with the core / remote control defaults so both projects
// share a consistent naming/style. Original macro values preserved via macro
// aliases for backward compatibility with existing source files.
// NOTE: TIMER_MIN here intentionally differs from the remote (1.0s vs 0.1s)
// because the Timer device enforces a higher practical lower bound.
// ---------------------------------------------------------------------------
#pragma once

namespace Defaults {
	static constexpr uint8_t DEFAULT_CHANNEL = 1;
	// Hardware Pins (ESP32-C3 Seeed XIAO) - replicate for symmetry; adjust if hardware diverges
	static constexpr int RELAY_PIN = 2;
	static constexpr int BTN_UP = 3;
	static constexpr int BTN_DOWN = 4;
	static constexpr int BTN_HASH = 9;
	static constexpr int BTN_STAR = 10;
	static constexpr int OLED_SDA = 6;  // Timer unit may not populate display; harmless if unused
	static constexpr int OLED_SCL = 7;

	// Battery ADC pin
	static constexpr int BAT_ADC_PIN = 34;

	// Timing bounds (tenths of seconds) specific to Timer unit behavior
	static constexpr unsigned long TIMER_MIN = 10;     // 1.0s (original macro value)
	static constexpr unsigned long TIMER_MAX = 60000;  // 6000.0s (100 minutes)
	static constexpr int DIGITS = 5;                   // 5 digits (XXXX.X)

	// (Optional) UI timing constants included for parity; device may ignore most
	static constexpr unsigned long MENU_PROGRESS_START_MS = 500;
	static constexpr unsigned long MENU_PROGRESS_FULL_MS  = 3000;
	static constexpr float MENU_SCROLL_SPEED = 5.0f;
	static constexpr unsigned long EDIT_INITIAL_DELAY_MS   = 400;
	static constexpr unsigned long EDIT_REPEAT_INTERVAL_MS = 120;
	static constexpr unsigned long EDIT_BLINK_INTERVAL_MS  = 350;
	static constexpr unsigned long MENU_FULL_BLINK_INTERVAL_MS = 400;
	static constexpr unsigned long MENU_RESULT_TIMEOUT_MS = 5000;
	static constexpr unsigned long LOOP_DELAY_MS = 10;

	inline const char* VERSION() { return "FogMachineTimer v1.0"; }
}

// ---------------------------------------------------------------------------
// Backward compatibility macros (legacy code reference layer)
// ---------------------------------------------------------------------------
#define BAT_ADC_PIN Defaults::BAT_ADC_PIN
#define DIGITS Defaults::DIGITS
#define TIMER_MIN Defaults::TIMER_MIN
#define TIMER_MAX Defaults::TIMER_MAX

