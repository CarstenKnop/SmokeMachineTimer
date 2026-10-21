using System.Text.Json.Nodes;
using FogConsole.Protocol;

namespace FogConsole.Simulator;

public sealed partial class SimDevice
{
    private sealed class ChanCounters
    {
        public long Tx, Fail, Rx;
    }

    private sealed class SimSettings
    {
        public int TxPower = 80;
        public int Brightness = 200;
        public int AutoOff = 60;
        public int RssiLow = -90;
        public int RssiHigh = -50;
        public int[] BatCal = { 3300, 3750, 4150 };
        public int Net = 1;
    }

    // ---- self state -----------------------------------------------------------------------

    private readonly SimSettings _settings = new();
    private int _boots = 12;
    private string _rstReason = "deepsleep";
    private long _rstPowerOn = 3, _rstBrownout = 0, _rstWdt = 0, _rstPanic = 0, _rstSw = 5, _rstDeepSleep = 40, _rstOther = 0;
    private long _heap = 181_000, _heapMin = 150_000, _loopMaxUs = 8_200;
    private long _bootAtMs;
    private long _tx, _txOk, _txFail, _txWd, _txDrop, _rx, _rxBad, _rxForeign, _rxDrop;
    private long _logTx, _logTxOk, _logTxFail, _logRx;                // snapshot for the debug summary line
    private readonly SortedDictionary<int, ChanCounters> _chan = new();
    private long _cmds, _cmdsOk, _cmdsTimeout, _cmdsRejected, _retries;
    private double _rttSum, _rttMax;
    private long _rttN;
    private long _sweeps, _recovers, _foundCount, _migrations;
    private long _pcLinesRx, _pcLinesTx, _pcBadJson, _pcTxDrop;
    private double _battery = 87.0;

    // PC session
    private long _lastPcMs = -1_000_000;
    private int _logLevel = -1;   // -1 = off
    private bool _sniff;
    private int _statsInterval;
    private long _nextStatsMs;
    private long _nextInfoPollMs;
    private int _infoPollCursor;
    private long _nextFleetMs;
    private long _lastFleetEmit;
    private long _nextRemoteMs;
    private bool _fleetDirty;

    private bool PcPresent => _now - _lastPcMs < 6000;

    // activities that occupy the radio
    private long _radioBusyUntil;
    private bool _scanFlag;       // survey/recover in progress
    private bool _discoverOn, _discoverSweep;
    private long _nextDiscoverMs;
    private int _sweepCh = 1;
    private long _nextSweepHopMs;

    public int HomeChannel { get { lock (_lock) return _homeCh; } }

    private void InitSelf()
    {
        _bootAtMs = _now;
    }

    private static readonly double[,] DemoTimes =
    {
        { 1.5, 10 }, { 1.5, 10 }, { 2, 20 }, { 0.5, 5 }, { 1, 10 },
        { 2.5, 15 }, { 1, 12 }, { 3, 30 }, { 1.5, 8 }, { 0.8, 6 },
    };

    private static readonly int[] DemoRssi = { -52, -58, -61, -67, -49, -79, -72, -64, -55, -70 };

