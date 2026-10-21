using System.Text.Json.Nodes;
using FogConsole.Protocol;

namespace FogConsole.Simulator;

public sealed partial class SimDevice
{
    private static JsonObject RstJson(long poweron, long brownout, long wdt, long panic, long sw, long deepsleep, long other) => new()
    {
        ["poweron"] = poweron, ["brownout"] = brownout, ["wdt"] = wdt, ["panic"] = panic, ["sw"] = sw, ["deepsleep"] = deepsleep, ["other"] = other,
    };

    /// <summary>Gives the radio counters a plausible history (the remote has been running for a while).</summary>
    private void PreloadCounters()
    {
        if (!IsRemote) return;
        _chan[_homeCh] = new ChanCounters { Tx = 1100, Fail = 15, Rx = 3300 };
        int other = _homeCh == 1 ? 11 : 1;
        _chan[other] = new ChanCounters { Tx = 12, Fail = 2, Rx = 0 };
        _tx = 1112;
        _txFail = 17;
        _txOk = _tx - _txFail;
        _rx = 3300;
        _rxBad = 2;
        _cmds = 40;
        _cmdsOk = 38;
        _cmdsTimeout = 2;
        _retries = 11;
        _rttSum = 9.4 * 38;
        _rttN = 38;
        _rttMax = 88;
        _sweeps = 3;
        _recovers = 1;
        _foundCount = 1;
    }

    /// <summary>The <c>stats</c> event: counters of the device itself (<c>self</c>). Per-timer counters travel in <c>devStat</c> events.</summary>
    private JsonObject BuildStats()
    {
        var o = new JsonObject { ["t"] = "stats", ["ms"] = _now };
        long loopsPerS = 800 + (long)(30 * Math.Sin(_now / 3000.0));

        if (!IsRemote)
        {
            var t = _timers[0];
            var timer = BuildTimerInfo(t);
            timer.Remove("age"); // a directly connected timer reports its own counters live
            o["self"] = new JsonObject
            {
                ["kind"] = "timer", ["up"] = (long)(t.UptimeMs / 1000), ["boots"] = t.Boot, ["heap"] = t.Heap, ["heapMin"] = t.HeapMin,
                ["loopMaxUs"] = t.C.LoopMaxUs, ["loopsPerS"] = loopsPerS, ["rstReason"] = t.RstReason,
                ["rst"] = RstJson(t.RstPowerOn, t.RstBrownout, t.RstWdt, t.RstPanic, t.RstSw, t.RstDeepSleep, t.RstOther),
                ["radio"] = new JsonObject
                {
                    ["ch"] = _homeCh, ["tx"] = t.C.Tx, ["txOk"] = t.C.Tx - t.C.TxFail, ["txFail"] = t.C.TxFail, ["txWd"] = 0, ["txDrop"] = t.C.TxDrop,
                    ["rx"] = t.C.Rx, ["rxBad"] = t.C.RxBad, ["rxForeign"] = t.C.RxForeign, ["rxDrop"] = t.C.RxDrop,
                },
                ["pc"] = new JsonObject { ["linesRx"] = _pcLinesRx, ["linesTx"] = _pcLinesTx, ["badJson"] = _pcBadJson, ["txDrop"] = _pcTxDrop },
                ["timer"] = timer,
            };
            return o;
        }

        var chan = new JsonArray();
        if (_chan.TryGetValue(_homeCh, out var home))
            chan.Add(new JsonObject { ["ch"] = _homeCh, ["tx"] = home.Tx, ["fail"] = home.Fail, ["rx"] = home.Rx });
        foreach (var (ch, c) in _chan)
            if (ch != _homeCh)
                chan.Add(new JsonObject { ["ch"] = ch, ["tx"] = c.Tx, ["fail"] = c.Fail, ["rx"] = c.Rx });

        o["self"] = new JsonObject
        {
            ["kind"] = "remote", ["up"] = (_now - _bootAtMs) / 1000, ["boots"] = _boots, ["heap"] = _heap, ["heapMin"] = _heapMin,
            ["loopMaxUs"] = _loopMaxUs, ["loopsPerS"] = loopsPerS, ["rstReason"] = _rstReason,
            ["rst"] = RstJson(_rstPowerOn, _rstBrownout, _rstWdt, _rstPanic, _rstSw, _rstDeepSleep, _rstOther),
            ["radio"] = new JsonObject
            {
                ["ch"] = _homeCh, ["tx"] = _tx, ["txOk"] = _txOk, ["txFail"] = _txFail, ["txWd"] = _txWd, ["txDrop"] = _txDrop,
                ["rx"] = _rx, ["rxBad"] = _rxBad, ["rxForeign"] = _rxForeign, ["rxDrop"] = _rxDrop,
            },
            ["chan"] = chan,
            ["link"] = new JsonObject
            {
                ["cmds"] = _cmds, ["ok"] = _cmdsOk, ["timeout"] = _cmdsTimeout, ["rejected"] = _cmdsRejected, ["retries"] = _retries,
                ["rttAvg"] = _rttN == 0 ? 0 : Math.Round(_rttSum / _rttN, 1), ["rttMax"] = Math.Round(_rttMax, 1),
                ["sweeps"] = _sweeps, ["recovers"] = _recovers, ["found"] = _foundCount, ["migrations"] = _migrations,
            },
            ["pc"] = new JsonObject { ["linesRx"] = _pcLinesRx, ["linesTx"] = _pcLinesTx, ["badJson"] = _pcBadJson, ["txDrop"] = _pcTxDrop },
        };
        return o;
    }

