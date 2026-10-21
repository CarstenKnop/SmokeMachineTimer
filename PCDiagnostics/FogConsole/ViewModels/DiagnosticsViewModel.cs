using System.Collections.ObjectModel;
using System.Globalization;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using FogConsole.Diagnostics;
using FogConsole.Hub;
using FogConsole.Protocol;
using FogConsole.Services;

namespace FogConsole.ViewModels;

/// <summary>One counter tile: name, value, optional per-second rate and a health colour.</summary>
public sealed partial class MetricViewModel : ObservableObject
{
    public MetricViewModel(string name) => Name = name;

    public string Name { get; }

    [ObservableProperty] private string _value = "-";
    [ObservableProperty] private string _rate = "";
    [ObservableProperty] private HealthLevel _level = HealthLevel.Ok;
    [ObservableProperty] private string _hint = "";

    public bool HasRate => Rate.Length > 0;
    partial void OnRateChanged(string value) => OnPropertyChanged(nameof(HasRate));
}

public sealed class MetricGroup
{
    private readonly Dictionary<string, MetricViewModel> _byName = new();
    public ObservableCollection<MetricViewModel> Items { get; } = new();

    public void Set(string name, string value, string rate = "", HealthLevel level = HealthLevel.Ok, string hint = "")
    {
        if (!_byName.TryGetValue(name, out var m))
        {
            m = new MetricViewModel(name);
            _byName[name] = m;
            Items.Add(m);
        }
        m.Value = value;
        m.Rate = rate;
        m.Level = level;
        m.Hint = hint;
    }

    public void Clear()
    {
        _byName.Clear();
        Items.Clear();
    }
}

public sealed record ChannelQualityRowViewModel(int Channel, long Tx, long Fail, long Rx, double? FailPercent, bool IsHome, double BarFraction)
{
    public string FailText => FailPercent is { } p ? $"{p:0.#}%" : "-";
    public string TxText => Fmt.Count(Tx);
    public string FailCountText => Fmt.Count(Fail);
    public string RxText => Fmt.Count(Rx);
    public HealthLevel Level => FailPercent is null ? HealthLevel.Ok : FailPercent > 25 ? HealthLevel.Bad : FailPercent > 10 ? HealthLevel.Warn : HealthLevel.Ok;
    public string ChannelText => IsHome ? $"{Channel}  (home)" : Channel.ToString();
}

public sealed record DiffRowViewModel(string Scope, string Device, string Name, string Baseline, string Current, string Delta, bool Increased);

/// <summary>Counters of one paired timer: link statistics kept by the remote plus the timer's own counters.</summary>
public sealed partial class DeviceDiagnosticsViewModel : ObservableObject
{
    private const int HistoryPoints = 300;
    private readonly List<double> _loss = new(), _rssi = new(), _failRate = new();
    private DevStatsEntry? _prev;
    private double _prevAt;

    public DeviceDiagnosticsViewModel(string mac) => Mac = mac;

    public string Mac { get; }
    public MetricGroup Link { get; } = new();
    public MetricGroup Timer { get; } = new();

    [ObservableProperty] private string _name = "";
    [ObservableProperty] private int _index;
    [ObservableProperty] private bool _online;
    [ObservableProperty] private IReadOnlyList<HealthChip> _chips = Array.Empty<HealthChip>();
    [ObservableProperty] private IReadOnlyList<double> _lossHistory = Array.Empty<double>();
    [ObservableProperty] private IReadOnlyList<double> _rssiHistory = Array.Empty<double>();
    [ObservableProperty] private IReadOnlyList<double> _failRateHistory = Array.Empty<double>();
    [ObservableProperty] private string _timerAgeText = "no data yet";
    [ObservableProperty] private bool _hasTimer;
    [ObservableProperty] private HealthLevel _worst = HealthLevel.Ok;
    [ObservableProperty] private string _lossNow = "-";
    [ObservableProperty] private string _rssiNow = "-";
    [ObservableProperty] private string _failNow = "-";

