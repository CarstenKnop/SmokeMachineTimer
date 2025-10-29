# ESP32 FogMachineTimer & FogMachineRemoteControl Specification

## Overview

---

## FogMachineTimer (Slave Device)
- Always powered on; responds to remote signals at all times.
- It'll boot up and use the last stored timer values. The timer will allways run and update the output!
- Implements timer logic from the original SmokeMachineTimer project.
- Default timer values:
  - Toff: 10 sec
- Remote can change Ton and Toff.
- Remote can read/write identification name.

- Stores Ton, Toff, and name in EEPROM.
- Values are only stored after remote editing is complete and verified.
- Stores the active ESP-NOW channel in EEPROM with magic/version guards. Invalid or out-of-range values trigger a factory reset of the channel storage segment before normal operation resumes.

### Communication
- ESP-NOW only; responds to remote commands:
  - Pairing
  - Output override
  - Name read/write
  - RSSI query
  - Channel set/update (`SET_CHANNEL`) and status broadcasts that include the current channel
- On boot, applies the persisted channel once Wi-Fi/ESP-NOW is initialized. If the remote later issues a `SET_CHANNEL`, the timer validates the request, stores it, and immediately retunes the radio.

### General
- Typically only one remote, but multiple are supported.
- Output to show communication with slaves is on D0.
- SSD1315 OLED display (128x64) and 4 buttons (Up, Down, #, *). There are no left/right buttons; code should treat the former LEFT as '#' and RIGHT as '*'.
- Only discovers and pairs with FogMachineTimers via ESP-NOW.

### Button Mapping & Behavior
| Physical | GPIO | Name in Code | Purpose |

# ESP32 FogMachineTimer & FogMachineRemoteControl Specification

|----------|------|--------------|---------|
| Up       | 3    | BTN_UP       | Increment values / navigate up |
| Down     | 4    | BTN_DOWN     | Decrement values / navigate down |
| *        | 10   | BTN_STAR (legacy RIGHT) | Toggle output override (on master) / future quick action |

Legacy identifiers LEFT/RIGHT remain temporarily as aliases in code; new development must use HASH/STAR terminology to avoid confusion.

### UI & Menu System
- Main UI and menu system based on original SmokeMachineTimer project:
  - Progress bar (holding #) to enter menu
  - Menus for:
    - Slave device discovery & pairing (list MAC/RSSI, sorted by RSSI)
    - Renaming paired slaves (edit one char at a time, up to 15 chars, read current name from slave)
    - Selecting active slave for main screen
    - Battery calibration (menu item, guided 3-point calibration)
    - Reset Timer (factory reset the selected FogMachineTimer)
    - Reset Remote (wipe remote's paired devices and calibration, then restart)
    - Display blanking (with deep sleep)
    - Animated vertical text scrolling in menus and elswhere can be added if needed (displays like the menu animations when horizontally scrolling)
- Main screen:

### Device Management
- Name conflicts: remote updates slave name if mismatch detected.
- Renaming: updates both remote and slave EEPROM after edit and verification.
- Deleting/unpairing: removes from remote EEPROM.


### Battery Monitoring
### Power Management
- Display blanking menu sets blanking time.
- Discovery sweeps across channels 1–13 until a timer responds, ensuring the remote can locate timers left on a different channel. Once a target is selected, the remote broadcasts `SET_CHANNEL` to move the timer onto the saved channel before regular control begins.
### Communication
- ESP-NOW only; manages dynamic peer list for each transmission.

- Use OOP and SOLID principles where appropriate.
  - Menus, device management, calibration, communication, UI, etc.
- Each class in its own file; use clear, first-time-use comments.
---
## Persistence & Verification
- EEPROM writes only after edits are complete and verified (timer values, names, calibration, display blanking).

- 3-point calibration (raw ADC values for 0%, 50%, 100%) stored in remote's EEPROM.
- Remote provides guided calibration menu.
## Next Steps
- Implement missing features and menus.

- Integrate battery calibration and verification flows.
- Ensure all persistence and verification requirements are met.

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
