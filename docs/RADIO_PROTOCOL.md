# FogLink 2 — radio protocol & design

FogLink is the ESP-NOW protocol between the **remote** (battery, sleeps most of the time) and up to **ten
timers** (always listening, but they lose power while their smoke machine heats up).
The implementation lives in [`lib/FogLink`](../lib/FogLink); the protocol logic is plain C++ and is tested on a PC
against a simulated radio ([`tools/native-tests`](../tools/native-tests)).

## Why the old link was unreliable

| Old behaviour | Effect | FogLink 2 |
|---------------|--------|-----------|
| Every command and every status was "reliable" with **unlimited retries every 200 ms** | A timer that was powered off (heating) collected hundreds of queued frames, flooded the channel for everybody, and executed the **stale commands when it came back** (a queued `TOGGLE` could fire minutes later) | Commands are short-lived **jobs with a deadline** (2.5 s). After that they are discarded and can never run late |
| Retried commands were executed again (no de-duplication) | `TOGGLE` retried after a lost ACK toggled twice | Every command carries an id; the timer remembers the last one per sender and **only echoes** a repeat |
| The remote **polled** the active timer 4–7×/s and every poll needed an ACK and a reply | ≈ 30 frames/s on the air for one timer, collisions with everything else | **No polling.** Timers announce themselves every second; commands are answered by the next status |
| Promiscuous (sniffer) mode on both ends all the time to read RSSI | Every Wi-Fi frame in the building was copied to the CPU; receive buffers ran dry and ESP-NOW frames were lost | RSSI comes from the receive metadata (`rx_ctrl`). Promiscuous mode is used only for the 3 s channel survey |
| Protocol code ran **inside the Wi-Fi callback** (logging, EEPROM commit, `std::vector` edits) while `loop()` used the same data | Data races, heap corruption, blocked radio task | Callbacks only copy a frame into a queue. Everything else runs in `loop()` |
| `Serial.printf` per packet + binary protocol on the same port | Blocked when nobody reads USB; corrupted the PC framing | Logs are ring-buffered and sent as JSON `log` events only when a PC asks |
| Remote + timer on one fixed channel, discovery by passive hopping | Timers left on another channel were lost; no way to see why | Channel survey, **fleet migration** (2-phase) and automatic **recovery sweeps** |
| EEPROM layout overlaps (battery calibration inside the paired-device block) | With more than 3 timers paired, calibrating the battery corrupted the list | NVS key/value storage with CRC; no shared address map |
| Floating-point seconds on the wire, `lastSwitch = now` drift | 0.1 s was not exactly representable; cycle drifted with loop jitter | Integer tenths of a second, drift-free phase timing |

## Principles

1. **Timers announce, the remote listens.** State travels from the timers by heartbeat; the remote never has to ask.
2. **Commands are idempotent and short-lived.** A command is retried until the timer echoes it or its deadline passes.
3. **Unicast gets a free link-layer ACK.** ESP-NOW unicast frames are acknowledged (and retried) by the Wi-Fi MAC;
   the send callback tells us whether the peer's radio heard it. FogLink adds an application echo only for *effect*.
4. **Never block, never share.** One TX frame in flight, receive queue drained by the main loop.
5. **Everything is counted.** Both ends keep performance counters (see `SERIAL_PROTOCOL.md`).

## Frame

```
 0      1     2    3     4    5     6 ...        6+len
+------+-----+----+-----+----+-----+--------------+----------+
|magic |ver  |net |type |seq |len  | payload      | crc16 LE |
|0xF6  |2    |1.. |     |    |     |              |          |
+------+-----+----+-----+----+-----+--------------+----------+
```
* `net` — network id (default 1). Frames from another network are counted and ignored.
* `type` — 1 `STATUS`, 2 `PROBE`, 3 `CMD`, 4 `INFO`, 5 `OTA`.
* `seq` — the sender's frame counter. Gaps in it are how the remote measures **packet loss** of each timer without any extra traffic.
* `crc16` — CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over header + payload. (Wi-Fi already protects every frame with a
  32-bit FCS; this CRC guards against foreign ESP-NOW users, truncation and version mix-ups.)
  Test vector: `crc16("123456789") = 0x29B1`; the golden frame
  `F6 02 01 03 2A 04 07 01 00 00 14 AC` is checked in the PC tests.
