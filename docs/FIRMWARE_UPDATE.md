# Updating the firmware over USB

*How Fog Console puts new firmware on a remote or a timer over a USB cable — and what happens behind the button.*

> **Timers inside a machine usually have no accessible USB port.** They are updated **over the air through the remote** instead, see
> [`OTA_UPDATE.md`](OTA_UPDATE.md). The USB route described here is for the remote, for bench work, for the very first installation of the OTA-capable firmware
> (a timer needs the two-slot layout once) and for recovery.

![The Firmware page](../PCDiagnostics/docs/firmware-dark.png)

## Contents

1. [The short way](#1-the-short-way)
2. [Where the firmware comes from](#2-where-the-firmware-comes-from)
3. [What gets written](#3-what-gets-written)
4. [Safety and recovery](#4-safety-and-recovery)
5. [How it works](#5-how-it-works)
6. [Without Fog Console](#6-without-fog-console)
7. [Troubleshooting](#7-troubleshooting)
8. [What has been verified](#8-what-has-been-verified)

> **Terminology:** “fog machine” (fogger) and “smoke machine” are both common. “Fog” is the US trade name, “smoke machine” the UK/Danish everyday term. Neither makes real smoke. I use “smoke machine” for the machine and “fog” for the burst.

---

## 1. The short way

1. Plug the device in with a **USB data cable** — the remote, or the timer you want to update (each device is updated over its **own**
   cable; nothing is sent over the radio). A remote in deep sleep has no USB port: press UP or DOWN first.
2. Open **Firmware** in Fog Console (`Ctrl+7`). The port of the device is pre-selected; if the PC tool is already connected to it, that is
   the one.
3. Pick the firmware in the list. The tool recognises **timer** and **remote** firmware by themselves and offers the one that fits the
   connected device first.
4. Leave **Update (recommended)** selected and press **Flash firmware**.

**Check device** does the first half of that without writing anything: it resets the device into its bootloader, identifies the chip and its
MAC address, compares the flash with the chosen firmware (*“the flash already holds exactly this firmware”* or *“differs in: application”*) and
restarts the device. It is the safe way to find out whether the cable, the driver and the reset sequence work with a new board. The MAC address
is also how the tool tells you **which** device it just talked to: when it belongs to a timer on the dashboard the result ends with *“This is Fog3.”*

The tool closes its own connection to the port, resets the device into its bootloader, writes and verifies the new firmware, restarts the
device, reconnects, and finally tells you which firmware version the device reports now. Expect roughly half a minute for a timer and
a little longer for the remote.

![While flashing](../PCDiagnostics/docs/firmware-flashing-dark.png)

![Done](../PCDiagnostics/docs/firmware-done-dark.png)

The **offline demo** has the same page with a *simulated* chip (violet SIMULATED marking) and two demo images, so you can try the whole
procedure without touching hardware.

---

## 2. Where the firmware comes from

The list on the Firmware page is filled from three places:

| Source | When |
|--------|------|
| **Installed with the app** — a `firmware` folder next to `FogConsole.exe` with a `timer` and a `remote` subfolder | After `tools\package-firmware.ps1` and a build of Fog Console, or in a published copy of the tool |
| **Built in this repository** — the PlatformIO output folders `FogMachineTimer\.pio\build\*` and `FogMachineRemoteControl\.pio\build\*` | When the tool is started from a source checkout and the firmware has been built with `pio run` |
| **Chosen by you** — *Choose file…* (a `.bin`) or *Choose folder…* (a build folder) | Anything else; the choice is remembered |

A **folder** holds `firmware.bin` and, to make a complete installation possible, `bootloader.bin`, `partitions.bin` and `boot_app0.bin`.
A **single `.bin`** is an application image (only *Update* is possible) or a *merged* image that starts at flash address 0 — that one is
split up so that the settings partition is not overwritten.

`tools\package-firmware.ps1` builds both firmwares with PlatformIO and stages exactly these files:

```powershell
powershell -ExecutionPolicy Bypass -File tools\package-firmware.ps1
```

Every firmware image carries a small identity string, `FOGFW|timer|2.0.0|` or `FOGFW|remote|2.0.0|`
([`lib/FogLink/FogIdent.h`](../lib/FogLink/FogIdent.h)). That is how the tool knows what a file is, which version it holds, and that it
should ask before a *timer* image is written to a *remote* (or the other way round).

Before anything is written the image itself is checked: ESP32 image header, built for the **ESP32-C3**, checksum and SHA-256 digest
(a truncated or damaged download is refused), and it must fit the application partition.

---

## 3. What gets written

| Mode | Writes | Keeps | Use it when |
|------|--------|-------|-------------|
| **Update** *(default)* | the application and the OTA data (slot 0 boots) | settings, pairing, names, boot loader, partition table | normal updates |
| **Reinstall** | boot loader, partition table, OTA data, application | settings, pairing, names | the device carried some other firmware before, or a boot loader problem is suspected. Needs a complete firmware (a build folder) |
| **Clean install** | as *Reinstall*, and **erases the settings** | nothing — the device starts like new | a remote has to forget all timers, or a timer should lose its name and times |

Parts that the flash already contains are **skipped** (the chip calculates an MD5 of each region and the tool compares it), so a second
update of the same firmware writes nothing and says so. An *Update* also checks the **partition table** of the device first: if it differs
from the one in the build folder, the boot loader and the partition table are written too — otherwise the new application would not
fit where the old layout expects it. That is how a device with the **older single-slot layout** (firmware 2.0) gets the two-slot layout of
firmware 2.1: its settings and pairing at `0x9000` stay where they are.

An *Update* always writes **slot 0** and resets the **OTA data**, so the firmware that was just written is the one that starts — also on a
device that was last updated over the air and ran from slot 1.

Flash map of both firmwares (`partitions/fog_ota.csv`, 4 MB flash):

| Address | Size | Content |
|--------|------|---------|
| `0x0000` | 0x8000 | boot loader |
| `0x8000` | 0x1000 | partition table |
| `0x9000` | 0x5000 | **NVS: settings, pairing list, counters** |
| `0xE000` | 0x2000 | OTA data (`boot_app0.bin`): which slot boots |
| `0x10000` | 0x1E0000 | **application, slot 0 (`app0`)** — the USB update writes here |
| `0x1F0000` | 0x1E0000 | **application, slot 1 (`app1`)** — written by over-the-air updates |
| `0x3D0000` | 0x10000 | core dump |

---

## 4. Safety and recovery

* **The ROM bootloader cannot be overwritten.** It lives in the chip, not in the flash. Whatever goes wrong while flashing — a pulled cable, a
  cancelled run, a failed verification — the device can be flashed again.
* **If a device does not answer** after a failed attempt: hold its **BOOT** button, tap **RESET** (or plug the cable in while holding
  BOOT), release BOOT, tick *The device is already in download mode* on the Firmware page and press **Flash**.
* **Never during a show.** The device restarts; its output is off until the new firmware runs.
* A run that **fails or is cancelled** still sends the device back to its application (unless the cable is gone), so it does not sit
  in the bootloader looking dead. After a cancel in the middle of a write the application may be incomplete; flash again.
* **Closing Fog Console** during an update asks for confirmation for the same reason.
* A **timer** is on its machine, which cuts the power while it heats up. Update timers on the bench, or with the machine switched off and the
  USB cable supplying the power.
* The settings partition is only touched by **Clean install**.

---

## 5. How it works

The ESP32-C3 has a **ROM bootloader** with a serial protocol — the one esptool speaks. With the built-in USB-Serial/JTAG controller of the
XIAO board the control lines of the COM port are decoded by the controller itself, so no extra hardware is needed:

```text
 1  Fog Console closes its connection to the port (the remote stays awake only while the PC tool is connected)
 2  DTR/RTS sequence  (0,0) → (1,0) → (1,1) → (0,1) → (0,0)     chip resets with IO0 low → ROM download mode
 3  SYNC (0x08)       the ROM answers with a burst of replies
 4  GET_SECURITY_INFO chip id must be 5 = ESP32-C3, else nothing is written;   READ_REG: MAC address (eFuse)
 5  SPI_ATTACH, SPI_SET_PARAMS (4 MB)
 6  for every part:   SPI_FLASH_MD5 → unchanged? skip
                      FLASH_BEGIN (erases the region) → FLASH_DATA × n (1 KiB blocks, last one padded with 0xFF)
 7  for every part:   SPI_FLASH_MD5 → must equal the MD5 of the file
 8  reset: RTC watchdog (WRITE_REG) and EN pulse via RTS → the new application starts
 9  Fog Console reconnects, reads `hello`, shows the new firmware version
```

Packets are SLIP framed (`0xC0` delimiter, `0xDB` escape). The command layout was taken from the behaviour of **esptool 5.4.0** (its `--trace`
output is the reference). Details worth knowing:

* The ROM loader is used **without a flasher stub** and without compression — the same path as `esptool --no-stub`. It is slower than a
  stub, but it needs nothing beyond what is in the chip. Expect something like half a minute for the timer (1.1 MB) and a little more for the
  remote (1.3 MB); the page shows the speed and the time left.
* The erase size in `FLASH_BEGIN` is the exact size of the data; each data block carries an XOR checksum seeded with `0xEF`.
* `FLASH_END` is deliberately **not** sent: with the ROM loader it makes the chip run the application at once, before verification.
* The flash size is assumed to be **4 MB** (what the XIAO ESP32-C3 has). The boot loader header must say 4 MB too; the tool warns when it does not.
* The same code (`FogConsole/Flashing`) is used by the offline demo against a **simulated ROM** (`SimulatedRom`), which is also what the tests run
  against — including the real esptool (see below).

**Over the air instead of USB?** Timers are usually built into the machines, so their USB ports cannot be reached. For them the PC sends the
image to the **remote** (which has its USB cable), the remote forwards it by radio to the timer, and the timer writes it into the other slot: see
[`OTA_UPDATE.md`](OTA_UPDATE.md). The one thing OTA cannot do is install the *first* two-slot firmware on a timer that still has the old single-slot layout:
that one time needs the USB route above.

---

## 6. Without Fog Console

The usual PlatformIO route still works (the PC tool and PlatformIO do the same thing, so close one before using the other):

```powershell
pio run -d FogMachineTimer -t upload --upload-port COM7
pio run -d FogMachineRemoteControl -t upload --upload-port COM5
```

Or with esptool directly (the addresses are the flash map above):

```powershell
esptool --chip esp32c3 --port COM7 write-flash 0x0 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin 0x10000 firmware.bin
```

---

## 7. Troubleshooting

| Message / symptom | Cause / what to do |
|-------------------|--------------------|
| “COMx is in use by another program” | A serial monitor, PlatformIO, or another copy of Fog Console has the port. Close it |
| “The device did not enter its bootloader” | Put it into download mode by hand: hold BOOT, tap RESET, release BOOT; tick the *already in download mode* box; flash. Also try another cable or USB port. If the remote's USB port is gone, it is asleep — press UP or DOWN |
| The device “is running its application and did not react to the reset request” | The USB controller did not see the reset sequence. Same remedy: download mode by hand |
| “This device is an ESP32-S3 … nothing was written” | The tool only programs ESP32-C3 boards |
| “Verification failed for the application” | A flash write error or a bad USB connection. Try again with another cable; if it repeats, the flash chip may be defective |
| “The connection to the device was lost” | The cable was pulled or the device reset by itself. Replug and flash again (download mode by hand if needed) |
| “The image is built for ESP32-S3, not for the ESP32-C3” | Wrong firmware file |
| “The image is damaged: the checksum does not match” | The file is truncated or corrupt; build or copy it again |
| After flashing the device does not come back by itself | Press its RESET button or replug the cable. The PC tool reconnects automatically |
| *Check device* works, but flashing fails | The failing step is in the **Log**; send it along |
| The Firmware page lists nothing | Build with `pio run`, run `tools\package-firmware.ps1`, or use *Choose file…* / *Choose folder…* |
| A timer still reports the old version | Versions are read from the timer through the remote; wait a few seconds or reconnect |

The **Log** under the buttons records every step (copy it with the *Copy* button when you report a problem).

---

## 8. What has been verified

* **Against the real esptool.** The test suite runs esptool 5.4.0 (the copy PlatformIO installs) against the simulated chip, over a TCP
  socket, and checks that it can detect, read the MAC address of, write and verify it, and that the packets of Fog Console's own flasher are
  byte-for-byte those esptool sends for the same job.
* **Simulated faults:** a pulled cable, cancelling at any moment, a chip that ignores the reset, a wrong chip, corrupted writes, a busy port, a
  different partition layout, settings that must (or must not) survive — see `FogConsole.Tests/FlashingTests.cs` and
  `FirmwareViewModelTests.cs`.
* **Not yet:** a real ESP32-C3 on a real USB cable. The reset sequence and the restart after flashing follow esptool's (and the tool copes with the USB port being re-enumerated by the reset), but they are the part
  that depends on the Windows USB driver and the board. Start with **Check device** (it writes nothing), then flash; please report what you see
  (the Log on the Firmware page is what is needed). Until then `pio run -t upload` remains the fallback.
