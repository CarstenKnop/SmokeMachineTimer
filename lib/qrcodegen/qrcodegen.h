// Minimal subset of Nayuki QR Code generator (C) Project Nayuki; license: MIT.
// Trimmed for small embedded use: only byte mode, automatic version selection.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Error correction level.
typedef enum { QR_ECC_LOW=0, QR_ECC_MEDIUM, QR_ECC_QUARTILE, QR_ECC_HIGH } qrcodegen_Ecc;

// Public API
bool qrcodegen_encodeText(const char *text, uint8_t *tempBuffer, uint8_t *qrBuffer,
                          qrcodegen_Ecc ecl, int minVersion, int maxVersion,
                          int mask, bool boostEcl);

int  qrcodegen_getSize(const uint8_t *qr);
bool qrcodegen_getModule(const uint8_t *qr, int x, int y);

// Expanded sizing for fuller implementation (support versions up to 10)
#define QRCODEGEN_MAX_VERSION 10
#define QRCODEGEN_QR_BUFFER_LEN 600   // sufficient for versions <=10 using bit-packed storage
#define QRCODEGEN_TEMP_BUFFER_LEN 800 // workspace for interleaving & EC generation

#ifdef __cplusplus
}
#endif