* Little endian everywhere; max 250 bytes per frame.

### `STATUS` (timer → everyone, 38 byte payload)
`flags`(output on, phase on, just booted, identify, heartbeat, reply) · `mode` (auto/forceOn/paused) · `onTenths` ·
`offTenths` · `elapsedMs` (in the phase) · `channel` · `rssiAtTimer` · `lastCmdId`/`lastCmdResult`/`lastCmdSrc` (the **echo**) ·
`fwMajor/Minor` · `startPhaseOn` · `bootCount` · `uptimeS` · `name[10]`.

Sent as: heartbeat every 1 s (±12 %, so ten timers never beat in step); immediately on every phase/output change; three
announcements in the first 450 ms after power-up; as a unicast reply to a `CMD`/`PROBE`.

### `PROBE` (remote → broadcast or one timer, 2 bytes)
"Send me your STATUS." Broadcast probes ask timers to answer after a random 0…60 ms so replies do not collide. A unicast probe
doubles as a **ping with MAC acknowledgement** (used by link tests and channel searches).

### `CMD` (remote → one timer)
`cmdId`(1…255) · `op` · `epoch` · arguments: `SetTimes(on,off tenths)` · `SetMode(auto|forceOn|paused, leaseMs)` · `Toggle` · `Reset` ·
`SetPhase(phase, elapsedMs)` · `SetName` · `SetChannel` · `FactoryReset(0xA5)` · `Identify(ms)` · `Reboot(0x5A)` ·
`SetStartup(phase)` · `GetInfo`.

