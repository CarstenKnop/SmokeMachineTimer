Connectivity & Menu Architecture Notes
=====================================

Connectivity Glyphs (upper-left unless overridden by status chars):

* W : STA connected + AP active (dual mode)
* S : STA connected + AP suppressed (STA-only)
* P : AP active only (no STA)

 
* X : AP suppressed and STA disconnected (transient / reconnect)
* A : AP forced always-on (user apAlwaysOn) with no STA (STA connected still shows W)

Implementation:

* `ConnectivityStatus` struct is a lightweight snapshot injected into `DisplayManager` via `setConnectivityStatus()` each loop.

 
* Display logic selects glyph only when no higher-priority menu/dirty indicators are shown.

WiFi Menu Items Modularization:

* Each WiFi-related action now lives under `src/core/MenuItems/`:
  * WiFiEnableToggle.{h,cpp}

 
  * WiFiResetConfirm.{h,cpp}
  * WiFiForgetConfirm.{h,cpp}
  * WiFiApAlwaysToggle.{h,cpp}
* `MenuSystem::getMenuName()` references their exported NAME constants.
* Actions currently transition into transient states (e.g. WIFI_ENABLE_TOGGLE) whose visual feedback can be expanded later.

Rick QR Bitmap:

 

* Stored in PROGMEM (flash) as flattened array to save ~625 bytes RAM.
* Accessed with `pgm_read_byte()` in `drawRick`.

 

Future Enhancements:


 
* Inject STA details (IP, RSSI) into `drawWiFiInfo` by extending `ConnectivityStatus` or adding a richer status provider.
* Uniform result/confirmation screens for WiFi actions.


 
---

Recent Additions (Oct 2025)
---------------------------

### Bitmap Icons

* Replaced single-character glyphs with 12x8 bitmap icons (`ICON_WIFI_*`).
* Activity marker (2x2 square at icon top-right).
* Steady when one or more AP clients are associated.
* Blinks (~300 ms interval) for ~8 s after the most recent authenticated HTTP request.

### SSE Status Extensions

* Appended fields (added in `AsyncPortalService::loop()` after JSON callback):
  * `apClients` – current associated station count.
  * `lastAuthMs` – milliseconds since last successful Basic Auth (or -1 if none yet).
  * `staRssi` – current STA RSSI (duplicated from base section if connected).

### Dashboard Enhancements

* Additions to `/dashboard`:
  * Relay toggle button (POST `/api/relayToggle`).
  * Inline timer OFF/ON update form (posts to `/control`).
  * Live class-based styling of relay button (green=ON, red=OFF).

### WiFi Page Enhancements (`/wifi`)

* Inline scan trigger/poll (`/scan?start=1` then `/scan`).
* Join actions per row:
  * Open: single button posts to `/join`.
  * Secured: password form posts `ssid` + `pass`.
* Config toggles: `/api/wifiEnabled`, `/api/apAlways`.

### AP/Config Mirrors

* `AsyncPortalService` mirrors (`wifiEnabledFlag`, `apAlwaysFlag`) via `initConfigMirror()`.

### Relay Toggle Callback

* `setRelayToggleCallback` flips relay (disabled during edit).

### Info Screen Additions

* Uptime (d h), heap, AP clients, auth activity, RSSI, version.

### Potential Follow-Ups

* Persist STA password securely (currently not stored after join).
* Add JSON field for relay manual override vs cycle state.
* Integrate CSRF token for POST endpoints if exposed beyond trusted LAN.
* Replace blinking square with inverse icon flash for more noticeable activity.
