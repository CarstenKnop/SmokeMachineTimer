using FogConsole.Protocol;

namespace FogConsole.Diagnostics;

public enum HealthLevel { Ok, Info, Warn, Bad }

/// <summary>A short verdict shown as a chip, e.g. "rxDrop&gt;0".</summary>
public sealed record HealthChip(HealthLevel Level, string Label, string Detail);

/// <summary>Turns performance counters into human readable verdicts.</summary>
public static class HealthRules
{
    /// <summary>A main loop pass longer than this means something blocked the radio loop.</summary>
    public const long LoopWarnUs = 20_000;

    public static IReadOnlyList<HealthChip> EvaluateRemote(SelfStats s, RemoteRates? rates)
    {
        var chips = new List<HealthChip>();

        if (s.Radio.RxDrop > 0)
        {
            bool now = rates is { RxDrop: > 0 };
            chips.Add(new HealthChip(now ? HealthLevel.Bad : HealthLevel.Warn, now ? "rxDrop rising" : $"rxDrop {s.Radio.RxDrop}",
                $"{s.Radio.RxDrop} received frames were dropped because the receive queue overflowed: the device was overloaded (CPU too busy), real packet loss inside the device."));
        }
        if (s.LoopMaxUs > LoopWarnUs)
            chips.Add(new HealthChip(HealthLevel.Warn, $"loop {s.LoopMaxUs / 1000.0:0.#} ms",
                $"The slowest main-loop pass took {s.LoopMaxUs / 1000.0:0.0} ms (> {LoopWarnUs / 1000} ms): something blocked the radio loop."));

        double? failPct = CounterMath.Percent(s.Radio.TxFail, s.Radio.Tx);
        if (failPct is { } fp && s.Radio.Tx >= 50 && fp > 10)
            chips.Add(new HealthChip(fp > 25 ? HealthLevel.Bad : HealthLevel.Warn, $"txFail {fp:0}%",
                "Many unicast frames were not acknowledged: poor link, timers on another channel or interference."));
        if (rates?.TxFailPercent is { } rp && rates.Tx >= 1 && rp > 25)
            chips.Add(new HealthChip(HealthLevel.Bad, $"txFail now {rp:0}%", "More than a quarter of the frames sent during the last interval were not acknowledged."));

        AddResetChips(chips, s.Rst);

        if (s.Pc.BadJson > 0)
            chips.Add(new HealthChip(HealthLevel.Info, $"pc badJson {s.Pc.BadJson}", "The remote received lines from the PC that were not valid JSON."));
        if (s.Pc.TxDrop > 0)
            chips.Add(new HealthChip(HealthLevel.Warn, $"pc txDrop {s.Pc.TxDrop}", "The remote could not send some lines to the PC (USB output queue full)."));
        if (s.HeapMin is > 0 and < 20_000)
            chips.Add(new HealthChip(HealthLevel.Warn, "low heap", $"The free heap dropped to {s.HeapMin} bytes."));

        if (chips.Count == 0)
            chips.Add(new HealthChip(HealthLevel.Ok, "healthy", "No dropped frames, no loop stalls, no power or crash resets."));
        return chips;
    }

