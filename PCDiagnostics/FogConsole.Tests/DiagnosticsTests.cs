using System.Text.Json;
using System.Text.Json.Nodes;
using FogConsole.Diagnostics;
using FogConsole.Protocol;
using Xunit;

namespace FogConsole.Tests;

public class DiagnosticsTests
{
    private static StatsSnapshot Snap(long ms, Action<JsonObject>? tweak = null)
    {
        var o = JsonNode.Parse("""
            {"t":"stats","ms":10000,
             "self":{"kind":"remote","up":100,"boots":3,"heap":180000,"heapMin":150000,"loopMaxUs":8000,"loopsPerS":800,"rstReason":"sw",
                     "rst":{"poweron":1,"brownout":0,"wdt":0,"panic":0,"sw":2,"deepsleep":0,"other":0},
                     "radio":{"ch":6,"tx":1000,"txOk":950,"txFail":50,"txWd":0,"txDrop":0,"rx":2000,"rxBad":1,"rxForeign":0,"rxDrop":0},
                     "chan":[{"ch":6,"tx":900,"fail":30,"rx":1900},{"ch":1,"tx":100,"fail":20,"rx":100}],
                     "link":{"cmds":10,"ok":9,"timeout":1,"rejected":0,"retries":4,"rttAvg":8.0,"rttMax":50,"sweeps":1,"recovers":0,"found":0,"migrations":0},
                     "pc":{"linesRx":100,"linesTx":1000,"badJson":0,"txDrop":0}},
             "devs":[{"i":0,"mac":"AA:BB:CC:DD:EE:01",
                      "link":{"rx":900,"expected":1000,"lost":100,"loss":10,"dups":0,"macOk":400,"macFail":40,"cmds":5,"cmdsOk":5,"cmdsTimeout":0,"retries":1,"rttAvg":7.0,"rttMax":20,
                              "rssiMin":-80,"rssiAvg":-70,"rssiMax":-60,"offlineEvents":0,"offlineMs":0,"reboots":0},
                      "timer":{"age":1000,"fw":"2.0.0","up":500,"boots":4,"rstReason":"poweron","heap":200000,"heapMin":190000,"loopMaxUs":3000,"ch":6,"net":1,
                               "rst":{"poweron":4,"brownout":0,"wdt":0,"panic":0,"sw":0,"deepsleep":0,"other":0},
                               "rx":400,"rxBad":0,"rxForeign":0,"rxDrop":0,"rxProbes":0,"rxCmds":5,"tx":1000,"txFail":10,"txDrop":0,"beats":950,"replies":10,
                               "cmdsRun":5,"cmdsDup":0,"cmdsRejected":0,"rssiMin":-80,"rssiAvg":-70,"rssiMax":-60,"rssiLast":-65}}]}
            """)!.AsObject();
        o["ms"] = ms;
        tweak?.Invoke(o);
        if (o["devs"] is JsonArray devArray)
            foreach (var d in devArray)
                d!["ms"] = ms;
        var root = JsonDocument.Parse(o.ToJsonString()).RootElement;
        // the protocol delivers the device's own counters in a "stats" event and every timer in its own "devStat" event
        var snap = ProtocolParser.ParseStats(root);
        var devs = root.TryGetProperty("devs", out var arr)
            ? arr.EnumerateArray().Select(ProtocolParser.ParseDevStat).ToList()
            : new List<DevStatsEntry>();
        return snap.WithDevs(devs);
    }

    // ---- counters and rates ---------------------------------------------------------------

    [Theory]
    [InlineData(10, 25, 15, false)]
    [InlineData(10, 10, 0, false)]
    [InlineData(1000, 7, 7, true)]       // device rebooted: counters count since boot
    [InlineData(0, 0, 0, false)]
    public void Counter_delta_handles_restarts(long prev, long cur, long expected, bool restarted)
    {
        Assert.Equal(expected, CounterMath.Delta(prev, cur, out bool r));
        Assert.Equal(restarted, r);
    }

