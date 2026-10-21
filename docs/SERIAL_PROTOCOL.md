# FogSerial/2 — USB control protocol

This is the protocol between the **PC tool (Fog Console)** and a **remote control** (or, for
bench work, a single **timer** plugged in directly over USB).
It replaces the old binary `ReliableSerial` / `DebugProtocol` stack.

* Transport: USB-CDC virtual COM port of the XIAO ESP32-C3 (baud rate is irrelevant, use 115200).
* Framing: **JSON Lines** — one UTF-8 JSON object per line, terminated by `\n` (accept `\r\n`).
  Max line length 4096 bytes in both directions.
* Any received line that does not start with `{` is plain text (ROM boot log, ESP-IDF log, …).
  The PC tool must show it in the console and otherwise ignore it. The firmware itself never
  prints plain text once a PC session is active; its logs travel as `log` events.
* Every JSON object has a type string in `"t"`.
  * **Request** (PC → device): has `"id"` (integer ≥ 1, unique among in-flight requests).
  * **Response** (device → PC): `{"t":"res","id":<same>,"ok":true|false, ...}`. Exactly one per request.
    On failure: `"ok":false,"err":"<code>","msg":"<human text>"`.
  * **Event** (device → PC): no `id`. Streamed while a PC session is active.
* Error codes: `badRequest` (malformed JSON/missing field), `badArg`, `unknownType`,
  `unsupported` (valid, but not for this device kind), `noDevice` (bad `dev`/`mac`),
  `busy`, `full`, `notReady`, `failed`.
* **Session / keep-alive:** the device treats the PC as present for 6 s after the last valid line.
  The PC sends `ping` every 2 s. While present the device streams events (`fleet`, `remote`, …) and
  keeps the OLED awake / never deep-sleeps (so USB stays alive). When the PC goes silent the stream stops.
* Numbers: timer durations are **seconds with 0.1 s resolution** (`1.5`, `10.0`, range 0.1 … 9999.9).
  RSSI is dBm (negative integer), `0` means "unknown". MACs are upper-case `"AA:BB:CC:DD:EE:FF"`.
  Device names are 1–9 printable ASCII characters.

## Device kinds

| `kind`   | Meaning |
|----------|---------|
| `remote` | Battery remote. Owns the fleet (up to **10** timers), speaks ESP-NOW to them. All fleet requests are valid. |
| `timer`  | A smoke-machine timer connected directly by USB. Behaves like a fleet of exactly one device (`dev` 0 = itself). Fleet management requests return `unsupported`. |

## Requests

### `hello`
```json
{"t":"hello","id":1}
```
Response:
```json
{"t":"res","id":1,"ok":true,"kind":"remote","fw":"2.0.0","proto":2,"mac":"AA:BB:CC:00:11:22",
 "name":"Remote","ch":6,"net":1,"upMs":123456,"rst":"deepsleep","boots":12,"heap":182000,
 "build":"Oct 20 2026 12:00:00","maxDevs":10}
```
`rst` ∈ `poweron, ext, sw, panic, wdt, deepsleep, brownout, usb, other`.

### `ping`
`{"t":"ping","id":2}` → `{"t":"res","id":2,"ok":true,"ms":123456}` (device `millis()`; use it for latency).

### `get`
`{"t":"get","id":3,"what":"fleet"|"settings"|"stats"}`

* `fleet` → res `ok` and the device immediately emits one `fleet` event.
* `settings` → res contains `"settings":{…}` (see below).
* `stats` → res contains `"stats":{…}` (the `self` counters, same object as in a `stats` event) and the device then emits one `devStat` event per paired timer (see *Performance counters* below).
* `devStats` (needs `dev`/`mac`, remote only) → the remote asks that timer for its own counters over the radio
  (`GetInfo`) and answers when they arrive or after 3 s: res contains `"devStat":{…}` (same object as a `devStat` event,
  `"timer":null` if the timer did not answer). The response can therefore take **up to 3.5 s** — use a 5 s timeout.

