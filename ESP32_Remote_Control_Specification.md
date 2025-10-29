# ESP32-C3 ESP-NOW Remote Control Specification

1. Project Overview
This document outlines the functional and technical specifications for a battery-powered, universal remote control based on the Seeed Studio XIAO ESP32C3. The remote will use a small OLED screen for its user interface and will communicate with other ESP32-based slave devices using the ESP-NOW protocol.

Key features include an intuitive pairing system for discovering and managing slave devices, dynamic peer management to overcome device limits, and aggressive power-saving through deep sleep to ensure long battery life.

2. Hardware Components
Microcontroller: Seeed Studio XIAO ESP32C3

Display: 128x64 I2C OLED Display (SSD1315/SSD1306)
- I2C pins: SDA = D4 (GPIO6), SCL = D5 (GPIO7). If SCL is needed for PWR sensing on GPIO7, firmware may prefer an alternate I2C pin pair.

User Input: 4x Momentary Push Buttons (GPIO 3, 4, 9, 10)

Power: 3.7V LiPo Battery
- Battery monitor: voltage divider (2x 200kΩ); ADC read and calibrated to percent
- Charger/Power sense: CHG on D6 (GPIO21, active‑low), PWR on D7 (GPIO7, active‑high); both with internal pulldowns

Enclosure: Custom 3D printed or off-the-shelf project box.

3. Core Functionality
3.1. Power Management & Deep Sleep
To maximize battery life, the remote blanks the OLED (sleep mode: display OFF + charge pump OFF) and then enters deep sleep. While blanked/asleep, background polling, promiscuous RSSI, and discovery are paused to avoid COMM LED blips.

Wake-up Source: Any of the 4 buttons (GPIO 3, 4, 9, 10) brings the device out of deep sleep.

Implementation (ESP32‑C3): GPIO‑based deep sleep wake is configured for low‑level on the button pins using gpio_wakeup_enable() and esp_deep_sleep_enable_gpio_wakeup(mask, ESP_GPIO_WAKEUP_GPIO_LOW).

Resume UX: After deep‑sleep wake, the splash screen is skipped for a fast resume to the prior screen.

3.2. Battery Monitoring
The remote will display the remaining battery percentage on the OLED screen.

Hardware Circuit: The battery's voltage will be measured via a voltage divider (2x 200kΩ resistors) that halves the voltage. The center tap of the divider will be connected to ADC pin A0.

Software Logic:

Read the raw value using analogReadMilliVolts(A0).

Double the read value to calculate the true battery voltage.

Convert the calculated voltage to a percentage using a predefined mapping (e.g., 4.2V = 100%, 3.2V = 0%).

4. Communications Architecture
4.1. Shared ReliableProtocol Core
- All transports (ESP-NOW and UART debug) share the `ReliableProtocol` library for framing, CRC16 integrity checks, packet IDs, retransmit windows, and transport statistics.
- `ReliableProtocol::TransportStats` tracks counters for delivered, retried, lost, and corrupt frames plus timestamps for last error; these metrics must remain thread-safe and queryable at runtime.
- Message structs (STATUS, COMMAND, DEBUG) are validated against the protocol header length and CRC before processing; mismatches increment the stats and are dropped.

4.2. ESP-NOW Transport
4.2.1. Dynamic Peer Management
To control a virtually unlimited number of slaves, the remote will not maintain a persistent list of ESP-NOW peers. Instead, it will manage peers dynamically for each transmission.

Workflow:

User selects a target device from the menu.

The remote retrieves the target's MAC address from persistent storage.

Call esp_now_add_peer() to register the device.

Call esp_now_send() to transmit the command.

On successful delivery acknowledgment (or timeout), call esp_now_del_peer() to unregister the device.

4.2.2. Message Structure
All communications use a standardized struct to ensure consistency between the remote and all slave devices. The device name is limited to 9 characters (name[10] with NUL), and STATUS includes rssiAtTimer. Messages are strictly validated by struct size.

4.2.3. Status Update Strategy (Push + Poll Hybrid)
- The remote polls the active device periodically and on user actions (e.g., after Select/Reset/Toggle/Set Timer).
- Slaves must proactively push a STATUS packet to the remote on significant state changes:
   - Output state transitions (OFF->ON or ON->OFF)
   - Timer value changes applied (SET_TIMER)
   - Name changes (SET_NAME)
- For many slaves in the environment, push traffic is bounded because only the selected/active device typically changes frequently; background devices send rare updates (at state transitions).
- The remote de-duplicates rapid identical STATUS packets within a short window (e.g., 150 ms) to avoid display thrash.

4.2.4. Channel Management
The remote owns the active ESP-NOW channel for the system. A dedicated Channel Settings menu covers persistence, scanning, and synchronization.

Responsibilities:

- Persist the selected channel in EEPROM with validation on boot (magic + version guards). Invalid or unsupported entries trigger a factory reset of the channel storage segment.
- Apply the stored channel to the Wi-Fi/ESP-NOW radio after any temporary scan completes.
- When a new channel is selected via the debug interface, send the `SET_CHANNEL` protocol message to the active timer while the remote is still tuned to the previous channel, then retune the remote once the request is queued. This ordering guarantees both endpoints remain in sync and prevents timers from timing out on the first post-hop query.
- Provide a channel survey that asynchronously scans 2.4 GHz channels 1–13, ranks them by AP count and aggregate RSSI, and exposes the ordered list to the UI. The survey parks the radio back on the stored channel when finished.
- Minimise flash wear: do not rewrite EEPROM if the chosen channel already matches the stored value.
- Notify CommManager when the stored channel changes so that discovery, pairing, and existing peers hop immediately.

UI requirements:

- Channel Settings menu displays the current stored/active channel, scan status, and ranked survey results (channel, network count, aggregate signal).
- User can trigger a scan, select a candidate, and commit. Saving updates EEPROM, reapplies the channel, and emits a protocol update to connected timers.
- While a scan is running the menu reflects the in-progress state; cancelling clears interim results.

4.2.5. Discovery Channel Sweep
Discovery/pairing now sweeps through all supported channels until a timer responds. CommManager schedules timed hops (e.g., every few hundred milliseconds) across channels 1–13 during discovery to locate timers that were left on another channel. On success, discovery locks back to the stored channel. RemoteChannelManager keeps the stored channel authoritative so subsequent operations stay consistent.

4.2.6. Channel Sync Messaging
Protocol includes `SET_CHANNEL` messages. Whenever the stored channel changes (manual selection or during pairing) the remote sends `SET_CHANNEL` to the target timer so both sides remain on the same channel. The message is dispatched before the remote retunes locally to avoid a momentary split. Timers acknowledge by switching channels and replying with STATUS.

5. Pairing and Device Management
The remote will feature a user-friendly system for discovering, pairing, and managing slave devices.

5.1. Pairing Mode
Initiation (Remote): The user will navigate to a "Pair New Device" option in the remote's menu to initiate scanning.

Discovery (Remote):

- Remote sweeps through all supported channels (1–13) during discovery. It transmits the broadcast discovery frame on the current sweep channel, listens briefly, then advances to the next channel until responses arrive or the sweep times out.
- Responses are recorded with RSSI and the channel they were heard on. When the user completes pairing, the remote immediately sends `SET_CHANNEL` with the currently stored channel so the new timer hops to the remote's channel.

Discovery Response (Slave): Slaves listen on their configured ESP-NOW channel (persisted in EEPROM). Upon receiving the broadcast discovery request they respond directly to the remote. Timers without valid channel information fall back to the default channel before responding.

5.2. Pairing UI
After scanning, the remote will display a list of all discovered, available slave devices on the OLED screen.

Sorting: The list must be sorted by RSSI in descending order, placing the slaves with the strongest signal at the top.

Display Format: Each entry in the list will show:

The slave's MAC address.

The slave's assigned name, if it has been previously paired and named. If it is a new device, this area can be blank or show "New Device".

User Action: The user can scroll through the list and select a slave to pair with. Upon selection, the remote will prompt the user to assign an optional friendly name (e.g., "Living Room Light").

5.3. Persistent Storage
All paired device information (MAC Address and assigned Name) will be stored in the ESP32's non-volatile flash memory using the Preferences.h library. This ensures the device list is retained through deep sleep and power cycles.

5.4. Device Management Menu
A dedicated "Manage Devices" menu will allow the user to:

View a list of all paired devices.

Select a device to Rename.

Select a device to Delete (un-pair).

6. User Interface (UI) Flow
[ Power On / Wake from Sleep ]
      |
      v
[ Main Screen ]
   - Shows list of paired devices
   - Shows battery percentage and charging/plug overlays (lightning when charging via CHG; plug when external power via PWR and not charging; charging takes precedence)
   - User scrolls to select a device
      |
      +-----> [ Press Control Button ] -> Send Command to Selected Device -> Go to Sleep
      |
      +-----> [ Press Menu Button ]
                  |
                  v
              [ Menu Screen ]
                 - Pair New Device
                 - Manage Devices
                 - About
                    |
                    +-----> [ Pair New Device ] -> Scan for devices -> Display sorted list -> User selects & names -> Save -> Return to Main
                    |
                    +-----> [ Manage Devices ] -> Show paired list -> User selects device -> [ Rename / Delete ] -> Save -> Return to Menu

7. Resets
- Reset Timer: Factory reset the currently selected FogMachineTimer (slave). The remote sends a FACTORY_RESET command and requests status; the remote does not restart.
- Reset Remote: Wipes the remote's paired devices and battery calibration and restarts the remote.

---

Appendix: Updates as of 2025-10-09

This section documents changes implemented since the original spec.

Protocol and Storage

- Name length reduced to 9 characters across protocol, UI, and EEPROM; wire struct uses name[10] (9 + NUL). No backward compatibility with older layouts.
- STATUS includes rssiAtTimer (int8) measured at the timer. Remote strictly validates message size and drops short packets.
- Remote maintains paired devices (MAC + name) in EEPROM; dynamic peer ensured before each send.

UI/UX