    private void BuildTimers()
    {
        if (_opt.Kind == SimKind.Timer)
        {
            var t = new SimTimer(ToWireMac(1), "FogTimer") { Channel = _homeCh, Boot = 7, Slot = 0, RstPowerOn = 7 };
            t.Ota = new SimOtaReceiver(_opt.OtaKey);
            t.Phase = CyclePhase.Off;
            t.UptimeMs = 3_600_000;
            t.C.AddRssi(-58);
            t.NextBeatMs = _now + 100;
            _timers.Add(t);
            return;
        }

        int total = Math.Clamp(_opt.TotalTimers, 1, 13);
        int pairedCount = Math.Clamp(_opt.PairedTimers, 0, Math.Min(total, MaxDevs));
        int otherCh = _homeCh + 5 <= 13 ? _homeCh + 5 : _homeCh - 5;
        for (int i = 0; i < total; i++)
        {
            bool paired = i < pairedCount;
            var t = new SimTimer(ToWireMac(i + 1), paired ? $"Fog{i + 1}" : "FogTimer")
            {
                Ton = DemoTimes[i % 10, 0],
                Toff = DemoTimes[i % 10, 1],
                Channel = _homeCh,
                Boot = 1 + _rng.Next(1, 9),
                BaseRssi = DemoRssi[i % 10] + _rng.Next(-2, 3),
                RssiPhase = _rng.NextDouble() * 6.28,
                UptimeMs = (long)(_rng.Next(5, 90) * 60_000.0),
                NextBeatMs = _now + _rng.Next(0, 500),
                Slot = i,
            };
            t.BaseRssiT = t.BaseRssi + _rng.Next(-3, 4);
            t.Ota = new SimOtaReceiver(_opt.OtaKey);
            if (paired && i == _opt.OtaNoSlotIndex) t.Ota.HasSlot = false;
            if (paired && i == _opt.OtaLegacyIndex) { t.Fw = "2.0.0"; t.Ota.Supported = false; }
            t.RstPowerOn = t.Boot;
            t.RstBrownout = t.Boot > 3 ? _rng.Next(0, 2) : 0;
            t.RstSw = _rng.Next(0, 3);
            t.RstWdt = 0;
            t.Phase = _rng.Next(2) == 0 ? CyclePhase.On : CyclePhase.Off;
            t.ElapsedMs = _rng.NextDouble() * t.PhaseDurationMs * 0.9;
            t.Heap = 200_000 + _rng.Next(0, 4000);
            t.HeapMin = t.Heap - _rng.Next(2000, 20000);
            t.C.LoopMaxUs = 2500 + _rng.Next(0, 1800);
            // some history so that counters do not start at zero
            long seconds = (long)(t.UptimeMs / 1000);
            t.C.Beats = seconds * 2;
            t.C.Tx = seconds * 2 + seconds / 2;
            t.C.Rx = seconds;
            t.C.RxProbes = seconds / 20;
            t.C.RxCmds = seconds / 40;
            t.C.Replies = seconds / 40;
            t.C.CmdsRun = t.C.RxCmds - 1;
            t.C.AddRssi((int)t.BaseRssiT);
            if (i == 5) t.ExtraLoss = 0.03; // the "far away" machine
            if (!paired && i == total - 1) t.Channel = otherCh;
            _timers.Add(t);
            if (paired) _paired.Add(t);
        }

        // initial remote-side knowledge: everything that is powered has just been heard
        foreach (var t in _paired)
        {
            t.EverHeard = true;
            t.LastHeardMs = _now - 100;
            t.WasOnline = true;
            t.LastRssi = (int)t.BaseRssi;
            t.LastRssiT = (int)t.BaseRssiT;
            t.L.Expected = (long)(t.UptimeMs / 500);
            t.L.Rx = (long)(t.L.Expected * (1 - LossProbability(t)));
            t.L.MacOk = (long)(t.UptimeMs / 1000);
            t.L.MacFail = (long)(t.L.MacOk * LossProbability(t) * 0.6);
            t.L.AddRssi((int)t.BaseRssi - 3);
            t.L.AddRssi((int)t.BaseRssi + 4);
            t.L.AddRssi((int)t.BaseRssi);
            t.L.OfflineEvents = t.Boot > 4 ? _rng.Next(0, 3) : 0;
            t.L.OfflineMs = t.L.OfflineEvents * 40_000;
            t.L.Reboots = Math.Max(0, t.Boot - 2);
            for (int k = 0; k < 32; k++) t.RecordFrame(_rng.NextDouble() < LossProbability(t));
            RefreshKnown(t);
        }

        if (_opt.Demo && _paired.Count >= 4)
        {
            // Fog4 is already heating up: powered off, last seen 12 s ago.
            var heating = _paired[3];
            heating.PowerCut(45_000, brownout: false);
            heating.LastHeardMs = _now - 12_000;
            heating.WasOnline = false;
            heating.L.OfflineEvents++;
            heating.OfflineSinceMs = _now - 8_500;
        }
        if (_opt.Demo && _paired.Count >= 2)
        {
            // Fog2 loses power 3 s after start for 12 s and comes back with boot+1 (brownout).
            var victim = _paired[1];
            Schedule(3_000, () => victim.PowerCut(12_000, brownout: true));
        }
    }

