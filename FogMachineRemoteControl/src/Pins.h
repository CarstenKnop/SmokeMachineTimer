#ifndef PINS_H
#define PINS_H

#include <Arduino.h>

// Explicit raw GPIO mappings (avoid ambiguity between Dn macros and raw numbers)
// NOTE: The working reference project used raw 6/7 for I2C. We mirror that here.

// Communication / status output (spec: remote output on D0 logical). If the board's D0 differs
// from raw GPIO 0 update COMM_OUT_GPIO accordingly.
static constexpr int COMM_OUT_GPIO = 0;   // adjust if board variant differs
static constexpr int BAT_ADC_GPIO  = 1;   // battery divider input

// Primary I2C GPIO (from working project):
static constexpr int OLED_SDA_GPIO_PRIMARY = 6;
static constexpr int OLED_SCL_GPIO_PRIMARY = 7;
// Alternate fallback (if board was wired differently or silk mislabeled)
static constexpr int OLED_SDA_GPIO_ALT     = 4;
static constexpr int OLED_SCL_GPIO_ALT     = 5;
static constexpr int OLED_RST_GPIO         = -1; // not wired

// Buttons (active-low). Original project used Up, Down, #, * (no left/right).
static constexpr int BUTTON_UP_GPIO    = 3;   // D1 silk
static constexpr int BUTTON_DOWN_GPIO  = 4;   // D2 silk (also alt SDA if reused)
static constexpr int BUTTON_HASH_GPIO  = 9;   // '#' button (was LEFT)
static constexpr int BUTTON_STAR_GPIO  = 10;  // '*' button (was RIGHT)

// Provide legacy macro-style names used elsewhere if any
#define COMM_OUT_PIN    COMM_OUT_GPIO
#define BAT_ADC_PIN     BAT_ADC_GPIO
#define OLED_SDA_PIN    OLED_SDA_GPIO_PRIMARY
#define OLED_SCL_PIN    OLED_SCL_GPIO_PRIMARY
#define OLED_SDA_PIN_ALT OLED_SDA_GPIO_ALT
#define OLED_SCL_PIN_ALT OLED_SCL_GPIO_ALT
#define BUTTON_UP_PIN    BUTTON_UP_GPIO
#define BUTTON_DOWN_PIN  BUTTON_DOWN_GPIO
#define BUTTON_LEFT_PIN  BUTTON_HASH_GPIO   // legacy alias
#define BUTTON_RIGHT_PIN BUTTON_STAR_GPIO   // legacy alias
#define BUTTON_HASH_PIN  BUTTON_HASH_GPIO
#define BUTTON_STAR_PIN  BUTTON_STAR_GPIO

#define MENU_ENTER_BUTTON_PIN BUTTON_HASH_PIN  // Long-press '#' enters menu

#endif // PINS_H
