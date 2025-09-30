# SmokeMachineTimer (Modular Refactor)

## Overview

Firmware for Seeed XIAO ESP32-C3 controlling a relay with configurable OFF/ON cycle times (0000.1–9999.9 s), OLED UI, menu system, and screensaver. Refactored into clean subsystems with clear separation of persistence, UI, timing, and hardware abstraction.

## Architecture

Component responsibilities:

- `Defaults.h`: All non-persisted constants (pins, timing, UI speeds, version banner). Single source of truth.
- `Config.h`: EEPROM-backed persisted values (`offTime`, `onTime` in tenths, `screensaverDelaySec` in seconds). Validation + conditional saves.
- `Buttons.h`: Polling + edge detection for Up / Down / # / * (active-low inputs with pull-ups).
- `TimerController.h`: Relay state machine (alternating OFF/ON periods) + digit-by-digit edit buffers and dirty tracking.
- `Screensaver.h`: Inactivity tracking using absolute deadline (`nextBlankAt`) for precise blank timing; wake & consume logic.
- `MenuSystem.h`: Long-hold (#) progress bar → selection list → saver edit (0↔990 rollover) or temporary result screen.
- `DisplayManager.h`: All rendering (timers, menu scrolling, progress, saver editor) isolated from logic modules.
- `main.cpp`: Orchestrates modules — gathers button input, updates systems, renders frame, dispatches persistence saves.

## Timing Model

Timers stored as tenths of a second. `TimerController::tick(now)` accumulates real elapsed milliseconds and advances the timer counter once per 100 ms step for accurate duration rather than per-loop increments. Relay toggles after OFF or ON target counts reached.

## Screensaver

- Configurable 0 (OFF) or 10–990 s in 10 s steps.
- Precise blank schedule via `nextBlankAt` updated on user activity or configuration change.
- Wakes on any button press; first post-wake input is consumed (debounce UX).

## Menu Interaction

1. Hold `#` — progress bar appears after 0.5 s; fills by 3 s.
2. Release after full fill → scrolling menu.
3. `Up/Down` navigate (smooth interpolated scroll).
4. `#` selects (Saver opens editor; others show result screen for 5 s).
5. `*` exits menu / cancels saver edit.

### Saver Editor

- Values: OFF or 10–990 s (10 s increments).
- Rollover: 990→OFF (up), OFF→990 (down).
- `#` saves; `*` cancels.

## Editing Cycle Times

Press Up or Down while running enters edit mode. Each digit (OFF then ON) is edited sequentially; `#` advances digit, `*` saves & exits early. Values constrained to 0000.1–9999.9 s.

## Persistence

EEPROM layout:

```text
[0..3]  offTime (uint32_t, tenths)
[4..7]  onTime  (uint32_t, tenths)
[8..9]  screensaverDelaySec (uint16_t)
```

Writes occur only when values actually change (timers on edit exit, screensaver on save).

## Build / Platform

- PlatformIO (Arduino framework) for Seeed XIAO ESP32-C3.
- Libraries: Adafruit_SSD1306, Adafruit_GFX, EEPROM.

## Extending

Add new persisted settings through Config (update struct + layout comment). Add new menu entries in `MenuSystem::MENU_NAMES` and handle their selection in `handleSelect`.

## Future Enhancements (Optional)

- Add unit display (e.g., blinking colon or progress bar for cycle phase).
- Introduce logging or serial debug flag in Defaults.
- Provide calibration UI for relay cycle alignment.

## License

(Add license details here if desired.)