### `statsCfg`
`{"t":"statsCfg","id":4,"intervalMs":1000}` – stream a `stats` event plus one `devStat` event per paired timer every `intervalMs` (≥ 500, `0` = off). While streaming is on
the remote additionally polls every paired, online timer with `GetInfo` round-robin (one per second) so the timer-side
counters are never older than ~10 s. A timer connected directly just streams its own counters.

### `set` (settings)
`{"t":"set","id":4,"settings":{"brightness":200,"autoOff":60}}` → res `ok` with the full new `settings`.
Unknown keys are ignored. **Remote settings:**

| key | type | meaning |
|-----|------|---------|
| `txPower` | int | ESP-NOW TX power in 0.25 dBm units, 8…84 (84 = 21 dBm) |
| `brightness` | int | OLED contrast 5…255 |
| `autoOff` | int | seconds until display blanks + deep sleep, 0 = never |
| `rssiLow`, `rssiHigh` | int | dBm mapped to 0 / 6 signal bars (`rssiHigh ≥ rssiLow+5`) |
| `batCal` | `[mv0,mv50,mv100]` | battery millivolts that count as 0 %, 50 %, 100 % (defaults 3300 / 3750 / 4150) |
| `net` | int | network id (read-only in this firmware, always 1) |

**Timer settings** (direct USB only): `startPhase` (`"off"`/`"on"`), `name`, `ton`, `toff` (seconds), `led` (bool).

### `cmd` — control a timer
```json
{"t":"cmd","id":5,"dev":2,"op":"setTimes","on":1.5,"off":10.0}
```
`dev` is the index in the fleet list **or** use `"mac":"AA:BB:…"` instead. `"dev":"all"` addresses every
paired timer (one job per timer). The response only says whether the command was **queued**:
`{"t":"res","id":5,"ok":true,"queued":1}`. The real outcome arrives later as a `cmdResult` event
carrying the same `id` (one event per addressed timer).
Optional `"confirm":false` (honoured for `mode`; other ops are always confirmed) makes the command fire-and-forget: one short attempt, **no** `cmdResult` event. Use it for the repeated
`mode forceOn` refreshes of a "hold to fog" button (send the first `forceOn` with the default `confirm`, refresh every
300 ms with `confirm:false`, then send `mode auto` normally).