    public void Update(DevStatsEntry e, double now, string name, bool online, int fleetRssi)
    {
        Name = name;
        Index = e.Index;
        Online = online;

        DeviceRates? rates = null;
        if (_prev is not null) rates = StatsAnalyzer.ComputeDevice(_prev, e, now - _prevAt);
        _prev = e;
        _prevAt = now;

        if (e.Link is { } l)
        {
            Link.Set("Loss", l.Loss >= 0 ? $"{l.Loss}%" : "-", rates?.LossPercent is { } lp ? $"{lp:0.#}% now" : "",
                l.Loss >= 25 ? HealthLevel.Bad : l.Loss >= 10 ? HealthLevel.Warn : HealthLevel.Ok, "Percent of this timer's frames lost over the last 32 expected.");
            Link.Set("Lost / expected", $"{Fmt.Count(l.Lost)} / {Fmt.Count(l.Expected)}", rates is null ? "" : $"{Fmt.Rate(rates.LostPerS)} lost", HealthLevel.Ok,
                "Frames missing from the timer's frame counter versus frames expected.");
            var mf = l.MacFailPercent;
            Link.Set("MAC fail", mf is { } m ? $"{m:0.#}%" : "-", rates?.MacFailPercent is { } rmf ? $"{rmf:0.#}% now" : "",
                mf is > 25 ? HealthLevel.Bad : mf is > 10 ? HealthLevel.Warn : HealthLevel.Ok,
                $"{Fmt.Count(l.MacFail)} of {Fmt.Count(l.MacOk + l.MacFail)} unicast frames to this timer were not acknowledged. A healthy link has almost none.");
            Link.Set("RSSI min / avg / max", l.RssiAvg == 0 ? "-" : $"{l.RssiMin} / {l.RssiAvg} / {l.RssiMax}", "", l.RssiAvg is < 0 and < -80 ? HealthLevel.Warn : HealthLevel.Ok, "dBm at the remote");
            Link.Set("Offline", $"{l.OfflineEvents}x · {Fmt.Duration(l.OfflineMs / 1000)}", "", HealthLevel.Ok, "How often the timer went offline and for how long in total.");
            Link.Set("Reboots", l.Reboots.ToString(), "", HealthLevel.Ok, "Times the timer restarted (lost power).");
            Link.Set("Commands", $"{l.CmdsOk}/{l.Cmds}", l.CmdsTimeout > 0 ? $"{l.CmdsTimeout} timeout" : "", l.CmdsTimeout > 0 ? HealthLevel.Warn : HealthLevel.Ok, "Commands confirmed / submitted");
            Link.Set("Retries / dups", $"{l.Retries} / {l.Dups}", "", HealthLevel.Ok, "Extra transmissions, and duplicate frames received");
            Link.Set("RTT avg / max", l.RttAvg > 0 ? $"{l.RttAvg:0.#} / {l.RttMax:0} ms" : "-", "", HealthLevel.Ok, "From first send to the echo");
            LossNow = l.Loss >= 0 ? $"{l.Loss}%" : "-";
            RssiNow = fleetRssi != 0 ? $"{fleetRssi} dBm" : "-";
            FailNow = rates is null ? "-" : $"{rates.MacFailPerS:0.0}/s";

            Add(_loss, l.Loss >= 0 ? l.Loss : double.NaN);
            Add(_rssi, fleetRssi != 0 ? fleetRssi : double.NaN);
            Add(_failRate, rates?.MacFailPerS ?? 0);
            LossHistory = _loss.ToArray();
            RssiHistory = _rssi.ToArray();
            FailRateHistory = _failRate.ToArray();
        }

        HasTimer = e.Timer is not null;
        if (e.Timer is { } t)
        {
            TimerAgeText = t.AgeMs < 1500 ? "just now" : $"{t.AgeMs / 1000.0:0} s old";
            var tr = rates?.Timer;
            Timer.Set("Received", Fmt.Count(t.Rx), tr is null ? "" : Fmt.Rate(tr.Rx), HealthLevel.Ok, "Valid frames the timer received.");
            Timer.Set("Rx bad / foreign", $"{t.RxBad} / {t.RxForeign}", "", HealthLevel.Ok, "CRC errors / frames of other networks.");
            Timer.Set("Rx drop", Fmt.Count(t.RxDrop), tr is { RxDrop: > 0 } ? Fmt.Rate(tr.RxDrop) : "", t.RxDrop > 0 ? (tr is { RxDrop: > 0 } ? HealthLevel.Bad : HealthLevel.Warn) : HealthLevel.Ok,
                "Receive queue overflows inside the timer: it was too busy to keep up.");
            Timer.Set("Sent", Fmt.Count(t.Tx), tr is null ? "" : Fmt.Rate(tr.Tx), HealthLevel.Ok, "Frames the timer transmitted (heartbeats and replies).");
            var tf = CounterMath.Percent(t.TxFail, t.Tx);
            Timer.Set("Tx fail", $"{Fmt.Count(t.TxFail)}" + (tf is { } p ? $" ({p:0.#}%)" : ""), tr?.TxFailPercent is { } rp ? $"{rp:0.#}% now" : "", tf is > 10 ? HealthLevel.Warn : HealthLevel.Ok,
                "Frames the timer sent that nobody acknowledged (broadcasts are not acknowledged).");
            Timer.Set("Cmds dup", Fmt.Count(t.CmdsDup), "", HealthLevel.Ok, "Retransmitted commands that were recognised and not executed twice.");
            Timer.Set("Cmds run / rejected", $"{t.CmdsRun} / {t.CmdsRejected}", "", t.CmdsRejected > 0 ? HealthLevel.Warn : HealthLevel.Ok);
            Timer.Set("Loop max", $"{t.LoopMaxUs / 1000.0:0.#} ms", "", t.LoopMaxUs > HealthRules.LoopWarnUs ? HealthLevel.Warn : HealthLevel.Ok, "Slowest main-loop pass since boot. Above 20 ms something blocked the radio loop.");
            Timer.Set("Brownouts", t.Rst.Brownout.ToString(), "", t.Rst.Brownout > 0 ? HealthLevel.Warn : HealthLevel.Ok, "Resets caused by a supply voltage dip (power supply sagging).");
            Timer.Set("Power-ons", t.Rst.PowerOn.ToString(), "", HealthLevel.Ok, "Cold starts after a power cut.");
            Timer.Set("Watchdog / panic", $"{t.Rst.Wdt} / {t.Rst.Panic}", "", t.Rst.Wdt + t.Rst.Panic > 0 ? HealthLevel.Bad : HealthLevel.Ok, "Firmware crashes.");
            Timer.Set("Uptime", Fmt.Duration(t.UpSeconds), "", HealthLevel.Ok, $"Boot #{t.Boots}, last reset: {t.RstReason}");
            Timer.Set("Heap (min)", $"{t.Heap / 1000.0:0} ({t.HeapMin / 1000.0:0}) kB", "", HealthLevel.Ok);
            Timer.Set("RSSI at timer", t.RssiAvg == 0 ? "-" : $"{t.RssiMin} / {t.RssiAvg} / {t.RssiMax}", t.RssiLast == 0 ? "" : $"last {t.RssiLast}", HealthLevel.Ok, "Signal of the remote's frames as the timer hears them (min / avg / max dBm).");
        }
        else
        {
            TimerAgeText = "no data yet";
        }

        var chips = HealthRules.EvaluateDevice(e, rates, online);
        Chips = chips;
        Worst = HealthRules.Worst(chips);
    }