    [Fact]
    public void Per_second_and_percent_guard_against_division_by_zero()
    {
        Assert.Equal(5, CounterMath.PerSecond(10, 2));
        Assert.Equal(0, CounterMath.PerSecond(10, 0));
        Assert.Equal(0, CounterMath.PerSecond(10, -1));
        Assert.Null(CounterMath.Percent(5, 0));
        Assert.Equal(25.0, CounterMath.Percent(1, 4));
    }

    [Fact]
    public void Remote_rates_are_deltas_per_second()
    {
        var a = Snap(10_000);
        var b = Snap(12_000, o =>
        {
            var radio = o["self"]!["radio"]!;
            radio["tx"] = 1100;
            radio["txOk"] = 1040;
            radio["txFail"] = 60;
            radio["rx"] = 2300;
            radio["rxDrop"] = 4;
            o["self"]!["pc"]!["linesRx"] = 106;
        });
        double secs = StatsAnalyzer.IntervalSeconds(a, b, fallbackSeconds: 99);
        Assert.Equal(2.0, secs);

        var r = StatsAnalyzer.ComputeRates(a, b, secs).Remote;
        Assert.Equal(50, r.Tx);
        Assert.Equal(45, r.TxOk);
        Assert.Equal(5, r.TxFail);
        Assert.Equal(150, r.Rx);
        Assert.Equal(2, r.RxDrop);
        Assert.Equal(3, r.PcLinesRx);
        Assert.Equal(10.0, r.TxFailPercent);         // 10 of 100 frames in this interval
        Assert.False(r.Restarted);
    }

    [Fact]
    public void Interval_falls_back_to_pc_time_when_the_device_clock_does_not_advance()
    {
        var a = Snap(10_000);
        Assert.Equal(1.5, StatsAnalyzer.IntervalSeconds(a, Snap(10_000), 1.5));
        Assert.Equal(1.5, StatsAnalyzer.IntervalSeconds(a, Snap(500), 1.5)); // device restarted: clock went backwards
        Assert.Equal(0, StatsAnalyzer.IntervalSeconds(a, Snap(10_000), -3));
    }

    [Fact]
    public void A_restarted_remote_is_detected_and_rates_use_the_new_counters()
    {
        var a = Snap(10_000);
        var b = Snap(11_000, o =>
        {
            o["self"]!["up"] = 2;
            o["self"]!["radio"]!["tx"] = 30;
            o["self"]!["radio"]!["rx"] = 60;
        });
        var r = StatsAnalyzer.ComputeRates(a, b, 1.0).Remote;
        Assert.True(r.Restarted);
        Assert.Equal(30, r.Tx);          // counted since boot
        Assert.Equal(60, r.Rx);
        Assert.True(r.Tx >= 0 && r.TxFail >= 0);
    }

    [Fact]
    public void Device_link_rates_and_interval_percentages()
    {
        var a = Snap(10_000);
        var b = Snap(12_000, o =>
        {
            var l = o["devs"]![0]!["link"]!;
            l["rx"] = 990;
            l["expected"] = 1100;
            l["lost"] = 110;
            l["macOk"] = 460;
            l["macFail"] = 60;
        });
        var rates = StatsAnalyzer.ComputeRates(a, b, 2.0);
        var d = rates.Devs["AA:BB:CC:DD:EE:01"];
        Assert.Equal(45, d.RxPerS);
        Assert.Equal(50, d.ExpectedPerS);
        Assert.Equal(5, d.LostPerS);
        Assert.Equal(10, d.MacFailPerS);
        Assert.Equal(25.0, d.MacFailPercent!.Value, 6);                // 20 of 80
        Assert.Equal(10.0, d.LossPercent!.Value, 6);                   // 10 of 100
        Assert.False(d.LinkRestarted);
    }

    [Fact]
    public void Timer_rates_use_the_timers_own_time_base_not_the_event_time()
    {
        // the timer answered 3 s before the first event and 1 s before the second one: 8 s apart in timer time
        var a = Snap(10_000, o => o["devs"]![0]!["timer"]!["age"] = 3000);
        var b = Snap(12_000, o =>
        {
            var t = o["devs"]![0]!["timer"]!;
            t["age"] = 1000;
            t["rx"] = 480;
            t["tx"] = 1080;
            t["txFail"] = 18;
            t["beats"] = 1030;
        });
        var rates = StatsAnalyzer.ComputeRates(a, b, 2.0);
        var t = rates.Devs["AA:BB:CC:DD:EE:01"].Timer!;
        Assert.Equal(4.0, t.Seconds, 6);     // (12000-1000) - (10000-3000) = 4000 ms
        Assert.Equal(20, t.Rx);              // 80 / 4 s
        Assert.Equal(20, t.Tx);
        Assert.Equal(2, t.TxFail);
        Assert.Equal(10.0, t.TxFailPercent!.Value, 6);
    }

