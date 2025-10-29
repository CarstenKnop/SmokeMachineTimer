# FogMachineRemoteControl — Specs & Notes

Date: 2025-10-10 (updated: Auto Off replaces Display Blanking; deep-sleep wake splash behavior simplified)

## Hardware (Seeed XIAO ESP32C3)

- MCU: ESP32-C3 (RISC-V single core @ 160 MHz)
- Flash: 4 MB, SRAM: 400 KB
- Wireless: 802.11 b/g/n WiFi, BLE 5.0
- GPIO: 11 exposed, 4 ADC inputs
- I2C: OLED on SDA=GPIO6, SCL=GPIO7 (primary), with fallback on SDA=4/SCL=5
- Buttons (active-low):
  - UP: D1 (GPIO3)
  - DOWN: D2 (GPIO4)
  - #: GPIO9
  - *: GPIO10
- Battery/Charger:
  - ADC: A0 (GPIO2) via divider
  - CHG: GPIO20 (charging status)
  - PWR: GPIO21 (power present)

Resources: Seeed wiki and datasheet links are in code comments and PlatformIO board docs.

## Deep Sleep & Wake-up (Important)

- Limitation (from Seeed XIAO ESP32C3 wiki): Only D0–D3 support deep-sleep GPIO wake.
- Our mapping means only UP (D1/GPIO3) and DOWN (D2/GPIO4) can wake the device.
- The # (GPIO9) and * (GPIO10) buttons cannot wake from deep sleep on this board.

Fix suggestions if you need #/* to wake:

- Rewire #/* to D0–D3 (any of those pins wake).
- OR hardware-wire (e.g., diode-OR) #/* onto a wake-capable pin (UP/DOWN) preserving normal operation.
- OR change button assignments so the desired wake buttons occupy D0–D3.

Implementation:

- Firmware uses GPIO wake on low with internal pull-ups held during sleep.
- Wake mask is restricted to UP and DOWN only.
- On wake, the device boots normally (no splash-skip flag persisted).

## UI/UX Specs

- Main screen: OFF/ON/TIME rows, active device marker, RSSI bars (Timer-side), battery/charge indicator.
- Battery indicator:
  - Charging: battery outline blinks filled.
  - Powered (USB present), not charging: battery is replaced by a small plug/USB glyph to avoid TIME overlap.
  - Normal: battery outline with percent fill.
- Firmware Update mode: countdown centered with +2px vertical nudge down.
- Menu system:
  - “Active Timer” selection reachable via UP/DOWN from main.
  - Context-aware return (menu vs shortcut).
  - Show RSSI screen polls visible devices every ~1s (lists Remote (R) and Timer (T) columns; T uses last status).
  - RSSI Calibration: configure Low/High thresholds in dBm (defaults -100/-80), clamped to [-120, 0] and enforced min span of 5 dB.
    - Live preview shows Timer-side RSSI and updates during calibration.
    - Values are persisted to EEPROM and applied on boot; main-screen bars honor these thresholds.
  - Auto Off: replaces “Display Blanking.” Sets display auto-off timeout (seconds), 0 = Off; persisted to EEPROM.
  - Reset: prompts “Power Cycle Remote?”; confirming reboots the remote.
  - Also available: Pair Timer, Rename Device, Edit Timers, OLED Brightness, WiFi TX Power.
  - Battery Calibration: # starts, then # steps to next point; at last point, # saves+exits; * cancels.
  - Auto Off: configurable display auto-off timeout (0=Off) replaces previous "Display Blanking" terminology; no special EEPROM wake flags are used.

## Charger signal polarity

- CHG is treated as active-HIGH (HIGH means charging) based on current hardware.
- PWR is active-HIGH (HIGH = powered present). Adjust `Defaults.h` if hardware differs.

## Build

- PlatformIO environment: `xiao_esp32c3`
- Arduino-ESP32 core; OLED via Adafruit SSD1306/GFX.

## Diagnostics & Debugging

- The USB CDC serial port exposes a reliable diagnostic channel layered on the shared `ReliableProtocol` core (CRC16 frames, ack/nak retries, transport stats).
- `ReliableSerial` wraps the hardware UART and mirrors ESP-NOW reliability semantics so higher layers can share framing, packet IDs, and retransmit logic.
- `DebugProtocol` defines command/response packets (ping, transport stats, timer state, EEPROM read/write) used by both ESP-NOW debug packets and the serial bridge.
- `DebugSerialBridge` runs in the remote firmware, forwarding requests from the PC to timers via `CommManager`, aggregating transport metrics from both the ESP-NOW and serial links, and returning structured responses.
- Transport health (lost frames, retries, uptime, last error) is tracked in `ReliableProtocol::TransportStats` and made fetchable over the debug link to support tooling.
- A companion Windows diagnostic app (`PCDiagnostics/DebugConsole`) consumes `DebugProtocol` over USB serial, providing live stats, channel data, and EEPROM tools for field analysis.

## Notes

- Only draw the battery indicator on the main screen to keep menu uncluttered.
- RSSI bars use the Timer-side RSSI (negative dBm) and map linearly to 0–6 bars over the configured Low/High range.
- Calibration and settings (TX power, OLED brightness, Auto Off, RSSI bounds) persist to EEPROM and load at boot.