    private static void Add(List<double> list, double v)
    {
        list.Add(v);
        if (list.Count > HistoryPoints) list.RemoveAt(0);
    }
}

/// <summary>The Diagnostics page: performance counters of the remote and of every timer, rates, verdicts, channel quality, compare.</summary>
public sealed partial class DiagnosticsViewModel : ObservableObject, IPageViewModel, IMessageSink
{
    private const int StreamIntervalMs = 1000;

    private readonly AppServices _app;
    private readonly ChannelQualityAccumulator _channels = new();
    private readonly Dictionary<string, DevStatsEntry> _devEntries = new(StringComparer.OrdinalIgnoreCase);
    private StatsSnapshot? _latest;
    private StatsSnapshot? _previous;
    private StatsRates? _rates;
    private StatsSnapshot? _baseline;
    private bool _active;
    private double _lastStatsAt = double.MinValue;
    private double _prevStatsAt;
    private double _lastCompareAt;
    private readonly List<double> _remoteFailHistory = new();

    public DiagnosticsViewModel(AppServices app)
    {
        _app = app;
    }

    public NavPage Page => NavPage.Diagnostics;
    public FleetState Fleet => _app.Fleet;

    public MetricGroup Radio { get; } = new();
    public MetricGroup Sys { get; } = new();
    public MetricGroup Resets { get; } = new();
    public MetricGroup LinkSummary { get; } = new();
    public MetricGroup Pc { get; } = new();
    public MetricGroup SelfTimer { get; } = new();
    public ObservableCollection<DeviceDiagnosticsViewModel> Devices { get; } = new();
    public ObservableCollection<ChannelQualityRowViewModel> ChannelRows { get; } = new();
    public ObservableCollection<DiffRowViewModel> CompareRows { get; } = new();

