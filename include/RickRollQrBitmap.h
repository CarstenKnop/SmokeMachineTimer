#pragma once
// RickRoll QR bitmap placeholder. Will be populated after serial dump.
// Define QR_RICK_BITMAP to force using this static bitmap instead of generated encoder.
// Each entry is 0=white,1=black. Modules are 2x2 pixels when rendered.
// For now allocate for up to Version 2 (25x25).

#include <stdint.h>

#include <pgmspace.h>
#define RICK_QR_SIZE 25
// Stored in flash (PROGMEM) as a flat row-major array length RICK_QR_SIZE*RICK_QR_SIZE
extern const uint8_t RICK_QR_BITMAP[] PROGMEM;