    private void RefreshKnown(SimTimer t)
    {
        var k = t.Known;
        k.Phase = t.Phase;
        k.ElapsedMs = t.ElapsedMs;
        k.On = t.OutputOn;
        k.Mode = t.Mode;
        k.Ton = t.Ton;
        k.Toff = t.Toff;
        k.Boot = t.Boot;
        k.UpS = (long)(t.UptimeMs / 1000);
        k.Fw = t.Fw;
        k.Channel = t.Channel;
        k.AtMs = _now;
    }

    // ---- main step ------------------------------------------------------------------------

    private void StepRemote()
    {
        StepTimerEngines();
        StepBeats();
        StepOnline();
        StepPolls();
        StepOta();
        StepDiscovery();
        StepInfoPoll();
        StepMisc();
        StepEmit();
    }

    private bool Chance(double p) => _rng.NextDouble() < p;

    private void StepTimerEngines()
    {
        double perStep = _opt.PowerCutsPerTimerPerMinute / 60.0 / (1000.0 / StepMs);
        foreach (var t in _timers)
        {
            if (!t.Powered)
            {
                t.PowerOffMs -= StepMs;
                if (t.PowerOffMs <= 0)
                {
                    t.Boot_(_now, t.NextBootReason);
                    t.NextBeatMs = _now + 150 + _rng.Next(0, 300);
                    t.LastRssiT = (int)t.BaseRssiT;
                }
                continue;
            }

            bool changed = t.StepCycle(StepMs);
            if (changed && _paired.Contains(t) && IsOnline(t)) _fleetDirty = true;

            if (_opt.Chaos && Chance(perStep))
                t.PowerCut(20_000 + _rng.Next(0, 70_000), brownout: Chance(0.25));
        }
    }

    // ---- radio model ----------------------------------------------------------------------

    private double LossProbability(SimTimer t)
    {
        double r = t.BaseRssi;
        double p = 0.004 + 0.55 / (1.0 + Math.Exp((r + 83.0) / 3.2));
        p += _band.InterferenceLoss(_homeCh) + t.ExtraLoss + _extraLoss;
        return Math.Clamp(p, 0.0, 0.9);
    }

    private int SampleRssi(SimTimer t, double baseRssi)
    {
        double drift = 2.5 * Math.Sin(_now / 1000.0 / 18.0 + t.RssiPhase);
        return (int)Math.Clamp(Math.Round(baseRssi + drift + NextGaussian() * 1.6), -98, -30);
    }

    private ChanCounters Chan(int ch)
    {
        if (!_chan.TryGetValue(ch, out var c)) _chan[ch] = c = new ChanCounters();
        return c;
    }

    private double Rtt()
    {
        double u = 1.0 - _rng.NextDouble();
        double rtt = 4.0 + -Math.Log(u) * 4.5;
        if (Chance(0.03)) rtt *= 4;
        return Math.Round(rtt, 1);
    }

    /// <summary>One unicast frame remote → timer. Updates counters on both ends. Returns true when the MAC ack arrived.</summary>
    private bool Unicast(SimTimer t, string kind, int? onChannel = null)
    {
        int ch = onChannel ?? _homeCh;
        _tx++;
        // per-channel counters describe normal operation on the home channel; channel searches are excluded
        var cc = onChannel is null ? Chan(ch) : null;
        if (cc is not null) cc.Tx++;
        bool reachable = t.Powered && t.Channel == ch;
        bool ok = reachable && _rng.NextDouble() >= LossProbability(t) * 0.6;
        if (ok)
        {
            _txOk++;
            t.L.MacOk++;
            t.C.Rx++;
            if (kind == "CMD") t.C.RxCmds++;
            else if (kind is "PING" or "INFO") t.C.RxProbes++;
            int rssiT = SampleRssi(t, t.BaseRssiT);
            t.C.AddRssi(rssiT);
            t.LastRssiT = rssiT;
        }
        else
        {
            _txFail++;
            if (cc is not null) cc.Fail++;
            t.L.MacFail++;
        }
        SniffFrame("tx", t.Mac, kind, kind == "CMD" ? 28 : 16, ok ? t.LastRssiT : 0);
        return ok;
    }