    [Fact]
    public void Timer_rates_are_absent_when_no_fresh_timer_data_arrived()
    {
        // the same cached GetInfo answer in two consecutive events: nothing new to compute
        var a = Snap(10_000, o => o["devs"]![0]!["timer"]!["age"] = 1000);
        var b = Snap(11_000, o => o["devs"]![0]!["timer"]!["age"] = 2000);
        Assert.Null(StatsAnalyzer.ComputeRates(a, b, 1.0).Devs["AA:BB:CC:DD:EE:01"].Timer);

        var noTimer = Snap(12_000, o => o["devs"]![0]!["timer"] = null);
        Assert.Null(StatsAnalyzer.ComputeRates(a, noTimer, 2.0).Devs["AA:BB:CC:DD:EE:01"].Timer);
    }

    [Fact]
    public void A_rebooted_timer_is_flagged_and_uses_its_new_counters()
    {
        var a = Snap(10_000, o => o["devs"]![0]!["timer"]!["age"] = 0);
        var b = Snap(12_000, o =>
        {
            var t = o["devs"]![0]!["timer"]!;
            t["age"] = 0;
            t["boots"] = 5;
            t["rx"] = 10;
            t["tx"] = 20;
            t["txFail"] = 0;
        });
        var t = StatsAnalyzer.ComputeRates(a, b, 2.0).Devs["AA:BB:CC:DD:EE:01"].Timer!;
        Assert.True(t.Restarted);
        Assert.Equal(5, t.Rx);
        Assert.Equal(10, t.Tx);
    }

    [Fact]
    public void New_devices_without_a_previous_sample_are_skipped()
    {
        var a = Snap(10_000, o => o["devs"] = new JsonArray());
        var b = Snap(11_000);
        Assert.Empty(StatsAnalyzer.ComputeRates(a, b, 1.0).Devs);
    }

    // ---- health verdicts ------------------------------------------------------------------

    [Fact]
    public void A_clean_remote_is_healthy()
    {
        var chips = HealthRules.EvaluateRemote(Snap(1).Self, null);
        var chip = Assert.Single(chips);
        Assert.Equal(HealthLevel.Ok, chip.Level);
    }

    [Fact]
    public void RxDrop_means_the_device_is_overloaded()
    {
        var s = Snap(1, o => o["self"]!["radio"]!["rxDrop"] = 12).Self;
        var chip = Assert.Single(HealthRules.EvaluateRemote(s, null), c => c.Label.StartsWith("rxDrop"));
        Assert.Equal(HealthLevel.Warn, chip.Level);
        Assert.Contains("overloaded", chip.Detail);

        var rising = new RemoteRates(1, false, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 0, 0, null);
        Assert.Equal(HealthLevel.Bad, Assert.Single(HealthRules.EvaluateRemote(s, rising), c => c.Label.StartsWith("rxDrop")).Level);
    }

    [Theory]
    [InlineData(19_999, false)]
    [InlineData(20_001, true)]
    [InlineData(65_000, true)]
    public void Loop_stalls_above_20_ms_are_flagged(long us, bool flagged)
    {
        var s = Snap(1, o => o["self"]!["loopMaxUs"] = us).Self;
        Assert.Equal(flagged, HealthRules.EvaluateRemote(s, null).Any(c => c.Label.StartsWith("loop")));
    }