**Boot epoch.** A command that is retried across a power cut must not run again on the rebooted timer — a replayed `Toggle` would
switch the output ON right after power-up. Commands whose replay is harmful (`Toggle`, `SetPhase`, `forceOn`, `Reboot`,
`FactoryReset`) carry the boot epoch (low byte of the timer's boot counter) the remote last saw; a timer that has rebooted since
answers them with result `Stale` and does **not** execute them. Idempotent commands (times, name, channel, auto…) carry
epoch 0 = any boot, so recovery after a power cut still works.

### `INFO` (timer → remote, 100 bytes)
Reply to `GetInfo`: firmware, reset reason, lifetime reset counts (power-on / brown-out / watchdog / panic / sw), heap, slowest
loop pass, and all rx/tx/command counters of the timer. This is the timer half of the performance counters.

### `OTA` (both directions, firmware update, firmware 2.1+)
A transfer protocol of its own on top of the same frame: `[op][session][body]`. The PC sends `Begin` / `Data` / `Poll` / `Verify` /
`Activate` / `Abort` (through the remote, which only forwards them to a paired timer, one frame at a time, after any due `CMD`), the timer
answers with `State` frames (what it has, what is missing, erase/verify progress, errors). The timer writes the application slot that is not
running, checks its SHA-256, and the boot loader switches slot (with rollback). Everything — frame layouts, selective acknowledgement,
timings, safety, test vectors — is in [`OTA_UPDATE.md`](OTA_UPDATE.md). While the PC is updating, the remote keeps the radio on the
home channel (no discovery, recovery sweeps, channel moves or link tests).

## Delivery of a command (remote)

```
submit(job, deadline = now + 2.5 s)
  send CMD (unicast)
    MAC ack  ?  yes → wait 100 ms for the STATUS echo (lastCmdId == cmdId)   → done: OK / Rejected
                no  → retry after 12–45 ms (slower after repeated failures: 100 ms, 250 ms)
  deadline reached  → job discarded, result = Timeout (never executes later)
```
* A status counts as the *echo* of a command only if it is a direct reply (`ST_REPLY`): heartbeats repeat the last echo and
  must not confirm a new command that happens to reuse the id.
* One transmission in flight at a time; the head job of each timer is served round-robin, so a dead timer cannot starve the others.
* A newer `SetTimes`/`SetName`/`SetMode` replaces an older one that has not been transmitted yet (coalescing).
* **Hold-to-fog** is a *dead-man lease*: `SetMode forceOn(1000 ms)` keeps the output on only while it is refreshed (every
  300 ms, fire-and-forget). If the remote sleeps, dies or loses the link, the output falls back to auto within a second.

## Timer behaviour (power-cut tolerant)

* Output pin is driven to its safe level **first thing** in `setup()`.
* Settings (on/off times, name, channel, start phase) are in NVS (power-fail safe, CRC protected) and only written when
  they change. A boot counter and reset-cause statistics are kept for diagnosing the power supply.
* After power-up it announces itself within ~0.3 s on its stored channel (`ST_JUST_BOOTED` + incremented `bootCount`); the
  remote shows "power-cycled".
* The cycle starts at the beginning of the **OFF** phase after a power-up (configurable: `startPhase`).
* Cycle timing is integer milliseconds with drift-free phase boundaries; times are validated (0.1 … 9999.9 s).
* The loop is watchdog-protected; any reset leaves the output off.
* Commands carrying a stale id from a retry are **not executed twice** (dedup window 6 s per sender).

## Channels

* There is one **home channel** (default 1, valid 1…13). The remote owns it; timers store theirs.
* **Survey** (remote): active scan of all access points + a 90 ms promiscuous traffic count per channel; neighbours bleed into
  each other (±4 channels) so the score accounts for overlap. The menu / PC tool shows the ranking.
* **Fleet migration** (2-phase): the remote sends `SetChannel` to every *online* timer and waits for each echo (the timer
  acknowledges first, hops 200 ms later). If at least one timer confirmed — or the user forces it — the remote follows and
  stores the new home channel. Offline timers keep their old channel hint.
* **Recovery sweep**: a timer that comes back (after heating) on a channel the remote has left is found by a unicast probe on
  each candidate channel (the MAC ack tells instantly whether it listens there — ≈ 25 ms per channel). The remote then sends
  `SetChannel(home)` on that channel and returns. Runs automatically for timers that stay offline (first after 8 s, then
  back-off up to 2 min), and on demand ("Find Timers").
* An excursion can never strand the remote: every excursion is limited to 15 s, a channel whose sends keep failing is skipped,
  and removing a timer while one is running ends it.
* While a timer is simply powered off, nothing is wasted on the air: sweeps are rare and short, the remote stays on home.

## Several remotes

The design assumes **one active remote**. A second remote can pair with the same timers and control them, but the two do not
coordinate: if they use different home channels each one will keep fetching the timers onto its own channel, and a command
echo can be overwritten by the other remote's command. Use one remote per system (or different timers per remote).

## Remote sleep

The remote deep-sleeps after the *Auto Off* time and wakes on UP/DOWN. Because timers announce every second, a freshly woken
remote knows its fleet within ~0.3 s (it also broadcasts a probe immediately).

## Tuning constants (`FogProto.h`, `FogRemoteLink.h`)

| constant | value | meaning |
|----------|-------|---------|
| heartbeat | 1 s ±12 % | timer → broadcast status |
| online window | 4.5 s | a timer is "offline" if silent longer |
| command budget | 2.5 s | job deadline (350 ms for lease refreshes) |
| echo wait | 100 ms | after a MAC-acked send, before retrying |
| max attempts | 16 | per command |
| TX watchdog | 120 / 150 ms | radio / link: lost send callback |
| recovery | 8 s → 120 s | auto-recovery back-off |
| update queue / hold | 8 frames / 15 s | remote's bridge for `OTA` frames; radio hold that ends 15 s after the last update frame |
| update erase | 1 sector / 20 ms | timer erases its new slot gently, so the cycle and the heartbeats keep running |
| update silence | 30 s | a transfer with no frame for this long is dropped by the timer |

## Verification

`tools/native-tests` runs the **real** `TimerNode` and `RemoteLink` code against a simulated medium (loss, burst loss, MAC
retries with lost ACKs, power cuts, channel changes, `millis()` wrap) — 524 checks (198 for the protocol described above, the rest for the firmware update, see [`OTA_UPDATE.md`](OTA_UPDATE.md)): all ten timers visible within 1.5 s,
no command executed twice under 40 % loss, stale commands never run after a power cut, dead-man lease holds under loss and
expires without the remote, migration commits/aborts correctly, a late timer on the old channel is brought home, idle remote
sends nothing. Run it with `powershell -File tools/native-tests/run.ps1`.
