// Minimal Arduino shim so the real Adafruit_GFX and the firmware's screen code build on a PC.
#pragma once
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Print.h"

#define PROGMEM
#define F(s) s
#define HIGH 1
#define LOW 0
typedef bool boolean;
typedef uint8_t byte;
inline unsigned long millis() { return 0; }
inline unsigned long micros() { return 0; }

#include <string>
class String {
public:
    String() {}
    String(const char* s) : s_(s ? s : "") {}
    unsigned length() const { return static_cast<unsigned>(s_.size()); }
    const char* c_str() const { return s_.c_str(); }
    char operator[](unsigned i) const { return s_[i]; }
private:
    std::string s_;
};
#define DEG_TO_RAD 0.017453292519943295769236907684886
#define radians(deg) ((deg) * DEG_TO_RAD)
class __FlashStringHelper;
#define __builtin_bswap16(x) _byteswap_ushort(x)
