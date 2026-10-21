using System.Collections.ObjectModel;
using System.Collections.Specialized;
using System.Globalization;
using System.Text;
using System.Text.Json.Nodes;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using FogConsole.Hub;
using FogConsole.Protocol;
using FogConsole.Services;

namespace FogConsole.ViewModels;

/// <summary>An observable collection that can change many items with a single notification (cheap for virtualised lists).</summary>
public sealed class BulkObservableCollection<T> : ObservableCollection<T>
{
    public void ReplaceAll(IEnumerable<T> items)
    {
        Items.Clear();
        foreach (var i in items) Items.Add(i);
        Reset();
    }

    public void AddRange(IEnumerable<T> items)
    {
        bool any = false;
        foreach (var i in items)
        {
            Items.Add(i);
            any = true;
        }
        if (any) Reset();
    }

    public void RemoveFirst(int count)
    {
        count = Math.Min(count, Items.Count);
        if (count <= 0) return;
        for (int i = 0; i < count; i++) Items.RemoveAt(0);
        Reset();
    }

    public void ClearAll()
    {
        if (Items.Count == 0) return;
        Items.Clear();
        Reset();
    }

    private void Reset()
    {
        OnPropertyChanged(new System.ComponentModel.PropertyChangedEventArgs(nameof(Count)));
        OnPropertyChanged(new System.ComponentModel.PropertyChangedEventArgs("Item[]"));
        OnCollectionChanged(new NotifyCollectionChangedEventArgs(NotifyCollectionChangedAction.Reset));
    }
}

/// <summary>
/// A bounded buffer (the master) plus a filtered, observable view of it. Pausing freezes the view while the buffer keeps
/// collecting; resuming catches up.
/// </summary>
public sealed class FilteredRing<T>
{
    private readonly List<T> _master = new();
    private Func<T, bool> _filter = _ => true;
    private bool _paused;

    public FilteredRing(int capacity) => Capacity = Math.Max(100, capacity);

    public int Capacity { get; set; }
    public BulkObservableCollection<T> View { get; } = new();
    public int TotalCount => _master.Count;
    public IReadOnlyList<T> Master => _master;

    public bool Paused
    {
        get => _paused;
        set
        {
            if (_paused == value) return;
            _paused = value;
            if (!value) Rebuild();
        }
    }

    public void SetFilter(Func<T, bool> filter)
    {
        _filter = filter;
        Rebuild();
    }

    public void Add(IReadOnlyList<T> items)
    {
        if (items.Count == 0) return;
        _master.AddRange(items);
        int overflow = _master.Count - Capacity;
        if (overflow > Capacity / 10)
        {
            // trim in chunks so we do not rebuild the view on every batch
            _master.RemoveRange(0, overflow);
            if (!_paused) Rebuild();
            return;
        }
        if (_paused) return;
        var visible = items.Where(_filter).ToList();
        if (visible.Count > 0) View.AddRange(visible);
    }

    public void Clear()
    {
        _master.Clear();
        View.ClearAll();
    }

    public void Rebuild() => View.ReplaceAll(_master.Where(_filter));
}

public sealed class LogRow
{
    public required DateTime Time { get; init; }
    public required LogLevel Level { get; init; }
    public required string Source { get; init; }
    public required string Text { get; init; }
    public long DeviceMs { get; init; }

    public string TimeText => Time.ToString("HH:mm:ss.fff", CultureInfo.InvariantCulture);
    public string LevelText => Level switch { LogLevel.Error => "ERR", LogLevel.Warn => "WRN", LogLevel.Info => "INF", LogLevel.Debug => "DBG", _ => "" };
    public string Line => $"{TimeText} {LevelText} {Source,-8} {Text}";
}

public sealed class PacketRow
{
    public required DateTime Time { get; init; }
    public required string Dir { get; init; }
    public required string Mac { get; init; }
    public required string Kind { get; init; }
    public required int Len { get; init; }
    public required int Rssi { get; init; }
    public required long Seq { get; init; }
    public required string Hex { get; init; }
    public long DeviceMs { get; init; }

    public string TimeText => Time.ToString("HH:mm:ss.fff", CultureInfo.InvariantCulture);
    public string Arrow => Dir == "tx" ? "TX →" : "RX ←";
    public string RssiText => Rssi == 0 ? "" : Rssi.ToString(CultureInfo.InvariantCulture);
}

public sealed class RawRow
{
    public required DateTime Time { get; init; }
    public required RawDirection Direction { get; init; }
    public required RawKind Kind { get; init; }
    public required string Text { get; init; }

