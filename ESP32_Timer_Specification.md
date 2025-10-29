# ESP32 FogMachineTimer & FogMachineRemoteControl Specification

## Overview

- System comprises the FogMachineTimer (always-powered slave) and the FogMachineRemoteControl (battery-powered master).
- Timer boots with the last stored TON/TOFF values and immediately resumes timing/output logic derived from the original SmokeMachineTimer project.
- Remote can read/write timer configuration (TON, TOFF, device name) and persists paired device metadata in its own EEPROM.

## FogMachineTimer (Slave Device)

- Always powered; responds to remote commands at all times without requiring a wake cycle.
- Default timer values: TON follows project defaults, TOFF defaults to 10 seconds until remote updates them.
- Stores TON, TOFF, and friendly name in EEPROM only after remote edits are verified.
- Persists the active ESP-NOW channel with magic/version guards; invalid entries trigger a scoped channel-storage reset before normal operation resumes.

### Communication

- Uses the shared `ReliableProtocol` core for framing (CRC16 verification, packet IDs, ack/nak exchanges, retry windows) so ESP-NOW and debug transports expose identical semantics and statistics.
- Primary control path is ESP-NOW via `ReliableEspNow`; the timer responds to pairing, output override, name read/write, RSSI queries, and `SET_CHANNEL` updates and broadcasts status packets that include the current channel.
- On boot, applies the persisted channel once Wi-Fi/ESP-NOW is initialized. Subsequent `SET_CHANNEL` requests are validated, persisted, and applied immediately. When the remote issues a channel hop via the debug bridge it sends `SET_CHANNEL` before retuning locally; the timer must process and ack this message promptly so the follow-up stats queries succeed on the new channel without transient timeouts.
- `ReliableProtocol::TransportStats` instances back link-health counters (delivered, retried, lost, corrupt) and are queryable via debug requests from the remote.
- All inbound packets are size-checked against the protocol header and CRC before handling; failures increment stats and are discarded.

### General Hardware Notes

- Typically only one remote is active, but multiple remotes are supported by honoring the reliable protocol semantics.
- Output indicator for slave communication remains on GPIO D0.
- SSD1315 OLED display (128×64) and four buttons (UP, DOWN, #, *) follow the original SmokeMachineTimer layout; legacy LEFT/RIGHT identifiers remain as aliases but new work should use HASH/STAR terminology.
- Discovery/pairing is limited to FogMachineTimers through ESP-NOW.

### Button Mapping & Behavior

| Physical | GPIO | Name in Code | Purpose |
|----------|------|--------------|---------|
| Up       | 3    | BTN_UP       | Increment values / navigate up |
| Down     | 4    | BTN_DOWN     | Decrement values / navigate down |
| #        | 9    | BTN_HASH (legacy LEFT) | Confirm actions / progress bar hold |
| *        | 10   | BTN_STAR (legacy RIGHT) | Toggle output override / quick actions |

### UI & Menu System

- Main UI mirrors the original project with immediate timer status, progress bars, and remote-triggered overrides.
- Menus provide: slave discovery & pairing (MAC/RSSI sorted), rename flow (15 characters, remote pulls current name), active slave selection, battery calibration, timer factory reset, remote factory reset, and display auto-off/deep sleep configuration.
- Menu animations (vertical text scroll) remain optional niceties carried over from the previous implementation.

### Device Management

- Remote resolves name conflicts by updating the timer when mismatches appear.
- Renaming updates both timer and remote EEPROM post-verification.
- Deleting/unpairing removes the timer from the remote’s EEPROM list.

### Power & Battery

- Display auto-off menu configures blanking/deep sleep timing; remote resumes discovery after wake.
- Remote guides three-point battery calibration (0%, 50%, 100%) and persists data in its EEPROM for display interpolation.
- Discovery sweeps channels 1–13 until a timer responds; once selected, the remote broadcasts `SET_CHANNEL` so the timer joins the stored channel before standard control traffic resumes.

### Diagnostics & Debugging

- Timer consumes `DebugProtocol` packets forwarded by the remote (over ESP-NOW) to expose transport stats, recent error codes, and timer configuration snapshots.
- EEPROM read/write helpers are limited to whitelisted regions (channel storage, timer settings) and reuse existing verification steps before commits.
- Transport counters are captured atomically before serialization to prevent races with ISR-driven comm loops.
- Debug responses return current TON/TOFF values, override state, RSSI data, and channel information so the PC diagnostics tool can present real-time telemetry. The diagnostics client now retries each per-channel poll until both remote and timer payloads arrive; timers should answer `GetTimerStats` quickly even during scan sweeps so retries converge without exceeding timeout limits.

## Persistence & Verification

- EEPROM writes only occur after edits are complete and verified (timer values, names, calibration data, display blanking).
- Three-point battery calibration stores raw ADC values for 0%, 50%, and 100% charge.
- Remote provides guided calibration menus and requests confirmation from the timer after each persisted write.

## Next Steps

- Implement remaining menu flows and ensure UI parity with the updated specification.
- Integrate battery calibration verification paths where placeholders remain.
- Confirm all persistence logic adheres to verification requirements and avoids unnecessary flash wear.

## Implementation Plan

### FogMachineTimer (Slave)

- Refactor code to keep output on D0, drop unused display/buttons/battery logic from legacy timer, and maintain an always-on timer loop.
- Implement reliable ESP-NOW protocol for pairing, timer value changes, output overrides, state resets, RSSI queries, name read/write, and `DebugProtocol` handling.
- Isolate timer logic, communications, and configuration management into dedicated classes/files (OOP/SOLID).

### FogMachineRemoteControl (Master)

- Maintain SSD1315 OLED handling, four-button input, and dynamic peer management.
- Provide UI/menu flows for pairing, renaming, selection, RSSI display, battery calibration, display auto-off/deep sleep, and debug tooling integration.
- Manage device metadata (MAC/name EEPROM), handle name conflicts, support renaming/deleting, and integrate debug serial bridging for diagnostics.

## Coding Standards

- Apply OOP and SOLID principles; keep menus, device management, calibration, communication, and UI logic modular.
- Place each class in its own file with concise, first-use comments when the intent is non-obvious.
- Avoid patchwork comments; maintain readable, self-explanatory structure that favors reuse across transports and tooling.