    [ObservableProperty] private IReadOnlyList<HealthChip> _remoteChips = Array.Empty<HealthChip>();
    [ObservableProperty] private HealthLevel _remoteWorst = HealthLevel.Ok;
    [ObservableProperty] private string _remoteTitle = "Remote";
    [ObservableProperty] private string _statusText = "Waiting for the first counters...";
    [ObservableProperty] private bool _hasData;
    [ObservableProperty] private bool _hasBaseline;
    [ObservableProperty] private string _baselineText = "No baseline stored.";
    [ObservableProperty] private IReadOnlyList<double> _remoteFailHistoryValues = Array.Empty<double>();
    [ObservableProperty] private string _remoteFailNow = "-";
    [ObservableProperty] private bool _isDirectTimer;
    [ObservableProperty] private bool _hasSelfTimer;

    public bool IsConnected => _app.Fleet.IsLive;
    public bool HasDevices => Devices.Count > 0;
    public bool HasChannelRows => ChannelRows.Count > 0;

    // ---- streaming control -----------------------------------------------------------------

    public void Activate()
    {
        _active = true;
        _ = SendStatsCfgAsync(StreamIntervalMs);
        OnPropertyChanged(nameof(IsConnected));
    }

    public void Deactivate()
    {
        _active = false;
        _ = SendStatsCfgAsync(0);
    }

    /// <summary>After a (re-)connection: restart the stream if the page is visible.</summary>
    public Task ApplySessionAsync()
    {
        _devEntries.Clear();
        _previous = _latest = null;
        _rates = null;
        return _active ? SendStatsCfgAsync(StreamIntervalMs) : Task.CompletedTask;
    }

    private async Task SendStatsCfgAsync(int intervalMs)
    {
        if (!_app.IsConnected) return;
        try
        {
            var res = await _app.Hub.RequestAsync(Requests.StatsCfg(intervalMs), retries: 1).ConfigureAwait(true);
            if (!res.Ok && intervalMs > 0) _app.Toasts.Warning($"Could not start the counter stream: {res.Describe()}");
        }
        catch (HubException)
        {
            // connection problems are shown by the connection chip
        }
    }

    [RelayCommand]
    private async Task RefreshNow()
    {
        if (!_app.IsConnected) return;
        try
        {
            await _app.Hub.RequestAsync(Requests.GetStats(), retries: 1).ConfigureAwait(true);
        }
        catch (HubException ex)
        {
            _app.Toasts.Warning($"Could not read the counters: {ex.Message}");
        }
    }

    // ---- messages --------------------------------------------------------------------------

    public void OnMessage(Message message)
    {
        switch (message)
        {
            case StatsMessage s:
                OnStats(s.Snapshot);
                break;
            case DevStatMessage d:
                OnDevStat(d.Entry);
                break;
        }
    }

