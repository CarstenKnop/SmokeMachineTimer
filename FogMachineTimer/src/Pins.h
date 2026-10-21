// Pins.h - FogMachineTimer wiring (Seeed XIAO ESP32-C3). Override any of these from platformio.ini build_flags.
#pragma once

#include <Arduino.h>

// Output that switches the smoke machine's relay (XIAO "D3" = GPIO5), active high.
// !! Add an external pull-down (e.g. 10 kOhm to GND for an active-high relay input): from reset until setup() runs the pin
// !! floats for ~100-400 ms and no firmware can hold it. Verify this against your relay board. The original SmokeMachineTimer project used D0 (GPIO2) for the relay;
// !! the FogMachineTimer firmware has always driven D3. Nothing else is ever driven by this firmware by default.
#ifndef FOG_OUTPUT_GPIO
#define FOG_OUTPUT_GPIO 5
#endif
#ifndef FOG_OUTPUT_ACTIVE_HIGH
#define FOG_OUTPUT_ACTIVE_HIGH 1
#endif

// Optional status LED (active high). -1 = none. Used for the "Identify" function and a link heartbeat.
// To use D0 (GPIO2) for it:  -DFOG_STATUS_LED_GPIO=2   (only if nothing else is wired to that pin!)
#ifndef FOG_STATUS_LED_GPIO
#define FOG_STATUS_LED_GPIO -1
#endif
