using System.Collections.ObjectModel;
using System.IO;
using System.Windows.Threading;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using FogConsole.Hub;
using FogConsole.Protocol;
using FogConsole.Services;
using FogConsole.Simulator;
using FogConsole.Transport;

namespace FogConsole.ViewModels;

public sealed record NavItem(NavPage Page, string Title, string Glyph, string Tooltip, string Shortcut);

public enum ChipLevel { Neutral, Ok, Warn, Bad, Sim }

/// <summary>Demo controls of the simulator (number of timers, packet loss, power cuts, interference, pause, seed).</summary>
public sealed partial class SimulatorPanelViewModel : ObservableObject
{
    private readonly AppServices _app;
    private readonly Func<Task> _restart;

    public SimulatorPanelViewModel(AppServices app, Func<Task> restart)
    {
        _app = app;
        _restart = restart;
        app.Sim.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(SimulatorController.TimePaused)) OnPropertyChanged(nameof(PauseText));
        };
    }

    public SimulatorController Sim => _app.Sim;
    public ObservableCollection<DeviceViewModel> Devices => _app.Fleet.Devices;

    [ObservableProperty] private DeviceViewModel? _cutTarget;
    [ObservableProperty] private bool _isOpen;

    public string PauseText => Sim.TimePaused ? "Resume time" : "Pause time";
    public string PauseGlyph => Sim.TimePaused ? "" : "";

    public int TimerCount
    {
        get => Sim.TimerCount;
        set
        {
            Sim.TimerCount = Math.Clamp(value, 1, SimulatorController.MaxTimers);
            OnPropertyChanged();
        }
    }

    [RelayCommand]
    private void CutPower()
    {
        var target = CutTarget ?? _app.Fleet.Selected ?? _app.Fleet.Devices.FirstOrDefault();
        if (target is null)
        {
            _app.Toasts.Warning("There is no timer to cut.");
            return;
        }
        if (Sim.CutPower(target.Index, 30))
            _app.Toasts.Info($"Power to {target.DisplayName} cut for 30 s - like a smoke machine heating up. It reboots when the power returns.");
    }

    [RelayCommand]
    private void Interfere()
    {
        int ch = Sim.Interfere(60);
        if (ch > 0) _app.Toasts.Info($"A jammer appears on channel {ch} for 60 s. Watch the link quality drop; run a survey and move the system to a quieter channel.");
    }

    [RelayCommand]
    private void TogglePause()
    {
        Sim.TimePaused = !Sim.TimePaused;
        _app.Clock.Paused = Sim.TimePaused;
        OnPropertyChanged(nameof(PauseText));
        OnPropertyChanged(nameof(PauseGlyph));
        _app.Toasts.Info(Sim.TimePaused ? "Simulated time is paused." : "Simulated time runs again.");
    }

    [RelayCommand]
    private async Task Restart()
    {
        IsOpen = false;
        await _restart();
    }

    [RelayCommand]
    private void NewSeed()
    {
        Sim.Seed = Random.Shared.Next(1, 100_000);
    }
}

/// <summary>Owns the connection, the pages, the top bar and the status bar.</summary>
public sealed partial class MainViewModel : ObservableObject
{
    private readonly EventPump _pump;
    private readonly List<IMessageSink> _sinks = new();
    private readonly DispatcherTimer? _tick;
    private long _lastLines;
    private double _lastLinesAt;
    private int _tickCount;
    private HubState _lastToastState = HubState.Disconnected;
    private bool _startedFromCli;