    private void OnStats(StatsSnapshot snap)
    {
        double now = _app.Clock.Seconds;
        var prev = _latest;
        var full = snap.WithDevs(CurrentDevs());
        _previous = prev;
        _latest = full;
        _lastStatsAt = now;
        HasData = true;
        IsDirectTimer = snap.Self.Kind == DeviceKind.Timer;
        RemoteTitle = IsDirectTimer ? "Timer connected by USB" : "Remote";

        RemoteRates? r = null;
        if (prev is not null)
        {
            double secs = StatsAnalyzer.IntervalSeconds(prev, snap, now - _prevStatsAt);
            r = StatsAnalyzer.ComputeRemote(prev.Self, snap.Self, secs);
            TimerRates? selfTimer = IsDirectTimer && prev.Self.Timer is not null && snap.Self.Timer is not null
                ? StatsAnalyzer.ComputeTimerRates(prev.Self.Timer, snap.Self.Timer, secs)
                : null;
            _rates = new StatsRates(r, _rates?.Devs ?? new Dictionary<string, DeviceRates>(), selfTimer);
        }
        _prevStatsAt = now;

        BuildRemoteMetrics(snap.Self, r);
        var chips = HealthRules.EvaluateRemote(snap.Self, r);
        RemoteChips = chips;
        RemoteWorst = HealthRules.Worst(chips);

        _channels.Add(snap.Self.Chan);
        RebuildChannelRows(snap.Self.Radio.Channel);

        if (r is not null)
        {
            _remoteFailHistory.Add(r.TxFail);
            if (_remoteFailHistory.Count > 300) _remoteFailHistory.RemoveAt(0);
            RemoteFailHistoryValues = _remoteFailHistory.ToArray();
            RemoteFailNow = $"{r.TxFail:0.0}/s";
        }

        HasSelfTimer = snap.Self.Timer is not null;
        if (snap.Self.Timer is { } st) BuildSelfTimerMetrics(st, _rates?.SelfTimer);
        StatusText = $"Streaming every {StreamIntervalMs / 1000.0:0.#} s";
    }

    private IReadOnlyList<DevStatsEntry> CurrentDevs() => _devEntries.Values.OrderBy(d => d.Index).ToList();

    private void OnDevStat(DevStatsEntry e)
    {
        double now = _app.Clock.Seconds;
        _devEntries[e.Mac] = e;
        if (_latest is not null) _latest = _latest.WithDevs(CurrentDevs());

        // drop timers that are no longer paired
        var macs = _app.Fleet.Devices.Select(d => d.Mac).ToHashSet(StringComparer.OrdinalIgnoreCase);
        foreach (var gone in Devices.Where(d => !macs.Contains(d.Mac)).ToList())
        {
            Devices.Remove(gone);
            _devEntries.Remove(gone.Mac);
        }

        var vm = Devices.FirstOrDefault(d => string.Equals(d.Mac, e.Mac, StringComparison.OrdinalIgnoreCase));
        if (vm is null)
        {
            vm = new DeviceDiagnosticsViewModel(e.Mac);
            int at = Devices.TakeWhile(d => d.Index < e.Index).Count();
            Devices.Insert(at, vm);
            OnPropertyChanged(nameof(HasDevices));
        }
        var fleetDev = _app.Fleet.ByMac(e.Mac);
        vm.Update(e, now, fleetDev?.DisplayName ?? $"timer {e.Index}", fleetDev?.Online ?? false, fleetDev?.Rssi ?? 0);
    }

    // ---- remote metrics --------------------------------------------------------------------

