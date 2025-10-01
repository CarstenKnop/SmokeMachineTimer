#pragma once
#include <Arduino.h>
// Minimal placeholder for QR code generation.
// For now, we won't implement a full QR; we'll approximate by hashing the URL and drawing pattern blocks.

struct QRCodeRender {
  static constexpr int SIZE = 25; // 25x25 pseudo matrix
  bool bits[SIZE][SIZE];
};

class QRCodeGenerator {
public:
  static void generate(const String& text, QRCodeRender& out) {
    // Simple pseudo-random fill based on FNV-1a hash sequence (NOT a real QR code!)
    uint32_t hash=2166136261u; for (size_t i=0;i<text.length();++i){ hash ^= (uint8_t)text[i]; hash *= 16777619u; }
    for (int y=0;y<QRCodeRender::SIZE;++y) {
      for (int x=0;x<QRCodeRender::SIZE;++x) {
        // Evolve hash
        hash ^= (x + 131*y + 0x9e3779b9);
        hash *= 16777619u;
        out.bits[y][x] = ((hash >> 17) & 1);
      }
    }
  }
};
