using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Text.Json.Nodes;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using FogConsole.Hub;
using FogConsole.Protocol;
using FogConsole.Services;
using FogConsole.Simulator;

namespace FogConsole.ViewModels;

/// <summary>Settings of the remote (or the directly connected timer), application preferences and the about box.</summary>
public sealed partial class SettingsViewModel : ObservableObject, IPageViewModel
{
    private readonly AppServices _app;
    private readonly Action _logBuffersChanged;
    private RemoteSettings? _loaded;

    public SettingsViewModel(AppServices app, Action logBuffersChanged)
    {
        _app = app;
        _logBuffersChanged = logBuffersChanged;
        var s = app.Settings.Current;
        _theme = s.Theme;
        _autoConnect = s.AutoConnect;
        _autoReconnect = s.AutoReconnect;
        _logBufferSize = s.LogBufferSize;
        _packetBufferSize = s.PacketBufferSize;
        _simKindIndex = s.SimulatorKind == "Timer" ? 1 : 0;
        app.Fleet.Remote.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName is nameof(RemoteInfoViewModel.Kind) or nameof(RemoteInfoViewModel.Millivolts))
            {
                OnPropertyChanged(nameof(IsRemote));
                OnPropertyChanged(nameof(IsTimer));
                OnPropertyChanged(nameof(LiveMillivolts));
            }
        };
        app.Fleet.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(FleetState.IsLive))
            {
                OnPropertyChanged(nameof(CanEdit));
                ApplyCommand.NotifyCanExecuteChanged();
                ReloadCommand.NotifyCanExecuteChanged();
            }
        };
    }

    public NavPage Page => NavPage.Settings;

    public void Activate()
    {
        OnPropertyChanged(nameof(CanEdit));
        OnPropertyChanged(nameof(IsRemote));
        OnPropertyChanged(nameof(IsTimer));
        OnPropertyChanged(nameof(About));
    }

    public void Deactivate() { }

    public RemoteInfoViewModel Remote => _app.Fleet.Remote;
    public bool IsRemote => Remote.IsRemote || !Remote.IsTimer;
    public bool IsTimer => Remote.IsTimer;
    public bool CanEdit => _app.Fleet.IsLive;
    public int LiveMillivolts => Remote.Millivolts;

    public IReadOnlyList<string> Themes { get; } = new[] { "System", "Light", "Dark" };
    public IReadOnlyList<string> SimulatorKinds { get; } = new[] { "Remote with timers", "Single timer (direct USB)" };
    public IReadOnlyList<string> StartPhases { get; } = new[] { "OFF", "ON" };
    public IReadOnlyList<int> BufferSizes { get; } = new[] { 1000, 2000, 5000, 10000, 20000, 50000 };

    // ---- remote settings -------------------------------------------------------------------

    [ObservableProperty] private int _txPower = 80;
    [ObservableProperty] private int _brightness = 200;
    [ObservableProperty] private double _autoOff = 60;
    [ObservableProperty] private int _rssiLow = -90;
    [ObservableProperty] private int _rssiHigh = -50;
    [ObservableProperty] private double _batCal0 = 3300;
    [ObservableProperty] private double _batCal50 = 3750;
    [ObservableProperty] private double _batCal100 = 4150;
    [ObservableProperty] private int _net = 1;
    [ObservableProperty] private bool _hasLoaded;
    [ObservableProperty] private string _statusText = "Connect to a remote to read its settings.";

    // timer (direct USB)
    [ObservableProperty] private string _timerName = "";
    [ObservableProperty] private int _timerStartPhaseIndex;
    [ObservableProperty] private double _timerOn = 1.5;
    [ObservableProperty] private double _timerOff = 10;
    [ObservableProperty] private bool _timerLed = true;

    public string TxPowerText => $"{TxPower * 0.25:0.##} dBm  (\u2248 {Math.Pow(10, TxPower * 0.25 / 10.0):0.#} mW)";
    public string RssiHint => RssiHigh < RssiLow + 5 ? "The upper threshold must be at least 5 dBm above the lower one." : $"{RssiLow} dBm or weaker shows 0 bars, {RssiHigh} dBm or stronger shows 6 bars.";
    public bool RssiValid => RssiHigh >= RssiLow + 5;
    public bool BatValid => BatCal0 < BatCal50 && BatCal50 < BatCal100;
    public string BatHint => BatValid ? "Battery voltages (mV) that count as 0 %, 50 % and 100 %." : "The three points must rise: 0 % < 50 % < 100 %.";

    partial void OnTxPowerChanged(int value) => OnPropertyChanged(nameof(TxPowerText));
    partial void OnRssiLowChanged(int value) { OnPropertyChanged(nameof(RssiHint)); OnPropertyChanged(nameof(RssiValid)); }
    partial void OnRssiHighChanged(int value) { OnPropertyChanged(nameof(RssiHint)); OnPropertyChanged(nameof(RssiValid)); }
    partial void OnBatCal0Changed(double value) { OnPropertyChanged(nameof(BatValid)); OnPropertyChanged(nameof(BatHint)); }
    partial void OnBatCal50Changed(double value) { OnPropertyChanged(nameof(BatValid)); OnPropertyChanged(nameof(BatHint)); }
    partial void OnBatCal100Changed(double value) { OnPropertyChanged(nameof(BatValid)); OnPropertyChanged(nameof(BatHint)); }

    /// <summary>Loads values from a <c>settings</c> object (from <c>get settings</c> or the answer to <c>set</c>).</summary>
    public void LoadFrom(RemoteSettings s)
    {
        _loaded = s;
        try
        {
            if (s.TxPower is { } tp) TxPower = tp;
            if (s.Brightness is { } br) Brightness = br;
            if (s.AutoOff is { } ao) AutoOff = ao;
            if (s.RssiLow is { } lo) RssiLow = lo;
            if (s.RssiHigh is { } hi) RssiHigh = hi;
            if (s.BatCal is { Count: 3 } bc) { BatCal0 = bc[0]; BatCal50 = bc[1]; BatCal100 = bc[2]; }
            if (s.Net is { } n) Net = n;
            if (s.Name is { } name) TimerName = name;
            if (s.StartPhase is { } sp) TimerStartPhaseIndex = sp == "on" ? 1 : 0;
            if (s.Ton is { } ton) TimerOn = ton;
            if (s.Toff is { } toff) TimerOff = toff;
            if (s.Led is { } led) TimerLed = led;
        }
        finally
        {
        }
        HasLoaded = true;
        StatusText = "Settings read from the device.";
        if (s.RssiLow is not null && s.RssiHigh is not null)
        {
            Remote.RssiLow = s.RssiLow.Value;
            Remote.RssiHigh = s.RssiHigh.Value;
            _app.Fleet.RefreshThresholds();
        }
        ApplyCommand.NotifyCanExecuteChanged();
    }

    public void OnDisconnected()
    {
        HasLoaded = false;
        StatusText = "Not connected.";
    }

    /// <summary>Reads the settings after connecting.</summary>
    public async Task LoadAsync()
    {
        if (!_app.IsConnected) return;
        try
        {
            var res = await _app.Hub.RequestAsync(Requests.GetSettings(), retries: 1).ConfigureAwait(true);
            if (res is { Ok: true, Settings: { } s }) LoadFrom(s);
            else StatusText = $"Could not read the settings: {res.Describe()}";
        }
        catch (HubException ex)
        {
            StatusText = $"Could not read the settings: {ex.Message}";
        }
    }

    private bool CanApplyOrReload() => _app.Fleet.IsLive;

    [RelayCommand(CanExecute = nameof(CanApplyOrReload))]
    private Task Reload() => LoadAsync();

    [RelayCommand(CanExecute = nameof(CanApplyOrReload))]
    private async Task Apply()
    {
        var set = new JsonObject();
        if (IsTimer)
        {
            var name = TimerName.Trim();
            if (name.Length is < 1 or > 9 || name.Any(c => c < ' ' || c > '~')) { _app.Toasts.Warning("Names are 1-9 printable ASCII characters."); return; }
            set["name"] = name;
            set["startPhase"] = TimerStartPhaseIndex == 1 ? "on" : "off";
            set["ton"] = CommandSpec.RoundTenth(TimerOn);
            set["toff"] = CommandSpec.RoundTenth(TimerOff);
            set["led"] = TimerLed;
        }
        else
        {
            if (!RssiValid) { _app.Toasts.Warning(RssiHint); return; }
            if (!BatValid) { _app.Toasts.Warning(BatHint); return; }
            var l = _loaded;
            if (l?.TxPower != TxPower) set["txPower"] = TxPower;
            if (l?.Brightness != Brightness) set["brightness"] = Brightness;
            if (l?.AutoOff != (int)AutoOff) set["autoOff"] = (int)AutoOff;
            if (l?.RssiLow != RssiLow) set["rssiLow"] = RssiLow;
            if (l?.RssiHigh != RssiHigh) set["rssiHigh"] = RssiHigh;
            var cal = new[] { (int)BatCal0, (int)BatCal50, (int)BatCal100 };
            if (l?.BatCal is null || !l.BatCal.SequenceEqual(cal)) set["batCal"] = new JsonArray(cal.Select(c => (JsonNode?)JsonValue.Create(c)).ToArray());
        }
        if (set.Count == 0)
        {
            _app.Toasts.Info("Nothing changed.");
            return;
        }
        try
        {
            var res = await _app.Hub.RequestAsync(Requests.Set(set)).ConfigureAwait(true);
            if (res is { Ok: true, Settings: { } s })
            {
                LoadFrom(s);
                _app.Toasts.Success(IsTimer ? "Timer settings saved." : "Remote settings saved.");
            }
            else
            {
                _app.Toasts.Error($"The device refused the settings: {res.Describe()}");
            }
        }
        catch (HubException ex)
        {
            _app.Toasts.Error($"Could not save the settings: {ex.Message}");
        }
    }

    [RelayCommand]
    private void UseLiveAs(string? point)
    {
        int mv = Remote.Millivolts;
        if (mv <= 0)
        {
            _app.Toasts.Warning("No live battery voltage yet.");
            return;
        }
        switch (point)
        {
            case "0": BatCal0 = mv; break;
            case "50": BatCal50 = mv; break;
            case "100": BatCal100 = mv; break;
        }
    }

    // ---- application preferences -----------------------------------------------------------

    [ObservableProperty] private string _theme;
    [ObservableProperty] private bool _autoConnect;
    [ObservableProperty] private bool _autoReconnect;
    [ObservableProperty] private int _logBufferSize;
    [ObservableProperty] private int _packetBufferSize;
    [ObservableProperty] private int _simKindIndex;

    partial void OnThemeChanged(string value)
    {
        _app.Settings.Current.Theme = value;
        _app.Settings.SaveSoon();
        _app.Theme.Apply(value);
    }

    partial void OnAutoConnectChanged(bool value)
    {
        _app.Settings.Current.AutoConnect = value;
        _app.Settings.SaveSoon();
    }

    partial void OnAutoReconnectChanged(bool value)
    {
        _app.Settings.Current.AutoReconnect = value;
        _app.Settings.SaveSoon();
        _app.Hub.AutoReconnect = value;
    }

    partial void OnLogBufferSizeChanged(int value)
    {
        _app.Settings.Current.LogBufferSize = Math.Clamp(value, 500, 100_000);
        _app.Settings.SaveSoon();
        _logBuffersChanged();
    }

    partial void OnPacketBufferSizeChanged(int value)
    {
        _app.Settings.Current.PacketBufferSize = Math.Clamp(value, 500, 100_000);
        _app.Settings.SaveSoon();
        _logBuffersChanged();
    }

    partial void OnSimKindIndexChanged(int value)
    {
        _app.Settings.Current.SimulatorKind = value == 1 ? "Timer" : "Remote";
        _app.Settings.SaveSoon();
        _app.Sim.Kind = value == 1 ? SimKind.Timer : SimKind.Remote;
    }

    // ---- about -----------------------------------------------------------------------------

    public IReadOnlyList<(string Label, string Value)> About
    {
        get
        {
            var asm = Assembly.GetExecutingAssembly();
            var version = asm.GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion?.Split('+')[0] ?? "2.0.0";
            var r = Remote;
            var list = new List<(string, string)>
            {
                ("Fog Console", version),
                ("Protocol", "FogSerial/2 (JSON lines)"),
                ("Runtime", System.Runtime.InteropServices.RuntimeInformation.FrameworkDescription),
            };
            if (r.Kind != DeviceKind.Unknown)
            {
                list.Add(("Device", $"{r.KindText} \"{r.Name}\""));
                list.Add(("Firmware", $"{r.Firmware} (protocol {r.Protocol})"));
                list.Add(("MAC", r.Mac));
                list.Add(("Build", r.Build));
                list.Add(("Boots", $"{r.Boots}  (last reset: {r.ResetReason})"));
                if (r.IsRemote) list.Add(("Capacity", $"up to {r.MaxDevs} timers"));
                list.Add(("Connection", _app.Sim.IsActive ? "Simulator (no hardware)" : _app.Hub.TransportName));
            }
            else
            {
                list.Add(("Device", "not connected"));
            }
            list.Add(("Settings file", SettingsService.DefaultPath));
            list.Add(("Crash log", CrashLog.PathOnDisk));
            return list;
        }
    }

    [RelayCommand]
    private void OpenDataFolder()
    {
        try
        {
            Directory.CreateDirectory(SettingsService.AppDataDirectory);
            Process.Start(new ProcessStartInfo("explorer.exe", SettingsService.AppDataDirectory) { UseShellExecute = true });
        }
        catch (Exception ex)
        {
            _app.Toasts.Warning($"Could not open the folder: {ex.Message}");
        }
    }
}