    private void BuildRemoteMetrics(SelfStats s, RemoteRates? r)
    {
        string R(double? v) => r is null || v is null ? "" : Fmt.Rate(v.Value);
        HealthLevel Lv(long count, double? rate, HealthLevel whenNonZero = HealthLevel.Warn) =>
            count > 0 ? (rate is > 0 ? HealthLevel.Bad : whenNonZero) : HealthLevel.Ok;

        var rd = s.Radio;
        Radio.Set("Sent (tx)", Fmt.Count(rd.Tx), R(r?.Tx), HealthLevel.Ok, "Frames handed to the radio.");
        Radio.Set("Acknowledged (txOk)", Fmt.Count(rd.TxOk), R(r?.TxOk), HealthLevel.Ok, "Unicast frames acknowledged by the receiver's MAC.");
        double? fp = CounterMath.Percent(rd.TxFail, rd.Tx);
        Radio.Set("Failed (txFail)", $"{Fmt.Count(rd.TxFail)}" + (fp is { } p ? $" ({p:0.#}%)" : ""), r is null ? "" : $"{Fmt.Rate(r.TxFail)}" + (r.TxFailPercent is { } rp ? $" · {rp:0}% now" : ""),
            fp is > 25 && rd.Tx >= 50 ? HealthLevel.Bad : fp is > 10 && rd.Tx >= 50 ? HealthLevel.Warn : HealthLevel.Ok, "Unicast frames that were NOT acknowledged. High = poor link or wrong channel.");
        Radio.Set("Received (rx)", Fmt.Count(rd.Rx), R(r?.Rx), HealthLevel.Ok, "Valid FogLink frames received.");
        Radio.Set("Bad (rxBad)", Fmt.Count(rd.RxBad), R(r?.RxBad), HealthLevel.Ok, "Frames with a CRC / length / version error.");
        Radio.Set("Foreign (rxForeign)", Fmt.Count(rd.RxForeign), R(r?.RxForeign), HealthLevel.Ok, "Frames of other networks / other ESP-NOW users.");
        Radio.Set("Dropped (rxDrop)", Fmt.Count(rd.RxDrop), r is { RxDrop: > 0 } ? Fmt.Rate(r.RxDrop) : "", Lv(rd.RxDrop, r?.RxDrop), "Receive queue overflow: the CPU was too busy, real packet loss INSIDE the device.");
        Radio.Set("TX queue full (txDrop)", Fmt.Count(rd.TxDrop), "", rd.TxDrop > 0 ? HealthLevel.Warn : HealthLevel.Ok, "Frames discarded because the transmit queue was full.");
        Radio.Set("TX watchdog (txWd)", Fmt.Count(rd.TxWd), "", rd.TxWd > 0 ? HealthLevel.Warn : HealthLevel.Ok, "The send callback never arrived and the watchdog fired.");

        Sys.Set("Uptime", Fmt.Duration(s.UpSeconds), "", HealthLevel.Ok, $"Boot #{s.Boots}, last reset: {s.RstReason}");
        Sys.Set("Boots", s.Boots.ToString(), "", HealthLevel.Ok);
        Sys.Set("Loop max", $"{s.LoopMaxUs / 1000.0:0.#} ms", "", s.LoopMaxUs > HealthRules.LoopWarnUs ? HealthLevel.Warn : HealthLevel.Ok,
            "Slowest main-loop pass since boot. Above 20 ms something blocked the radio loop.");
        Sys.Set("Loops / s", Fmt.Count(s.LoopsPerS), "", HealthLevel.Ok);
        Sys.Set("Heap", $"{s.Heap / 1000.0:0} kB", "", HealthLevel.Ok);
        Sys.Set("Heap min", $"{s.HeapMin / 1000.0:0} kB", "", s.HeapMin is > 0 and < 20_000 ? HealthLevel.Warn : HealthLevel.Ok, "Lowest free heap since boot.");

        var rs = s.Rst;
        Resets.Set("Power-on", rs.PowerOn.ToString(), "", HealthLevel.Ok, "Lifetime reset counts, persisted over reboots.");
        Resets.Set("Brownout", rs.Brownout.ToString(), "", rs.Brownout > 0 ? HealthLevel.Warn : HealthLevel.Ok, "Supply voltage dipped: weak battery / supply.");
        Resets.Set("Watchdog", rs.Wdt.ToString(), "", rs.Wdt > 0 ? HealthLevel.Bad : HealthLevel.Ok);
        Resets.Set("Panic", rs.Panic.ToString(), "", rs.Panic > 0 ? HealthLevel.Bad : HealthLevel.Ok);
        Resets.Set("Software", rs.Sw.ToString(), "", HealthLevel.Ok);
        Resets.Set("Deep sleep", rs.DeepSleep.ToString(), "", HealthLevel.Ok);
        Resets.Set("Other", rs.Other.ToString(), "", HealthLevel.Ok);

        var lk = s.Link;
        LinkSummary.Set("Commands", $"{lk.Ok}/{lk.Cmds}", R(r?.Cmds), HealthLevel.Ok, "Commands confirmed / submitted.");
        LinkSummary.Set("Timeouts", lk.Timeout.ToString(), "", lk.Timeout > 0 ? HealthLevel.Warn : HealthLevel.Ok, "Commands that were never confirmed (timer off / out of range).");
        LinkSummary.Set("Rejected", lk.Rejected.ToString(), "", lk.Rejected > 0 ? HealthLevel.Warn : HealthLevel.Ok);
        LinkSummary.Set("Retries", Fmt.Count(lk.Retries), R(r?.Retries), HealthLevel.Ok, "Extra transmissions.");
        LinkSummary.Set("RTT avg / max", lk.RttAvg > 0 ? $"{lk.RttAvg:0.#} / {lk.RttMax:0} ms" : "-", "", HealthLevel.Ok, "From first send to the echo.");
        LinkSummary.Set("Sweeps / recovers", $"{lk.Sweeps} / {lk.Recovers}", "", HealthLevel.Ok, "Channel sweeps run and lost-timer recoveries tried.");
        LinkSummary.Set("Found / migrations", $"{lk.Found} / {lk.Migrations}", "", HealthLevel.Ok, "Timers found elsewhere and fleet channel moves.");

        Pc.Set("Lines from PC", Fmt.Count(s.Pc.LinesRx), R(r?.PcLinesRx), HealthLevel.Ok);
        Pc.Set("Lines to PC", Fmt.Count(s.Pc.LinesTx), R(r?.PcLinesTx), HealthLevel.Ok);
        Pc.Set("Bad JSON", s.Pc.BadJson.ToString(), "", s.Pc.BadJson > 0 ? HealthLevel.Info : HealthLevel.Ok, "Lines from the PC that were not valid JSON.");
        Pc.Set("USB TX drops", s.Pc.TxDrop.ToString(), "", s.Pc.TxDrop > 0 ? HealthLevel.Warn : HealthLevel.Ok, "Lines the device could not send to the PC.");
    }