    public string TimeText => Time.ToString("HH:mm:ss.fff", CultureInfo.InvariantCulture);
    public string Arrow => Direction switch { RawDirection.Tx => "→", RawDirection.Rx => "←", _ => "•" };
    public string Line => $"{TimeText} {Arrow} {Text}";
}

public sealed record RawTemplate(string Name, string Json)
{
    public override string ToString() => Name;
}

/// <summary>Log, packet inspector and raw JSON console.</summary>
public sealed partial class ConsoleViewModel : ObservableObject, IPageViewModel, IMessageSink
{
    private static int _rawId = 1_100_000_000;

    private readonly AppServices _app;
    private readonly List<LogRow> _logPending = new();
    private readonly List<PacketRow> _packetPending = new();
    private readonly List<RawRow> _rawPending = new();
    private readonly List<string> _history = new();
    private int _historyPos = -1;

    public ConsoleViewModel(AppServices app)
    {
        _app = app;
        var s = app.Settings.Current;
        LogRing = new FilteredRing<LogRow>(s.LogBufferSize);
        PacketRing = new FilteredRing<PacketRow>(s.PacketBufferSize);
        RawRing = new FilteredRing<RawRow>(s.LogBufferSize);
        _deviceLogLevel = s.LogLevel;
        Templates = new[]
        {
            new RawTemplate("hello", "{\"t\":\"hello\"}"),
            new RawTemplate("ping", "{\"t\":\"ping\"}"),
            new RawTemplate("get fleet", "{\"t\":\"get\",\"what\":\"fleet\"}"),
            new RawTemplate("get settings", "{\"t\":\"get\",\"what\":\"settings\"}"),
            new RawTemplate("get stats", "{\"t\":\"get\",\"what\":\"stats\"}"),
            new RawTemplate("get devStats (dev 0)", "{\"t\":\"get\",\"what\":\"devStats\",\"dev\":0}"),
            new RawTemplate("statsCfg 1000 ms", "{\"t\":\"statsCfg\",\"intervalMs\":1000}"),
            new RawTemplate("statsCfg off", "{\"t\":\"statsCfg\",\"intervalMs\":0}"),
            new RawTemplate("cmd toggle (dev 0)", "{\"t\":\"cmd\",\"dev\":0,\"op\":\"toggle\"}"),
            new RawTemplate("cmd setTimes (dev 0)", "{\"t\":\"cmd\",\"dev\":0,\"op\":\"setTimes\",\"on\":1.5,\"off\":10.0}"),
            new RawTemplate("cmd mode forceOn 1 s (dev 0)", "{\"t\":\"cmd\",\"dev\":0,\"op\":\"mode\",\"mode\":\"forceOn\",\"leaseMs\":1000}"),
            new RawTemplate("cmd mode auto (dev 0)", "{\"t\":\"cmd\",\"dev\":0,\"op\":\"mode\",\"mode\":\"auto\"}"),
            new RawTemplate("cmd identify all", "{\"t\":\"cmd\",\"dev\":\"all\",\"op\":\"identify\"}"),
            new RawTemplate("select dev 0", "{\"t\":\"select\",\"dev\":0}"),
            new RawTemplate("discover on", "{\"t\":\"discover\",\"on\":true,\"sweep\":false}"),
            new RawTemplate("discover off", "{\"t\":\"discover\",\"on\":false,\"sweep\":false}"),
            new RawTemplate("survey", "{\"t\":\"survey\"}"),
            new RawTemplate("linkTest dev 0", "{\"t\":\"linkTest\",\"dev\":0,\"count\":50,\"gapMs\":40}"),
            new RawTemplate("recover", "{\"t\":\"recover\"}"),
            new RawTemplate("logCfg debug", "{\"t\":\"logCfg\",\"level\":\"debug\"}"),
            new RawTemplate("sniff on", "{\"t\":\"sniff\",\"on\":true}"),
            new RawTemplate("set brightness 200", "{\"t\":\"set\",\"settings\":{\"brightness\":200}}"),
        };
        ApplyLogFilter();
        ApplyPacketFilter();
    }

    public NavPage Page => NavPage.Console;
    public void Activate() => Flush();
    public void Deactivate() { }