| `op` | extra fields | effect on the timer |
|------|--------------|---------------------|
| `ping` | – | no-op; refreshes status |
| `setTimes` | `on`, `off` (s) | store new ON / OFF durations (persisted) |
| `mode` | `mode`: `"auto"`, `"forceOn"` or `"paused"`; `leaseMs` (forceOn only, 200…5000, default 1000) | `forceOn` holds the output on **only while it is refreshed** (dead-man lease); `paused` = output off, cycle frozen |
| `toggle` | – | flip the phase (ON↔OFF) and restart the phase timer |
| `reset` | – | output off, restart cycle at the beginning of the OFF phase |
| `setPhase` | `phase`:`"on"`/`"off"`, `elapsed` (s, default 0) | jump to a phase at a given elapsed time (used for staggering) |
| `name` | `name` | rename (persisted on the timer *and* in the remote's list) |
| `channel` | `ch` (1…13) | move this one timer to another home channel (persisted) |
| `startPhase` | `phase`:`"off"`/`"on"` | which phase a timer starts in after a power cycle (persisted) |
| `identify` | `ms` (default 5000) | blink the timer's LED so you can find the machine |
| `reboot` | – | restart the timer |
| `factoryReset` | – | wipe the timer's stored settings |

### Fleet management (remote only)
* `{"t":"select","id":6,"dev":1}` – make timer 1 the remote's *active* timer (the one its buttons control).
* `{"t":"pair","id":7,"mac":"AA:…","name":"Fog1"}` – add a timer (typically one from a `found` event). `name` optional.
  The remote also moves that timer to its home channel if it was found elsewhere.
* `{"t":"unpair","id":8,"dev":1}` – remove from the list (indices above shift down; a `fleet` event follows).
* `{"t":"discover","id":9,"on":true,"sweep":false}` – start/stop discovery. While on, every timer heard
  (paired or not) produces/refreshes a `found` event. `sweep:true` also hops through channels 1…13
  to find timers left on another channel.

### Channel tools (remote only)
* `{"t":"survey","id":10}` – measure the 2.4 GHz band (takes ≈ 3 s, radio is busy meanwhile). Emits
  13 `surveyCh` events and one `surveyDone`.
* `{"t":"channel","id":11,"ch":11,"force":false}` – change the **system home channel**. The remote tells every
  *online* timer (2-phase: confirm, then all hop), then follows. Progress arrives as `migrate` events.
  Unless `force` is true the change is abandoned if no timer confirms (so you cannot strand yourself).
* `{"t":"linkTest","id":12,"dev":0,"count":50,"gapMs":40}` – send `count` unicast pings (max 200) and measure.
  Emits `linkTest` progress events and a final one with `"done":true`.
* `{"t":"recover","id":13}` – look for timers that went quiet on other channels (sweep); same as the menu's *Find Timers*.

### Diagnostics
* `{"t":"logCfg","id":14,"level":"debug"}` – enable `log` events at `error|warn|info|debug|off`.
* `{"t":"sniff","id":15,"on":true}` – enable `frame` events (every FogLink radio frame, rate limited to 30/s).

### Power / reset
* `{"t":"reboot","id":16}` – restart the device (the port will re-enumerate).
* `{"t":"factoryReset","id":17,"scope":"remote"}` – wipe the remote's pairings & settings. (`"all"` also factory-resets
  every paired timer first.) Requires `"confirm":true`.

## Performance counters

Every device counts what happens on its radio so packet drops, retries, bad channels and power problems are visible.
All counters are `uint32` and count since boot, except `rst` (lifetime reset counts, persisted). They are delivered by
`get stats`, by the periodic `stats` + `devStat` events (see `statsCfg`) and by `get devStats`.

`stats` event (the device itself):
```json
{"t":"stats","ms":123456,
 "self":{"kind":"remote","up":1234,"boots":12,"heap":181000,"heapMin":150000,"loopMaxUs":8200,"loopsPerS":820,
         "rstReason":"deepsleep",
         "rst":{"poweron":3,"brownout":0,"wdt":0,"panic":0,"sw":5,"deepsleep":40,"other":0},
         "radio":{"ch":6,"tx":1200,"txOk":1180,"txFail":20,"txWd":0,"txDrop":0,"rx":3400,"rxBad":2,"rxForeign":0,"rxDrop":0},
         "chan":[{"ch":6,"tx":1100,"fail":15,"rx":3300},{"ch":1,"tx":12,"fail":2,"rx":0}],
         "link":{"cmds":40,"ok":38,"timeout":2,"rejected":0,"retries":11,"rttAvg":9.4,"rttMax":88,
                 "sweeps":3,"recovers":1,"found":1,"migrations":0},
         "pc":{"linesRx":120,"linesTx":4400,"badJson":0,"txDrop":0}}}
```
`devStat` event (one per paired timer, sent right after each `stats` event; remote only):
```json
{"t":"devStat","ms":123456,"i":0,"mac":"AA:BB:CC:DD:EE:01",
 "link":{"rx":3400,"expected":3520,"lost":120,"loss":3,"dups":0,"macOk":900,"macFail":12,
         "cmds":20,"cmdsOk":19,"cmdsTimeout":1,"retries":5,"rttAvg":8.1,"rttMax":60,
         "rssiMin":-80,"rssiAvg":-61,"rssiMax":-50,"offlineEvents":3,"offlineMs":95000,"reboots":2},
 "timer":{"age":2300,"fw":"2.0.0","up":3600,"boots":7,"rstReason":"poweron",
          "heap":200000,"heapMin":180000,"loopMaxUs":3100,"ch":6,"net":1,
          "rst":{"poweron":6,"brownout":1,"wdt":0,"panic":0,"sw":0,"other":0},
          "rx":820,"rxBad":0,"rxForeign":2,"rxDrop":0,"rxProbes":10,"rxCmds":14,
          "tx":3700,"txFail":3,"txDrop":0,"beats":3600,"replies":24,
          "cmdsRun":14,"cmdsDup":3,"cmdsRejected":0,
          "rssiMin":-85,"rssiAvg":-60,"rssiMax":-48,"rssiLast":-58}}
```
`timer` is `null` until the first answer from that timer arrived; `age` = ms since that answer.