    public MainViewModel(AppServices app, EventPump pump, DispatcherTimer? tick)
    {
        App = app;
        _pump = pump;
        _tick = tick;

        // build the object graph
        app.Fleet = new FleetState(app);
        app.Commands = new CommandCenter(app.Hub, app.Ui, app.Toasts, app.Fleet.NameOfMac, app.Fleet.NameOfIndex);
        app.Hold = new HoldController(spec => app.Hub.BeginCommand(spec), TimeProvider.System, msg => app.Ui.Post(() => app.Toasts.Warning(msg)),
            mac => app.Fleet.NameOfMac(mac) ?? mac);
        app.Navigate = Navigate;

        Dashboard = new DashboardViewModel(app, ConnectAsync, StartSimulatorAsync);
        Device = new DeviceDetailViewModel(app);
        Pairing = new PairingViewModel(app);
        Channels = new ChannelsViewModel(app);
        Diagnostics = new DiagnosticsViewModel(app);
        Console = new ConsoleViewModel(app);
        Settings = new SettingsViewModel(app, () => Console.ApplyBufferSizes());
        SimPanel = new SimulatorPanelViewModel(app, RestartSimulatorAsync);

        Pages = new IPageViewModel[] { Dashboard, Device, Pairing, Channels, Diagnostics, Console, Settings }
            .ToDictionary(p => p.Page);
        NavItems = new[]
        {
            new NavItem(NavPage.Dashboard, "Dashboard", "", "All timers at a glance (Ctrl+1)", "Ctrl+1"),
            new NavItem(NavPage.Device, "Device", "", "Details and settings of the selected timer (Ctrl+2)", "Ctrl+2"),
            new NavItem(NavPage.Pairing, "Pairing", "", "Discover, pair and unpair timers (Ctrl+3)", "Ctrl+3"),
            new NavItem(NavPage.Channels, "Channels", "", "Channel survey, moving the system, link lab (Ctrl+4)", "Ctrl+4"),
            new NavItem(NavPage.Diagnostics, "Diagnostics", "", "Performance counters of the remote and every timer (Ctrl+5)", "Ctrl+5"),
            new NavItem(NavPage.Console, "Console", "", "Log, packet inspector and raw JSON console (Ctrl+6)", "Ctrl+6"),
            new NavItem(NavPage.Settings, "Settings", "", "Remote settings, preferences and about (Ctrl+7)", "Ctrl+7"),
        };
        _selectedNav = NavItems[0];
        _currentPage = Dashboard;

        _sinks.AddRange(new IMessageSink[] { app.Fleet, Pairing, Channels, Diagnostics, Console });

        // hub -> UI thread
        app.Hub.MessageReceived += m => _pump.Post(m);
        app.Hub.RawLineSeen += r => _pump.Post(r);
        app.Hub.StateChanged += s => _pump.Post(s);
        app.Hub.Connected += h => _pump.Post(h);
        _pump.ItemReady += OnPumpItem;

        if (_tick is not null)
        {
            _tick.Tick += (_, _) => OnTick();
            _tick.Start();
        }

        app.Hub.AutoReconnect = app.Settings.Current.AutoReconnect;
        app.Sim.Kind = app.Settings.Current.SimulatorKind == "Timer" ? SimKind.Timer : SimKind.Remote;
        app.Theme.Changed += () => OnPropertyChanged(nameof(IsDarkTheme));
    }

    public AppServices App { get; }
    public HubClient Hub => App.Hub;
    public FleetState Fleet => App.Fleet;
    public ToastService Toasts => App.Toasts;
    public SimulatorController Sim => App.Sim;

    public DashboardViewModel Dashboard { get; }
    public DeviceDetailViewModel Device { get; }
    public PairingViewModel Pairing { get; }
    public ChannelsViewModel Channels { get; }
    public DiagnosticsViewModel Diagnostics { get; }
    public ConsoleViewModel Console { get; }
    public SettingsViewModel Settings { get; }
    public SimulatorPanelViewModel SimPanel { get; }
    public IReadOnlyDictionary<NavPage, IPageViewModel> Pages { get; }
    public IReadOnlyList<NavItem> NavItems { get; }

    public bool IsDarkTheme => App.Theme.Effective == EffectiveTheme.Dark;

    // ---- navigation ------------------------------------------------------------------------

    [ObservableProperty] private NavItem _selectedNav;
    [ObservableProperty] private object _currentPage;

    public NavPage CurrentNav => SelectedNav.Page;

    partial void OnSelectedNavChanged(NavItem? oldValue, NavItem newValue)
    {
        if (oldValue is not null) Pages[oldValue.Page].Deactivate();
        CurrentPage = Pages[newValue.Page];
        Pages[newValue.Page].Activate();
        OnPropertyChanged(nameof(CurrentNav));
    }

