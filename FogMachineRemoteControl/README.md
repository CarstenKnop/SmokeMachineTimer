# FogMachineRemoteControl

Firmware of the handheld remote (Seeed XIAO ESP32-C3, SSD1306 OLED 128×64, four buttons UP / DOWN / # / \*, LiPo battery).
It controls up to ten timers, sleeps when idle and bridges the PC tool over USB.

```powershell
pio run -d FogMachineRemoteControl             # build
pio run -d FogMachineRemoteControl -t upload   # flash
powershell -File tools/oled-preview/run.ps1    # render all screens to docs/images/oled (needs one build first)
```

Fog Console can flash it too (page *Firmware*, [`docs/FIRMWARE_UPDATE.md`](../docs/FIRMWARE_UPDATE.md)). It recognises the image by the identity string
`FOGFW|remote|<version>|` that `src/main.cpp` compiles in (`FOG_FW_IDENT`, [`lib/FogLink/FogIdent.h`](../lib/FogLink/FogIdent.h)).

| Folder | Role |
|--------|------|
| `src/main.cpp` | boot, main loop, input handling, deep sleep |
| `src/link/` | `Fleet` (radio pump + `Fog::RemoteLink` + paired list), `ChannelSurvey` |
| `src/pc/PcBridge.*` | USB JSON protocol ([`docs/SERIAL_PROTOCOL.md`](../docs/SERIAL_PROTOCOL.md)) |
| `src/menu/` | `MenuState` (pure data) and `MenuSystem` (navigation, editors, actions) |
| `src/ui/` | `Screens.*` (every screen as a pure function of `UiModel` — also rendered on the PC for the manual), `DisplayManager` (hardware), `ButtonInput` (time-based debounce) |
| `src/config/`, `src/battery/` | NVS settings, battery voltage/percent |
| `../lib/FogLink/` | protocol, radio, storage, USB link (shared with the timer) |

**Firmware update of the timers:** the remote is the radio bridge for it. Fog Console sends the image over USB, the remote forwards the frames to one paired timer
(`otaTx` / `otaRx` / `otaHold` in [`docs/SERIAL_PROTOCOL.md`](../docs/SERIAL_PROTOCOL.md), design in [`docs/OTA_UPDATE.md`](../docs/OTA_UPDATE.md)). The remote keeps no part of the
image; while an update runs it stays on the home channel and refuses discovery, channel moves, link tests and recovery sweeps (a toast says *Update running*).
The remote itself is updated over USB. Both firmwares use the two-slot layout `../partitions/fog_ota.csv`.

Controls and screens are explained with pictures in the [user manual](../docs/MANUAL.md).