For a timer connected directly by USB the `stats` event has only `self` (`"kind":"timer"`) and `self.timer` holds the timer
counters (same fields as above, without `age`); there are no `devStat` events.

| group | field | meaning |
|-------|-------|---------|
| radio | `tx` / `txOk` / `txFail` | frames handed to the radio / acknowledged by the receiver's MAC / **not** acknowledged (unicast only — broadcast has no ack) |
| radio | `txWd` | send callback never arrived (watchdog fired) · `txDrop` TX queue full |
| radio | `rx` | valid FogLink frames received · `rxBad` failed CRC/length/version · `rxForeign` other networks / other ESP-NOW users · `rxDrop` receive queue overflow (CPU too busy → **real packet loss inside the device**) |
| chan | per channel | unicast `tx`, `fail` (MAC no-ack) and `rx` counted on that channel — compare channels over time (channel *searches* are excluded) |
| link | `cmds`,`ok`,`timeout`,`rejected` | commands submitted and how they ended · `retries` extra transmissions · `rttAvg/Max` ms from first send to echo |
| link | `sweeps`,`recovers`,`found`,`migrations` | channel sweeps run, lost-timer recoveries tried, timers found elsewhere, fleet channel moves |
| dev.link | `rx`,`expected`,`lost`,`loss` | frames received from that timer / expected from its frame counter / missing / percent over the last 32 |
| dev.link | `macOk`,`macFail` | unicast frames to that timer acknowledged or not (a healthy link has ≈ 0 fails) |
| dev.link | `offlineEvents`,`offlineMs`,`reboots` | times it went offline, total time offline, times it rebooted (power cut) |
| timer | `beats`,`replies` | heartbeats and direct replies sent · `cmdsDup` retransmitted commands that were recognised and not re-executed |
| both | `loopMaxUs` | slowest main-loop pass since boot — large values (> 20 000 µs) mean something blocked the radio loop |
| both | `rst` | lifetime reset counts by cause (`brownout` + `poweron` explain power-cut behaviour of the machines) |

## Events (device → PC)

### `fleet` — main status stream (every 500 ms, plus immediately on change)
```json
{"t":"fleet","seq":812,"active":0,"ch":6,"devs":[
 {"i":0,"mac":"AA:BB:CC:DD:EE:01","name":"Fog1","online":1,"on":1,"ph":"on","mode":"auto",
  "ton":1.5,"toff":10.0,"el":0.8,"rssi":-61,"rssiT":-58,"age":340,"loss":3,"ch":6,
  "boot":7,"up":3600,"fw":"2.0","pend":0,"id":0}
]}
```
| field | meaning |
|-------|---------|
| `i` | list index (stable until an unpair) |
| `online` | 1 if a frame was heard within 4.5 s |
| `on` | output currently on (relay energised) |
| `ph` | `"on"`/`"off"` current cycle phase |
| `mode` | `auto` / `forceOn` / `paused` |
| `ton`,`toff` | configured durations, seconds |
| `el` | seconds elapsed in the current phase, extrapolated by the remote to "now" (UI interpolates between events) |
| `rssi` / `rssiT` | signal at the remote / at the timer (dBm, 0 = unknown) |
| `age` | ms since the last frame from this timer |
| `loss` | % of the timer's frames lost recently (−1 = not enough data) |
| `ch` | channel the timer was last seen on |
| `boot` | timer boot counter (increments on every power-up), `up` = timer uptime in seconds. A `boot` increase = the timer lost power |
| `fw` | timer firmware version |
| `pend` | commands still queued for this timer |
| `id` | stable slot id (0…9) |