    [Fact]
    public void High_tx_failure_ratio_points_to_the_link_or_the_channel()
    {
        var s = Snap(1, o =>
        {
            o["self"]!["radio"]!["tx"] = 1000;
            o["self"]!["radio"]!["txFail"] = 300;
        }).Self;
        var chip = Assert.Single(HealthRules.EvaluateRemote(s, null), c => c.Label.StartsWith("txFail"));
        Assert.Equal(HealthLevel.Bad, chip.Level);
        Assert.Contains("channel", chip.Detail);

        // few frames: no verdict yet
        var tiny = Snap(1, o => { o["self"]!["radio"]!["tx"] = 10; o["self"]!["radio"]!["txFail"] = 9; }).Self;
        Assert.DoesNotContain(HealthRules.EvaluateRemote(tiny, null), c => c.Label.StartsWith("txFail"));
    }

    [Fact]
    public void Brownouts_and_crashes_are_reported()
    {
        var s = Snap(1, o =>
        {
            o["self"]!["rst"]!["brownout"] = 2;
            o["self"]!["rst"]!["wdt"] = 1;
            o["self"]!["rst"]!["panic"] = 3;
        }).Self;
        var chips = HealthRules.EvaluateRemote(s, null);
        Assert.Contains(chips, c => c.Label == "brownout 2" && c.Level == HealthLevel.Warn && c.Detail.Contains("power supply"));
        Assert.Contains(chips, c => c.Label.StartsWith("watchdog") && c.Level == HealthLevel.Bad);
        Assert.Contains(chips, c => c.Label.StartsWith("panic") && c.Level == HealthLevel.Bad);
        Assert.Equal(HealthLevel.Bad, HealthRules.Worst(chips));
    }

    [Fact]
    public void Device_verdicts_cover_link_and_timer_side()
    {
        var snap = Snap(1, o =>
        {
            var d = o["devs"]![0]!;
            d["link"]!["macFail"] = 200;                 // 200 of 600 = 33 %
            d["link"]!["loss"] = 15;
            d["link"]!["rssiAvg"] = -84;
            d["link"]!["offlineEvents"] = 3;
            d["link"]!["offlineMs"] = 95_000;
            d["link"]!["reboots"] = 2;
            d["timer"]!["rxDrop"] = 5;
            d["timer"]!["rst"]!["brownout"] = 1;
            d["timer"]!["cmdsDup"] = 3;
            d["timer"]!["loopMaxUs"] = 30_000;
        });
        var chips = HealthRules.EvaluateDevice(snap.Devs[0], null, online: true);
        Assert.Contains(chips, c => c.Label.StartsWith("macFail") && c.Level == HealthLevel.Bad && c.Detail.Contains("wrong channel"));
        Assert.Contains(chips, c => c.Label == "loss 15%" && c.Level == HealthLevel.Warn);
        Assert.Contains(chips, c => c.Label.StartsWith("weak"));
        Assert.Contains(chips, c => c.Label == "offline x3");
        Assert.Contains(chips, c => c.Label == "rebooted x2");
        Assert.Contains(chips, c => c.Label.StartsWith("rxDrop"));
        Assert.Contains(chips, c => c.Label == "brownout 1" && c.Detail.Contains("power supply"));
        Assert.Contains(chips, c => c.Label.StartsWith("cmdsDup"));
        Assert.Contains(chips, c => c.Label.StartsWith("loop"));
        Assert.DoesNotContain(chips, c => c.Level == HealthLevel.Ok);
    }

    [Fact]
    public void A_healthy_device_gets_an_ok_chip_and_an_offline_one_a_warning()
    {
        var snap = Snap(1, o => { var l = o["devs"]![0]!["link"]!; l["macFail"] = 2; l["loss"] = 2; l["lost"] = 20; });
        Assert.Equal(HealthLevel.Ok, HealthRules.EvaluateDevice(snap.Devs[0], null, true)[0].Level);
        Assert.Contains(HealthRules.EvaluateDevice(snap.Devs[0], null, online: false), c => c.Label == "offline" && c.Level == HealthLevel.Warn);
    }

    [Fact]
    public void A_device_without_timer_counters_says_so()
    {
        var snap = Snap(1, o => o["devs"]![0]!["timer"] = null);
        Assert.Contains(HealthRules.EvaluateDevice(snap.Devs[0], null, true), c => c.Label == "no timer data");
    }

    // ---- per-channel accumulation ---------------------------------------------------------

