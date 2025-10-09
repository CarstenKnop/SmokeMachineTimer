# ESP32 FogMachineTimer & FogMachineRemoteControl Specification

## Overview
This document merges all requirements for the FogMachineTimer (slave) and FogMachineRemoteControl (master) devices, including battery calibration, menu/UI, pairing, and persistence. It supersedes previous notes and specifications.

---

## FogMachineTimer (Slave Device)

### General
- Multiple FogMachineTimers can exist in the system.
- Output to control the fog machine is on D0.
- No display or buttons or battery (powered from the fog machine).
- Only discoverable and pairable via ESP-NOW by FogMachineRemoteControl.
- Always powered on; responds to remote signals at all times.
- It'll boot up and use the last stored timer values. The timer will allways run and update the output!

### Timer Functionality
- Implements timer logic from the original SmokeMachineTimer project.
- Default timer values:
  - Ton: 0.1 sec
  - Toff: 10 sec
- Remote can change Ton and Toff.
- Remote can override output (D0).
- Remote can reset timer to 0.0 sec or set current state.
- Remote can request RSSI value.
- Remote can read/write identification name.

### Persistence
- Stores Ton, Toff, and name in EEPROM.
- Values are only stored after remote editing is complete and verified.

### Communication
- ESP-NOW only; responds to remote commands:
  - Pairing
  - Timer value changes
  - Output override
  - State reset/set
  - Name read/write
  - RSSI query

---

## FogMachineRemoteControl (Master Device)

### General
- Typically only one remote, but multiple are supported.
- Output to show communication with slaves is on D0.
- SSD1315 OLED display (128x64) and 4 buttons (Up, Down, #, *). There are no left/right buttons; code should treat the former LEFT as '#' and RIGHT as '*'.
- Only discovers and pairs with FogMachineTimers via ESP-NOW.

### Button Mapping & Behavior
| Physical | GPIO | Name in Code | Purpose |
|----------|------|--------------|---------|
| Up       | 3    | BTN_UP       | Increment values / navigate up |
| Down     | 4    | BTN_DOWN     | Decrement values / navigate down |
| #        | 9    | BTN_HASH (legacy LEFT) | Select/advance digit, long press enters menu or exits edit (contextual) |
| *        | 10   | BTN_STAR (legacy RIGHT) | Toggle output override (on master) / future quick action |

Legacy identifiers LEFT/RIGHT remain temporarily as aliases in code; new development must use HASH/STAR terminology to avoid confusion.

### UI & Menu System
- Main UI and menu system based on original SmokeMachineTimer project:
  - Progress bar (holding #) to enter menu
  - Menus for:
    - Slave device discovery & pairing (list MAC/RSSI, sorted by RSSI)
    - Renaming paired slaves (edit one char at a time, up to 15 chars, read current name from slave)
    - Selecting active slave for main screen
    - Displaying paired slaves and RSSI (both as seen by remote and as reported by slave)
    - Battery calibration (menu item, guided 3-point calibration)
    - Reset Timer (factory reset the selected FogMachineTimer)
    - Reset Remote (wipe remote's paired devices and calibration, then restart)
    - Display blanking (with deep sleep)
    - Animated vertical text scrolling in menus and elswhere can be added if needed (displays like the menu animations when horizontally scrolling)
- Main screen:
  - Shows Ton/Toff and current time and output state from active slave (live values)
  - If no signal: shows "No signal to selected fog machine"
  - If no slave selected: shows "No fog machine selected"
  - Bottom progress bar appears while '#' is held (pre menu-entry threshold).

### Device Management
- Stores MAC and name of paired slaves in EEPROM.
- Name conflicts: remote updates slave name if mismatch detected.
- Renaming: updates both remote and slave EEPROM after edit and verification.
- Deleting/unpairing: removes from remote EEPROM.

### Battery Calibration
- Menu item for 3-point calibration:
  - Guides user to set ADC values for 0%, 50%, 100% battery states.
  - Displays battery percentage.

### Battery Monitoring
- Measures battery voltage via ADC (BAT_ADC_PIN, with voltage divider if needed - like on the homepage for the Seeed XIAO ESP32C3).
- Uses 3-point calibration (raw ADC values for 0%, 50%, 100%) for accurate percentage mapping.
- Battery calibration (3-point ADC) is stored in EEPROM.
- Battery indicator in the top left corner of the display (only 1½ char wide for it not to interfere with the timer values).

### Power Management
- Display blanking menu sets blanking time.
- When blanked, remote enters deep sleep to save power.
- Wakes on any button press.

### Communication
- ESP-NOW only; manages dynamic peer list for each transmission.
- Handles pairing, renaming, timer value changes, output override, state reset/set, RSSI queries, battery calibration.

---

## Coding Standards & Organization
- Use OOP and SOLID principles where appropriate.
- Organize code into subfolders/classes:
  - Menus, device management, calibration, communication, UI, etc.
- Each class in its own file; use clear, first-time-use comments.
- No update/patch comments; only explanatory comments for human readers.
- Code should be readable and maintainable, not look generated.

---

## Persistence & Verification
- EEPROM writes only after edits are complete and verified (timer values, names, calibration, display blanking).
- Verification: remote requests confirmation from slave after each write.
- Preserves EEPROM writes as much as possible.

---

## Battery Calibration Details
- 3-point calibration (raw ADC values for 0%, 50%, 100%) stored in remote's EEPROM.
- Remote provides guided calibration menu.
- Remote uses piecewise linear interpolation for battery percentage.
- Battery percentage shown in remote UI.

---

## Next Steps
- Refactor project source to match this specification.
- Implement missing features and menus.
- Integrate battery calibration and verification flows.
- Ensure all persistence and verification requirements are met.
- Maintain clear code structure and comments.

---

This specification merges all previous notes and requirements. Use it as the master reference for further development.

---

Implementation Plan

FogMachineTimer (Slave)

Refactor code: output on D0, no display/buttons/battery logic, always-on timer loop.
Implement: ESP-NOW protocol for pairing, timer value changes, output override, state reset/set, RSSI query, name read/write.
EEPROM: store Ton, Toff, name only after verified edit.
OOP: timer logic, comms, config in separate classes/files.
FogMachineRemoteControl (Master)

Refactor code: output on D0, SSD1315 OLED, 4 buttons, dynamic peer management.
UI/Menu: progress bar, animated vertical/horizontal scrolling, menus for pairing, renaming, selection, RSSI display, battery calibration, display blanking/deep sleep.
Device management: MAC/name EEPROM, name conflict resolution, renaming, deleting.
Battery: ADC reading, 3-point calibration, piecewise interpolation, indicator in top left (1½ char wide), calibration menu.
Power: blanking/deep sleep, wake on button.
OOP: menus, device manager, calibration, comms, UI in own files/subfolders.
Persistence & Verification

EEPROM writes only after edit/verification.
Remote requests confirmation from slave after each write.
Coding Standards

OOP, SOLID, clear comments, no update/patch comments, readable structure.