When no timers are paired `devs` is `[]`. For `kind:"timer"` there is one entry with `i:0`.

### `remote` — the remote itself (1 Hz)
```json
{"t":"remote","bat":87,"mv":3987,"chg":0,"usb":1,"ch":6,"net":1,"up":1234,"heap":181000,
 "menu":"main","blank":0,"scan":0,"txq":0}
```
`bat` percent, `mv` millivolts, `chg` charging, `usb` external power present, `menu` current screen name,
`blank` display blanked, `scan` discovery/survey running, `txq` queued radio jobs.
For `kind:"timer"` this event is `{"t":"remote",…}` with `up`,`heap`,`ch`,`net` only.

### `cmdResult`
```json
{"t":"cmdResult","id":5,"dev":2,"mac":"AA:…","op":"setTimes","result":"ok","attempts":2,"ms":180}
```
`result` ∈ `ok` (the timer echoed the command), `rejected` (timer said invalid, or the command was issued for an earlier boot of a timer that has been power-cycled since — repeat it), `timeout` (no confirmation within
the command's time budget — it was **discarded**, it will not run later), `cancelled`.

### `found` (discovery)
```json
{"t":"found","mac":"AA:…","name":"FogTimer","rssi":-60,"ch":6,"paired":-1,"ton":1.5,"toff":10.0,"on":0,"boot":3}
```
`paired` = list index or −1.

### `surveyCh` / `surveyDone`
```json
{"t":"surveyCh","ch":6,"aps":4,"apRssi":-48,"frames":212,"avgRssi":-71,"score":37}
{"t":"surveyDone","best":[11,1,6],"cur":6}
```
`score` 0…100, **lower = quieter**. `best` lists channels best-first.

### `migrate`
```json
{"t":"migrate","stage":"start","ch":11,"targets":3}
{"t":"migrate","stage":"dev","dev":0,"ok":true}
{"t":"migrate","stage":"done","ch":11,"ok":2,"failed":1,"committed":true}
```

### `linkTest`
```json
{"t":"linkTest","id":12,"dev":0,"sent":20,"macAck":20,"echo":19,"rssi":-61,"rttMs":7.2,"rttMax":31,"done":false}
```
`macAck` = frames the timer's radio acknowledged, `echo` = frames the timer answered with an application reply.

### `log`
`{"t":"log","lvl":"info","ms":123456,"src":"link","msg":"T2 online ch6"}`

### `frame` (packet inspector)
`{"t":"frame","dir":"rx","ms":123456,"mac":"AA:…","kind":"STATUS","len":46,"rssi":-60,"seq":17,"hex":"F6 02 01 …"}`

## Example session (remote)
```
PC  → {"t":"hello","id":1}
DEV → {"t":"res","id":1,"ok":true,"kind":"remote",…}
PC  → {"t":"get","id":2,"what":"settings"}
DEV → {"t":"res","id":2,"ok":true,"settings":{…}}
DEV → {"t":"fleet","seq":1,…}                       (stream starts)
PC  → {"t":"cmd","id":3,"dev":0,"op":"mode","mode":"forceOn","leaseMs":1000}
DEV → {"t":"res","id":3,"ok":true,"queued":1}
DEV → {"t":"cmdResult","id":3,"dev":0,"op":"mode","result":"ok","attempts":1,"ms":14}
PC  → {"t":"ping","id":4}    (every 2 s)
```
