# FogMachineTimer – ESP32-C3 Relay Cycle Controller

> **⚠️ WORK IN PROGRESS — CURRENTLY NOT WORKING**
>
> This project is under active development and should be considered **experimental and non-functional at this stage**. The firmware is **not currently working as intended** and has not been fully tested or validated on hardware.
>
> Features and components described below may be incomplete, partially implemented, untested, or currently broken. The documentation describes the intended design and development direction rather than a guaranteed working feature set.
>
> **Do not use this firmware for unattended, production, or safety-critical applications.**
>
> Hardware connected to mains voltage or other hazardous loads must not be operated with this unfinished firmware.

## Overview

FogMachineTimer is intended to be firmware for the Seeed XIAO ESP32-C3 that controls a relay in repeating OFF / ON time cycles, with an OLED interface and four-button control.
  
The long-term goal is to support cyclic control of fog machines, ventilation equipment, dosing pumps, and other appropriately rated loads.

### Current Status

**Overall status: 🔴 NOT WORKING / DEVELOPMENT**

The project currently contains a mixture of planned functionality, partially implemented functionality, and code that has not yet been sufficiently tested.

The following areas are still subject to debugging and validation:

- Basic OFF / ON timer operation
- Relay control
- Button input and editing
- OLED display and UI
- Screensaver behaviour
- EEPROM persistence
- Wi-Fi / SoftAP operation
- Captive portal
- STA scanning and connection
- Remote timer control
- OTA updates
- QR-code functionality
- AP / STA state management
- Authentication
- SSE / live status reporting

Until these functions have been tested on the target hardware and confirmed to work reliably, they should be regarded as **WIP / unverified**.

### Development Goal

The intended final system will provide:

- Configurable OFF / ON cycle times
- Live countdown and phase indication
- Non-blocking timer editing
- OLED status display
- Configurable screensaver
- Persistent configuration
- Optional Wi-Fi connectivity
- Local web interface
- Remote timer control
- OTA firmware updates
- Wi-Fi provisioning and QR-code support

These are **development targets**, not a claim that all functionality is currently operational.