    private void StepBeats()
    {
        foreach (var t in _timers)
        {
            if (!t.Powered || _now < t.NextBeatMs) continue;
            t.NextBeatMs += 500 + _rng.Next(-30, 31);
            t.C.Beats++;
            t.C.Tx++;
            bool paired = _paired.Contains(t);
            if (t.Channel != _homeCh) continue;               // nobody is listening on its channel
            if (_now < _radioBusyUntil) continue;             // our radio is busy with a survey / sweep
            if (!paired) continue;                            // not our timer: only heard during discovery

            t.L.Expected++;
            bool lost = _rng.NextDouble() < LossProbability(t);
            t.RecordFrame(lost);
            if (lost)
            {
                if (Chance(0.2)) t.C.TxFail++;
                Log(3, "radio", $"T{IndexOf(t)} status beat missed (loss {t.LossPercentWindow:0}%)");
                continue;
            }

            _rx++;
            Chan(_homeCh).Rx++;
            t.L.Rx++;
            int rssi = SampleRssi(t, t.BaseRssi);
            t.LastRssi = rssi;
            t.L.AddRssi(rssi);

            var k = t.Known;
            bool wasOnline = IsOnline(t);
            if (k.Boot != 0 && t.Boot != k.Boot) { t.L.Reboots++; _fleetDirty = true; }
            if (k.Phase != t.Phase || k.Mode != t.Mode || k.On != t.OutputOn || !wasOnline) _fleetDirty = true;
            t.EverHeard = true;
            t.LastHeardMs = _now;
            RefreshKnown(t);
            SniffFrame("rx", t.Mac, "STATUS", 46, rssi);
        }
    }

    private void StepOnline()
    {
        foreach (var t in _paired)
        {
            bool online = IsOnline(t);
            int idx = IndexOf(t);
            if (online && !t.WasOnline)
            {
                t.WasOnline = true;
                _fleetDirty = true;
                Log(2, "link", $"T{idx} online ch{t.Channel} boot {t.Boot}");
            }
            else if (!online && t.WasOnline)
            {
                t.WasOnline = false;
                t.L.OfflineEvents++;
                _fleetDirty = true;
                Log(1, "link", $"T{idx} offline (no frame for 4.5 s)");
            }
            if (!online && t.EverHeard) t.L.OfflineMs += StepMs;
        }
    }

    private void StepPolls()
    {
        if (_now < _radioBusyUntil) return;
        foreach (var t in _paired)
        {
            if (!IsOnline(t)) continue;
            // one poll per second per timer, staggered by slot
            if (((_now / StepMs) + t.Slot * 2) % (1000 / StepMs) != 0) continue;
            if (Unicast(t, "PING"))
            {
                t.C.Replies++;
                t.C.Tx++;
                _rx++;
                Chan(_homeCh).Rx++;
            }
        }
    }

    // ---- discovery ------------------------------------------------------------------------

    private void StepDiscovery()
    {
        if (!_discoverOn || _opt.Kind == SimKind.Timer) return;

        if (_discoverSweep && _now >= _nextSweepHopMs)
        {
            _nextSweepHopMs = _now + 300;
            _sweepCh = _sweepCh % 13 + 1;
            _tx++;
            if (_sweepCh == 1) _sweeps++;
            foreach (var t in _timers)
                if (t.Powered && t.Channel == _sweepCh && t.Channel != _homeCh && _sweepCh != _homeCh)
                    AnnounceFound(t, _sweepCh);
        }

        if (_now >= _nextDiscoverMs)
        {
            _nextDiscoverMs = _now + 1000;
            foreach (var t in _timers)
                if (t.Powered && t.Channel == _homeCh)
                    AnnounceFound(t, _homeCh);
        }
    }

    private void AnnounceFound(SimTimer t, int ch)
    {
        _rx++;
        if (ch == _homeCh) Chan(ch).Rx++;
        int rssi = SampleRssi(t, t.BaseRssi);
        Emit(new JsonObject
        {
            ["t"] = "found", ["mac"] = t.Mac, ["name"] = t.Name, ["rssi"] = rssi, ["ch"] = ch,
            ["paired"] = IndexOf(t), ["ton"] = t.Ton, ["toff"] = t.Toff, ["on"] = t.OutputOn ? 1 : 0, ["boot"] = t.Boot,
        });
        SniffFrame("rx", t.Mac, "ANNOUNCE", 32, rssi);
    }