    [Fact]
    public void Channel_quality_accumulates_deltas_over_time()
    {
        var acc = new ChannelQualityAccumulator();
        acc.Add(new[] { new ChannelCounters(6, 100, 5, 300) });
        acc.Add(new[] { new ChannelCounters(6, 150, 8, 420), new ChannelCounters(1, 10, 10, 0) });
        var rows = acc.Rows;
        var c6 = rows.Single(r => r.Channel == 6);
        Assert.Equal(150, c6.Tx);
        Assert.Equal(8, c6.Fail);
        Assert.Equal(420, c6.Rx);
        Assert.Equal(8 * 100.0 / 150, c6.FailPercent!.Value, 6);
        Assert.Equal(100.0, rows.Single(r => r.Channel == 1).FailPercent);
        Assert.Equal(new[] { 1, 6 }, rows.Select(r => r.Channel));
    }

    [Fact]
    public void Channel_quality_survives_a_remote_reboot_and_a_channel_change()
    {
        var acc = new ChannelQualityAccumulator();
        acc.Add(new[] { new ChannelCounters(6, 1000, 50, 3000) });
        // remote rebooted: counters restart from zero; the new reading is what happened since
        acc.Add(new[] { new ChannelCounters(6, 40, 1, 100) });
        // system moved to channel 11
        acc.Add(new[] { new ChannelCounters(6, 40, 1, 100), new ChannelCounters(11, 60, 0, 200) });
        var rows = acc.Rows;
        Assert.Equal(1040, rows.Single(r => r.Channel == 6).Tx);
        Assert.Equal(51, rows.Single(r => r.Channel == 6).Fail);
        Assert.Equal(60, rows.Single(r => r.Channel == 11).Tx);
        Assert.Equal(0.0, rows.Single(r => r.Channel == 11).FailPercent);
    }

    [Fact]
    public void Channel_quality_reset_starts_counting_from_now()
    {
        var acc = new ChannelQualityAccumulator();
        var now = new[] { new ChannelCounters(6, 500, 20, 900) };
        acc.Add(now);
        acc.Reset(now);
        Assert.Empty(acc.Rows);
        acc.Add(new[] { new ChannelCounters(6, 530, 22, 960) });
        var r = Assert.Single(acc.Rows);
        Assert.Equal(30, r.Tx);
        Assert.Equal(2, r.Fail);
        Assert.Equal(60, r.Rx);
    }

    [Fact]
    public void Channel_failure_percentage_is_null_without_traffic()
    {
        Assert.Null(new ChannelQualityRow(3, 0, 0, 5).FailPercent);
    }

    // ---- snapshot compare and export ------------------------------------------------------

    [Fact]
    public void Snapshot_compare_shows_deltas_since_the_baseline()
    {
        var baseline = Snap(10_000);
        var now = Snap(70_000, o =>
        {
            o["self"]!["radio"]!["tx"] = 1500;
            o["self"]!["radio"]!["txFail"] = 80;
            o["self"]!["loopMaxUs"] = 33_000;
            o["devs"]![0]!["link"]!["macFail"] = 55;
            o["devs"]![0]!["timer"]!["rst"]!["brownout"] = 1;
        });
        var rows = StatsCompare.Compare(baseline, now);
        DiffRow Row(string scope, string dev, string name) => rows.Single(r => r.Scope == scope && r.Device == dev && r.Name == name);

        var tx = Row("remote", "remote", "radio.tx");
        Assert.Equal(1000, tx.Baseline);
        Assert.Equal(1500, tx.Current);
        Assert.Equal(500, tx.Delta);
        Assert.True(tx.IsCounter);
        Assert.Equal(30, Row("remote", "remote", "radio.txFail").Delta);
        Assert.Equal(15, Row("link", "AA:BB:CC:DD:EE:01", "macFail").Delta);
        Assert.Equal(1, Row("timer", "AA:BB:CC:DD:EE:01", "rst.brownout").Delta);
        var loop = Row("remote", "remote", "loopMaxUs");
        Assert.False(loop.IsCounter);
        Assert.Equal(25_000, loop.Delta);

        var interesting = StatsCompare.Interesting(rows);
        Assert.Contains(interesting, r => r.Name == "radio.tx");
        Assert.Contains(interesting, r => r.Name == "loopMaxUs");
        Assert.DoesNotContain(interesting, r => r.Name == "radio.rxBad");   // unchanged
    }

