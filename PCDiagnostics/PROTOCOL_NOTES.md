# Protocol notes for Fog Console

Written while implementing Fog Console against `docs/SERIAL_PROTOCOL.md` (FogSerial/2, revision 2: performance
counters, `devStat`, `confirm:false`). The spec file itself is untouched. This file lists what the PC tool assumes
where the spec is silent, what it tolerates, and a few questions for the firmware side.

## How the tool uses the protocol (for orientation)

* Session: `hello` once, then `ping` every 2 s (also used for the latency figure). If four pings in a row go
  unanswered the tool treats the device as gone and reconnects by itself (the port may also vanish for a moment after
  `reboot`, `factoryReset` or a USB re-enumeration; the tool then waits for it and reconnects when it comes back).
* Timeouts: 1.5 s for ordinary requests, 4 s for `cmd` outcomes (`cmdResult`), **5 s for `get devStats`** (the remote
  may take up to 3.5 s).
* After `hello`: `get settings`, `get fleet`, `logCfg` with the level chosen in the Console page (default `info`,
  remembered between runs) and, only if "capture frames" is ticked, `sniff`.
* Counter streaming: `statsCfg` with `intervalMs:1000` **only while the Diagnostics page is visible**, `statsCfg 0` when
  the page is left (or the app closes). After a reconnect it is sent again if the page is still visible.
* Hold to fog: the first `mode forceOn` is sent with the default `confirm` (`leaseMs:1000`), then every 300 ms
  `mode forceOn` with `confirm:false`; on release `mode auto` is sent normally (retried up to three times). `confirm:false`
  is used for nothing else.
* Lines that do not start with `{` are shown in Console > Raw and otherwise ignored. A line that starts with `{` but
  does not parse is counted as "bad JSON" (status bar and Diagnostics > USB connection) and shown in Raw.
* The serial port is opened with DTR and RTS **off** so that opening the port does not reset an ESP32-C3 into the
  bootloader on boards where those lines are wired to EN/IO9.

## Assumptions made

1. **`get stats` response.** The spec says `res.stats` is "the `self` counters, same object as in a `stats` event".
   The parser accepts that shape and also a nested `{"self":{...}}` form, so a firmware that returns the whole `stats`
   event body inside `res.stats` works too. Following `devStat` events (and the `res.devStat` of `get devStats`) are
   merged into the same snapshot by `mac`/index.
2. **`timer: null` in `devStat`** means "no answer yet": the page shows "no timer data" instead of zeros and does not
   compute rates for the timer side. `age` is used to show how old the timer's counters are.
3. **Counters are monotonic since boot.** Rates are computed from consecutive snapshots using the `ms` of the event
   (the PC clock when `ms` is missing). If a counter goes *down* the device is assumed to have restarted and the new
   reading is taken as the amount counted since then - never a negative rate.
4. **Frame length.** The spec says 4096 bytes. The line reader tolerates up to 8192 and drops longer lines (counted as
   bad), so a slightly oversized `stats` event is not lost silently. The tool's own requests are far below 4096.
5. **`online`** is taken from the `online` flag of the `fleet` event (spec: a frame within 4.5 s). The tool does not
   run its own timeout per timer; when the connection to the remote itself is lost (unanswered pings) the dashboard
   is greyed out as a whole.
6. **Countdown ring.** `el` is "extrapolated to now" by the remote. The UI re-bases on every `fleet` event and
   interpolates at 20 Hz in between, wrapping at the phase length (`ton` or `toff`). A phase change in the next event
   simply re-synchronises the ring.
7. **REBOOTED badge.** Shown for 30 s when `boot` of a timer increases while the tool is watching (power cut /
   brownout). Purely client side.
8. **Release after hold.** On release the tool sends `mode auto`, except when the timer was `paused` before the hold:
   then it restores `mode paused`, so holding the button on a paused timer does not silently resume it. Please tell me
   if the firmware wants to define this differently.
9. **Pairing.** `pair` is sent with the MAC from the `found` event and the name typed in the list; the result is
   whatever the response and the following `fleet` event say. Timers on another channel (`found.ch` differs) are
   marked "other channel" and need the sweep option.
