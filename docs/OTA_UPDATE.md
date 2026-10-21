# Over-the-air firmware update of the timers

*Updating a timer that sits inside a smoke machine — through the remote, which is connected to the PC by USB.*

> **Terminology:** “fog machine” (fogger) and “smoke machine” are both common. “Fog” is the US trade name, “smoke machine” the UK/Danish everyday term. Neither makes real smoke. I use “smoke machine” for the machine and “fog” for the burst.

## Contents

1. [Overview](#1-overview)
2. [Requirements and the one-time USB step](#2-requirements-and-the-one-time-usb-step)
3. [Flash layout](#3-flash-layout)
4. [The transfer protocol (radio)](#4-the-transfer-protocol-radio)
5. [The tunnel through the remote (USB)](#5-the-tunnel-through-the-remote-usb)
6. [Behaviour of every party](#6-behaviour-of-every-party)
7. [Safety](#7-safety)
8. [Test vectors](#8-test-vectors)

---

## 1. Overview

```text
 Fog Console (PC)  ──USB, JSON lines──▶  remote  ──ESP-NOW──▶  timer (inside the machine)
   firmware.bin                          radio bridge            writes the other application slot
   windowed sender, retries              no image data kept      verifies, switches slot, reboots
```

* The **PC** reads the timer's firmware image, cuts it into 224-byte chunks and runs the whole transfer: it sends a window of chunks,
  learns from the timer's *State* reports which ones arrived, re-sends the missing ones, asks the timer to verify, and finally to
  activate. It also decides when to give up or to start again.
* The **remote** is only a **radio bridge** for this: it forwards frames from the PC to a paired timer and frames from the timers back to the PC.
  It stores nothing of the image and has no state machine for it, so its firmware stays small and the remote itself needs no update to
  get the next timer firmware.
* The **timer** writes the chunks into the application slot that is *not* running, checks the SHA-256 of what it wrote, switches the boot
  slot and restarts. A new firmware that does not come up (it resets within seconds) is **rolled back by the boot loader**.
  The running application is never touched, so a lost cable, a power cut or a bad transfer leaves the timer working with its old firmware.

A timer can also receive the same transfer straight over **its own USB cable** (no remote needed): the PC sends the same messages to it
(§5.4). That is the easiest way to try the feature on the bench.

## 2. Requirements and the one-time USB step

OTA needs **two application slots** and the OTA data partition. The first firmware that has them (2.1.0, `partitions/fog_ota.csv`)
must therefore be installed **once over USB** — Fog Console's USB update does that automatically (*Update* notices the different partition layout and
writes boot loader and partition table too; settings and pairing are kept). After that every update can be done over the air.
A timer without that layout answers the capability probe with `noSlot` (Fog Console says *“needs a USB update first”*), a timer with an older
firmware does not answer at all (*“firmware without OTA support”*).

The remote needs firmware 2.1.0 or newer for the tunnel (its `hello` lists the capability `ota`).

## 3. Flash layout

`partitions/fog_ota.csv` (4 MB flash, used by both firmwares):

| Name | Offset | Size | |
|------|--------|------|---|
| nvs | `0x9000` | `0x5000` | settings, pairing list, counters (unchanged position: survives the migration) |
| otadata | `0xE000` | `0x2000` | which slot boots, and whether it is verified |
| app0 (ota_0) | `0x10000` | `0x1E0000` (1.875 MB) | application |
| app1 (ota_1) | `0x1F0000` | `0x1E0000` | application |
| coredump | `0x3D0000` | `0x10000` | |

(The images are about 1.1 MB for the timer and 1.3 MB for the remote.) The USB flasher always writes slot 0 and resets the OTA data, so a USB update
takes effect even if the device last ran from slot 1.

## 4. The transfer protocol (radio)

A new FogLink frame type, `Type::Ota = 5` (frame format and CRC unchanged, see [`RADIO_PROTOCOL.md`](RADIO_PROTOCOL.md)). The payload is

```text
[op u8][session u8][body ...]            all multi-byte numbers little endian
```

`session` is a random non-zero number chosen by the PC for one transfer to one timer. Frames of another session than the active one are
answered with an error (`busy`; the same session number used from another sender address is also `busy`). Session `0` is only used by the
capability probe (`Poll`); a `Begin` with session `0` is a `badRequest`.

### 4.1 PC → timer

| op | name | body | meaning |
|----|------|------|---------|
| `0x01` | **Begin** | `size u32` · `sha256[32]` · `chunk u16` · `fw[3]` · `flags u8` · `auth[16]` = 58 bytes | start (or resume: see 6.1) a transfer of an image of `size` bytes (at most 0x1E0000) with this SHA-256, cut into `chunk`-byte chunks (`chunk` 32…232; Fog Console uses 224). `fw` = major, minor, patch of the image, `flags` = 0 (reserved) |
| `0x02` | **Data** | `seq u16` · bytes | chunk number `seq`, bytes `seq*chunk .. seq*chunk+n`; `n = chunk` except for the last chunk (`n = size − seq*chunk`) |
| `0x03` | **Poll** | – | “send your State”. With `session = 0` it is the capability probe |
| `0x04` | **Verify** | – | all chunks have been sent: check the SHA-256 of everything written |
| `0x05` | **Activate** | `magic u8 = 0xA7` | switch to the new application and reboot (only valid in state *Verified*) |
| `0x06` | **Abort** | – | forget the transfer and return to *Idle* |

`auth` = the first 16 bytes of `HMAC-SHA256(key, "FOGOTA1" ‖ session ‖ size u32 ‖ sha256 ‖ chunk u16 ‖ fw[3] ‖ flags)`, where `key` is the
UTF-8 text of the *OTA key* (default `FogLink-OTA-v1`, overridable in the timer build with `-DFOG_OTA_KEY="\"…\""` and in Fog Console's
settings). It keeps casual or accidental senders out; it is **not** protection against someone who can read a firmware dump (§7).

### 4.2 timer → PC: *State* (`0x81`)

```text
[0x81][session][state u8][error u8][contig u16][mask u32][progress u8][total u16][slot u8]      14 bytes
```

| field | meaning |
|-------|---------|
| `session` | the active session. In the answer to `Poll`, `Abort` and when idle it is the session that was asked about (the probe uses `0`); an answer with `error = busy` carries the session that is active |
| `state` | `0` Idle · `1` Erasing · `2` Receiving · `3` Verifying · `4` Verified · `5` Activating · `6` Failed |
| `error` | `0` none · `1` busy (another session is active) · `2` noSlot (layout without a second slot / OTA unsupported) · `3` tooBig · `4` badImage (not an ESP32-C3 image, or not timer firmware) · `5` hashMismatch · `6` flashError · `7` badRequest (arguments, or wrong state) · `8` authFailed · `9` timeout (the session was dropped after 30 s of silence) · `10` lowResources |
| `contig` | number of chunks received **without a gap from chunk 0**: chunks `0 .. contig−1` are written |
| `mask` | bit *i* (LSB = *i* 0) set ⇔ chunk `contig+1+i` has been received (so 32 chunks beyond the first missing one) |
| `progress` | percent of the current activity: erase done (state *Erasing*), hash done (state *Verifying*), otherwise `100` |
| `total` | number of chunks of this transfer, `ceil(size / chunk)` (0 when idle) |
| `slot` | index of the slot being written (0 or 1) |

`state = Idle, error = 0` as the answer to the probe means *“OTA capable, ready”* (the `session` field echoes the session of the request: `0` for the probe; a sender
accepts `0` or the session it asked about). When `state = Failed` the `error` says why; the
timer stays in *Failed* (reporting it) until it gets `Abort` or a new `Begin`.

A State is sent **immediately** in answer to `Begin`, `Poll`, `Verify`, `Activate`, `Abort`; for frames of another or an unknown session the answer is limited
to one per 20 ms (a stray `Data` frame to an idle timer is answered with `State(Idle, error 0)`); and
**on its own**: after every 8 accepted Data frames, whenever a Data frame arrives out of order (a chunk beyond `contig`), when the
erase finishes, when the check finishes, and once `contig == total`. At most one self-initiated State per 20 ms.

### 4.3 The sender (what Fog Console does)

```text
probe:      Poll(session 0)  → State(Idle, error 0)  capable | State(error 2) noSlot | no answer: no OTA support
begin:      Begin            → State(Erasing) … State(Receiving)       (re-send Begin every 400 ms, up to 10 s; then Poll every 500 ms while Erasing, up to 60 s)
stream:     window W = 24:   send Data(seq) for the chunks in [contig, contig+W) that are not marked in `mask`; a chunk is re-sent when it
            has not been confirmed 150 ms after it was sent; no State for 400 ms → Poll
verify:     when contig == total:  Verify → State(Verifying) … State(Verified)     (re-send every 500 ms, up to 30 s)
activate:   Activate         → State(Activating); the timer reboots ≈350 ms later (a re-sent Activate may reach the new firmware, which answers
            State(Idle): *Idle together with a new boot count after Activate means success* — do not start the transfer again)
finish:     wait (up to 60 s) until the timer's heartbeat shows a new boot count and the new firmware version
            (the fleet list reports the version as *major.minor*: the sender compares those, and the patch only when both sides know it)
```

A reboot of the timer in the middle of a transfer (power cut: its heartbeat shows a new boot count, State says *Idle*) loses the transfer; the
sender starts it again (up to three times). Sending `Abort` is polite, not required.

## 5. The tunnel through the remote (USB)

New FogSerial messages ([`SERIAL_PROTOCOL.md`](SERIAL_PROTOCOL.md)); `d` is **base64 (standard alphabet, padded) of the OTA frame payload**
(`[op][session][body]`, at most 242 bytes ⇒ at most 324 characters).

### 5.1 `hello`

The remote and a timer on USB add `"caps":["ota"]` to their `hello` answer when they support the tunnel / the local receiver.

### 5.2 `otaTx` — PC → remote → timer

```json
{"t":"otaTx","id":17,"dev":3,"d":"AUKIEwAA..."}            // dev = index in the fleet list; "mac":"AA:BB:.." also accepted
{"t":"res","id":17,"ok":true,"acked":true}                  // after the radio finished: the timer's MAC acknowledged the frame
{"t":"res","id":17,"ok":true,"acked":false}                 // the frame left the remote but nobody acknowledged it
{"t":"res","id":17,"ok":false,"err":"busy"}                 // the remote's transmit queue (8 frames) is full: retry shortly
{"t":"res","id":17,"ok":false,"err":"noDevice"}             // not a paired timer
{"t":"res","id":17,"ok":false,"err":"badArg"}               // bad base64, empty, or longer than 242 bytes
```

The remote sends one frame at a time, in order, with the normal FogLink MAC retries; commands to timers (`cmd`) keep priority over OTA frames.
Fog Console keeps **at most 6 `otaTx` requests outstanding** to hide the USB round trip (more only make the sender's resend timer run while frames still wait in the
remote's queue and cause resend storms). Every `otaTx` also renews the hold (5.4).

### 5.3 `otaRx` — timer → remote → PC (event)

```json
{"t":"otaRx","dev":3,"mac":"AA:BB:CC:DD:EE:03","rssi":-61,"d":"gUICAAUA..."}
```

One event for every `Type::Ota` frame received from a paired timer.

### 5.4 `otaHold` and busy radio

```json
{"t":"otaHold","id":18,"on":true}        // Fog Console repeats it about every 5 s while a transfer runs
```

While the hold is on (it expires by itself 15 s after the last `otaHold`/`otaTx`) the remote does not start anything that moves the
radio off the home channel or hogs it: *discover*, *survey*, *channel move*, *link test*, automatic recovery sweeps and the menu actions
with the same effect are refused with `err:"busy"` (`"firmware update running"`).

A **timer connected by USB** accepts the same `otaTx` (`dev` 0 or omitted, answered with `acked:true` at once) and emits its replies as `otaRx`
events (with `dev:0`; the `mac` is the timer's own); `otaHold` is accepted and ignored.

## 6. Behaviour of every party

### 6.1 The timer (`lib/FogLink/FogOta.*`, platform part `FogMachineTimer/src/EspOtaTarget.*`)

* Receives `Type::Ota` from any remote of its network; the session is bound to the sender's MAC (replies go there).
* **Begin** (state *Idle* or *Failed*, or the *same* session again): checks `auth` (a failing Begin is answered with an error State and changes nothing else), finds the slot to write (the other OTA slot, never the running one), checks
  `0 < size ≤ slot size` and `chunk` 32…232. Then it **erases the slot in 4 KiB sectors, one sector per loop pass at most every 20 ms** (so the
  cycle and the heartbeats keep running; a pass is delayed by about 45 ms), reporting *Erasing* with the percentage; after the last sector it is *Receiving*.
  A repeated `Begin` of the *same* session with the same parameters only returns the current State (that is how a resume after a PC or remote restart
  works). Any other `Begin` while a session is active → `busy`.
* **Data**: only in *Receiving*; `seq < total`, length exactly right; a chunk that was already received is ignored (but counts for the next State).
  Written straight into the slot. Chunk 0 must start with the ESP image magic `0xE9` and chip id 5 (ESP32-C3) or the transfer fails with `badImage`.
* **Verify** (needs `contig == total`): computes SHA-256 over the `size` bytes in the slot, a slice per loop pass; compares it with the `Begin` hash; the
  image must also contain the identity string `FOGFW|timer|` (a *remote* image cannot be put on a timer). On success *Verified*, else *Failed* (`hashMismatch` / `badImage`).
* **Activate** (state *Verified*, magic `0xA7`): drives the output off, asks the boot loader to verify the image and boot it next time
  (`esp_ota_set_boot_partition`), answers *Activating*, reboots ≈350 ms later. The output stays off from the Activate until the reboot (the cycle cannot switch
  it back on in between). If the call fails → *Failed* `flashError`.
* **Silence**: 30 s without any frame of the session in *Erasing / Receiving / Verifying / Verified* → back to *Idle* (next State carries `timeout`).
  *Failed* never times out: it is reported until `Abort` or a new `Begin`.
* The chunk bookkeeping covers 61 440 chunks (about 7.5 KB of RAM), so any window size works.
* **After a new image booted:** it runs in the boot loader's *pending verify* state; after 6 s of healthy running it confirms itself
  (`esp_ota_mark_app_valid_cancel_rollback`). A reset before that (crash loop, a power cut in the first seconds) makes the boot loader return to the previous firmware.
* The timer keeps **cycling** the whole time; only the pass that erases a sector is delayed (≈ 45 ms).

### 6.2 The remote

Bridge only (§5): `otaTx` → one frame, `Type::Ota`, unicast to the timer's MAC on the home channel; `Type::Ota` frames from paired timers → `otaRx`.
Not forwarded while the radio is away from the home channel (excursions); `otaHold` as in 5.4. While an update runs the remote's display only shows an
*“Update running”* notice when someone picks a menu action that is refused because of it.

### 6.3 Fog Console

The *Firmware* page, section *Over the air*: choose the timer firmware, tick the timers, press **Update**. Timers are updated one after another; every one
shows its stage (probe, erase, transfer with speed, verify, restart, version check), the result and the number of re-sent chunks. A transfer to a timer that is
offline, or on another channel, does not start. Hold-to-fog and the other commands keep working during an update, but a timer heating up (power cut) ends its transfer.

## 7. Safety

* **A wrong or damaged image cannot be activated:** the SHA-256 given in `Begin` must match what was written, the image must be an ESP32-C3 image with the
  timer identity string, and the boot loader checks the image once more before it switches.
* **The running firmware is never overwritten.** Power cut, lost radio, lost cable, wrong key: the timer keeps its old firmware (and its settings).
* **Rollback:** a new firmware that crashes or resets within 6 s of its first start is replaced by the old one by the boot loader.
* **Not secure against an attacker.** The radio link is not encrypted, and the default OTA key is in this repository and in every firmware dump. Anyone who knows the key and is in
  radio range can push a firmware. For an installation that matters: set your own key (`-DFOG_OTA_KEY` in the timer build, *OTA key* in Fog Console's settings), build your own firmware, and keep it private.
* Do not update machines during a show: the timers reboot, and a timer reboot switches its output off.

## 8. Test vectors

Image: 5000 bytes, byte *i* = `(i*7+3) & 0xFF`; key `FogLink-OTA-v1`; session `0x42`; chunk 224; image version 2.1.0; flags 0; FogLink net 1.

```text
sha256        34398b85297bf7d9dfb59b8d511d8bbb44ab23e891570e4395e7871475fc8afb
hmac input    464f474f544131 42 88130000 34398b85…8afb e000 020100 00          ("FOGOTA1", session, size, sha256, chunk, fw, flags)
auth          7e37afee6477849bcd24393f2018a79a
Begin payload 01428813000034398b85297bf7d9dfb59b8d511d8bbb44ab23e891570e4395e7871475fc8afbe000020100007e37afee6477849bcd24393f2018a79a     (60 bytes: op, session, 58 body)
Begin frame   f6020105073c01428813000034398b85297bf7d9dfb59b8d511d8bbb44ab23e891570e4395e7871475fc8afbe000020100007e37afee6477849bcd24393f2018a79a643e
              (seq 7, type 5, length 0x3c)
Data(0) payload head   02420000 030a1118 1f262d34 …           228 bytes (op, session, seq, 224 data); frame CRC 1793
Data(22) payload head  02421600 c3cad1d8 dfe6edf4 …           76 bytes (the last chunk: 72 data bytes); frame CRC 59d8 (seq 9)
Poll(0) frame f602010501020300ebb7                             (seq 1, payload 03 00)
State payload 8142 02 00 0500 05000000 64 1700 01              = session 0x42, Receiving, no error, contig 5, mask 0b101, progress 100, total 23, slot 1
State frame   f6020105030e81420200050005000000641700018a02     (seq 3)
              base64 of the State payload: gUICAAUABQAAAGQXAAE=
HMAC with the key "other"  →  5f2a426305c696639802d693a83bf393   (a different key gives a different auth)
```
