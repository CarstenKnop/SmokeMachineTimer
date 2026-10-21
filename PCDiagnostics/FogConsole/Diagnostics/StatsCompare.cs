using System.Globalization;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using FogConsole.Protocol;

namespace FogConsole.Diagnostics;

/// <summary>One number out of a stats snapshot.</summary>
public sealed record StatMetric(string Scope, string Device, string Name, double Value, bool IsCounter);

/// <summary>Flattens a <see cref="StatsSnapshot"/> into named metrics (used by compare and export).</summary>
public static class StatsFlatten
{
    public static IReadOnlyList<StatMetric> Flatten(StatsSnapshot s)
    {
        var list = new List<StatMetric>();
        void Add(string scope, string dev, string name, double v, bool counter = true) => list.Add(new StatMetric(scope, dev, name, v, counter));

        var self = s.Self;
        const string R = "remote";
        Add(R, R, "up", self.UpSeconds, false);
        Add(R, R, "boots", self.Boots, false);
        Add(R, R, "heap", self.Heap, false);
        Add(R, R, "heapMin", self.HeapMin, false);
        Add(R, R, "loopMaxUs", self.LoopMaxUs, false);
        Add(R, R, "loopsPerS", self.LoopsPerS, false);
        AddRst(list, R, R, "rst", self.Rst);
        var r = self.Radio;
        Add(R, R, "radio.tx", r.Tx);
        Add(R, R, "radio.txOk", r.TxOk);
        Add(R, R, "radio.txFail", r.TxFail);
        Add(R, R, "radio.txWd", r.TxWd);
        Add(R, R, "radio.txDrop", r.TxDrop);
        Add(R, R, "radio.rx", r.Rx);
        Add(R, R, "radio.rxBad", r.RxBad);
        Add(R, R, "radio.rxForeign", r.RxForeign);
        Add(R, R, "radio.rxDrop", r.RxDrop);
        foreach (var c in self.Chan)
        {
            Add(R, R, $"chan{c.Channel}.tx", c.Tx);
            Add(R, R, $"chan{c.Channel}.fail", c.Fail);
            Add(R, R, $"chan{c.Channel}.rx", c.Rx);
        }
        var l = self.Link;
        Add(R, R, "link.cmds", l.Cmds);
        Add(R, R, "link.ok", l.Ok);
        Add(R, R, "link.timeout", l.Timeout);
        Add(R, R, "link.rejected", l.Rejected);
        Add(R, R, "link.retries", l.Retries);
        Add(R, R, "link.rttAvg", l.RttAvg, false);
        Add(R, R, "link.rttMax", l.RttMax, false);
        Add(R, R, "link.sweeps", l.Sweeps);
        Add(R, R, "link.recovers", l.Recovers);
        Add(R, R, "link.found", l.Found);
        Add(R, R, "link.migrations", l.Migrations);
        Add(R, R, "pc.linesRx", self.Pc.LinesRx);
        Add(R, R, "pc.linesTx", self.Pc.LinesTx);
        Add(R, R, "pc.badJson", self.Pc.BadJson);
        Add(R, R, "pc.txDrop", self.Pc.TxDrop);

        if (self.Timer is { } st) AddTimer(list, "timer", "self", st);

        foreach (var d in s.Devs)
        {
            string dev = d.Mac;
            if (d.Link is { } k)
            {
                Add("link", dev, "rx", k.Rx);
                Add("link", dev, "expected", k.Expected);
                Add("link", dev, "lost", k.Lost);
                Add("link", dev, "loss", k.Loss, false);
                Add("link", dev, "dups", k.Dups);
                Add("link", dev, "macOk", k.MacOk);
                Add("link", dev, "macFail", k.MacFail);
                Add("link", dev, "cmds", k.Cmds);
                Add("link", dev, "cmdsOk", k.CmdsOk);
                Add("link", dev, "cmdsTimeout", k.CmdsTimeout);
                Add("link", dev, "retries", k.Retries);
                Add("link", dev, "rttAvg", k.RttAvg, false);
                Add("link", dev, "rttMax", k.RttMax, false);
                Add("link", dev, "rssiMin", k.RssiMin, false);
                Add("link", dev, "rssiAvg", k.RssiAvg, false);
                Add("link", dev, "rssiMax", k.RssiMax, false);
                Add("link", dev, "offlineEvents", k.OfflineEvents);
                Add("link", dev, "offlineMs", k.OfflineMs);
                Add("link", dev, "reboots", k.Reboots);
            }
            if (d.Timer is { } t) AddTimer(list, "timer", dev, t);
        }
        return list;
    }

