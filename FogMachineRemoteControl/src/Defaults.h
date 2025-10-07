#pragma once

namespace Defaults {
  // Buttons (active-low with INPUT_PULLUP)
  static constexpr int BTN_UP = 3;
  static constexpr int BTN_DOWN = 4;
  static constexpr int BTN_HASH = 9;
  static constexpr int BTN_STAR = 10;

  // OLED I2C pins (Seeed XIAO ESP32C3 defaults)
  static constexpr int OLED_SDA = 6;
  static constexpr int OLED_SCL = 7;
  // 0,1,2,3 for 0/90/180/270 deg rotations
  static constexpr uint8_t OLED_ROTATION = 0;
}
