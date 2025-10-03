# FogMachineTimer – ESP32-C3 Relay Cycle Controller

## Overview

FogMachineTimer is firmware for the Seeed XIAO ESP32-C3 that drives a relay in repeating OFF / ON time cycles with a clear OLED interface and four‑button control. Suitable for pulsing fog/fog machines, ventilation, dosing pumps, or other cyclic loads (ensure proper electrical ratings).

Core capabilities:

- OFF / ON durations 0000.1 – 9999.9 s (0.1 s resolution)
- Live countdown & phase indication
- Non-blocking digit-by-digit editing while running
- Configurable inactivity screensaver
- EEPROM persistence (timers, screensaver, Wi‑Fi flags & creds)
- On‑demand captive portal + OTA + remote control + STA scan/join

## Features

| Area | Feature |
|------|---------|
| Timing | Independent OFF/ON phase (0.1–9999.9 s) |
| Editing | Digit-by-digit live edit without stopping cycle |
| Display | 128×64 SSD1306/SSD1315 OLED, progress bar, status glyphs |
| Input | Four buttons: Up, Down, # (Select), * (Cancel/Exit) |
| Power Saving | Configurable inactivity screensaver (OFF or 10–990 s) |
| Persistence | EEPROM-backed settings (timers, saver, Wi‑Fi, AP flag, STA SSID) |
| Networking | Async SoftAP captive portal, remote control, OTA, JSON APIs |
| STA Mode | Async scan/join, auto‑reconnect, AP suppression on stable STA |
| AP Mgmt | Idle auto-stop + optional Always-On override (persisted) |
| Indicators | Transient NET/STA flashes + connectivity glyph (W/S/P/X) |
| Security | Basic Auth (separate control vs OTA credentials) |
| Provisioning | Dynamic Wi‑Fi QR & captive probe endpoints |

## Hardware Requirements

