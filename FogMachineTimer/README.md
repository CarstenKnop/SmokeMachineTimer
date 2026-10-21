# FogMachineTimer

Firmware of the timer that sits on one smoke machine (Seeed XIAO ESP32-C3 + relay). It repeats **OFF for Toff, ON for Ton**
(0.1 – 9999.9 s each, stored on the device), answers the remote over ESP-NOW and survives the machine cutting its power.

```powershell
pio run -d FogMachineTimer                 # build
pio run -d FogMachineTimer -t upload       # flash the connected board
```

Fog Console can flash it too (page *Firmware*, [`docs/FIRMWARE_UPDATE.md`](../docs/FIRMWARE_UPDATE.md)). It recognises the image by the identity string
`FOGFW|timer|<version>|` that `src/main.cpp` compiles in (`FOG_FW_IDENT`, [`lib/FogLink/FogIdent.h`](../lib/FogLink/FogIdent.h)).

| File | Role |
|------|------|
| `src/main.cpp` | start-up order (safe output first, NVS, radio, watchdog) and the loop |
| `src/EspTimerPlatform.*` | ESP32 side of `Fog::TimerPlatform`: radio queue, NVS, LED, reset statistics |
| `src/UsbConsole.*` | JSON-lines endpoint so Fog Console can configure/debug this timer over USB |
| `src/Pins.h` | wiring (relay = D3/GPIO5 active high; optional status LED) — **check against your board** |
| `src/EspOtaTarget.*` | the update slot as `Fog::OtaTarget`: erase / write / read / activate, and confirming a fresh image |
| `../lib/FogLink/FogTimerNode.*`, `FogCycle.*` | the protocol brain and the OFF/ON state machine (tested on a PC) |
| `../lib/FogLink/FogOta.*`, `FogSha256.*` | firmware update receiver, SHA-256 / HMAC (tested on a PC) |

Behaviour in brief: output off at power-up → settings from NVS → radio on the stored channel within ~0.3 s → boot
announcement → cycle starts with the OFF phase. Commands are executed once even if the remote retries them.
Details: [`docs/RADIO_PROTOCOL.md`](../docs/RADIO_PROTOCOL.md), user manual: [`docs/MANUAL.md`](../docs/MANUAL.md).

**Over-the-air update:** the timer receives its next firmware through a remote that is connected to Fog Console ([`docs/OTA_UPDATE.md`](../docs/OTA_UPDATE.md)).
`lib/FogLink/FogOta.*` is the receiver (platform independent, host-tested), `src/EspOtaTarget.*` maps it to the IDF (`esp_partition`, `esp_ota_ops`): it writes the
application slot that is *not* running, the boot loader switches slot after a SHA-256 check, and a new image that resets within 6 s of its first start is rolled
back (`main.cpp`: `verifyRollbackLater()` + `EspOtaTarget::confirmRunningImage()`). Needs the two-slot layout `../partitions/fog_ota.csv`, which
`platformio.ini` already selects: the first firmware with it goes on over USB, every later one over the air. The transfer is authenticated with an *OTA key*
(default `FogLink-OTA-v1`; set your own with `-DFOG_OTA_KEY=\"...\"` in `build_flags` and the same text in Fog Console's settings).

USB: plain-text log lines when no PC is attached; the PC protocol ([`docs/SERIAL_PROTOCOL.md`](../docs/SERIAL_PROTOCOL.md))
when Fog Console talks to it.
