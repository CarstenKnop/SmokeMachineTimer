# FogMachineTimer

Firmware of the timer that sits on one smoke machine (Seeed XIAO ESP32-C3 + relay). It repeats **OFF for Toff, ON for Ton**
(0.1 – 9999.9 s each, stored on the device), answers the remote over ESP-NOW and survives the machine cutting its power.

```powershell
pio run -d FogMachineTimer                 # build
pio run -d FogMachineTimer -t upload       # flash the connected board
```

| File | Role |
|------|------|
| `src/main.cpp` | start-up order (safe output first, NVS, radio, watchdog) and the loop |
| `src/EspTimerPlatform.*` | ESP32 side of `Fog::TimerPlatform`: radio queue, NVS, LED, reset statistics |
| `src/UsbConsole.*` | JSON-lines endpoint so Fog Console can configure/debug this timer over USB |
| `src/Pins.h` | wiring (relay = D3/GPIO5 active high; optional status LED) — **check against your board** |
| `../lib/FogLink/FogTimerNode.*`, `FogCycle.*` | the protocol brain and the OFF/ON state machine (tested on a PC) |

Behaviour in brief: output off at power-up → settings from NVS → radio on the stored channel within ~0.3 s → boot
announcement → cycle starts with the OFF phase. Commands are executed once even if the remote retries them.
Details: [`docs/RADIO_PROTOCOL.md`](../docs/RADIO_PROTOCOL.md), user manual: [`docs/MANUAL.md`](../docs/MANUAL.md).

USB: plain-text log lines when no PC is attached; the PC protocol ([`docs/SERIAL_PROTOCOL.md`](../docs/SERIAL_PROTOCOL.md))
when Fog Console talks to it.