10. **Battery calibration helper.** `remote.mv` is used as the "live voltage" for the *Use live voltage* buttons
    (`batCal` is in millivolts). The tool assumes `mv` is the filtered battery voltage in mV, not a calibrated value.
11. **`txPower`** is shown as dBm = value / 4 (spec: 8...84 in 0.25 dBm units, 84 = 21 dBm) with the approximate mW.
12. **Direct timer (`kind:"timer"`).** The tool treats it as a fleet of one (`dev` 0), hides pairing/channel tools
    and shows `self.timer` on the Diagnostics page. `get stats` is expected to return `self.timer` as in the spec's
    last paragraph of "Performance counters".
13. **Channel move.** The tool offers "Move system to channel n" with the optional *force* checkbox (`force:true`)
    and shows `migrate` events as progress; it does not retry on its own.
14. **Factory reset of the remote** sends `{"t":"factoryReset","scope":"remote","confirm":true}`; "also reset all
    timers" uses `"scope":"all"`. Both are behind a confirmation dialog.

## Questions for the firmware side

* `cmdResult` for `dev:"all"`: one event per timer with the same `id` (as the spec says). The tool names the timer
  by the `mac` of each result and falls back to the `dev` index; it assumes `dev` is the list index at the time of
  sending (an `unpair` in between would shift it).
* `get stats` while `statsCfg` streaming is on: does it reset the streaming phase (next `stats` event one interval
  later)? The tool does not depend on it.
* `surveyCh` events: emitted as each channel finishes or all at once at the end? The tool handles both, but could show
  a progress bar if they are progressive.
* `linkTest` with `count` 200 and `gapMs` 40 takes about 8 s and the radio is busy meanwhile. Commands sent to timers in
  that time may time out; is `busy` the error code the remote returns for a second survey / link test / channel
  change while one is running? (The tool shows whatever `err`/`msg` arrives.)
* `sniff` rate limit (30/s): is it global or per direction? The Packets tab only shows what arrives.
* `hello.rst` gives the last reset reason; should the Diagnostics page also show it for the timer (`timer.rstReason`)?
  It does at the moment.

## Simulator behaviour that is an approximation

The simulator implements the protocol as written, with these model choices (all deterministic for a given `--seed`):

* Radio: loss probability follows RSSI (logistic around -83 dBm) plus interference from the neighbouring access points
  (survey scores) plus the demo "extra loss"; MAC failures are correlated with the same probability.
* Counters are monotonic since boot; `rxDrop` grows in bursts together with a loop stall (`loopMaxUs` > 20 ms);
  brownout counts grow after simulated power cuts; per-channel counters exclude channel searches; the remote polls one
  online timer per second with `GetInfo` while streaming is on.
* The timer comes back from a power cut 20 - 90 s later with `boot + 1`, and the remote calls it offline after 4.5 s.
* `reboot` / `factoryReset` of the remote drops the simulated port for about 3 s, the next session starts with a few
  plain-text ROM boot lines, and the device comes back with `boots + 1`.
* The remote event has no `adc`; `mv` follows the battery percentage along the default `batCal` points (3300/3750/4150).

## Answers from the firmware side

* `cmdResult` for `dev:"all"`: one event per timer, same `id`, with `dev` (list index when the command was *queued*) and `mac`.
  Use the `mac`; an `unpair` in between shifts the indices (the firmware remaps pending results, but the PC should prefer `mac`).
* `get stats` does not touch the `statsCfg` stream.
* `surveyCh` events are all sent **at the end** of the survey (13 of them, then `surveyDone`); the survey takes about 3 s and the
  radio is busy meanwhile.
* The error code for a second survey / link test / channel change while one is running is `busy`.
* The `sniff` limit (30 frames/s) is global, both directions together.
* `hello.rst` is the reset reason of the last boot; the timer's own `rstReason` is also in its counters (`devStat.timer.rstReason`).
* A `cmdResult` with `result:"rejected"` also means *stale*: the command was issued for an earlier boot of a timer that has been
  power‑cycled since (Toggle, SetPhase, fog‑on, Reboot, FactoryReset are bound to the boot they were issued for). Repeat the command.
* `pair` with a `name` writes that name to the timer when the timer is on the remote's channel.