    public FilteredRing<LogRow> LogRing { get; }
    public FilteredRing<PacketRow> PacketRing { get; }
    public FilteredRing<RawRow> RawRing { get; }
    public BulkObservableCollection<LogRow> LogRows => LogRing.View;
    public BulkObservableCollection<PacketRow> PacketRows => PacketRing.View;
    public BulkObservableCollection<RawRow> RawRows => RawRing.View;
    public IReadOnlyList<RawTemplate> Templates { get; }
    public IReadOnlyList<string> DeviceLogLevels { get; } = new[] { "error", "warn", "info", "debug", "off" };
    public IReadOnlyList<string> LevelFilters { get; } = new[] { "All (debug)", "Info and above", "Warnings and errors", "Errors only" };

    [ObservableProperty] private int _selectedTab;

    // ---- log -------------------------------------------------------------------------------

    [ObservableProperty] private string _logFilter = "";
    [ObservableProperty] private int _levelFilterIndex;
    [ObservableProperty] private bool _logAutoScroll = true;
    [ObservableProperty] private bool _logPaused;
    [ObservableProperty] private string _deviceLogLevel;

    public string LogCountText => $"{LogRing.View.Count:N0} of {LogRing.TotalCount:N0} lines";

    partial void OnLogFilterChanged(string value) => ApplyLogFilter();
    partial void OnLevelFilterIndexChanged(int value) => ApplyLogFilter();
    partial void OnLogPausedChanged(bool value)
    {
        LogRing.Paused = value;
        OnPropertyChanged(nameof(LogCountText));
    }

    partial void OnDeviceLogLevelChanged(string value)
    {
        _app.Settings.Current.LogLevel = value;
        _app.Settings.SaveSoon();
        _ = SendLogLevelAsync();
    }

    private void ApplyLogFilter()
    {
        string text = LogFilter.Trim();
        int maxLevel = LevelFilterIndex switch { 1 => (int)LogLevel.Info, 2 => (int)LogLevel.Warn, 3 => (int)LogLevel.Error, _ => (int)LogLevel.Debug };
        LogRing.SetFilter(r => (int)r.Level <= maxLevel &&
            (text.Length == 0 || r.Text.Contains(text, StringComparison.OrdinalIgnoreCase) || r.Source.Contains(text, StringComparison.OrdinalIgnoreCase)));
        OnPropertyChanged(nameof(LogCountText));
    }

    public async Task SendLogLevelAsync()
    {
        if (!_app.IsConnected) return;
        try { await _app.Hub.RequestAsync(Requests.LogCfg(DeviceLogLevel), retries: 1).ConfigureAwait(true); }
        catch (HubException) { /* reported by the connection chip */ }
    }

    [RelayCommand]
    private void ClearLog()
    {
        LogRing.Clear();
        OnPropertyChanged(nameof(LogCountText));
    }

    [RelayCommand]
    private void CopyLog()
    {
        var sb = new StringBuilder();
        foreach (var r in LogRing.View) sb.AppendLine(r.Line);
        _app.Dialogs.CopyText(sb.ToString());
        _app.Toasts.Info($"{LogRing.View.Count:N0} log lines copied to the clipboard.");
    }

    [RelayCommand]
    private void SaveLog()
    {
        var path = _app.Dialogs.PickSaveFile("Save log", "Text file (*.txt)|*.txt|All files (*.*)|*.*", $"fogconsole-log-{DateTime.Now:yyyyMMdd-HHmmss}.txt");
        if (path is null) return;
        try
        {
            _app.Dialogs.SaveText(path, string.Join(Environment.NewLine, LogRing.View.Select(r => r.Line)) + Environment.NewLine);
            _app.Toasts.Success($"Log saved to {path}");
        }
        catch (Exception ex)
        {
            _app.Toasts.Error($"Could not save the log: {ex.Message}");
        }
    }

    /// <summary>Adds a line generated by the application itself (connect, disconnect ...).</summary>
    public void AppLog(LogLevel level, string text)
    {
        _logPending.Add(new LogRow { Time = DateTime.Now, Level = level, Source = "app", Text = text });
    }

    // ---- packets ---------------------------------------------------------------------------

    [ObservableProperty] private string _packetFilter = "";
    [ObservableProperty] private bool _captureFrames;
    [ObservableProperty] private bool _packetPaused;
    [ObservableProperty] private bool _packetAutoScroll = true;

    public string PacketCountText => $"{PacketRing.View.Count:N0} of {PacketRing.TotalCount:N0} frames";

    partial void OnPacketFilterChanged(string value) => ApplyPacketFilter();
    partial void OnPacketPausedChanged(bool value)
    {
        PacketRing.Paused = value;
        OnPropertyChanged(nameof(PacketCountText));
    }

