// FogBase64.h - standard base64 (RFC 4648, '+' '/' alphabet, '=' padding) for the OTA tunnel messages.
// Header-only and free of platform includes so the host tests use the very same code as the firmware.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace Fog {

// Characters needed for n input bytes (without a terminating NUL).
constexpr size_t base64EncodedLen(size_t n) { return (n + 2) / 3 * 4; }

// Writes the text and a NUL; returns the number of characters, or 0 when out does not have room for them plus the NUL.
inline size_t base64Encode(const uint8_t* in, size_t n, char* out, size_t cap) {
    static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const size_t need = base64EncodedLen(n);
    if (cap < need + 1) return 0;
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = (uint32_t(in[i]) << 16) | (i + 1 < n ? uint32_t(in[i + 1]) << 8 : 0) | (i + 2 < n ? uint32_t(in[i + 2]) : 0);
        out[o++] = kAlphabet[(v >> 18) & 63];
        out[o++] = kAlphabet[(v >> 12) & 63];
        out[o++] = i + 1 < n ? kAlphabet[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? kAlphabet[v & 63] : '=';
    }
    out[o] = 0;
    return o;
}

// Decodes strictly (padding required, no whitespace). Returns the number of bytes, or -1 for bad input / not enough room.
inline int base64Decode(const char* in, size_t n, uint8_t* out, size_t cap) {
    if (!in || n == 0 || n % 4 != 0) return -1;
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    size_t o = 0;
    for (size_t i = 0; i < n; i += 4) {
        const bool last = i + 4 == n;
        const int a = val(in[i]), b = val(in[i + 1]);
        if (a < 0 || b < 0) return -1;
        const bool pad2 = last && in[i + 2] == '=' && in[i + 3] == '=';
        const bool pad1 = last && !pad2 && in[i + 3] == '=';
        const int c = pad2 ? 0 : val(in[i + 2]);
        const int d = (pad1 || pad2) ? 0 : val(in[i + 3]);
        if (c < 0 || d < 0) return -1;
        const uint32_t v = (uint32_t(a) << 18) | (uint32_t(b) << 12) | (uint32_t(c) << 6) | uint32_t(d);
        const size_t bytes = pad2 ? 1 : (pad1 ? 2 : 3);
        if (o + bytes > cap) return -1;
        out[o++] = static_cast<uint8_t>(v >> 16);
        if (bytes > 1) out[o++] = static_cast<uint8_t>(v >> 8);
        if (bytes > 2) out[o++] = static_cast<uint8_t>(v);
    }
    return static_cast<int>(o);
}

}  // namespace Fog
