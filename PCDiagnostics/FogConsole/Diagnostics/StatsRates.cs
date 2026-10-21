using FogConsole.Protocol;

namespace FogConsole.Diagnostics;

/// <summary>Helpers for monotonic uint32 counters that restart at zero when a device reboots.</summary>
public static class CounterMath
{
    /// <summary>
    /// Difference between two readings of a counter. When the counter went backwards the device has restarted
    /// (counters count since boot), so the new reading itself is the amount counted since the restart.
    /// </summary>
    public static long Delta(long prev, long cur, out bool restarted)
    {
        if (cur >= prev)
        {
            restarted = false;
            return cur - prev;
        }
        restarted = true;
        return cur;
    }

    public static long Delta(long prev, long cur) => Delta(prev, cur, out _);

    public static double PerSecond(long delta, double seconds) => seconds > 0 ? delta / seconds : 0;

    public static double? Percent(long part, long whole) => whole > 0 ? 100.0 * part / whole : null;
}

public sealed record RemoteRates(
    double Seconds, bool Restarted,
    double Tx, double TxOk, double TxFail, double TxDrop,
    double Rx, double RxBad, double RxForeign, double RxDrop,
    double Cmds, double CmdTimeouts, double Retries,
    double PcLinesRx, double PcLinesTx,
    double? TxFailPercent);

public sealed record TimerRates(
    double Seconds, bool Restarted,
    double Rx, double RxDrop, double Tx, double TxFail, double CmdsDup, double Beats,
    double? TxFailPercent);

public sealed record DeviceRates(
    string Mac, double Seconds, bool LinkRestarted,
    double RxPerS, double ExpectedPerS, double LostPerS, double MacOkPerS, double MacFailPerS,
    double? MacFailPercent, double? LossPercent,
    TimerRates? Timer);

/// <param name="SelfTimer">Rates of the counters of a timer connected directly by USB (<c>self.timer</c>).</param>
public sealed record StatsRates(RemoteRates Remote, IReadOnlyDictionary<string, DeviceRates> Devs, TimerRates? SelfTimer = null);

/// <summary>Derives per-second rates from two consecutive <see cref="StatsSnapshot"/>s.</summary>
public static class StatsAnalyzer
{
    /// <summary>
    /// Length of the interval between two snapshots in seconds: taken from the device clock when it advanced,
    /// otherwise from the fallback (PC time between arrivals).
    /// </summary>
    public static double IntervalSeconds(StatsSnapshot prev, StatsSnapshot cur, double fallbackSeconds)
    {
        double dev = (cur.Ms - prev.Ms) / 1000.0;
        return dev > 0 ? dev : Math.Max(fallbackSeconds, 0);
    }

    public static StatsRates ComputeRates(StatsSnapshot prev, StatsSnapshot cur, double seconds)
    {
        var remote = ComputeRemote(prev.Self, cur.Self, seconds);

        var devs = new Dictionary<string, DeviceRates>(StringComparer.OrdinalIgnoreCase);
        foreach (var cd in cur.Devs)
        {
            var pd = prev.Devs.FirstOrDefault(d => string.Equals(d.Mac, cd.Mac, StringComparison.OrdinalIgnoreCase));
            if (pd is null || cd.Link is null || pd.Link is null) continue;
            devs[cd.Mac] = ComputeDevice(pd, cd, prev, cur, seconds);
        }
        TimerRates? selfTimer = null;
        if (cur.Self.Timer is { } ct && prev.Self.Timer is { } pt)
            selfTimer = ComputeTimerRates(pt, ct, seconds);
        return new StatsRates(remote, devs, selfTimer);
    }

    /// <summary>Rates of the device's own counters between two <c>stats</c> events.</summary>
    public static RemoteRates ComputeRemote(SelfStats p, SelfStats c, double seconds)
    {
        bool restarted = c.UpSeconds < p.UpSeconds || c.Radio.Tx < p.Radio.Tx || c.Radio.Rx < p.Radio.Rx;

        long dTx = CounterMath.Delta(p.Radio.Tx, c.Radio.Tx);
        long dFail = CounterMath.Delta(p.Radio.TxFail, c.Radio.TxFail);
        var remote = new RemoteRates(
            seconds, restarted,
            CounterMath.PerSecond(dTx, seconds),
            CounterMath.PerSecond(CounterMath.Delta(p.Radio.TxOk, c.Radio.TxOk), seconds),
            CounterMath.PerSecond(dFail, seconds),
            CounterMath.PerSecond(CounterMath.Delta(p.Radio.TxDrop, c.Radio.TxDrop), seconds),
            CounterMath.PerSecond(CounterMath.Delta(p.Radio.Rx, c.Radio.Rx), seconds),
            CounterMath.PerSecond(CounterMath.Delta(p.Radio.RxBad, c.Radio.RxBad), seconds),
            CounterMath.PerSecond(CounterMath.Delta(p.Radio.RxForeign, c.Radio.RxForeign), seconds),
            CounterMath.PerSecond(CounterMath.Delta(p.Radio.RxDrop, c.Radio.RxDrop), seconds),
            CounterMath.PerSecond(CounterMath.Delta(p.Link.Cmds, c.Link.Cmds), seconds),
            CounterMath.PerSecond(CounterMath.Delta(p.Link.Timeout, c.Link.Timeout), seconds),
            CounterMath.PerSecond(CounterMath.Delta(p.Link.Retries, c.Link.Retries), seconds),
            CounterMath.PerSecond(CounterMath.Delta(p.Pc.LinesRx, c.Pc.LinesRx), seconds),
            CounterMath.PerSecond(CounterMath.Delta(p.Pc.LinesTx, c.Pc.LinesTx), seconds),
            CounterMath.Percent(dFail, dTx));
        return remote;
    }