    partial void OnCaptureFramesChanged(bool value) => _ = SendSniffAsync();

    private void ApplyPacketFilter()
    {
        string text = PacketFilter.Trim();
        PacketRing.SetFilter(p => text.Length == 0 ||
            p.Mac.Contains(text, StringComparison.OrdinalIgnoreCase) || p.Kind.Contains(text, StringComparison.OrdinalIgnoreCase) ||
            p.Dir.Equals(text, StringComparison.OrdinalIgnoreCase) || p.Hex.Contains(text, StringComparison.OrdinalIgnoreCase));
        OnPropertyChanged(nameof(PacketCountText));
    }

    public async Task SendSniffAsync()
    {
        if (!_app.IsConnected) return;
        try
        {
            var res = await _app.Hub.RequestAsync(Requests.Sniff(CaptureFrames), retries: 1).ConfigureAwait(true);
            if (!res.Ok) _app.Toasts.Warning($"Packet capture: {res.Describe()}");
        }
        catch (HubException) { /* connection problems are shown elsewhere */ }
    }

    [RelayCommand]
    private void ClearPackets()
    {
        PacketRing.Clear();
        OnPropertyChanged(nameof(PacketCountText));
    }

    [RelayCommand]
    private void ExportPackets()
    {
        var path = _app.Dialogs.PickSaveFile("Export packets", "CSV file (*.csv)|*.csv", $"fogconsole-packets-{DateTime.Now:yyyyMMdd-HHmmss}.csv");
        if (path is null) return;
        try
        {
            _app.Dialogs.SaveText(path, PacketsToCsv(PacketRing.View));
            _app.Toasts.Success($"{PacketRing.View.Count:N0} packets exported to {path}");
        }
        catch (Exception ex)
        {
            _app.Toasts.Error($"Could not export the packets: {ex.Message}");
        }
    }

    public static string PacketsToCsv(IEnumerable<PacketRow> rows)
    {
        var sb = new StringBuilder("time,deviceMs,dir,mac,kind,len,rssi,seq,hex\r\n");
        foreach (var p in rows)
            sb.Append(p.Time.ToString("o", CultureInfo.InvariantCulture)).Append(',').Append(p.DeviceMs).Append(',').Append(p.Dir).Append(',')
              .Append(p.Mac).Append(',').Append(p.Kind).Append(',').Append(p.Len).Append(',').Append(p.Rssi).Append(',').Append(p.Seq).Append(',')
              .Append('"').Append(p.Hex).Append('"').Append("\r\n");
        return sb.ToString();
    }

    // ---- raw -------------------------------------------------------------------------------

    [ObservableProperty] private string _rawInput = "";
    [ObservableProperty] private bool _rawAutoScroll = true;
    [ObservableProperty] private bool _rawPaused;
    [ObservableProperty] private RawTemplate? _selectedTemplate;
    [ObservableProperty] private string _rawValidation = "";

    partial void OnRawPausedChanged(bool value) => RawRing.Paused = value;

    partial void OnSelectedTemplateChanged(RawTemplate? value)
    {
        if (value is null) return;
        RawInput = value.Json;
        SelectedTemplate = null;
    }

    partial void OnRawInputChanged(string value)
    {
        if (string.IsNullOrWhiteSpace(value)) { RawValidation = ""; return; }
        try
        {
            var n = JsonNode.Parse(value);
            RawValidation = n is JsonObject ? "valid JSON object" : "valid JSON, but not an object";
        }
        catch
        {
            RawValidation = "not valid JSON (it will still be sent as typed)";
        }
    }

    [RelayCommand]
    private async Task SendRaw()
    {
        string text = RawInput.Trim();
        if (text.Length == 0) return;
        if (!_app.IsConnected)
        {
            _app.Toasts.Warning("Not connected.");
            return;
        }
        text = text.ReplaceLineEndings(" ");
        // convenience: give objects without an id one, so the answer can be matched
        try
        {
            if (JsonNode.Parse(text) is JsonObject o && !o.ContainsKey("id"))
            {
                var ordered = new JsonObject();
                if (o["t"] is { } t) ordered["t"] = t.DeepClone();
                ordered["id"] = Interlocked.Increment(ref _rawId);
                foreach (var (k, v) in o) if (k != "t") ordered[k] = v?.DeepClone();
                text = ordered.ToJsonString();
            }
        }
        catch { /* send as typed */ }

        if (_history.Count == 0 || _history[^1] != RawInput.Trim()) _history.Add(RawInput.Trim());
        _historyPos = -1;
        try
        {
            await _app.Hub.SendRawAsync(text).ConfigureAwait(true);
            RawInput = "";
        }
        catch (HubException ex)
        {
            _app.Toasts.Error($"Send failed: {ex.Message}");
        }
    }

