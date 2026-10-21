using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using FogConsole.Hub;
using FogConsole.Protocol;
using FogConsole.Services;

namespace FogConsole.ViewModels;

/// <summary>The grid of timer cards with the bulk actions bar.</summary>
public sealed partial class DashboardViewModel : ObservableObject, IPageViewModel
{
    private readonly AppServices _app;
    private readonly Func<Task> _connect;
    private readonly Func<Task> _startDemo;

    public DashboardViewModel(AppServices app, Func<Task> connect, Func<Task> startDemo)
    {
        _app = app;
        _connect = connect;
        _startDemo = startDemo;
        _bulkOn = 1.5;
        _bulkOff = 10.0;
        app.Fleet.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName is nameof(FleetState.IsLive) or nameof(FleetState.HasFleetData))
                RefreshState();
        };
        app.Fleet.Devices.CollectionChanged += (_, _) => RefreshState();
        app.Hub.StateChanged += _ => app.Ui.Post(RefreshState);
    }

    public NavPage Page => NavPage.Dashboard;
    public void Activate() => RefreshState();
    public void Deactivate() { }

    public FleetState Fleet => _app.Fleet;
    public ObservableCollection<DeviceViewModel> Devices => _app.Fleet.Devices;
    public SimulatorController Sim => _app.Sim;

    // ---- empty states ----------------------------------------------------------------------

    public HubState HubState => _app.Hub.State;
    public bool ShowConnectPrompt => _app.Hub.State == HubState.Disconnected;
    public bool ShowWaiting => _app.Hub.State is HubState.WaitingForDevice or HubState.Connecting && !_app.Fleet.HasFleetData;
    public bool ShowNoTimers => _app.Hub.State == HubState.Connected && _app.Fleet.HasFleetData && _app.Fleet.Devices.Count == 0;
    public bool ShowGrid => _app.Fleet.Devices.Count > 0;
    public bool CanBulk => _app.Fleet.IsLive && _app.Fleet.Devices.Count > 0;
    public bool IsDirectTimer => _app.Fleet.Remote.IsTimer;

    private void RefreshState()
    {
        OnPropertyChanged(nameof(HubState));
        OnPropertyChanged(nameof(ShowConnectPrompt));
        OnPropertyChanged(nameof(ShowWaiting));
        OnPropertyChanged(nameof(ShowNoTimers));
        OnPropertyChanged(nameof(ShowGrid));
        OnPropertyChanged(nameof(CanBulk));
        OnPropertyChanged(nameof(IsDirectTimer));
        OnPropertyChanged(nameof(DeviceCountText));
        PauseAllCommand.NotifyCanExecuteChanged();
        ResumeAllCommand.NotifyCanExecuteChanged();
        ResetAllCommand.NotifyCanExecuteChanged();
        ToggleAllCommand.NotifyCanExecuteChanged();
        ApplyTimesAllCommand.NotifyCanExecuteChanged();
        StaggerStartCommand.NotifyCanExecuteChanged();
    }

    public string DeviceCountText
    {
        get
        {
            int n = Devices.Count;
            int online = Devices.Count(d => d.Online);
            return n == 0 ? "" : $"{online} of {n} online";
        }
    }

    [RelayCommand]
    private Task Connect() => _connect();

    [RelayCommand]
    private Task StartDemo() => _startDemo();

    [RelayCommand]
    private void GoToPairing() => _app.Navigate(NavPage.Pairing);

    // ---- bulk actions ----------------------------------------------------------------------

    [ObservableProperty] private double _bulkOn;
    [ObservableProperty] private double _bulkOff;
    [ObservableProperty] private bool _bulkPopupOpen;

    private bool CanBulkExecute() => CanBulk;

    private Task BulkAll(CommandSpec spec, string title) => _app.Commands.RunBulkAsync(new[] { spec }, title, toastOnSuccess: false);

    [RelayCommand(CanExecute = nameof(CanBulkExecute))]
    private Task PauseAll() => BulkAll(CommandSpec.Mode(CommandTarget.All, TimerMode.Paused), "Pause all");

    [RelayCommand(CanExecute = nameof(CanBulkExecute))]
    private Task ResumeAll() => BulkAll(CommandSpec.Mode(CommandTarget.All, TimerMode.Auto), "Resume all");

    [RelayCommand(CanExecute = nameof(CanBulkExecute))]
    private Task ToggleAll() => BulkAll(CommandSpec.Toggle(CommandTarget.All), "Toggle all");

    [RelayCommand(CanExecute = nameof(CanBulkExecute))]
    private async Task ResetAll()
    {
        if (!_app.Dialogs.Confirm("Reset all timers?",
                "Every timer switches its output off and restarts its cycle at the beginning of the OFF phase. Use this to start a show from a defined state.",
                "Reset all", destructive: true))
            return;
        await BulkAll(CommandSpec.Reset(CommandTarget.All), "Reset all");
    }

    [RelayCommand(CanExecute = nameof(CanBulkExecute))]
    private async Task ApplyTimesAll()
    {
        double on = CommandSpec.RoundTenth(BulkOn), off = CommandSpec.RoundTenth(BulkOff);
        BulkPopupOpen = false;
        await BulkAll(CommandSpec.SetTimes(CommandTarget.All, on, off), $"Set ON {on:0.0} s / OFF {off:0.0} s on all");
    }

    /// <summary>
    /// Spreads the cycles of all online timers evenly over the OFF time so that they do not all fire at once:
    /// timer k of n jumps to the OFF phase at elapsed = toff * k / n.
    /// </summary>
    [RelayCommand(CanExecute = nameof(CanBulkExecute))]
    private async Task StaggerStart()
    {
        var targets = Devices.Where(d => d.Online).OrderBy(d => d.Index).ToList();
        if (targets.Count < 2)
        {
            _app.Toasts.Warning("Stagger start needs at least two online timers.");
            return;
        }
        var specs = StaggerPlan(targets.Select(d => (d.Mac, d.Toff)).ToList());
        await _app.Commands.RunBulkAsync(specs, $"Stagger start ({targets.Count} timers)", toastOnSuccess: true);
    }

    /// <summary>The setPhase commands that spread <paramref name="timers"/> evenly over their OFF time.</summary>
    public static IReadOnlyList<CommandSpec> StaggerPlan(IReadOnlyList<(string Mac, double Toff)> timers)
    {
        var list = new List<CommandSpec>();
        int n = timers.Count;
        for (int k = 0; k < n; k++)
            list.Add(CommandSpec.SetPhase(CommandTarget.ByMac(timers[k].Mac), CyclePhase.Off, timers[k].Toff * k / n));
        return list;
    }
}