    [Fact]
    public void Snapshot_compare_marks_counters_that_went_backwards_as_restarted()
    {
        var baseline = Snap(10_000);
        var after = Snap(20_000, o => o["self"]!["radio"]!["tx"] = 12);
        var row = StatsCompare.Compare(baseline, after).Single(r => r.Name == "radio.tx");
        Assert.True(row.Restarted);
        Assert.Equal(12, row.Delta);
    }

    [Fact]
    public void Flatten_exposes_every_counter_group()
    {
        var m = StatsFlatten.Flatten(Snap(1));
        Assert.Contains(m, x => x.Scope == "remote" && x.Name == "radio.rxDrop");
        Assert.Contains(m, x => x.Scope == "remote" && x.Name == "chan1.fail" && x.Value == 20);
        Assert.Contains(m, x => x.Scope == "remote" && x.Name == "rst.sw" && x.Value == 2);
        Assert.Contains(m, x => x.Scope == "link" && x.Name == "offlineMs");
        Assert.Contains(m, x => x.Scope == "timer" && x.Name == "cmdsDup");
        Assert.Contains(m, x => x.Scope == "timer" && x.Name == "rst.brownout");
        Assert.DoesNotContain(m, x => x.Name == "rssiAvg" && x.IsCounter);
    }

    [Fact]
    public void Json_export_contains_raw_stats_rates_verdicts_and_the_channel_table()
    {
        var a = Snap(10_000);
        var b = Snap(12_000, o => o["self"]!["radio"]!["tx"] = 1100);
        var rates = StatsAnalyzer.ComputeRates(a, b, 2.0);
        var acc = new ChannelQualityAccumulator();
        acc.Add(b.Self.Chan);

        var json = StatsExport.ToJson(b, rates, acc.Rows, new DateTimeOffset(2026, 10, 20, 12, 0, 0, TimeSpan.Zero));
        var root = JsonNode.Parse(json)!.AsObject();
        Assert.StartsWith("2026-10-20T12:00:00", root["exportedAt"]!.GetValue<string>());
        Assert.Equal(12_000, root["stats"]!["ms"]!.GetValue<long>());
        Assert.Equal(50, root["rates"]!["remote"]!["tx"]!.GetValue<double>());
        Assert.Equal(2, root["channelTable"]!.AsArray().Count);
        Assert.NotNull(root["verdicts"]!["remote"]);
        Assert.NotNull(root["verdicts"]!["AA:BB:CC:DD:EE:01"]);
    }

    [Fact]
    public void Csv_export_is_long_format_with_rates()
    {
        var a = Snap(10_000);
        var b = Snap(12_000, o => o["self"]!["radio"]!["tx"] = 1100);
        var csv = StatsExport.ToCsv(b, a, 2.0, new DateTimeOffset(2026, 10, 20, 12, 0, 0, TimeSpan.Zero),
            new Dictionary<string, string> { ["AA:BB:CC:DD:EE:01"] = "Fog, one" });
        var lines = csv.Split('\n', StringSplitOptions.RemoveEmptyEntries).Select(l => l.TrimEnd('\r')).ToList();
        Assert.Equal("time,scope,device,name,metric,value,perSecond", lines[0]);
        var tx = lines.Single(l => l.Contains(",remote,remote,,radio.tx,"));
        Assert.EndsWith(",1100,50", tx);
        var named = lines.First(l => l.Contains("AA:BB:CC:DD:EE:01"));
        Assert.Contains("\"Fog, one\"", named);                 // commas inside names are quoted
        Assert.All(lines.Skip(1), l => Assert.Equal(7, SplitCsv(l).Count));
    }

    private static List<string> SplitCsv(string line)
    {
        var parts = new List<string>();
        var cur = new System.Text.StringBuilder();
        bool quoted = false;
        foreach (char c in line)
        {
            if (c == '"') quoted = !quoted;
            else if (c == ',' && !quoted) { parts.Add(cur.ToString()); cur.Clear(); }
            else cur.Append(c);
        }
        parts.Add(cur.ToString());
        return parts;
    }
}
