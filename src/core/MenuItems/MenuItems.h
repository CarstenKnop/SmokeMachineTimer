#pragma once
#include <Arduino.h>

namespace MenuItems {
  enum Code : uint8_t {
    SAVER = 0,
    MENU2,
    MENU3,
    MENU4,
    MENU5,
    MENU6,
    MENU7,
    MENU8,
    MENU9,
    HELP,
    COUNT
  };

  extern const char* const NAMES[COUNT];

  inline const char* name(Code c) { return (c < COUNT) ? NAMES[c] : ""; }
}
