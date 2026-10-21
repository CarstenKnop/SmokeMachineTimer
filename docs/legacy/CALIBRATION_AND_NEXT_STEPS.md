# FogMachineTimer — Calibration, UI & Next Steps

This document summarizes the current implementation, the 3-point battery calibration feature, how to use the remote service UI to calibrate a timer, build/test commands, and recommended next steps so you can continue later.

## Quick status

- Two PlatformIO projects exist in this workspace:
  - `FogMachineTimer/` (ESP-NOW slave, timer logic, stores name & timers in EEPROM)
  - `FogMachineRemoteControl/` (ESP-NOW master, UI, pairs with timers, edit/save flows)
- Recent additions:
  - 3-point battery calibration support (raw ADC points) in the ESP-NOW protocol.
  - Timer now accepts `CALIB` messages and persists three raw ADC values in EEPROM.
  - Remote has a simple calibration editor (service mode) that sends `CALIB` to a selected timer.
  - Both projects compiled successfully after the changes.

## Files added/changed (important ones)

- FogMachineTimer/src/
  - `ESPNowProtocol.h` — added `MsgType::CALIB` and `uint16_t calibAdc[3]` to `ESPNowMsg`.
  - `Config.h` — `Config::Values` now contains `uint16_t calibAdc[3]` and `saveCalibration()`.
  - `main.cpp` — handles `CALIB` messages (saves to EEPROM) and includes calibration data in STATUS replies. Also added `getBatteryPercent()` (currently linear mapping; to be replaced with calibrated mapping).

- FogMachineRemoteControl/src/
  - `ESPNowProtocol.h` — mirrored protocol change (CALIB + calibAdc[3]).
  - `ESPNowMaster.h` / `ESPNowMaster.cpp` — `PeerInfo` now contains `calibAdc[3]`; `sendCalib()` added; onRecv copies calibration values into `PeerInfo`.
  - `UI.h` / `UI.cpp` — simple service-mode CALIB editor (enter with long-press star on the LIST screen). Up/Down adjust current raw ADC point, star cycles index, hash confirms and sends calibration to selected peer.
  - `DisplayManager.cpp` — shows calibration values for a peer in the top-right (raw ADC values, simple text display).

## Calibration design (current)

- Representation: three raw ADC sample points (uint16_t 0..4095) are stored per device in the timer's EEPROM.
- Protocol: `ESPNowMsg` contains `uint16_t calibAdc[3]`. A `CALIB` message sends these raw ADC points from the remote to a timer; the timer saves them and echoes them back in STATUS replies.
- Intended meaning (recommended): the three points correspond to known percentage anchors such as 0% / 50% / 100% battery states. The timer should use piecewise linear interpolation between these anchors to map measured ADC to percentage.

  Example mapping (not yet implemented):
  - calibAdc[0] => ADC reading representing 0% (battery empty)
  - calibAdc[1] => ADC reading representing 50%
  - calibAdc[2] => ADC reading representing 100%

- Current implementation: `getBatteryPercent()` uses a simple linear voltage conversion (raw -> 0..3.3V -> linear map 3.0..4.2V). This should be replaced with the piecewise interpolation using stored `calibAdc[]` for accurate per-device calibration.

## How to calibrate with the current UI

1. Power on both the remote (FogMachineRemoteControl) and the timer (FogMachineTimer).
2. Pair the devices using the remote UI (short star -> quick pair or other pairing flows).
3. In the remote LIST screen, long-press the STAR button (> ~1.2s) to enter the CALIB service editor for the selected peer.
4. Controls in CALIB service mode:
   - Up / Down: increase / decrease the current calibration ADC point (step = 16 raw counts).
   - STAR (edge): switch to next calibration index (0 -> 1 -> 2 -> 0).
   - HASH (edge): confirm and send the current three raw ADC values to the selected timer (remote sends `CALIB` message). This also triggers a save on the timer (stored in EEPROM).
5. The remote will show the peer's calibration values in the top-right of the display (raw ADC numbers). The timer will echo the saved calibration back in STATUS replies.

Notes:
- The remote editor edits raw ADC counts (0..4095). To perform a guided calibration you should measure/capture raw ADC values for target battery states using a multimeter, or implement a guided procedure (see Next Steps).

## How the timer should map measured ADC -> percentage (recommended implementation)

Replace the current `getBatteryPercent()` with a piecewise linear interpolation that uses the three saved ADC anchors:
- If ADC <= calib[0] -> 0%
- If ADC >= calib[2] -> 100%
- Else, if ADC <= calib[1], interpolate between calib[0] (0%) and calib[1] (50%) to get 0..50%
- Else, interpolate between calib[1] (50%) and calib[2] (100%) to get 50..100%

Add a small moving-average filter (e.g., 4-8 samples) to smooth readings before interpolation.

## Build commands (PowerShell)

Build FogMachineTimer:

```powershell
& 'C:\Users\Carsten.Knop\.platformio\penv\Scripts\pio.exe' run -d 'z:\source\ESP32\SmokeMachineTimer\FogMachineTimer'
```

Build FogMachineRemoteControl:

```powershell
& 'C:\Users\Carsten.Knop\.platformio\penv\Scripts\pio.exe' run -d 'z:\source\ESP32\SmokeMachineTimer\FogMachineRemoteControl'
```

Both projects have compiled successfully after the recent changes on your machine.

## Testing / verification checklist

- [ ] Pair remote and timer and verify the timer responds to PING with PONG and includes `batteryPercent`.
- [ ] Enter CALIB editor on the remote and send three calibration values; confirm the timer stores them (timer will echo them back in STATUS reply).
- [ ] Implement the piecewise interpolation on the timer and verify that the remote shows a sensible battery percentage that reflects the calibration.
- [ ] Perform smoothing: verify percent is stable over several readings.

## Recommended next steps (pick one or more)

1. Replace `getBatteryPercent()` with the piecewise interpolation + moving average using `config.get().calibAdc` (high priority).
2. Extend the remote CALIB editor with a guided graph UI (draw a small graph of ADC vs % and let the user drag points) and add textual instructions for measuring 0/50/100.
3. Persist `calibAdc` on the remote in `persistPeers()` so the remote remembers calibrations offline (optional; timer is authoritative).
4. Improve UX: indicate selected peer when entering CALIB (display selection highlight), add a "discard" action and confirm dialogs for saving calibration.
5. Add a quick calibration helper: remote can sample battery ADC via temporary PINGs and store min/median/max samples to auto-fill calib points (useful when a multimeter isn't available).
6. Add message ACKs/retries for critical operations (SAVE/CALIB) to ensure reliability in noisy environments.

## Where to change code to implement next steps

- Replace mapping: `FogMachineTimer/src/main.cpp` (function `getBatteryPercent()`) and use stored `Config::Values::calibAdc` (see `FogMachineTimer/src/Config.h`).
- Calibration UI improvements: `FogMachineRemoteControl/src/UI.cpp`, `DisplayManager.cpp`.
- Persist calib on remote: `FogMachineRemoteControl/src/ESPNowMaster.cpp` (extend `persistPeers()`/`loadPeers()` to save/load `calibAdc`).

## Quick TODO (short)

- [ ] Implement piecewise ADC->% mapping and smoothing on timer.
- [ ] Add guided calibration UI (graph) on remote.
- [ ] Persist remote-side calib values if desired.

---

If you want, I can implement item 1 (piecewise interpolation and smoothing) next and run a compile to verify. Or I can add a guided graphical calibration editor in the remote UI. Tell me which you'd like me to do next and I'll implement it and run the build.