    /// <summary>Rates between two <c>devStat</c> events of the same timer (null when the remote did not send link counters).</summary>
    public static DeviceRates? ComputeDevice(DevStatsEntry prev, DevStatsEntry cur, double fallbackSeconds)
    {
        if (prev.Link is null || cur.Link is null) return null;
        double seconds = cur.Ms > prev.Ms ? (cur.Ms - prev.Ms) / 1000.0 : Math.Max(fallbackSeconds, 0);
        var none = new StatsSnapshot();
        return ComputeDevice(prev, cur, none, none, seconds);
    }

    /// <summary>
    /// Rates of timer-side counters. <paramref name="tSeconds"/> is the time between the two moments the timer produced the data.
    /// </summary>
    public static TimerRates? ComputeTimerRates(TimerStats pt, TimerStats ct, double tSeconds)
    {
        if (tSeconds <= 0.05) return null;
        bool restarted = ct.Boots != pt.Boots || ct.Rx < pt.Rx || ct.Tx < pt.Tx;
        long tTx = CounterMath.Delta(pt.Tx, ct.Tx);
        long tFail = CounterMath.Delta(pt.TxFail, ct.TxFail);
        return new TimerRates(
            tSeconds, restarted,
            CounterMath.PerSecond(CounterMath.Delta(pt.Rx, ct.Rx), tSeconds),
            CounterMath.PerSecond(CounterMath.Delta(pt.RxDrop, ct.RxDrop), tSeconds),
            CounterMath.PerSecond(tTx, tSeconds),
            CounterMath.PerSecond(tFail, tSeconds),
            CounterMath.PerSecond(CounterMath.Delta(pt.CmdsDup, ct.CmdsDup), tSeconds),
            CounterMath.PerSecond(CounterMath.Delta(pt.Beats, ct.Beats), tSeconds),
            CounterMath.Percent(tFail, tTx));
    }

    private static DeviceRates ComputeDevice(DevStatsEntry pd, DevStatsEntry cd, StatsSnapshot prev, StatsSnapshot cur, double seconds)
    {
        var pl = pd.Link!;
        var cl = cd.Link!;
        bool linkRestarted = cl.Rx < pl.Rx || cl.Expected < pl.Expected;
        long dRx = CounterMath.Delta(pl.Rx, cl.Rx);
        long dExp = CounterMath.Delta(pl.Expected, cl.Expected);
        long dLost = CounterMath.Delta(pl.Lost, cl.Lost);
        long dOk = CounterMath.Delta(pl.MacOk, cl.MacOk);
        long dFail = CounterMath.Delta(pl.MacFail, cl.MacFail);

        TimerRates? timer = null;
        if (cd.Timer is { } ct && pd.Timer is { } pt)
        {
            // The timer counters travel through GetInfo and can be several seconds old. Use the time the
            // timer produced the data, not the time the devStat event was sent.
            long curMs = cd.Ms > 0 ? cd.Ms : cur.Ms;
            long prevMs = pd.Ms > 0 ? pd.Ms : prev.Ms;
            double tSeconds = ((curMs - ct.AgeMs) - (prevMs - pt.AgeMs)) / 1000.0;
            timer = ComputeTimerRates(pt, ct, tSeconds);
        }

        return new DeviceRates(
            cd.Mac, seconds, linkRestarted,
            CounterMath.PerSecond(dRx, seconds), CounterMath.PerSecond(dExp, seconds), CounterMath.PerSecond(dLost, seconds),
            CounterMath.PerSecond(dOk, seconds), CounterMath.PerSecond(dFail, seconds),
            CounterMath.Percent(dFail, dOk + dFail), CounterMath.Percent(dLost, dExp),
            timer);
    }
}