    public void Navigate(NavPage page)
    {
        var item = NavItems.First(n => n.Page == page);
        if (!ReferenceEquals(item, SelectedNav)) SelectedNav = item;
    }

    [RelayCommand]
    private void GoTo(string? page)
    {
        if (Enum.TryParse<NavPage>(page, true, out var p)) Navigate(p);
    }

    // ---- ports and connection --------------------------------------------------------------

    public ObservableCollection<PortInfo> Ports { get; } = new();

    [ObservableProperty] private PortInfo? _selectedPort;
    [ObservableProperty] private bool _portsLoading;
    [ObservableProperty] private HubState _state = HubState.Disconnected;
    [ObservableProperty] private string _connectionText = "Not connected";
    [ObservableProperty] private string _connectionDetail = "Choose a port and press Connect, or try the offline demo.";
    [ObservableProperty] private ChipLevel _connectionLevel = ChipLevel.Neutral;
    [ObservableProperty] private string _latencyText = "";
    [ObservableProperty] private bool _isBusyConnecting;
    [ObservableProperty] private string _linesText = "";
    [ObservableProperty] private string _badJsonText = "";
    [ObservableProperty] private bool _hasBadJson;
    [ObservableProperty] private string _protocolText = "FogSerial/2";

    /// <summary>True when the window is narrow: the top bar drops its labels (set by the window, a pure view concern).</summary>
    [ObservableProperty] private bool _isCompact;

    /// <summary>Shows the simulator demo controls as a card inside the window instead of a flyout (used for screenshots).</summary>
    [ObservableProperty] private bool _simPanelInline;

    public bool HasSelectedPort => SelectedPort is not null;
    public string PortPlaceholder => IsSimulated ? "Simulator (no port needed)" : Ports.Count == 0 ? "No serial port found" : "Choose a port";

    /// <summary>While simulated and connected the SIMULATED chip says it all; the connection chip only shows problems.</summary>
    public bool ShowConnectionChip => !(IsSimulated && State == HubState.Connected);

    public string RemoteChipTooltip =>
        $"{Fleet.Remote.KindText}: battery {Fleet.Remote.BatteryText} ({Fleet.Remote.Millivolts} mV), " +
        (Fleet.Remote.UsbPower ? "USB / external power present, " : "") +
        $"radio channel {Fleet.Remote.Channel}. The lock means: while this PC is connected the remote is kept awake (no display blanking, no deep sleep).";

    public bool IsConnected => State == HubState.Connected;
    public bool IsDisconnected => State == HubState.Disconnected;
    public bool IsWaiting => State == HubState.WaitingForDevice;
    public bool IsSimulated => Sim.IsActive;
    public bool CanChoosePort => State == HubState.Disconnected;
    public string ConnectButtonText => State == HubState.Disconnected ? "Connect" : State == HubState.Connecting ? "Connecting..." : "Disconnect";
    public string WindowTitle => IsSimulated ? "Fog Console - SIMULATED (no hardware connected)" : IsConnected ? $"Fog Console - {Fleet.Remote.KindText} {Fleet.Remote.Name}" : "Fog Console";
    public string WaitingText => "Device asleep or unplugged — waiting for it to come back...";
    public string ForceOnText
    {
        get
        {
            var names = Fleet.Devices.Where(d => d.IsForceOn).Select(d => d.DisplayName).ToList();
            return names.Count == 0 ? "" : $"FORCE ON — {string.Join(", ", names)} {(names.Count == 1 ? "is" : "are")} forced on. The output stays on only while the lease is refreshed.";
        }
    }

    partial void OnStateChanged(HubState value)
    {
        OnPropertyChanged(nameof(IsConnected));
        OnPropertyChanged(nameof(IsDisconnected));
        OnPropertyChanged(nameof(IsWaiting));
        OnPropertyChanged(nameof(CanChoosePort));
        OnPropertyChanged(nameof(ConnectButtonText));
        OnPropertyChanged(nameof(WindowTitle));
        OnPropertyChanged(nameof(ShowConnectionChip));
        OnPropertyChanged(nameof(PortPlaceholder));
    }

    partial void OnSelectedPortChanged(PortInfo? value) => OnPropertyChanged(nameof(HasSelectedPort));