    private void BuildSelfTimerMetrics(TimerStats t, TimerRates? tr)
    {
        SelfTimer.Set("Received", Fmt.Count(t.Rx), tr is null ? "" : Fmt.Rate(tr.Rx), HealthLevel.Ok);
        SelfTimer.Set("Rx bad / foreign", $"{t.RxBad} / {t.RxForeign}", "", HealthLevel.Ok);
        SelfTimer.Set("Rx drop", Fmt.Count(t.RxDrop), "", t.RxDrop > 0 ? HealthLevel.Warn : HealthLevel.Ok, "Receive queue overflows.");
        SelfTimer.Set("Sent / failed", $"{Fmt.Count(t.Tx)} / {Fmt.Count(t.TxFail)}", tr is null ? "" : Fmt.Rate(tr.Tx), HealthLevel.Ok);
        SelfTimer.Set("Beats / replies", $"{Fmt.Count(t.Beats)} / {Fmt.Count(t.Replies)}", "", HealthLevel.Ok, "Heartbeats and direct replies sent.");
        SelfTimer.Set("Cmds run / dup / rejected", $"{t.CmdsRun} / {t.CmdsDup} / {t.CmdsRejected}", "", HealthLevel.Ok);
        SelfTimer.Set("Loop max", $"{t.LoopMaxUs / 1000.0:0.#} ms", "", t.LoopMaxUs > HealthRules.LoopWarnUs ? HealthLevel.Warn : HealthLevel.Ok);
        SelfTimer.Set("Brownouts", t.Rst.Brownout.ToString(), "", t.Rst.Brownout > 0 ? HealthLevel.Warn : HealthLevel.Ok, "Supply voltage dips.");
        SelfTimer.Set("Power-ons", t.Rst.PowerOn.ToString(), "", HealthLevel.Ok);
    }

    // ---- channel quality -------------------------------------------------------------------

    private void RebuildChannelRows(int home)
    {
        var rows = _channels.Rows;
        long maxTx = rows.Count == 0 ? 1 : Math.Max(1, rows.Max(r => r.Tx));
        ChannelRows.Clear();
        foreach (var r in rows)
            ChannelRows.Add(new ChannelQualityRowViewModel(r.Channel, r.Tx, r.Fail, r.Rx, r.FailPercent, r.Channel == home, r.Tx / (double)maxTx));
        OnPropertyChanged(nameof(HasChannelRows));
    }

    [RelayCommand]
    private void ResetChannelTable()
    {
        _channels.Reset(_latest?.Self.Chan);
        ChannelRows.Clear();
        OnPropertyChanged(nameof(HasChannelRows));
        _app.Toasts.Info("Channel statistics cleared. They accumulate from now on, so you can compare channels over time.");
    }

    // ---- baseline / export -----------------------------------------------------------------

