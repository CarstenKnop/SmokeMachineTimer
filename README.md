# SmokeMachineTimer – ESP32-C3 Relay Cycle Controller

## Overview

SmokeMachineTimer is firmware for the Seeed XIAO ESP32-C3 that drives a relay in repeating OFF / ON time cycles with a clear on-device OLED interface and simple four‑button control. It is designed for applications such as pulsing a smoke/fog machine, timed ventilation, dosing pumps, or any low‑duty cyclic load (ensure relay and wiring are rated for your load).

Core capabilities:
- Adjustable OFF and ON durations from 0000.1 s to 9999.9 s (tenths resolution)
- Live countdown display for current phase
- Intuitive button-driven editing (digit-by-digit) while running
- Configurable inactivity screensaver (to protect OLED)
- Persistent storage of timing parameters in EEPROM
- Optional captive-portal / QR-based Wi‑Fi onboarding guide included (see below)

## Features

| Area | Feature |
|------|---------|
| Timing | Independent OFF and ON phase duration (0.1–9999.9 s) |
| Editing | Digit-by-digit edit flow without stopping the cycle |
| Display | 128×64 SSD1306 OLED status + progress indications |
| Input | Four buttons: Up, Down, # (Select), * (Cancel/Exit) |
| Power Saving | Inactivity screensaver (OFF or 10–990 s) |
| Persistence | EEPROM-backed retention of cycle and screensaver settings |
| Safety | Relay control separated from logic timing; predictable transitions |
| Provisioning (Doc) | Separate guide for QR code Wi‑Fi captive portal onboarding |

## Hardware Requirements

- Seeed XIAO ESP32-C3 module
- Relay module (transistor / opto-isolated if switching higher current)
- 128×64 I2C SSD1306 OLED display
- 4 × momentary buttons (Up, Down, #, *) with pull-ups (internal or external)
- Stable 5 V (or appropriate) supply sized for relay coil + device load

> Always follow electrical safety practices. For mains voltage loads, use properly rated relays, clearance distances, enclosures, and fusing.

## Button Functions (Runtime)

| Button | Short Press (Normal) | Edit Mode | Long Hold (#) |
|--------|----------------------|-----------|---------------|
| Up     | Enter edit (if idle) / Increment digit | Increment digit / navigate | — |
| Down   | Enter edit (if idle) / Decrement digit | Decrement digit / navigate | — |
| #      | Advance to next digit / confirm | Save value & move to next | Hold to open menu |
| *      | Exit edit / cancel changes | Save & exit early (when specified) | Exit menu |

## Cycle Editing Workflow

1. Press Up or Down to enter edit mode for the OFF time.
2. Adjust each digit using Up/Down; press # to move to the next digit.
3. After OFF time digits complete, ON time digits follow.
4. Press * early to accept all current edits and resume.
5. Bounds are clamped to 0000.1 – 9999.9 seconds.

The relay automatically transitions between OFF and ON phases using precise elapsed time accumulation (not loop iteration counts) for accuracy.

## Screensaver

- Configurable delay: OFF or 10–990 s in 10 s steps.
- When active, display blanks; any button wakes it.
- First button press after wake is consumed to avoid accidental edits.

## Data Persistence

Values stored in EEPROM only when changed to minimize flash wear:
```text
[0..3]  offTime (uint32_t, tenths)
[4..7]  onTime  (uint32_t, tenths)
[8..9]  screensaverDelaySec (uint16_t)
```

## Build / Platform

- PlatformIO (Arduino framework)
- Key Libraries: Adafruit_SSD1306, Adafruit_GFX, EEPROM

Typical PlatformIO `platformio.ini` snippet (example):
```ini
[env:xiao_esp32c3]
platform = espressif32
board = seeed_xiao_esp32c3
framework = arduino
monitor_speed = 115200
lib_deps =
  adafruit/Adafruit SSD1306
  adafruit/Adafruit GFX Library
```

## QR Code Onboarding / Captive Portal (Documentation)

This repository includes a dedicated guide describing how to provision an ESP32 device using a single Wi‑Fi QR code plus a captive portal (SoftAP + DNS hijack + local web UI). This is optional reference material for building a user-friendly configuration interface.

Read the full guide here: [QR Code Onboarding Guide](QRcode_Implementation.md)

The guide explains:
- Wi‑Fi QR encoding and escaping
- ESP32 SoftAP + wildcard DNS + captive portal strategies
- Cross‑platform portal behavior (Android / iOS / Windows)
- Security hardening (ephemeral credentials, OTA considerations)
- Async web server variant & REST endpoints
- Test matrix, troubleshooting, production checklist

## Component Summary

- `Defaults.h` – Central constants: pins, timings, UI parameters, version text
- `Config.h` – EEPROM-backed settings and validation
- `Buttons.h` – Poll & edge-detect for four inputs (active-low)
- `TimerController.h` – OFF/ON cycle state machine + edit buffer management
- `Screensaver.h` – Inactivity timing and wake handling
- `MenuSystem.h` – Long-hold (#) triggered menu and item selection
- `DisplayManager.h` – All OLED rendering routines
- `main.cpp` – Integration: input → logic update → display → persistence

## Extending the Firmware

Ideas:
- Add cloud reporting (MQTT) after local validation
- Integrate sensor feedback to auto-adjust ON duration
- Add multi-profile timing presets
- Include RTC or NTP sync (if enabling station mode)
- Provide serial/USB command interface for automation

## Safety & Reliability Notes

- Debounce and edit consumption logic reduce unintended timing edits
- EEPROM writes minimized to prolong flash endurance
- Ensure adequate relay coil flyback protection if using a discrete transistor driver
- Consider watchdog timer activation for long unattended operation

## Future Enhancements (Planned / Optional)

- Visual progress bar or phase animation
- Logging / diagnostic serial toggle
- Calibration or test pulse mode

## License

(Insert license details here)

---

If you have suggestions or encounter issues, feel free to open an issue or submit a pull request.