    public async Task RefreshPortsAsync()
    {
        PortsLoading = true;
        try
        {
            var list = await PortCatalog.GetPortsAsync().ConfigureAwait(true);
            var keep = SelectedPort?.Name ?? App.Settings.Current.LastPort;
            if (list.Count == Ports.Count && list.Zip(Ports).All(z => z.First.Name == z.Second.Name && z.First.Description == z.Second.Description)) return;
            Ports.Clear();
            foreach (var p in list) Ports.Add(p);
            SelectedPort = Ports.FirstOrDefault(p => p.Name == keep) ?? Ports.FirstOrDefault(p => p.IsEspressif) ?? Ports.FirstOrDefault();
        }
        catch (Exception ex)
        {
            Toasts.Warning($"Could not list the serial ports: {ex.Message}");
        }
        finally
        {
            PortsLoading = false;
        }
    }

    [RelayCommand]
    private Task RefreshPorts() => RefreshPortsAsync();

    [RelayCommand]
    private async Task ToggleConnection()
    {
        if (State == HubState.Disconnected) await ConnectAsync();
        else await DisconnectAsync();
    }

    public async Task ConnectAsync()
    {
        if (Sim.IsActive) await StopSimulatorAsync();
        var port = SelectedPort;
        if (port is null)
        {
            await RefreshPortsAsync();
            port = SelectedPort;
        }
        if (port is null)
        {
            Toasts.Warning("No serial port found. Plug in the remote's USB cable, or try the offline demo.");
            return;
        }
        await ConnectPortAsync(port.Name, waitIfAbsent: false);
    }

    public async Task ConnectPortAsync(string portName, bool waitIfAbsent)
    {
        App.Settings.Current.LastPort = portName;
        App.Settings.Current.LastWasSimulator = false;
        App.Settings.SaveSoon();
        Hub.AutoReconnect = App.Settings.Current.AutoReconnect;
        IsBusyConnecting = true;
        try
        {
            await Hub.ConnectAsync(() => new SerialTransport(portName), waitIfAbsent);
        }
        catch (Exception ex)
        {
            Toasts.Error(FriendlyConnectError(portName, ex));
        }
        finally
        {
            IsBusyConnecting = false;
        }
    }

    public static string FriendlyConnectError(string port, Exception ex) => ex switch
    {
        UnauthorizedAccessException => $"{port} is in use by another program (or the device is still starting). Close the other terminal / flasher and try again.",
        FileNotFoundException => $"{port} does not exist (any more). Plug the USB cable in again and refresh the port list.",
        IOException io when io.Message.Contains("does not exist", StringComparison.OrdinalIgnoreCase) => $"{port} does not exist (any more). Plug the USB cable in again and refresh the port list.",
        HubException h => $"{port}: {h.Message}",
        _ => $"Could not open {port}: {ex.Message}",
    };

    public async Task DisconnectAsync()
    {
        App.Hold.EndAll();
        await Hub.DisconnectAsync();
        if (Sim.IsActive) Sim.Stop();
        NotifySimChanged();
    }

    // ---- simulator -------------------------------------------------------------------------

    [RelayCommand]
    private async Task ToggleSimulator()
    {
        if (Sim.IsActive) await StopSimulatorAsync();
        else await StartSimulatorAsync();
    }

    public async Task StartSimulatorAsync()
    {
        if (Hub.State != HubState.Disconnected) await DisconnectAsync();
        Sim.StartNew();
        App.Clock.Paused = false;
        App.Settings.Current.LastWasSimulator = true;
        App.Settings.SaveSoon();
        NotifySimChanged();
        IsBusyConnecting = true;
        try
        {
            Hub.AutoReconnect = true;
            await Hub.ConnectAsync(() => Sim.CreateTransport(), waitIfAbsent: true);
            Toasts.Info("Simulator started: ten virtual smoke machine timers, no hardware needed.");
        }
        catch (Exception ex)
        {
            Toasts.Error($"Could not start the simulator: {ex.Message}");
            Sim.Stop();
            NotifySimChanged();
        }
        finally
        {
            IsBusyConnecting = false;
        }
    }

