using System.Collections.ObjectModel;
using System.Diagnostics;
using System.IO;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using FogConsole.Flashing;
using FogConsole.Hub;
using FogConsole.Protocol;
using FogConsole.Services;
using FogConsole.Transport;

namespace FogConsole.ViewModels;

/// <summary>A firmware offered on the Firmware page.</summary>
public sealed class FirmwareChoice
{
    public FirmwareChoice(FirmwareSet set) => Set = set;

    public FirmwareSet Set { get; }

    public string Title => Set.DisplayName;
    public string Path => Set.Source;
    public string KindText => Set.KindText;
    public bool KnownKind => Set.Kind != FirmwareKind.Unknown;
    public string ContentsText => Set.CanReinstall ? "complete" : "application only";
    public string ProblemText => string.Join(" ", Set.Problems);
    public bool HasProblems => Set.Problems.Count > 0;

    public string Detail
    {
        get
        {
            var size = Set.App.Length >= 1_000_000 ? $"{Set.App.Length / 1e6:0.0} MB" : $"{Set.App.Length / 1024.0:0} KB";
            var when = Set.FileTimeUtc == default ? "" : $" · file from {Set.FileTimeUtc.ToLocalTime():d MMM yyyy HH:mm}";
            return $"{(string.IsNullOrEmpty(Set.Origin) ? "Firmware file" : Set.Origin)} · application {size}{when}";
        }
    }
}

/// <summary>One timer of the paired fleet and the firmware version it reported.</summary>
public sealed class TimerFirmwareRow
{
    public required string Name { get; init; }
    public required string Mac { get; init; }
    public required string Version { get; init; }
    public required string Verdict { get; init; }
    public required ChipLevel Level { get; init; }
}

public enum FlashPhase { Idle, Running, Succeeded, Failed, Cancelled }

/// <summary>
/// Firmware update over USB: pick a device and a firmware, press Flash. The remote or timer is reset into its ROM
/// bootloader, written, verified and restarted; the connection of the PC tool is handed back and forth automatically.
/// </summary>
public sealed partial class FirmwareViewModel : ObservableObject, IPageViewModel
{
    private const int MaxLogLines = 600;

    private readonly AppServices _app;
    private readonly ObservableCollection<PortInfo> _ports;
    private readonly Func<Task> _refreshPorts;
    private readonly Func<Task> _disconnect;
    private readonly Func<string, Task> _reconnect;
    private CancellationTokenSource? _cts;
    private string? _simulatedVersion;

