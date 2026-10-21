using FogConsole.Protocol;

namespace FogConsole.Diagnostics;

public sealed record ChannelQualityRow(int Channel, long Tx, long Fail, long Rx)
{
    public double? FailPercent => CounterMath.Percent(Fail, Tx);
}

/// <summary>
/// Accumulates the remote's per-channel counters (<c>self.chan</c>) over time so different channels can be compared,
/// even across reboots of the remote (counters restart at zero) and across several channel changes.
/// </summary>
public sealed class ChannelQualityAccumulator
{
    private readonly Dictionary<int, (long Tx, long Fail, long Rx)> _last = new();
    private readonly SortedDictionary<int, (long Tx, long Fail, long Rx)> _total = new();

    /// <summary>Feeds the <c>chan</c> list of one stats event.</summary>
    public void Add(IReadOnlyList<ChannelCounters> counters)
    {
        foreach (var c in counters)
        {
            var prev = _last.TryGetValue(c.Channel, out var p) ? p : (Tx: 0L, Fail: 0L, Rx: 0L);
            long dTx = CounterMath.Delta(prev.Tx, c.Tx);
            long dFail = CounterMath.Delta(prev.Fail, c.Fail);
            long dRx = CounterMath.Delta(prev.Rx, c.Rx);
            var tot = _total.TryGetValue(c.Channel, out var t) ? t : (Tx: 0L, Fail: 0L, Rx: 0L);
            _total[c.Channel] = (tot.Tx + dTx, tot.Fail + dFail, tot.Rx + dRx);
            _last[c.Channel] = (c.Tx, c.Fail, c.Rx);
        }
    }

    /// <summary>Clears the accumulated totals; the next event only adds what happens from now on.</summary>
    public void Reset(IReadOnlyList<ChannelCounters>? current = null)
    {
        _total.Clear();
        _last.Clear();
        if (current is null) return;
        foreach (var c in current) _last[c.Channel] = (c.Tx, c.Fail, c.Rx);
    }

    public IReadOnlyList<ChannelQualityRow> Rows =>
        _total.Select(kv => new ChannelQualityRow(kv.Key, kv.Value.Tx, kv.Value.Fail, kv.Value.Rx)).ToList();
}