    public async Task StopSimulatorAsync()
    {
        App.Hold.EndAll();
        await Hub.DisconnectAsync();
        Sim.Stop();
        App.Clock.Paused = false;
        App.Settings.Current.LastWasSimulator = false;
        NotifySimChanged();
    }

    private async Task RestartSimulatorAsync()
    {
        await StopSimulatorAsync();
        await StartSimulatorAsync();
    }

    private void NotifySimChanged()
    {
        OnPropertyChanged(nameof(IsSimulated));
        OnPropertyChanged(nameof(WindowTitle));
        OnPropertyChanged(nameof(ShowConnectionChip));
        OnPropertyChanged(nameof(PortPlaceholder));
        if (!Sim.IsActive) SimPanel.IsOpen = false;
    }

    // ---- hub events on the UI thread -------------------------------------------------------

    private void OnPumpItem(object item)
    {
        switch (item)
        {
            case Message m:
                foreach (var sink in _sinks) sink.OnMessage(m);
                break;
            case RawLine r:
                Console.OnRaw(r);
                break;
            case HubStateChange change:
                OnStateChanged(change);
                break;
            case HelloInfo hello:
                OnConnected(hello);
                break;
        }
    }

    private void OnStateChanged(HubStateChange change)
    {
        State = change.New;
        switch (change.New)
        {
            case HubState.Connected:
                ConnectionLevel = ChipLevel.Ok;
                ConnectionText = "Connected";
                ConnectionDetail = Hub.TransportName;
                Fleet.SetLive(true, clear: false);
                break;
            case HubState.Connecting:
                ConnectionLevel = ChipLevel.Warn;
                ConnectionText = "Connecting...";
                ConnectionDetail = Hub.TransportName;
                break;
            case HubState.WaitingForDevice:
                ConnectionLevel = ChipLevel.Warn;
                ConnectionText = "Waiting for device";
                ConnectionDetail = change.Reason;
                App.Hold.EndAll();
                Fleet.SetLive(false, clear: false);
                ClearPageSessions();
                if (_lastToastState != HubState.WaitingForDevice)
                    Toasts.Warning("Device asleep or unplugged - waiting for it to come back. The connection resumes by itself.");
                break;
            case HubState.Disconnected:
                ConnectionLevel = ChipLevel.Neutral;
                ConnectionText = "Not connected";
                ConnectionDetail = string.IsNullOrEmpty(Hub.LastError) ? "Choose a port and press Connect, or try the offline demo." : Hub.LastError!;
                LatencyText = "";
                App.Hold.EndAll();
                Fleet.SetLive(false, clear: true);
                ClearPageSessions();
                break;
        }
        Console.AppLog(change.New == HubState.WaitingForDevice ? LogLevel.Warn : LogLevel.Info,
            change.New switch
            {
                HubState.Connected => $"Connected - {change.Reason}",
                HubState.Connecting => "Connecting...",
                HubState.WaitingForDevice => $"Waiting for the device - {change.Reason}",
                _ => string.IsNullOrEmpty(change.Reason) ? "Disconnected" : $"Disconnected - {change.Reason}",
            });
        _lastToastState = change.New;
        OnPropertyChanged(nameof(WindowTitle));
        Dashboard.Activate();
    }

    private void ClearPageSessions()
    {
        Pairing.ClearSession();
        Channels.ClearSession();
        Diagnostics.ClearSession();
        Settings.OnDisconnected();
    }

    private void OnConnected(HelloInfo hello)
    {
        Fleet.Remote.Apply(hello);
        Fleet.SetLive(true, clear: false);
        ProtocolText = $"FogSerial/{hello.Proto}  ·  {hello.Kind.ToString().ToLowerInvariant()} fw {hello.Fw}";
        ConnectionText = Sim.IsActive ? "Simulated" : "Connected";
        ConnectionDetail = $"{Fleet.Remote.KindText} \"{hello.Name}\"  fw {hello.Fw}  ·  {Hub.TransportName}";
        if (hello.Proto != 2) Toasts.Warning($"The device speaks protocol {hello.Proto}; this tool is written for protocol 2. Some things may not work.");
        Toasts.Success($"Connected to {Fleet.Remote.KindText.ToLowerInvariant()} \"{hello.Name}\" (firmware {hello.Fw}).");
        _ = InitializeSessionAsync();
    }

