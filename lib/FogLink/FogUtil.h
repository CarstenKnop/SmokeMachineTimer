// FogUtil.h - tiny pure helpers (MAC text, safe string copy) shared by firmware and host tests.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace Fog {

// "AA:BB:CC:DD:EE:FF" (needs 18 bytes)
inline void macToStr(const uint8_t mac[6], char out[18]) {
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

inline int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Accepts "AA:BB:CC:DD:EE:FF" or "AA-BB-..." (any case). Returns false if malformed.
inline bool parseMac(const char* s, uint8_t out[6]) {
    if (!s || strlen(s) != 17) return false;
    for (int i = 0; i < 6; ++i) {
        const int hi = hexVal(s[i * 3]), lo = hexVal(s[i * 3 + 1]);
        if (hi < 0 || lo < 0) return false;
        if (i < 5 && s[i * 3 + 2] != ':' && s[i * 3 + 2] != '-') return false;
        out[i] = static_cast<uint8_t>(hi * 16 + lo);
    }
    return true;
}

inline uint32_t secondsToTenths(double seconds) {
    if (seconds < 0) return 0;
    const double t = seconds * 10.0 + 0.5;
    return t > 4e9 ? 4000000000u : static_cast<uint32_t>(t);
}

}  // namespace Fog