    // ---- GetInfo polling (statsCfg) -------------------------------------------------------

    private void StepInfoPoll()
    {
        if (_statsInterval <= 0 || _now < _nextInfoPollMs || _paired.Count == 0) return;
        _nextInfoPollMs = _now + 1000;
        for (int n = 0; n < _paired.Count; n++)
        {
            var t = _paired[_infoPollCursor % _paired.Count];
            _infoPollCursor++;
            if (!IsOnline(t)) continue;
            if (Unicast(t, "INFO") || Unicast(t, "INFO"))
            {
                t.C.Replies++;
                t.C.Tx++;
                _rx++;
                Chan(_homeCh).Rx++;
                t.InfoJson = BuildTimerInfo(t);
                t.InfoAtMs = _now;
            }
            break;
        }
    }

    // ---- misc: battery, hiccups -----------------------------------------------------------

    private void StepMisc()
    {
        _battery = Math.Max(0, _battery - 0.011 * StepMs / 1000.0);

        if (_now % 5000 == 0)
        {
            _loopMaxUs = Math.Max(_loopMaxUs, 2500 + _rng.Next(0, 6000));
            _heap = 181_000 + _rng.Next(-1500, 1500);
            _heapMin = Math.Min(_heapMin, _heap - _rng.Next(0, 300));

            // chatter of a device running at debug level: a radio summary every 5 s, health every 15 s
            Log(3, "radio", $"5 s: tx {_tx - _logTx} ack {_txOk - _logTxOk} fail {_txFail - _logTxFail} rx {_rx - _logRx}");
            _logTx = _tx; _logTxOk = _txOk; _logTxFail = _txFail; _logRx = _rx;
            if (_now % 15_000 == 0) Log(3, "sys", $"heap {_heap} B (min {_heapMin}), loop max {_loopMaxUs / 1000.0:0.0} ms");
        }

        if (!_opt.Chaos) return;
        double perStep = StepMs / 1000.0;
        if (Chance(perStep / 90.0))
        {
            // the remote's own loop stalled: receive queue overflowed
            long dropped = _rng.Next(2, 15);
            _loopMaxUs = Math.Max(_loopMaxUs, 21_000 + _rng.Next(0, 40_000));
            _rxDrop += dropped;
            Log(1, "radio", $"rx queue overflow, {dropped} frames dropped");
        }
        foreach (var t in _timers)
        {
            if (!t.Powered || !Chance(perStep / 150.0)) continue;
            t.C.LoopMaxUs = Math.Max(t.C.LoopMaxUs, 21_000 + _rng.Next(0, 30_000));
            t.C.RxDrop += _rng.Next(1, 8);
        }
    }

    // ---- emission -------------------------------------------------------------------------

    private void StepEmit()
    {
        if (!PcPresent) return;
        if (_now >= _nextFleetMs || (_fleetDirty && _now - _lastFleetEmit >= 100)) EmitFleet();
        if (_now >= _nextRemoteMs) EmitRemote();
        if (_statsInterval > 0 && _now >= _nextStatsMs)
        {
            _nextStatsMs = _now + _statsInterval;
            EmitStats();
        }
    }

    private void OnSessionStart()
    {
        _nextFleetMs = _now;
        _nextRemoteMs = _now;
        _nextStatsMs = _now + Math.Max(_statsInterval, 500);
        _fleetDirty = true;
        Log(2, "usb", "PC session started");
    }