    /// <summary>Everything that has to be (re-)sent to the device after a connection was established.</summary>
    private async Task InitializeSessionAsync()
    {
        try
        {
            await Settings.LoadAsync();
            await Hub.RequestAsync(Requests.GetFleet(), retries: 1);
            await Console.ApplySessionAsync();
            await Diagnostics.ApplySessionAsync();
            Pairing.ReapplyAfterReconnect();
        }
        catch (HubException)
        {
            // the connection dropped again; the state machine reports it
        }
        catch (Exception ex)
        {
            CrashLog.Write("InitializeSession", ex);
        }
    }

    // ---- periodic work ---------------------------------------------------------------------

    private void OnTick()
    {
        _tickCount++;
        Fleet.Tick();
        Console.Flush();

        if (_tickCount % 5 == 0) OnPropertyChanged(nameof(ForceOnText));
        if (_tickCount % 20 == 3) OnPropertyChanged(nameof(RemoteChipTooltip));

        if (_tickCount % 20 == 0)
        {
            // ~1 Hz
            Pairing.Tick();
            Channels.Tick();
            Diagnostics.Tick();
            UpdateStatusBar();
        }
        if (_tickCount % 100 == 0 && State == HubState.Disconnected && !Sim.IsActive) _ = RefreshPortsAsync();
    }

    private void UpdateStatusBar()
    {
        double now = App.Clock.Seconds;
        var c = Hub.Counters;
        long lines = c.LinesRx;
        double dt = now - _lastLinesAt;
        if (_lastLinesAt > 0 && dt > 0.2) LinesText = $"{(lines - _lastLines) / dt:0} lines/s";
        _lastLines = lines;
        _lastLinesAt = now;

        HasBadJson = c.BadJson > 0;
        BadJsonText = c.BadJson == 0 ? "bad JSON: 0" : $"bad JSON: {c.BadJson}";

        if (Hub.State == HubState.Connected)
        {
            if (!Hub.IsResponsive)
            {
                LatencyText = "no answer";
                ConnectionLevel = ChipLevel.Bad;
                ConnectionText = "No answer";
            }
            else
            {
                var l = Hub.Latency;
                LatencyText = double.IsNaN(l.LastMs) ? "" : $"{l.LastMs:0} ms";
                ConnectionLevel = Sim.IsActive ? ChipLevel.Sim : ChipLevel.Ok;
                ConnectionText = Sim.IsActive ? "Simulated" : "Connected";
            }
        }
    }

    // ---- startup ---------------------------------------------------------------------------

    /// <summary>Applies the command line / settings after the window exists.</summary>
    public async Task StartAsync(StartupOptions options)
    {
        _startedFromCli = options.Sim;
        await RefreshPortsAsync();

        if (options.Page is { } page) Navigate(page);

        if (options.Sim)
        {
            Sim.Kind = options.SimTimer ? SimKind.Timer : Sim.Kind;
            if (options.Seed is { } seed) Sim.Seed = seed;
            if (options.Paired is { } n) Sim.TimerCount = Math.Max(1, n);
            Sim.DemoScenario = options.Demo;
            Sim.Chaos = !options.NoChaos;
            await StartSimulatorAsync();
            if (options.Select is { } sel)
            {
                // wait for the first fleet event, then select
                for (int i = 0; i < 50 && Fleet.Devices.Count == 0; i++) await Task.Delay(100);
                var dev = Fleet.ByIndex(sel);
                if (dev is not null) Fleet.SelectDevice(dev);
            }
        }
        else if (App.Settings.Current.AutoConnect && !options.IsScreenshot)
        {
            var last = App.Settings.Current.LastPort;
            if (!string.IsNullOrEmpty(last) && Ports.Any(p => p.Name == last))
                await ConnectPortAsync(last, waitIfAbsent: true);
        }
    }

    public bool StartedFromCli => _startedFromCli;

    public void ShutDown()
    {
        App.Hold.Dispose();
        App.Settings.Save();
        _tick?.Stop();
    }
}