    /// <summary>The <c>devStat</c> event for one paired timer (link counters kept by the remote plus the timer's own counters).</summary>
    private JsonObject BuildDevStat(int index, SimTimer t, bool timerAnswered = true)
    {
        var l = t.L;
        double loss = t.LossPercentWindow;
        return new JsonObject
        {
            ["t"] = "devStat",
            ["ms"] = _now,
            ["i"] = index,
            ["mac"] = t.Mac,
            ["link"] = new JsonObject
            {
                ["rx"] = l.Rx, ["expected"] = l.Expected, ["lost"] = l.Lost, ["loss"] = loss < 0 ? -1 : (int)Math.Round(loss), ["dups"] = l.Dups,
                ["macOk"] = l.MacOk, ["macFail"] = l.MacFail, ["cmds"] = l.Cmds, ["cmdsOk"] = l.CmdsOk, ["cmdsTimeout"] = l.CmdsTimeout,
                ["retries"] = l.Retries, ["rttAvg"] = Math.Round(l.RttAvg, 1), ["rttMax"] = Math.Round(l.RttMax, 1),
                ["rssiMin"] = l.RssiMin, ["rssiAvg"] = l.RssiAvg, ["rssiMax"] = l.RssiMax,
                ["offlineEvents"] = l.OfflineEvents, ["offlineMs"] = l.OfflineMs, ["reboots"] = l.Reboots,
            },
            ["timer"] = timerAnswered ? AgedInfo(t) : null,
        };
    }

    /// <summary>Emits a <c>stats</c> event followed by one <c>devStat</c> event per paired timer.</summary>
    private void EmitStats()
    {
        Emit(BuildStats());
        if (!IsRemote) return;
        for (int i = 0; i < _paired.Count; i++) Emit(BuildDevStat(i, _paired[i]));
    }

    private JsonObject? AgedInfo(SimTimer t)
    {
        if (t.InfoJson is null) return null;
        var copy = t.InfoJson.DeepClone().AsObject();
        copy["age"] = Math.Max(0, _now - t.InfoAtMs);
        return copy;
    }

    /// <summary>What a timer answers to <c>GetInfo</c>.</summary>
    private JsonObject BuildTimerInfo(SimTimer t)
    {
        var c = t.C;
        t.Heap = 200_000 + _rng.Next(-800, 800);
        t.HeapMin = Math.Min(t.HeapMin, t.Heap - _rng.Next(0, 200));
        return new JsonObject
        {
            ["age"] = 0, ["fw"] = t.Fw, ["up"] = (long)(t.UptimeMs / 1000), ["boots"] = t.Boot, ["rstReason"] = t.RstReason,
            ["heap"] = t.Heap, ["heapMin"] = t.HeapMin, ["loopMaxUs"] = c.LoopMaxUs, ["ch"] = t.Channel, ["net"] = _settings.Net,
            ["rst"] = RstJson(t.RstPowerOn, t.RstBrownout, t.RstWdt, t.RstPanic, t.RstSw, t.RstDeepSleep, t.RstOther),
            ["rx"] = c.Rx, ["rxBad"] = c.RxBad, ["rxForeign"] = c.RxForeign, ["rxDrop"] = c.RxDrop, ["rxProbes"] = c.RxProbes, ["rxCmds"] = c.RxCmds,
            ["tx"] = c.Tx, ["txFail"] = c.TxFail, ["txDrop"] = c.TxDrop, ["beats"] = c.Beats, ["replies"] = c.Replies,
            ["cmdsRun"] = c.CmdsRun, ["cmdsDup"] = c.CmdsDup, ["cmdsRejected"] = c.CmdsRejected,
            ["rssiMin"] = c.RssiMin, ["rssiAvg"] = c.RssiAvg, ["rssiMax"] = c.RssiMax, ["rssiLast"] = c.RssiLast,
        };
    }

    private void HandleDevStats(int id, JsonObject req)
    {
        if (!ResolveOne(id, req, out var t)) return;
        bool reachable = IsOnline(t) && t.Powered && t.Channel == _homeCh;
        bool delivered = false;
        for (int attempt = 0; attempt < 3 && reachable && !delivered; attempt++) delivered = Unicast(t, "INFO");
        if (delivered)
        {
            Schedule(80 + _rng.Next(0, 220), () =>
            {
                t.InfoJson = BuildTimerInfo(t);
                t.InfoAtMs = _now;
                Reply(id, r => r["devStat"] = BuildDevStat(IndexOf(t), t));
            });
        }
        else
        {
            // the timer did not answer within 3 s: devStat.timer is null
            Schedule(3000, () => Reply(id, r => r["devStat"] = BuildDevStat(IndexOf(t), t, timerAnswered: false)));
        }
    }
}