    private JsonObject FleetEntry(int i, SimTimer t)
    {
        bool online = IsOnline(t);
        var k = t.Known;
        double el = online ? t.ElapsedMs : k.ElapsedMs;
        int age = t.EverHeard ? (int)Math.Min(_now - t.LastHeardMs, 999_999) : 999_999;
        double loss = t.LossPercentWindow;
        return new JsonObject
        {
            ["i"] = i,
            ["mac"] = t.Mac,
            ["name"] = t.Name,
            ["online"] = online ? 1 : 0,
            ["on"] = online && t.OutputOn ? 1 : 0,
            ["ph"] = (online ? t.Phase : k.Phase) == CyclePhase.On ? "on" : "off",
            ["mode"] = ProtocolParser.ModeToWire(online ? t.Mode : k.Mode),
            ["ton"] = Math.Round(online ? t.Ton : k.Ton, 1),
            ["toff"] = Math.Round(online ? t.Toff : k.Toff, 1),
            ["el"] = Math.Round(el / 1000.0, 2),
            ["rssi"] = t.EverHeard ? t.LastRssi : 0,
            ["rssiT"] = t.EverHeard ? t.LastRssiT : 0,
            ["age"] = age,
            ["loss"] = loss < 0 ? -1 : (int)Math.Round(loss),
            ["ch"] = k.Channel == 0 ? t.Channel : k.Channel,
            ["boot"] = k.Boot,
            ["up"] = online ? (long)(t.UptimeMs / 1000) : k.UpS,
            ["fw"] = k.Fw,
            ["pend"] = t.Jobs.Count,
            ["id"] = t.Slot,
        };
    }

    private void EmitFleet()
    {
        _nextFleetMs = _now + 500;
        _lastFleetEmit = _now;
        _fleetDirty = false;
        var devs = new JsonArray();
        if (_opt.Kind == SimKind.Timer)
        {
            var t = _timers[0];
            devs.Add(new JsonObject
            {
                ["i"] = 0, ["mac"] = t.Mac, ["name"] = t.Name, ["online"] = 1, ["on"] = t.OutputOn ? 1 : 0,
                ["ph"] = t.Phase == CyclePhase.On ? "on" : "off", ["mode"] = ProtocolParser.ModeToWire(t.Mode),
                ["ton"] = Math.Round(t.Ton, 1), ["toff"] = Math.Round(t.Toff, 1), ["el"] = Math.Round(t.ElapsedMs / 1000.0, 2),
                ["rssi"] = 0, ["rssiT"] = 0, ["age"] = 0, ["loss"] = -1, ["ch"] = _homeCh, ["boot"] = t.Boot,
                ["up"] = (long)(t.UptimeMs / 1000), ["fw"] = t.Fw, ["pend"] = t.Jobs.Count, ["id"] = 0,
            });
        }
        else
        {
            for (int i = 0; i < _paired.Count; i++) devs.Add(FleetEntry(i, _paired[i]));
        }
        Emit(new JsonObject
        {
            ["t"] = "fleet", ["seq"] = ++_fleetSeq, ["active"] = _paired.Count == 0 ? -1 : Math.Min(_active, _paired.Count - 1),
            ["ch"] = _homeCh, ["devs"] = devs,
        });
    }

    /// <summary>Battery voltage in mV, consistent with the calibration points (mv0, mv50, mv100).</summary>
    private int CurrentMv()
    {
        var c = _settings.BatCal;
        double b = _battery;
        double mv = b <= 50 ? c[0] + (c[1] - c[0]) * b / 50.0 : c[1] + (c[2] - c[1]) * (b - 50) / 50.0;
        return (int)Math.Round(mv + NextGaussian() * 3);
    }

    private void EmitRemote()
    {
        _nextRemoteMs = _now + 1000;
        if (_opt.Kind == SimKind.Timer)
        {
            Emit(new JsonObject { ["t"] = "remote", ["up"] = _now / 1000, ["heap"] = _heap, ["ch"] = _homeCh, ["net"] = _settings.Net });
            return;
        }
        int bat = (int)Math.Round(_battery);
        int txq = _paired.Sum(p => p.Jobs.Count);
        Emit(new JsonObject
        {
            ["t"] = "remote", ["bat"] = bat, ["mv"] = CurrentMv(), ["chg"] = 0, ["usb"] = 1,
            ["ch"] = _homeCh, ["net"] = _settings.Net, ["up"] = (_now - _bootAtMs) / 1000, ["heap"] = _heap,
            ["menu"] = "main", ["blank"] = 0, ["scan"] = (_scanFlag || _discoverOn || _now < _radioBusyUntil) ? 1 : 0,
            ["txq"] = txq,
        });
    }
}