    private static void AddTimer(List<StatMetric> list, string scope, string dev, TimerStats t)
    {
        void Add(string name, double v, bool counter = true) => list.Add(new StatMetric(scope, dev, name, v, counter));
        Add("age", t.AgeMs, false);
        Add("up", t.UpSeconds, false);
        Add("boots", t.Boots, false);
        Add("heap", t.Heap, false);
        Add("heapMin", t.HeapMin, false);
        Add("loopMaxUs", t.LoopMaxUs, false);
        AddRst(list, scope, dev, "rst", t.Rst);
        Add("rx", t.Rx);
        Add("rxBad", t.RxBad);
        Add("rxForeign", t.RxForeign);
        Add("rxDrop", t.RxDrop);
        Add("rxProbes", t.RxProbes);
        Add("rxCmds", t.RxCmds);
        Add("tx", t.Tx);
        Add("txFail", t.TxFail);
        Add("txDrop", t.TxDrop);
        Add("beats", t.Beats);
        Add("replies", t.Replies);
        Add("cmdsRun", t.CmdsRun);
        Add("cmdsDup", t.CmdsDup);
        Add("cmdsRejected", t.CmdsRejected);
        Add("rssiMin", t.RssiMin, false);
        Add("rssiAvg", t.RssiAvg, false);
        Add("rssiMax", t.RssiMax, false);
        Add("rssiLast", t.RssiLast, false);
    }

    private static void AddRst(List<StatMetric> list, string scope, string dev, string prefix, ResetCounts r)
    {
        // lifetime counters: not reset by a reboot, but they do count up, so they are "counters"
        list.Add(new StatMetric(scope, dev, $"{prefix}.poweron", r.PowerOn, true));
        list.Add(new StatMetric(scope, dev, $"{prefix}.brownout", r.Brownout, true));
        list.Add(new StatMetric(scope, dev, $"{prefix}.wdt", r.Wdt, true));
        list.Add(new StatMetric(scope, dev, $"{prefix}.panic", r.Panic, true));
        list.Add(new StatMetric(scope, dev, $"{prefix}.sw", r.Sw, true));
        list.Add(new StatMetric(scope, dev, $"{prefix}.deepsleep", r.DeepSleep, true));
        list.Add(new StatMetric(scope, dev, $"{prefix}.other", r.Other, true));
    }
}

public sealed record DiffRow(string Scope, string Device, string Name, double Baseline, double Current, double Delta, bool IsCounter, bool Restarted);

/// <summary>"Snapshot compare": what changed between a stored baseline and now.</summary>
public static class StatsCompare
{
    public static IReadOnlyList<DiffRow> Compare(StatsSnapshot baseline, StatsSnapshot current)
    {
        var b = StatsFlatten.Flatten(baseline).ToDictionary(m => (m.Scope, m.Device, m.Name));
        var rows = new List<DiffRow>();
        foreach (var m in StatsFlatten.Flatten(current))
        {
            if (!b.TryGetValue((m.Scope, m.Device, m.Name), out var old)) continue;
            bool restarted = false;
            double delta;
            if (m.IsCounter)
            {
                delta = CounterMath.Delta((long)old.Value, (long)m.Value, out restarted);
            }
            else
            {
                delta = m.Value - old.Value;
            }
            rows.Add(new DiffRow(m.Scope, m.Device, m.Name, old.Value, m.Value, delta, m.IsCounter, restarted));
        }
        return rows;
    }