    public static IReadOnlyList<HealthChip> EvaluateDevice(DevStatsEntry d, DeviceRates? rates, bool online)
    {
        var chips = new List<HealthChip>();
        var link = d.Link;
        var t = d.Timer;

        if (!online)
            chips.Add(new HealthChip(HealthLevel.Warn, "offline", "No frame heard for more than 4.5 s: powered off (heating?), out of range or on another channel."));

        if (link is not null)
        {
            double? macFail = link.MacFailPercent;
            if (macFail is { } mf && link.MacOk + link.MacFail >= 30 && mf > 10)
                chips.Add(new HealthChip(mf > 25 ? HealthLevel.Bad : HealthLevel.Warn, $"macFail {mf:0}%",
                    "Many unicast frames to this timer were not acknowledged: poor link or wrong channel."));
            else if (rates?.MacFailPercent is { } rm && rm > 25 && rates.MacFailPerS > 0.2)
                chips.Add(new HealthChip(HealthLevel.Bad, $"macFail now {rm:0}%", "Most frames sent to this timer during the last interval were not acknowledged."));

            double? lossLifetime = CounterMath.Percent(link.Lost, link.Expected);
            int loss = link.Loss >= 0 ? link.Loss : (int)Math.Round(lossLifetime ?? 0);
            if (loss >= 10)
                chips.Add(new HealthChip(loss >= 25 ? HealthLevel.Bad : HealthLevel.Warn, $"loss {loss}%",
                    "A noticeable share of this timer's frames never arrived."));

            if (link.RssiAvg != 0 && link.RssiAvg < -80)
                chips.Add(new HealthChip(HealthLevel.Warn, $"weak {link.RssiAvg} dBm", "Average signal is very weak: move the timer or the remote closer, or check the antenna."));

            if (link.OfflineEvents > 0)
                chips.Add(new HealthChip(HealthLevel.Info, $"offline x{link.OfflineEvents}", $"Went offline {link.OfflineEvents} time(s), {link.OfflineMs / 1000.0:0} s in total."));
            if (link.Reboots > 0)
                chips.Add(new HealthChip(HealthLevel.Info, $"rebooted x{link.Reboots}", "The timer lost power and restarted (boot counter increased)."));
            if (link.CmdsTimeout > 0)
                chips.Add(new HealthChip(HealthLevel.Info, $"cmd timeout x{link.CmdsTimeout}", "Commands that the timer never confirmed."));
        }

        if (t is null)
        {
            chips.Add(new HealthChip(HealthLevel.Info, "no timer data", "The timer's own counters have not arrived yet (needs the timer to be online and Diagnostics streaming)."));
        }
        else
        {
            if (t.RxDrop > 0)
            {
                bool now = rates?.Timer is { RxDrop: > 0 };
                chips.Add(new HealthChip(now ? HealthLevel.Bad : HealthLevel.Warn, now ? "rxDrop rising" : $"rxDrop {t.RxDrop}",
                    "The timer dropped received frames because its receive queue overflowed: the device is overloaded."));
            }
            if (t.LoopMaxUs > LoopWarnUs)
                chips.Add(new HealthChip(HealthLevel.Warn, $"loop {t.LoopMaxUs / 1000.0:0.#} ms", "The timer's main loop was blocked for a long time."));
            AddResetChips(chips, t.Rst);
            double? tf = CounterMath.Percent(t.TxFail, t.Tx);
            if (tf is { } p && t.Tx >= 50 && p > 10)
                chips.Add(new HealthChip(HealthLevel.Warn, $"timer txFail {p:0}%", "The timer's own frames are often not acknowledged."));
            if (t.CmdsDup > 0)
                chips.Add(new HealthChip(HealthLevel.Info, $"cmdsDup {t.CmdsDup}", "Retransmitted commands that were recognised and not executed twice (acks got lost)."));
        }

        if (chips.Count == 0 || chips.All(c => c.Level == HealthLevel.Info))
            chips.Insert(0, new HealthChip(HealthLevel.Ok, "healthy", "No dropped frames or link problems."));
        return chips;
    }

    private static void AddResetChips(List<HealthChip> chips, ResetCounts rst)
    {
        if (rst.Brownout > 0)
            chips.Add(new HealthChip(HealthLevel.Warn, $"brownout {rst.Brownout}", "The supply voltage dipped below the safe level: the power supply is sagging (weak battery, thin cable, relay inrush)."));
        if (rst.Wdt > 0)
            chips.Add(new HealthChip(HealthLevel.Bad, $"watchdog {rst.Wdt}", "The watchdog reset the device: firmware got stuck."));
        if (rst.Panic > 0)
            chips.Add(new HealthChip(HealthLevel.Bad, $"panic {rst.Panic}", "The firmware crashed (panic/exception) and restarted."));
    }

    public static HealthLevel Worst(IEnumerable<HealthChip> chips) =>
        chips.Select(c => c.Level).DefaultIfEmpty(HealthLevel.Ok).Max();
}