- Main screen: small 6‑bar RSSI icon for the active timer; empty state reads “No paired” on first line, “timers.” on second; UP/DOWN enters Select Active.
- If the active timer status is stale (>5s) or missing, show “Timer disconnected…” two text lines lower to avoid overlap.
- When the active timer hasn’t provided a fresh status for >5s (stale or disconnected), the main screen shows “Timer disconnected…” two text lines lower to avoid overlap.
- Edit Name: inverted cursor on current character; STAR cancels; short ‘#’ moves right and saves/exits at end; long ‘#’ moves left.
- Edit Timers: two-row digit editor (TOFF/TON); short ‘#’ next/right; long ‘#’ left; values clamped to 0.1–9999.9 s before sending to timer.
- Show RSSI: columns Name, R (remote), T (timer), aligned with dBm units; ‘N/A’ shown when timer RSSI is stale (>3s) or invalid.
- Manage Devices: activate or delete; Select Active list also available directly via UP/DOWN from main.

Pairing

- “Pair Device” now scans continuously while on the screen; paused when the display is blanked and resumes on wake; list sorted by RSSI.

Settings (Persisted via RemoteConfig)

- WiFi TX Power: 0..84 qdBm; applied at boot and when saved (esp_wifi_set_max_tx_power).
- OLED Brightness: 5..255 (minimum enforced); live preview during edit; applied every render.

Power Management

- Display blanking puts OLED into low‑power sleep (display OFF + charge pump OFF), then enters MCU deep sleep.
- Wake on any button (GPIO 3/4/9/10) using GPIO‑based deep sleep wake (low‑level on button pins). Splash is skipped after deep‑sleep wake for a fast resume.
- While blanked, periodic status polling, RSSI sniffer, and discovery are paused to avoid COMM LED blips.

Charging / Power Indicators

- CHG input (active‑low) shows a lightning overlay inside the battery icon when charging.
- PWR input (active‑high) shows a plug overlay when external power is present and not charging; charging icon takes precedence over plug.
- Pins: CHG on D6 (GPIO21), PWR on D7 (GPIO7). Configure both with internal pulldowns.

Bounds

- Remote enforces SLAVE_TIMER_MIN_TENTHS=1 (0.1s) and SLAVE_TIMER_MAX_TENTHS=99999 (9999.9s) for timer edits before sending to the timer. Minimum OLED brightness is 5 and enforced on edit/save/boot.

RSSI Robustness

- Remote‑side promiscuous RSSI collection is enabled only while on the RSSI screen.
- Timer‑side RSSI is shown unless stale/invalid; invalid spikes (e.g., -127) are ignored for display purposes.

## 8. Boot-time Firmware Update Failsafe (STAR-hold)

Purpose: Provide a safe window to flash new firmware in case normal boot gets stuck or interferes with the USB bootloader.

Trigger
- Hold the STAR (*) button during power-on (first input check at boot) to enter the update window.

Behavior
- Duration: Up to 30 seconds while STAR is held. Releasing STAR immediately exits the window and continues normal boot.
- Startup Paused: EEPROM/device/comm initialization is deferred until the window ends to minimize interference with flashing.
- Deep Sleep: Not engaged during the window; the MCU remains awake and responsive for USB flashing.
- Display UI:
   - Splash includes a hint: “Hold * for update”.
      - The update window shows:
         - Title: “Firmware Update Mode”
         - Instructions: “Connect USB and flash.”, “Hold STAR to keep window.”, “Release STAR to boot.”
         - Status line: “Waiting for update...” (on the same line where a countdown would normally appear).
- Fallback: If the OLED/I2C fails to initialize, the on-screen message may not render, but the update window logic still runs, leaving ample time to flash.

## 9. Diagnostics & Debugging

- USB CDC serial (115200 baud) hosts a `ReliableSerial` transport that reuses `ReliableProtocol` framing, providing ack/nak retries and stats identical to ESP-NOW.
- `DebugProtocol` packets encapsulate diagnostic commands (ping, fetch transport stats, enumerate paired timers, read/write timer EEPROM segments, request channel data).
- Remote firmware instantiates `DebugSerialBridge` to multiplex debug requests between the PC and timers; it forwards requests through `CommManager`, relays responses back over serial, and exposes local transport stats.
- Debug traffic over ESP-NOW uses the same `DebugProtocol` packet IDs, letting the remote query timers for their `TransportStats`, retry counts, and recent error states.
- PC tooling (`PCDiagnostics/DebugConsole`, .NET 9 WPF) consumes the serial stream, presenting live link health, channel configuration, and EEPROM editors; it must tolerate hot-plugging and reconnect automatically. Channel scans now drive dual heatmap visualisations (remote RSSI vs timer RSSI) with gradient bars. The app retries each sample until both remote and timer stats are returned, annotates channels that fail (Timeout, TransportError, NotReady), and renders per-channel debug bands showing sample counts and the most recent RSSI.
- Lost-packet/retry counters are backed by per-transport structs in RAM; the debug protocol guarantees atomic snapshots when reporting the stats to avoid torn reads.