    [RelayCommand]
    private void SetBaseline()
    {
        if (_latest is null) return;
        _baseline = _latest;
        HasBaseline = true;
        BaselineText = $"Baseline stored at {DateTime.Now:HH:mm:ss}. Showing what changed since then.";
        UpdateCompare();
        _app.Toasts.Info("Baseline stored. Deltas since now are shown in the compare table.");
    }

    [RelayCommand]
    private void ClearBaseline()
    {
        _baseline = null;
        HasBaseline = false;
        BaselineText = "No baseline stored.";
        CompareRows.Clear();
    }

    private void UpdateCompare()
    {
        CompareRows.Clear();
        if (_baseline is null || _latest is null) return;
        var rows = StatsCompare.Interesting(StatsCompare.Compare(_baseline, _latest))
            .OrderBy(r => r.Scope == "remote" ? 0 : 1).ThenBy(r => r.Device).ThenBy(r => r.Name)
            .Take(150);
        foreach (var r in rows)
        {
            string dev = r.Device == "remote" ? "Remote" : r.Device == "self" ? "Timer" : _app.Fleet.ByMac(r.Device)?.DisplayName ?? r.Device;
            string scope = r.Scope switch { "remote" => "device", "link" => "link", "timer" => "timer", _ => r.Scope };
            string delta = (r.Delta >= 0 ? "+" : "") + r.Delta.ToString("0.##", CultureInfo.CurrentCulture) + (r.Restarted ? " (restarted)" : "");
            CompareRows.Add(new DiffRowViewModel(scope, dev, r.Name, r.Baseline.ToString("0.##", CultureInfo.CurrentCulture), r.Current.ToString("0.##", CultureInfo.CurrentCulture), delta, r.Delta > 0));
        }
    }

    [RelayCommand]
    private void CopyJson()
    {
        if (_latest is null)
        {
            _app.Toasts.Info("No counters received yet.");
            return;
        }
        _app.Dialogs.CopyText(StatsExport.ToJson(_latest, _rates, _channels.Rows, DateTimeOffset.Now));
        _app.Toasts.Success("Counters copied to the clipboard as JSON.");
    }

    [RelayCommand]
    private void ExportCsv()
    {
        if (_latest is null)
        {
            _app.Toasts.Info("No counters received yet.");
            return;
        }
        var path = _app.Dialogs.PickSaveFile("Export counters", "CSV file (*.csv)|*.csv", $"fogconsole-counters-{DateTime.Now:yyyyMMdd-HHmmss}.csv");
        if (path is null) return;
        try
        {
            var names = _app.Fleet.Devices.ToDictionary(d => d.Mac, d => d.DisplayName, StringComparer.OrdinalIgnoreCase);
            double secs = _previous is null ? 0 : StatsAnalyzer.IntervalSeconds(_previous, _latest, _lastStatsAt - _prevStatsAt);
            _app.Dialogs.SaveText(path, StatsExport.ToCsv(_latest, _previous, secs, DateTimeOffset.Now, names));
            _app.Toasts.Success($"Counters exported to {path}");
        }
        catch (Exception ex)
        {
            _app.Toasts.Error($"Could not export: {ex.Message}");
        }
    }

    // ---- housekeeping ----------------------------------------------------------------------

    public void Tick()
    {
        double now = _app.Clock.Seconds;
        OnPropertyChanged(nameof(IsConnected));
        if (HasData && _lastStatsAt > double.MinValue)
        {
            double age = now - _lastStatsAt;
            StatusText = age > 3.5 && _app.Fleet.IsLive
                ? $"No counters for {age:0} s - waiting..."
                : _active ? $"Streaming every {StreamIntervalMs / 1000.0:0.#} s · updated {Math.Max(0, age):0.0} s ago" : "Paused";
        }
        if (_baseline is not null && now - _lastCompareAt > 1)
        {
            _lastCompareAt = now;
            UpdateCompare();
        }
    }

    public void ClearSession()
    {
        Devices.Clear();
        _devEntries.Clear();
        _latest = _previous = null;
        _rates = null;
        HasData = false;
        StatusText = "Waiting for the first counters...";
        OnPropertyChanged(nameof(HasDevices));
    }
}