    /// <summary>The most interesting rows only (counters that changed, plus peak values).</summary>
    public static IReadOnlyList<DiffRow> Interesting(IEnumerable<DiffRow> rows) =>
        rows.Where(r => r.IsCounter ? r.Delta != 0 : r.Name is "loopMaxUs" or "rttMax" && r.Delta != 0).ToList();
}

public static class StatsExport
{
    private static readonly JsonSerializerOptions JsonOpts = new()
    {
        WriteIndented = true,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
    };

    /// <summary>The latest raw stats object plus derived rates, verdicts and the per-channel table.</summary>
    public static string ToJson(StatsSnapshot snap, StatsRates? rates, IEnumerable<ChannelQualityRow>? channelTable, DateTimeOffset at)
    {
        var root = new JsonObject
        {
            ["exportedAt"] = at.ToString("o", CultureInfo.InvariantCulture),
            ["stats"] = snap.Raw.ValueKind == JsonValueKind.Undefined ? null : JsonNode.Parse(snap.Raw.GetRawText()),
        };
        if (snap.Devs.Count > 0) root["devStats"] = JsonSerializer.SerializeToNode(snap.Devs, JsonOpts);
        if (rates is not null) root["rates"] = JsonSerializer.SerializeToNode(rates, JsonOpts);
        if (channelTable is not null) root["channelTable"] = JsonSerializer.SerializeToNode(channelTable.Select(r => new { r.Channel, r.Tx, r.Fail, r.Rx, failPercent = r.FailPercent }), JsonOpts);
        var verdicts = new JsonObject
        {
            ["remote"] = ChipsToJson(HealthRules.EvaluateRemote(snap.Self, rates?.Remote)),
        };
        foreach (var d in snap.Devs)
        {
            DeviceRates? devRates = null;
            rates?.Devs.TryGetValue(d.Mac, out devRates);
            verdicts[d.Mac] = ChipsToJson(HealthRules.EvaluateDevice(d, devRates, online: true));
        }
        root["verdicts"] = verdicts;
        return root.ToJsonString(JsonOpts);
    }

    private static JsonArray ChipsToJson(IEnumerable<HealthChip> chips)
    {
        var arr = new JsonArray();
        foreach (var c in chips) arr.Add(new JsonObject { ["level"] = c.Level.ToString(), ["label"] = c.Label, ["detail"] = c.Detail });
        return arr;
    }

    /// <summary>Long-format CSV (one metric per line); <paramref name="previous"/> adds a per-second rate column for counters.</summary>
    public static string ToCsv(StatsSnapshot snap, StatsSnapshot? previous, double seconds, DateTimeOffset at, IReadOnlyDictionary<string, string>? deviceNames = null)
    {
        var sb = new StringBuilder();
        sb.AppendLine("time,scope,device,name,metric,value,perSecond");
        var prev = previous is null
            ? new Dictionary<(string, string, string), StatMetric>()
            : StatsFlatten.Flatten(previous).ToDictionary(m => (m.Scope, m.Device, m.Name));
        string time = at.ToString("o", CultureInfo.InvariantCulture);
        foreach (var m in StatsFlatten.Flatten(snap))
        {
            string rate = "";
            if (m.IsCounter && prev.TryGetValue((m.Scope, m.Device, m.Name), out var old) && seconds > 0)
                rate = (CounterMath.Delta((long)old.Value, (long)m.Value) / seconds).ToString("0.###", CultureInfo.InvariantCulture);
            string name = deviceNames is not null && deviceNames.TryGetValue(m.Device, out var n) ? n : "";
            sb.Append(time).Append(',').Append(m.Scope).Append(',').Append(m.Device).Append(',').Append(Csv(name)).Append(',')
              .Append(m.Name).Append(',').Append(m.Value.ToString("0.###", CultureInfo.InvariantCulture)).Append(',').Append(rate).AppendLine();
        }
        return sb.ToString();
    }

    private static string Csv(string s) => s.Contains(',') || s.Contains('"') ? "\"" + s.Replace("\"", "\"\"") + "\"" : s;
}
