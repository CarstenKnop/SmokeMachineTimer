ESP32-C3 ESP-NOW Remote Control Specification
1. Project Overview
This document outlines the functional and technical specifications for a battery-powered, universal remote control based on the Seeed Studio XIAO ESP32C3. The remote will use a small OLED screen for its user interface and will communicate with other ESP32-based slave devices using the ESP-NOW protocol.

Key features include an intuitive pairing system for discovering and managing slave devices, dynamic peer management to overcome device limits, and aggressive power-saving through deep sleep to ensure long battery life.

2. Hardware Components
Microcontroller: Seeed Studio XIAO ESP32C3

Display: 128x64 I2C OLED Display (SSD1315 or similar)

User Input: 4x Momentary Push Buttons. Note: Some display modules come with integrated buttons which can simplify wiring.

Power:

3.7V LiPo Battery

Voltage Divider for battery monitoring: 2x 200kΩ resistors.

Enclosure: Custom 3D printed or off-the-shelf project box.

3. Core Functionality
3.1. Power Management & Deep Sleep
To maximize battery life, the remote will operate in deep sleep as its default state.

Wake-up Source: The device will wake from deep sleep when any of the 4 primary control buttons are pressed.

Wake-up Pins: The buttons must be connected to RTC-capable GPIO pins. The recommended pins are D0, D1, D2, and D3.

Implementation: The esp_sleep_enable_ext1_wakeup() function will be used to configure the device to wake when any of the selected pins are triggered.

Operation: Upon waking, the remote performs its required function (e.g., sends a command), and then automatically returns to deep sleep after a short period of inactivity (e.g., 10 seconds).

3.2. Battery Monitoring
The remote will display the remaining battery percentage on the OLED screen.

Hardware Circuit: The battery's voltage will be measured via a voltage divider (2x 200kΩ resistors) that halves the voltage. The center tap of the divider will be connected to ADC pin A0.

Software Logic:

Read the raw value using analogReadMilliVolts(A0).

Double the read value to calculate the true battery voltage.

Convert the calculated voltage to a percentage using a predefined mapping (e.g., 4.2V = 100%, 3.2V = 0%).

4. ESP-NOW Communication
4.1. Dynamic Peer Management
To control a virtually unlimited number of slaves, the remote will not maintain a persistent list of ESP-NOW peers. Instead, it will manage peers dynamically for each transmission.

Workflow:

User selects a target device from the menu.

The remote retrieves the target's MAC address from persistent storage.

Call esp_now_add_peer() to register the device.

Call esp_now_send() to transmit the command.

On successful delivery acknowledgment (or timeout), call esp_now_del_peer() to unregister the device.

4.2. Message Structure
All communications will use a standardized struct to ensure consistency between the remote and all slave devices.

5. Pairing and Device Management
The remote will feature a user-friendly system for discovering, pairing, and managing slave devices.

5.1. Pairing Mode
Initiation (Remote): The user will navigate to a "Pair New Device" option in the remote's menu to initiate scanning.

Discovery (Remote):

The remote broadcasts a special "discovery" message to the universal broadcast address (FF:FF:FF:FF:FF:FF).

It enters a listening state for 10-15 seconds, collecting responses.

For each response, it records the slave's MAC address and the RSSI (signal strength).

Discovery Response (Slave): Slaves must have a "pairing mode" (e.g., triggered on first boot or by a button press) where they listen for the discovery broadcast and reply directly to the remote with their MAC address.

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
   - Shows battery percentage
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