    public FirmwareViewModel(AppServices app, ObservableCollection<PortInfo> ports, Func<Task> refreshPorts, Func<Task> disconnect, Func<string, Task> reconnect)
    {
        _app = app;
        _ports = ports;
        _refreshPorts = refreshPorts;
        _disconnect = disconnect;
        _reconnect = reconnect;
        app.Fleet.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(FleetState.IsLive)) RefreshDeviceInfo();
        };
        app.Fleet.Devices.CollectionChanged += (_, _) => RefreshTimerRows();
        app.Fleet.Remote.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName is nameof(RemoteInfoViewModel.HasData) or nameof(RemoteInfoViewModel.Firmware) or nameof(RemoteInfoViewModel.Name)
                or nameof(RemoteInfoViewModel.Mac) or nameof(RemoteInfoViewModel.Kind))
                RefreshDeviceInfo();
        };
        ports.CollectionChanged += (_, _) =>
        {
            if (!IsBusy && (SelectedPort is null || !ports.Any(p => p.Name == SelectedPort.Name))) PickDefaultPort();
        };
    }

    public NavPage Page => NavPage.Firmware;

    /// <summary>Folder searched for a <c>firmware</c> subfolder and for the repository (the app's own folder; tests point it elsewhere).</summary>
    public string AppDirectory { get; set; } = AppContext.BaseDirectory;

    /// <summary>Opens the port of the device to flash. Replaceable so that tests can use a simulated chip.</summary>
    public Func<PortInfo, IFlashLink> OpenLink { get; set; } = port => SerialFlashLink.Open(port.Name, port.IsUsbJtag || port.IsEspressif);

    /// <summary>Speed of the simulated chip of the offline demo (bytes per second); slow enough to watch the progress bar.</summary>
    public int SimulatedBytesPerSecond { get; set; } = 250_000;

    /// <summary>How long the device is given to re-appear on USB before the PC tool reconnects (tests shorten it).</summary>
    public TimeSpan RestartWait { get; set; } = TimeSpan.FromSeconds(2);

    public ObservableCollection<PortInfo> Ports => _ports;
    public ObservableCollection<FirmwareChoice> Choices { get; } = new();
    public ObservableCollection<string> LogLines { get; } = new();
    public ObservableCollection<TimerFirmwareRow> Timers { get; } = new();

    [ObservableProperty] private PortInfo? _selectedPort;
    [ObservableProperty] private FirmwareChoice? _selected;
    [ObservableProperty] private FlashMode _mode = FlashMode.Update;
    [ObservableProperty] private bool _manualBoot;
    [ObservableProperty] private FlashPhase _phase = FlashPhase.Idle;
    [ObservableProperty] private double _progress;
    [ObservableProperty] private bool _progressIndeterminate;
    [ObservableProperty] private string _stageText = "";
    [ObservableProperty] private string _detailText = "";
    [ObservableProperty] private string _resultText = "";
    [ObservableProperty] private ChipLevel _resultLevel = ChipLevel.Neutral;
    [ObservableProperty] private string _deviceTitle = "No device";
    [ObservableProperty] private string _deviceDetail = "";
    [ObservableProperty] private string _catalogNote = "";

    public bool IsBusy => Phase == FlashPhase.Running;
    public bool HasResult => ResultText.Length > 0;
    public bool IsSimulated => _app.IsSimulated;
    public bool HasChoices => Choices.Count > 0;
    public bool HasTimers => Timers.Count > 0;
    public bool CanReinstall => Selected?.Set.CanReinstall ?? false;
    public bool HasLog => LogLines.Count > 0;
    public string LogText => string.Join(Environment.NewLine, LogLines);

    public bool IsUpdate { get => Mode == FlashMode.Update; set { if (value) Mode = FlashMode.Update; } }
    public bool IsReinstall { get => Mode == FlashMode.Reinstall; set { if (value) Mode = FlashMode.Reinstall; } }
    public bool IsCleanInstall { get => Mode == FlashMode.CleanInstall; set { if (value) Mode = FlashMode.CleanInstall; } }

    public string FlashButtonText => IsBusy ? "Flashing..." : "Flash firmware";

    public bool CanFlash => !IsBusy && Selected is { HasProblems: false } && (IsSimulated || SelectedPort is not null) && ModeIsPossible;

    /// <summary>Checking only needs a device (a firmware to compare with is optional).</summary>
    public bool CanCheck => !IsBusy && (IsSimulated || SelectedPort is not null);

    private bool ModeIsPossible => Mode == FlashMode.Update || CanReinstall;

    partial void OnSelectedPortChanged(PortInfo? value)
    {
        RefreshDeviceInfo();
        FlashCommand.NotifyCanExecuteChanged();
        CheckCommand.NotifyCanExecuteChanged();
    }

    partial void OnSelectedChanged(FirmwareChoice? value)
    {
        if (value is not null && !value.Set.CanReinstall && Mode != FlashMode.Update) Mode = FlashMode.Update;
        OnPropertyChanged(nameof(CanReinstall));
        FlashCommand.NotifyCanExecuteChanged();
        RefreshTimerRows();
    }

    partial void OnModeChanged(FlashMode value)
    {
        OnPropertyChanged(nameof(IsUpdate));
        OnPropertyChanged(nameof(IsReinstall));
        OnPropertyChanged(nameof(IsCleanInstall));
        FlashCommand.NotifyCanExecuteChanged();
    }

    partial void OnPhaseChanged(FlashPhase value)
    {
        OnPropertyChanged(nameof(IsBusy));
        OnPropertyChanged(nameof(FlashButtonText));
        FlashCommand.NotifyCanExecuteChanged();
        CheckCommand.NotifyCanExecuteChanged();
        CancelCommand.NotifyCanExecuteChanged();
    }

    partial void OnResultTextChanged(string value) => OnPropertyChanged(nameof(HasResult));

    // ---- page life cycle ----------------------------------------------------------------------------------------

    public void Activate()
    {
        _ = ActivateAsync();
    }

    public void Deactivate()
    {
    }

    public async Task ActivateAsync()
    {
        OnPropertyChanged(nameof(IsSimulated));
        if (!IsBusy)
        {
            await _refreshPorts().ConfigureAwait(true);
            PickDefaultPort();
            await ReloadChoicesAsync().ConfigureAwait(true);
        }
        RefreshDeviceInfo();
        RefreshTimerRows();
    }

    private void PickDefaultPort()
    {
        if (SelectedPort is not null && _ports.Any(p => p.Name == SelectedPort.Name)) return;
        var connected = _app.Hub.State == HubState.Connected && !IsSimulated ? _app.Hub.TransportName : null;
        SelectedPort = _ports.FirstOrDefault(p => connected is not null && p.Name == connected)
                       ?? _ports.FirstOrDefault(p => p.IsEspressif)
                       ?? _ports.FirstOrDefault();
    }

    /// <summary>Called when the simulator starts or stops: the offered firmware and the device differ.</summary>
    public void OnSimulationChanged()
    {
        _simulatedVersion = null;
        OnPropertyChanged(nameof(IsSimulated));
        FlashCommand.NotifyCanExecuteChanged();
        CheckCommand.NotifyCanExecuteChanged();
        RefreshDeviceInfo();
        _ = ReloadChoicesAsync();
    }

    // ---- firmware list ------------------------------------------------------------------------------------------

    private async Task ReloadChoicesAsync(string? select = null)
    {
        var keep = select ?? Selected?.Path;
        var picked = _app.Settings.Current.FirmwarePaths.ToList();
        var found = await Task.Run(() => FirmwareCatalog.Discover(AppDirectory, picked)).ConfigureAwait(true);

        Choices.Clear();
        foreach (var set in found.Sets) Choices.Add(new FirmwareChoice(set));
        if (IsSimulated)
        {
            // so that the offline demo always has something to flash
            Choices.Add(new FirmwareChoice(DemoFirmware.CreateSet(FirmwareKind.Remote, "2.1.0")));
            Choices.Add(new FirmwareChoice(DemoFirmware.CreateSet(FirmwareKind.Timer, "2.1.0")));
        }

        CatalogNote = found.Notes.Count > 0 ? "Not usable: " + string.Join("; ", found.Notes) : "";
        OnPropertyChanged(nameof(HasChoices));

        Selected = Choices.FirstOrDefault(c => keep is not null && string.Equals(c.Path, keep, StringComparison.OrdinalIgnoreCase))
                   ?? PreferredFor(Choices);
        RefreshTimerRows();
    }

    /// <summary>The firmware that fits the connected device, else the first one.</summary>
    private FirmwareChoice? PreferredFor(IEnumerable<FirmwareChoice> choices)
    {
        var list = choices.ToList();
        var want = ConnectedKind();
        return list.FirstOrDefault(c => want != FirmwareKind.Unknown && c.Set.Kind == want && !c.HasProblems)
               ?? list.FirstOrDefault(c => !c.HasProblems)
               ?? list.FirstOrDefault();
    }

    private FirmwareKind ConnectedKind() => !_app.IsConnected ? FirmwareKind.Unknown : _app.Fleet.Remote.Kind switch
    {
        DeviceKind.Remote => FirmwareKind.Remote,
        DeviceKind.Timer => FirmwareKind.Timer,
        _ => FirmwareKind.Unknown,
    };

    [RelayCommand]
    private async Task BrowseFile()
    {
        var path = _app.Dialogs.PickOpenFile("Choose a firmware file", "Firmware image (*.bin)|*.bin|All files (*.*)|*.*");
        if (path is not null) await AddPickedAsync(path).ConfigureAwait(true);
    }

    [RelayCommand]
    private async Task BrowseFolder()
    {
        var path = _app.Dialogs.PickFolder("Choose a firmware folder (PlatformIO build output: firmware.bin, bootloader.bin, partitions.bin)");
        if (path is not null) await AddPickedAsync(path).ConfigureAwait(true);
    }

    private async Task AddPickedAsync(string path)
    {
        try
        {
            FirmwareSet.Load(path);        // fail early with a clear message
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or InvalidDataException or ArgumentException)
        {
            _app.Toasts.Error($"That cannot be used as firmware: {ex.Message}");
            return;
        }

        var paths = _app.Settings.Current.FirmwarePaths;
        paths.RemoveAll(p => string.Equals(p, path, StringComparison.OrdinalIgnoreCase));
        paths.Insert(0, path);
        if (paths.Count > 6) paths.RemoveRange(6, paths.Count - 6);
        _app.Settings.SaveSoon();
        await ReloadChoicesAsync(Path.GetFullPath(path)).ConfigureAwait(true);
    }

    [RelayCommand]
    private async Task RefreshAll()
    {
        await _refreshPorts().ConfigureAwait(true);
        PickDefaultPort();
        await ReloadChoicesAsync().ConfigureAwait(true);
    }

    // ---- what is plugged in -------------------------------------------------------------------------------------

    /// <summary>The PC tool has this port open, or is waiting for it to appear: it must let go before the flasher can use it.</summary>
    private bool HubOwnsSelectedPort =>
        !IsSimulated && _app.Hub.State != HubState.Disconnected && SelectedPort is { } p &&
        string.Equals(_app.Hub.TransportName, p.Name, StringComparison.OrdinalIgnoreCase);

    private bool HubIsOnSelectedPort => HubOwnsSelectedPort && _app.Hub.State == HubState.Connected;

    private void RefreshDeviceInfo()
    {
        if (IsSimulated)
        {
            var r = _app.Fleet.Remote;
            DeviceTitle = "Simulated ESP32-C3";
            DeviceDetail = $"{(r.Name.Length > 0 ? $"{r.KindText} \"{r.Name}\" · " : "")}firmware {_simulatedVersion ?? (r.Firmware.Length > 0 ? r.Firmware : "2.0.0")} · no real hardware is touched";
        }
        else if (HubIsOnSelectedPort)
        {
            var r = _app.Fleet.Remote;
            DeviceTitle = $"{r.KindText} \"{r.Name}\"";
            DeviceDetail = $"firmware {r.Firmware} · {r.Mac} · connected on {SelectedPort!.Name}; the connection is closed during the update and restored afterwards";
        }
        else if (SelectedPort is { } p)
        {
            DeviceTitle = p.IsEspressif ? "ESP32 device" : "Serial device";
            DeviceDetail = $"{p.Display} · not connected to the PC tool; it is identified when flashing starts";
        }
        else
        {
            DeviceTitle = "No device";
            DeviceDetail = "Plug the remote or timer in with a USB cable and press Refresh. A sleeping remote wakes up when you press a button.";
        }
    }

    private void RefreshTimerRows()
    {
        Timers.Clear();
        var latest = Choices.Where(c => c.Set.Kind == FirmwareKind.Timer && !c.HasProblems)
                            .Select(c => c.Set.Version)
                            .OrderByDescending(v => v, Comparer<string?>.Create(Versions.Compare))
                            .FirstOrDefault();
        foreach (var d in _app.Fleet.Devices)
        {
            string version = d.Fw;
            string verdict;
            ChipLevel level;
            if (string.IsNullOrEmpty(version)) { verdict = "not reported yet"; level = ChipLevel.Neutral; }
            else if (latest is null) { verdict = "up to date?"; level = ChipLevel.Neutral; }
            else
            {
                int c = Versions.Compare(version, latest);
                (verdict, level) = c == 0 ? ("up to date", ChipLevel.Ok) : c < 0 ? ($"older than {latest}", ChipLevel.Warn) : ($"newer than {latest}", ChipLevel.Neutral);
            }
            Timers.Add(new TimerFirmwareRow { Name = d.DisplayName, Mac = d.Mac, Version = string.IsNullOrEmpty(version) ? "-" : version, Verdict = verdict, Level = level });
        }
        OnPropertyChanged(nameof(HasTimers));
    }

    // ---- flashing -----------------------------------------------------------------------------------------------

    [RelayCommand(CanExecute = nameof(CanFlash))]
    private Task Flash() => RunAsync(checkOnly: false);

    /// <summary>Resets the device into its bootloader, identifies it and compares its flash with the chosen firmware. Writes nothing.</summary>
    [RelayCommand(CanExecute = nameof(CanCheck))]
    private Task Check() => RunAsync(checkOnly: true);

    private async Task RunAsync(bool checkOnly)
    {
        var choice = Selected;
        if (IsBusy || (choice is null && !checkOnly)) return;
        var set = choice?.Set;
        var kind = set?.Kind ?? FirmwareKind.Unknown;
        var mode = Mode;
        var port = SelectedPort;
        bool simulated = IsSimulated;
        if (!simulated && port is null) return;

        FlashPlan plan;
        try
        {
            plan = set is null ? new FlashPlan { Segments = Array.Empty<FlashSegment>() } : set.BuildPlan(mode);
        }
        catch (InvalidOperationException ex)
        {
            _app.Toasts.Warning(ex.Message);
            return;
        }

        // is this the right firmware for the device we are connected to?
        bool hubOnPort = HubOwnsSelectedPort;
        if (!checkOnly)
        {
            var live = hubOnPort ? ConnectedKind() : FirmwareKind.Unknown;
            string target = simulated ? "the simulated device" : port!.Name;
            if (live != FirmwareKind.Unknown && kind != FirmwareKind.Unknown && live != kind)
            {
                if (!_app.Dialogs.Confirm("Wrong firmware for this device?",
                        $"{target} is a {live.ToString().ToLowerInvariant()}, but this is {kind.ToString().ToLowerInvariant()} firmware. " +
                        "Writing it would turn the device into the other kind.",
                        "Write it anyway", destructive: true))
                    return;
            }
            else if (kind == FirmwareKind.Unknown)
            {
                if (!_app.Dialogs.Confirm("Unknown kind of firmware",
                        "This file does not say whether it is firmware for a timer or for a remote. Make sure it is the right one for the device.",
                        "Continue", destructive: false))
                    return;
            }

            if (mode == FlashMode.CleanInstall &&
                !_app.Dialogs.Confirm("Erase the settings too?",
                    "A clean install also erases the settings partition: the pairing list of a remote, the name, times and channel of a timer. The device starts like new.",
                    "Erase and install", destructive: true))
                return;
        }

        // ---- go ----
        _cts = new CancellationTokenSource();
        LogLines.Clear();
        OnPropertyChanged(nameof(HasLog));
        OnPropertyChanged(nameof(LogText));
        ResultText = "";
        ResultLevel = ChipLevel.Neutral;
        Progress = 0;
        ProgressIndeterminate = true;
        DetailText = "";
        StageText = "Preparing...";
        Phase = FlashPhase.Running;
        var clock = Stopwatch.StartNew();
        string portName = port?.Name ?? "";

        var known = KnownDevices();            // before the connection is closed: which MAC address belongs to which device
        var result = await ExecuteAsync(plan, kind, port, simulated, hubOnPort, checkOnly, _cts.Token).ConfigureAwait(true);

        Progress = result.Success ? 1 : Progress;
        ProgressIndeterminate = false;
        DetailText = "";
        StageText = "";
        string text = result.Message;
        if (result.Success && !checkOnly)
        {
            _simulatedVersion = simulated ? set?.Version : null;
            text += $" ({clock.Elapsed.TotalSeconds:0.#} s)";
        }
        if (result.Mac is not null && known.TryGetValue(result.Mac.ToUpperInvariant(), out var who)) text += $" This is {who}.";
        Phase = result.Success ? FlashPhase.Succeeded : result.Cancelled ? FlashPhase.Cancelled : FlashPhase.Failed;
        ResultLevel = result.Success ? ChipLevel.Ok : result.Cancelled ? ChipLevel.Warn : ChipLevel.Bad;
        ResultText = text;
        AppendLog(text);

        // ---- hand the port back to the PC tool and look at what the device says now ----
        if (hubOnPort && result.Restarted)
        {
            ResultText = text + " Reconnecting...";
            string? now = await ReconnectAndReadVersionAsync(portName).ConfigureAwait(true);
            ResultText = now is null
                ? text + " The device did not come back by itself yet: press its reset button or replug the cable; the PC tool reconnects automatically."
                : text + $" It reports firmware {now}.";
            if (result.Success && !checkOnly && now is not null && set?.Version is not null && now != set.Version) ResultLevel = ChipLevel.Warn;
        }
        else if (hubOnPort)
        {
            await _reconnect(portName).ConfigureAwait(true);        // the PC tool picks the device up again whenever it shows
        }

        if (result.Success) _app.Toasts.Success(ResultText);
        else if (result.Cancelled) _app.Toasts.Warning(ResultText);
        else _app.Toasts.Error(ResultText);
        RefreshDeviceInfo();
        RefreshTimerRows();
        _cts?.Dispose();
        _cts = null;
    }

    /// <summary>The devices the PC tool knows by MAC address (the chip's factory MAC is what the remote and the timers report as their own).</summary>
    private Dictionary<string, string> KnownDevices()
    {
        var map = new Dictionary<string, string>();
        var r = _app.Fleet.Remote;
        if (r.Mac.Length > 0) map[r.Mac.ToUpperInvariant()] = r.Kind == DeviceKind.Timer ? $"timer \"{r.Name}\"" : $"the remote \"{r.Name}\"";
        foreach (var d in _app.Fleet.Devices)
            if (d.Mac.Length > 0) map[d.Mac.ToUpperInvariant()] = d.DisplayName;
        return map;
    }

    /// <summary>Hands the port over, runs the flasher on a worker thread and returns its outcome. Never throws.</summary>
    private async Task<FlashResult> ExecuteAsync(FlashPlan plan, FirmwareKind kind, PortInfo? port, bool simulated, bool closeHubFirst, bool checkOnly, CancellationToken ct)
    {
        void Post(Action a) => _app.Ui.Post(a);
        void Log(string line) => Post(() => AppendLog(line));
        Stopwatch? writing = null;
        var progress = new CallbackProgress<FlashProgress>(p => Post(() =>
        {
            if (Phase != FlashPhase.Running) return;                  // a late update must not overwrite the final state
            StageText = p.Message;
            ProgressIndeterminate = p.Stage is FlashStage.EnteringBootloader or FlashStage.Identifying or FlashStage.Checking;
            Progress = p.Fraction;
            if (p.Stage == FlashStage.Writing && p.Total > 0)
            {
                writing ??= Stopwatch.StartNew();
                double secs = Math.Max(0.2, writing.Elapsed.TotalSeconds);
                double rate = p.Done / secs;
                string eta = p.Done > 0 && rate > 0 ? $" · about {Fmt.Duration((long)Math.Ceiling((p.Total - p.Done) / rate))} left" : "";
                DetailText = $"{p.Done / 1024:N0} of {p.Total / 1024:N0} KB · {rate / 1024:0} KB/s{eta}";
            }
            else
            {
                DetailText = "";
            }
        }));

        string portName = port?.Name ?? "";
        try
        {
            if (closeHubFirst)
            {
                AppendLog($"Closing the connection to {portName} for the update.");
                await _disconnect().ConfigureAwait(true);
                await Task.Delay(300, CancellationToken.None).ConfigureAwait(true);
            }

            IFlashLink link;
            try
            {
                link = simulated ? CreateSimulatedChip(kind) : await Task.Run(() => OpenLink(port!), CancellationToken.None).ConfigureAwait(true);
            }
            catch (UnauthorizedAccessException)
            {
                return new FlashResult { Message = $"{portName} is in use by another program (a serial monitor, PlatformIO, another copy of this tool). Close it and try again." };
            }
            catch (Exception ex) when (ex is IOException or InvalidOperationException or ArgumentException)
            {
                return new FlashResult { Message = $"Could not open {portName}: {ex.Message}" };
            }

            try
            {
                var options = new FlashOptions { Entry = ManualBoot ? BootEntry.AlreadyThere : BootEntry.Auto, WriteNothing = checkOnly };
                return await Task.Run(() => FirmwareFlasher.RunAsync(link, plan, options, progress, Log, ct), CancellationToken.None).ConfigureAwait(true);
            }
            finally
            {
                link.Dispose();
            }
        }
        catch (Exception ex)
        {
            CrashLog.Write("Firmware update", ex);
            return new FlashResult { Message = $"Unexpected problem: {ex.Message}" };
        }
    }

    private async Task<string?> ReconnectAndReadVersionAsync(string portName)
    {
        var hello = new TaskCompletionSource<HelloInfo>(TaskCreationOptions.RunContinuationsAsynchronously);
        void OnHello(HelloInfo h) => hello.TrySetResult(h);
        _app.Hub.Connected += OnHello;
        try
        {
            await Task.Delay(RestartWait, CancellationToken.None).ConfigureAwait(true);
            AppendLog($"Reconnecting to {portName}...");
            await _reconnect(portName).ConfigureAwait(true);
            var done = await Task.WhenAny(hello.Task, Task.Delay(TimeSpan.FromSeconds(15))).ConfigureAwait(true);
            if (done != hello.Task) return null;
            var h = await hello.Task.ConfigureAwait(true);
            AppendLog($"The device answers: {h.Kind.ToString().ToLowerInvariant()} \"{h.Name}\", firmware {h.Fw}.");
            return h.Fw;
        }
        finally
        {
            _app.Hub.Connected -= OnHello;
        }
    }

    /// <summary>The virtual chip of the offline demo: holds the project's layout and an older application, so that updates look real.</summary>
    private SimulatedRom CreateSimulatedChip(FirmwareKind kind)
    {
        var old = DemoFirmware.CreateSet(kind == FirmwareKind.Timer ? FirmwareKind.Timer : FirmwareKind.Remote, "2.0.0");
        var flash = new byte[SimulatedRom.FlashSize];
        Array.Fill(flash, (byte)0xFF);
        old.Bootloader!.CopyTo(flash, 0);
        old.PartitionTableBin!.CopyTo(flash, 0x8000);
        old.BootApp0!.CopyTo(flash, 0xE000);
        old.App.CopyTo(flash, 0x10000);

        byte[] mac = { 0x40, 0x4C, 0xCA, 0x5B, 0x12, 0x34 };
        var text = _app.Fleet.Remote.Mac;
        if (!string.IsNullOrEmpty(text))
        {
            var parts = text.Split(':');
            if (parts.Length == 6 && parts.All(p => byte.TryParse(p, System.Globalization.NumberStyles.HexNumber, null, out _)))
                mac = parts.Select(p => Convert.ToByte(p, 16)).ToArray();
        }
        return new SimulatedRom(new SimulatedRomOptions { InitialFlash = flash, Mac = mac, BytesPerSecond = SimulatedBytesPerSecond });
    }

    [RelayCommand(CanExecute = nameof(IsBusy))]
    private void Cancel()
    {
        _cts?.Cancel();
        StageText = "Cancelling...";
    }

    [RelayCommand]
    private void CopyLog()
    {
        if (LogLines.Count > 0) _app.Dialogs.CopyText(LogText);
    }

    private void AppendLog(string line)
    {
        LogLines.Add($"{DateTime.Now:HH:mm:ss}  {line}");
        while (LogLines.Count > MaxLogLines) LogLines.RemoveAt(0);
        OnPropertyChanged(nameof(HasLog));
        OnPropertyChanged(nameof(LogText));
    }

    /// <summary>True while a flash is running: closing the window now would leave a device without working firmware.</summary>
    public bool MustNotClose => IsBusy;

    /// <summary>Cancels a running update (used when the application closes).</summary>
    public void CancelForShutdown() => _cts?.Cancel();
}

/// <summary>An <see cref="IProgress{T}"/> that calls back on the reporting thread (no sync-context hop, unlike Progress&lt;T&gt;).</summary>
internal sealed class CallbackProgress<T> : IProgress<T>
{
    private readonly Action<T> _action;
    public CallbackProgress(Action<T> action) => _action = action;
    public void Report(T value) => _action(value);
}