- Seeed XIAO ESP32-C3 module
- Relay module (properly rated; add flyback protection if needed)
- 128×64 I2C SSD1306/SSD1315 OLED display
- 4 × momentary buttons (Up, Down, #, *) with pull-ups
- Stable power supply sized for relay + load

> Follow electrical safety for mains applications (enclosure, fusing, clearance).

## Button Functions

| Button | Run (Short) | Edit Mode | Long Hold (#) |
|--------|-------------|-----------|---------------|
| Up     | Enter edit / Nudge up | Increment digit | — |
| Down   | Enter edit / Nudge down | Decrement digit | — |
| #      | Advance digit / confirm | Save digit & advance | Open menu (hold) |
| *      | Cancel edit / exit | Save & exit early (if allowed) | Exit menu |

## Editing Workflow

1. Press Up or Down to enter edit (OFF time first).
2. Adjust digit with Up/Down; `#` advances, wraps across OFF then ON digits.
3. `*` exits early saving current staged values.
4. Exiting normally saves changes if modified; cancelled edits discard.
5. Values clamped to safe min/max (0.1 – 9999.9 s).

## Screensaver

- OFF or 10–990 s (10 s increments).
- Blanks OLED; first wake press is consumed.
- Remaining seconds reported via JSON.

## EEPROM Layout (Extended)

```text
[0..3]  offTime (uint32_t, tenths)
[4..7]  onTime  (uint32_t, tenths)
[8..9]  screensaverDelaySec (uint16_t)
[10]   wifiEnabled (uint8_t)
[11..42] staSsid (32 bytes)
[43..74] staPass (32 bytes, placeholder not yet persisted securely)
[75]   apAlwaysOn (uint8_t)
```

Writes are debounced / conditional to limit flash wear.

## Networking & Portal

An asynchronous SoftAP + DNS captive portal (with probe endpoints) provides:

- Remote timer control (`/control`)
- OTA firmware upload (`/update`)
- JSON status (`/values`, `/api/timers`)
- STA scan/join (`/scan`, `/join`)
- Dynamic Wi‑Fi QR code (menu `QR`)

### Portal Lifecycle

| Trigger | Action |
|---------|--------|
| Enter Wi‑Fi/QR/Rick menu | Start AP/portal (if `wifiEnabled`) |
| 30 s after leaving portal menus | Stop AP (unless Always-On) |
| STA stable 5 s (connected) | Suppress AP (unless Always-On) |
| STA disconnect/fail (while suppressed) | Re-enable AP |
| AP Always-On flag set | Keep AP running (no idle stop/suppression) |

### AP / STA Indicators (OLED)

Glyph precedence (shown when not overridden by menu/dirty markers):

| Glyph | Meaning |
|-------|---------|
| W | STA connected + AP active (dual mode) |
| S | STA connected + AP suppressed (power save) |
| P | AP only (no STA) |
| X | AP suppressed & no STA (reconnect window) |

Transient tags top-right: `NET` (remote timer change), `STA` (new station connect).

### Authentication

HTTP Basic Auth (separate control vs OTA credentials). Default (change!):

```
admin / admin
```

Set in `main.cpp` via `setControlAuth()` / `setOtaAuth()`.

### Endpoints

| Method | Path | Purpose | Auth |
|--------|------|---------|------|
| GET | `/` | Landing + OTA form link | Control |
| POST | `/update` | OTA firmware upload | OTA |
| GET | `/values` | JSON status | Control |
| GET | `/api/timers` | Same JSON status | Control |
| GET | `/control` | Timer update form | Control |
| POST | `/control` | Apply OFF/ON (debounced) | Control |
| GET | `/join` | STA join UI (scan/status) | Control |
| POST | `/join` | Begin STA connect | Control |
| GET | `/scan` | JSON scan results / trigger | Control |
| Probes | `/generate_204` `/gen_204` `/hotspot-detect.html` `/ncsi.txt` `/connecttest.txt` | Captive triggers | None |
| GET | `/health` | Lightweight metrics (uptime, heap, AP/STA state) | Control |
| GET | `/events` | Server-Sent Events (live status JSON 1 Hz) | Control |

### JSON Status Example

```json
{
  "off":500,
  "on":120,
  "currentElapsed":37,
  "relay":1,
  "phase":"ON",
  "saverRemain":42,
  "wifiEnabled":1,
  "apIp":"192.168.4.1",
  "apActive":1,
  "apSuppressed":0,
  "staStatus":"CONNECTED",
  "staIp":"10.1.2.55",
  "staRssi":-54,
  "staConnected":1,
  "version":"FogMachineTimer v1.0"
}
```

Field additions:

- `apActive` / `apSuppressed` – AP state & suppression reason
- `staConnected` – Boolean convenience mirror of `staStatus`==`CONNECTED`

### Live Status Stream (SSE)

`/events` provides Server-Sent Events. Each second (while at least one client is connected) the device emits an event:

```
event: status
data: {"off":500,"on":120,...}

```

Reconnect logic is handled automatically by EventSource in browsers:

```html
<script>
const es = new EventSource('/events');
es.addEventListener('status', e => {
  const obj = JSON.parse(e.data);
  console.log('Status', obj);
});
</script>
```

### Health Metrics

`/health` returns a compact JSON snapshot:

```json
{
  "uptimeMs":123456,
  "freeHeap":123456,
  "apActive":1,
  "apSuppressed":0,
  "loopsPerSec":48,
  "remoteUpdates":3,
  "staState":"CONNECTED",
  "staRssi":-54
}
```

Use this for monitoring / watchdog dashboards without parsing the full status payload.

### Remote Timer Control

`POST /control` (form fields `off`, `on` in tenths). Debounce: 2 s since last apply.

Rules: range 1–99999; blocked during local edit; unchanged values accepted (no EEPROM write). Success triggers `NET` flash.

### STA Scan / Join

`/scan?start=1` begins async scan. `/join` lists results. Join attempt times out after 15 s; on success: AP suppression after stable window (unless Always-On) and `STA` flash.

Credentials: Currently only SSID stored (password persistence deferred).

### Dynamic Wi‑Fi QR

Menu `QR` renders Wi‑Fi join payload: `WIFI:T:<WPA|nopass>;S:<ssid>;P:<pass>;;` with adaptive quiet zone.

### Captive Probe Responses

Implements Android / Apple / Windows probe endpoints for automatic portal pop-up.

## Build / PlatformIO

`platformio.ini` snippet:

```ini
[env:seeed_xiao_esp32c3]
platform = espressif32
board = seeed_xiao_esp32c3
framework = arduino
lib_deps =
  adafruit/Adafruit SSD1306
  adafruit/Adafruit GFX Library
  https://github.com/me-no-dev/AsyncTCP.git
  https://github.com/me-no-dev/ESPAsyncWebServer.git
```

## Component Summary

| Module | Purpose |
|--------|---------|
| `Defaults.h` | Pins, constants, version string |
| `Config.h/.cpp` | EEPROM persistence + validation |
| `Buttons.h` | Edge detect / state polling |
| `TimerController.h` | OFF/ON timing + edit buffer |
| `Screensaver.h` | Inactivity blanking logic |
| `MenuSystem.h` | Menu navigation & transient states |
| `DisplayManager.*` | Rendering, QR, connectivity glyphs |
| `AsyncPortalService.h` | Portal, AP/STA state machine, routes |
| `main.cpp` | Integration / loop orchestration |

## Safety & Reliability

- EEPROM writes minimized (only on change + debounced remote updates)
- AP suppression lowers RF noise, power usage, and attack surface
- Basic Auth default creds MUST be changed for production
- Consider enabling a watchdog for unattended deployments

## Future Enhancements

- Secure STA password storage
- Token or digest auth / credential rotation
- WebSocket push updates
- MQTT / Home automation integration
- Timing profiles / presets
- Diagnostics & metrics endpoint

## License

(Insert license details here)

---

Feedback & contributions welcome.
| Button | Short Press (Normal) | Edit Mode | Long Hold (#) |
|--------|----------------------|-----------|---------------|
| Up     | Enter edit (if idle) / Increment digit | Increment digit / navigate | — |
| Down   | Enter edit (if idle) / Decrement digit | Decrement digit / navigate | — |

Rules:

- Range 1–99999 tenths each
- Disallowed during local edit (`EDIT MODE`)
- No-op updates return success with no change message

Visual Feedback: On a successful remote update the OLED flashes `NET` for ~1.5 s.
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

## Wi‑Fi / QR / Captive Portal / OTA / Remote Control

The firmware includes an on-demand SoftAP captive portal + OTA & remote control web UI using `AsyncTCP` + `ESPAsyncWebServer`. It only runs while you are in Wi‑Fi related menu screens (or shortly after) to save power and noise.

1. Hold `#` until the progress bar fills to open the menu.
2. Choose one of: `WiFi` (info), `QR` (dynamic Wi‑Fi QR), or `Rick` (fun static QR). These states start the SoftAP.
3. The SoftAP SSID is `FogTimerAP` (open by default unless you add a password in code).
4. Leaving those screens begins a ~30 s idle timer; when it expires the portal/AP stop (unless active operations like OTA are in progress).
5. A separate menu item `WiFi En` toggles Wi‑Fi subsystem enable/disable (persisted). If disabled, entering Wi‑Fi screens will not start the AP.

### Basic Auth Protection

```

Set in `main.cpp` via:

```cpp
asyncPortal.setAuth("admin","admin");
```

You can store or derive these from EEPROM / chip ID for better security (future enhancement).

### Endpoints Overview

| Method | Path | Purpose | Auth |
|--------|------|---------|------|
| GET | `/` | Portal landing + info | Yes |
| GET | `/update` | OTA upload form | Yes |
| POST | `/update` | OTA firmware upload | Yes |
| GET | `/values` | JSON status (timers + runtime) | Yes |
| GET | `/api/timers` | Alias JSON (off/on/phase etc.) | Yes |
| POST | `/control` | Remote timer update (JSON) | Yes |
| GET | `/join` | STA join form (scan, status) | Yes |
| POST | `/join` | Start STA connect (SSID/pass) | Yes |
| GET | `/scan` | JSON scan results (`?start` to trigger) | Yes |
| GET | `/generate_204` | Captive probe (Android) | No (204) |
| GET | `/gen_204` | Captive probe alt | No (204) |
| GET | `/hotspot-detect.html` | Captive probe (Apple) | No (200) |
| GET | `/ncsi.txt` | Captive probe (Windows) | No (200) |
| GET | `/connecttest.txt` | Captive probe (Windows legacy) | No (200) |

Unmatched hostnames are redirected to the portal via DNS + a global 302 handler (except the probe paths above which receive minimal responses to trigger captive assistants).

### JSON Status / Timers Schema

Current combined JSON (includes preliminary STA fields):

```json
{
  "off":500,
  "on":120,
  "currentElapsed":37,
  "relay":1,
  "phase":"ON",
  "saverRemain":42,
  "wifiEnabled":1,
  "apIp":"192.168.4.1",
  "staStatus":"IDLE",
  "version":"FogMachineTimer v1.0"
}
```

Field Notes:

- `off`, `on` – Phase durations (tenths of a second)
- `currentElapsed` – Tenths elapsed in active phase
- `relay` – 1 if relay energized
- `phase` – `ON` or `OFF`
- `saverRemain` – Seconds until screensaver activates (0 if disabled or already blank)
- `wifiEnabled` – Persistent toggle storing whether Wi‑Fi features are allowed
- `apIp` – SoftAP IP (when AP active)
- `staStatus` – One of `IDLE|SCANNING|CONNECTING|CONNECTED|FAILED`
- `staIp` – Station IP (present only when connected)
- `staRssi` – RSSI dBm (present only when connected)
- `version` – Firmware version string

Planned additions once station join is implemented: `staStatus`, `staIp`, `staRssi`.

### Remote Timer Control (`POST /control`)

Send a JSON body with one or both of `off` and `on` (tenths). Example:

```http
POST /control HTTP/1.1
Authorization: Basic ...
Content-Type: application/json

{"off":600,"on":150}
```

Validation Rules:

- Each value (if present) must be 1–99999 tenths (0.1–9999.9 s)
- Missing fields = leave unchanged
- Reject if currently in local edit mode (device has priority)
- On success: persists to EEPROM (only if changed) and restarts cycle with new timings

Responses:

- `200` JSON: `{ "status":"ok", "off":<newOff>, "on":<newOn> }`
- `400` JSON: `{ "error":"<reason>" }` (bad range, no fields, edit lock)

The OLED shows a transient `NET` indicator for ~1.5 s after a remote update is applied.

### Dynamic Wi‑Fi QR

The `QR` menu renders a Wi‑Fi join QR with adaptive quiet zone to fit the 64‑pixel height. Payload pattern:

```text
WIFI:T:WPA;S:FogTimerAP;P:<password>;;
```

If password is empty, `T:nopass` is used. Adjust quiet zone or shorten SSID if scanning issues occur.

### Captive Portal Probe Endpoints

Implemented minimalist responses to improve automatic OS portal pop-up:

| Path | Code | Notes |
|------|------|-------|
| `/generate_204` | 204 | Android standard |
| `/gen_204` | 204 | Some OEM variants |
| `/hotspot-detect.html` | 200 | Apple Captive Network Assistant |
| `/ncsi.txt` | 200 | Windows NCSI text |
| `/connecttest.txt` | 200 | Legacy Windows / Kindle |

### Wi‑Fi Enable / Reset

- `WiFi En` menu toggles a persistent flag (`wifiEnabled`). When off, the device won't start AP/portal.
- `WiFi Rst` clears stored (future) station credentials and may disable STA attempts.

### Future Station Mode (Planned)

Implemented initial version:

1. Visit `/join?scan=1` or click Scan to start async network scan.
2. When results populate, select a network (and provide password if not open).
3. POST `/join` initiates a non-blocking connection attempt (15 s timeout).
4. While connecting, `staStatus` = `CONNECTING`; success -> `CONNECTED` (with `staIp`, `staRssi`). Failure -> `FAILED`.
5. OLED flashes `STA` for ~1.5 s on first successful connection.
6. Credentials (currently only SSID) are persisted automatically upon success; password persistence planned (needs secure handling).

`/scan` JSON examples:

Scanning:

```json
{"status":"scanning"}
```

Results:

```json
{"status":"done","results":[{"ssid":"Net1","rssi":-52,"open":0},{"ssid":"Guest","rssi":-70,"open":1}],"staIp":"0.0.0.0"}
```

Additional background: [QR Code Onboarding Guide](QRcode_Implementation.md)

## Build / Platform

- PlatformIO (Arduino framework)
- Key Libraries: Adafruit_SSD1306, Adafruit_GFX, EEPROM, AsyncTCP, ESPAsyncWebServer

Typical `platformio.ini` excerpt:

```ini
[env:seeed_xiao_esp32c3]
platform = espressif32
board = seeed_xiao_esp32c3
framework = arduino
lib_deps =
  adafruit/Adafruit SSD1306
  adafruit/Adafruit GFX Library
  https://github.com/me-no-dev/AsyncTCP.git
  https://github.com/me-no-dev/ESPAsyncWebServer.git
```

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