    public void HistoryUp()
    {
        if (_history.Count == 0) return;
        _historyPos = _historyPos < 0 ? _history.Count - 1 : Math.Max(0, _historyPos - 1);
        RawInput = _history[_historyPos];
    }

    public void HistoryDown()
    {
        if (_history.Count == 0 || _historyPos < 0) return;
        _historyPos++;
        if (_historyPos >= _history.Count) { _historyPos = -1; RawInput = ""; }
        else RawInput = _history[_historyPos];
    }

    [RelayCommand]
    private void ClearRaw() => RawRing.Clear();

    [RelayCommand]
    private void CopyRaw()
    {
        _app.Dialogs.CopyText(string.Join(Environment.NewLine, RawRing.View.Select(r => r.Line)));
        _app.Toasts.Info($"{RawRing.View.Count:N0} raw lines copied to the clipboard.");
    }

    [RelayCommand]
    private void SaveRaw()
    {
        var path = _app.Dialogs.PickSaveFile("Save raw console", "Text file (*.txt)|*.txt", $"fogconsole-raw-{DateTime.Now:yyyyMMdd-HHmmss}.txt");
        if (path is null) return;
        try
        {
            _app.Dialogs.SaveText(path, string.Join(Environment.NewLine, RawRing.View.Select(r => r.Line)) + Environment.NewLine);
            _app.Toasts.Success($"Raw console saved to {path}");
        }
        catch (Exception ex)
        {
            _app.Toasts.Error($"Could not save: {ex.Message}");
        }
    }

    // ---- feeding ---------------------------------------------------------------------------

    public void OnRaw(RawLine line)
    {
        _rawPending.Add(new RawRow { Time = line.Time.LocalDateTime, Direction = line.Direction, Kind = line.Kind, Text = line.Text });
    }

    public void OnMessage(Message message)
    {
        switch (message)
        {
            case LogMessage l:
                _logPending.Add(new LogRow { Time = DateTime.Now, Level = l.Level, Source = l.Src, Text = l.Text, DeviceMs = l.Ms });
                break;
            case FrameMessage f:
                _packetPending.Add(new PacketRow
                {
                    Time = DateTime.Now, Dir = f.Dir, Mac = f.Mac, Kind = f.Kind, Len = f.Len, Rssi = f.Rssi, Seq = f.Seq, Hex = f.Hex, DeviceMs = f.Ms,
                });
                break;
            case PlainTextMessage p:
                _logPending.Add(new LogRow { Time = DateTime.Now, Level = p.MalformedJson ? LogLevel.Warn : LogLevel.Info, Source = "serial", Text = p.MalformedJson ? "malformed JSON: " + p.Text : p.Text });
                break;
        }
    }

    /// <summary>Moves buffered rows into the visible collections (called ~10x per second).</summary>
    public void Flush()
    {
        if (_logPending.Count > 0)
        {
            var batch = _logPending.ToArray();
            _logPending.Clear();
            LogRing.Add(batch);
            OnPropertyChanged(nameof(LogCountText));
        }
        if (_packetPending.Count > 0)
        {
            var batch = _packetPending.ToArray();
            _packetPending.Clear();
            PacketRing.Add(batch);
            OnPropertyChanged(nameof(PacketCountText));
        }
        if (_rawPending.Count > 0)
        {
            var batch = _rawPending.ToArray();
            _rawPending.Clear();
            RawRing.Add(batch);
        }
    }

    public void ApplyBufferSizes()
    {
        var s = _app.Settings.Current;
        LogRing.Capacity = s.LogBufferSize;
        RawRing.Capacity = s.LogBufferSize;
        PacketRing.Capacity = s.PacketBufferSize;
    }

    /// <summary>After (re-)connecting: restore the device-side diagnostics switches.</summary>
    public async Task ApplySessionAsync()
    {
        await SendLogLevelAsync();
        if (CaptureFrames) await SendSniffAsync();
    }

    public void OnDisconnected()
    {
        // the device forgets sniff / log settings with the session; keep the user's wish and re-send on reconnect
    }
}
