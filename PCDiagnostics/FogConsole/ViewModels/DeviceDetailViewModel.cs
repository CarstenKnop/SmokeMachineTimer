using System.Collections.ObjectModel;
using System.Collections.Specialized;
using System.ComponentModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using FogConsole.Protocol;
using FogConsole.Services;

namespace FogConsole.ViewModels;

public sealed record TimePresetItem(double On, double Off, bool IsCustom)
{
    public string Label => $"{Format(On)} / {Format(Off)} s";
    private static string Format(double v) => v % 1 == 0 ? v.ToString("0") : v.ToString("0.0#");
}

/// <summary>Large live view and settings of the selected timer.</summary>
public sealed partial class DeviceDetailViewModel : ObservableObject, IPageViewModel
{
    public static readonly (double On, double Off)[] BuiltInPresets = { (0.5, 5), (1, 10), (2, 20), (3, 30), (5, 60) };

    private readonly AppServices _app;
    private DeviceViewModel? _device;
    private bool _syncing;

    public DeviceDetailViewModel(AppServices app)
    {
        _app = app;
        app.Fleet.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(FleetState.Selected)) AttachDevice(app.Fleet.Selected);
            if (e.PropertyName == nameof(FleetState.IsLive)) RefreshCommands();
        };
        app.Commands.History.CollectionChanged += OnHistoryChanged;
        RebuildPresets();
        AttachDevice(app.Fleet.Selected);
    }

    public NavPage Page => NavPage.Device;
    public void Activate() { SyncFromDevice(force: false); RebuildRecent(); }
    public void Deactivate() { }

    public FleetState Fleet => _app.Fleet;
    public DeviceViewModel? Device => _device;
    public bool HasDevice => _device is not null;
    public bool IsDirectTimer => _app.Fleet.Remote.IsTimer;
    public IReadOnlyList<int> Channels { get; } = Enumerable.Range(1, 13).ToArray();
    public ObservableCollection<TimePresetItem> Presets { get; } = new();
    public ObservableCollection<CommandEntry> RecentCommands { get; } = new();

    [ObservableProperty] private double _onSeconds = 1.5;
    [ObservableProperty] private double _offSeconds = 10;
    [ObservableProperty] private bool _isDirty;
    [ObservableProperty] private CommandEntry? _lastApply;
    [ObservableProperty] private string _renameText = "";
    [ObservableProperty] private int _targetChannel = 6;
    [ObservableProperty] private int _startPhaseIndex;     // 0 = off, 1 = on
    [ObservableProperty] private CommandEntry? _lastOtherCommand;

    public double OnSlider
    {
        get => Math.Min(OnSeconds, 60);
        set { if (!_syncing && Math.Abs(value - Math.Min(OnSeconds, 60)) > 1e-9) OnSeconds = Math.Round(Math.Max(0.1, value), 1); }
    }

    public double OffSlider
    {
        get => Math.Min(OffSeconds, 60);
        set { if (!_syncing && Math.Abs(value - Math.Min(OffSeconds, 60)) > 1e-9) OffSeconds = Math.Round(Math.Max(0.1, value), 1); }
    }

    public bool CanEdit => _device is { CanControl: true };

    private void AttachDevice(DeviceViewModel? device)
    {
        if (ReferenceEquals(_device, device)) return;
        if (_device is not null) _device.PropertyChanged -= OnDevicePropertyChanged;
        _device = device;
        if (_device is not null) _device.PropertyChanged += OnDevicePropertyChanged;
        LastApply = null;
        LastOtherCommand = null;
        SyncFromDevice(force: true);
        RebuildRecent();
        OnPropertyChanged(nameof(Device));
        OnPropertyChanged(nameof(HasDevice));
        RefreshCommands();
    }

    private void OnDevicePropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        switch (e.PropertyName)
        {
            case nameof(DeviceViewModel.Ton) or nameof(DeviceViewModel.Toff):
                if (!IsDirty) SyncFromDevice(force: false);
                break;
            case nameof(DeviceViewModel.CanControl) or nameof(DeviceViewModel.Online):
                RefreshCommands();
                break;
        }
    }

    private void SyncFromDevice(bool force)
    {
        if (_device is null) return;
        if (!force && IsDirty) return;
        _syncing = true;
        try
        {
            if (_device.Ton > 0) OnSeconds = _device.Ton;
            if (_device.Toff > 0) OffSeconds = _device.Toff;
            if (force)
            {
                RenameText = _device.HubName;
                TargetChannel = _device.Channel > 0 ? _device.Channel : TargetChannel;
            }
            IsDirty = false;
        }
        finally
        {
            _syncing = false;
        }
        OnPropertyChanged(nameof(OnSlider));
        OnPropertyChanged(nameof(OffSlider));
    }

    partial void OnOnSecondsChanged(double value)
    {
        OnPropertyChanged(nameof(OnSlider));
        if (!_syncing) IsDirty = true;
        ApplyCommand.NotifyCanExecuteChanged();
    }

    partial void OnOffSecondsChanged(double value)
    {
        OnPropertyChanged(nameof(OffSlider));
        if (!_syncing) IsDirty = true;
        ApplyCommand.NotifyCanExecuteChanged();
    }

    partial void OnIsDirtyChanged(bool value) => ApplyCommand.NotifyCanExecuteChanged();

    private void RefreshCommands()
    {
        OnPropertyChanged(nameof(CanEdit));
        ApplyCommand.NotifyCanExecuteChanged();
        RenameCommand.NotifyCanExecuteChanged();
        MoveChannelCommand.NotifyCanExecuteChanged();
        ApplyStartPhaseCommand.NotifyCanExecuteChanged();
        IdentifyCommand.NotifyCanExecuteChanged();
        RebootCommand.NotifyCanExecuteChanged();
        FactoryResetCommand.NotifyCanExecuteChanged();
    }

    // ---- presets ---------------------------------------------------------------------------

    private void RebuildPresets()
    {
        Presets.Clear();
        foreach (var (on, off) in BuiltInPresets) Presets.Add(new TimePresetItem(on, off, false));
        foreach (var p in _app.Settings.Current.CustomPresets) Presets.Add(new TimePresetItem(p.On, p.Off, true));
    }

    [RelayCommand]
    private void UsePreset(TimePresetItem? preset)
    {
        if (preset is null) return;
        OnSeconds = preset.On;
        OffSeconds = preset.Off;
    }

    [RelayCommand]
    private void SavePreset()
    {
        double on = CommandSpec.RoundTenth(OnSeconds), off = CommandSpec.RoundTenth(OffSeconds);
        if (Presets.Any(p => Math.Abs(p.On - on) < 0.01 && Math.Abs(p.Off - off) < 0.01))
        {
            _app.Toasts.Info("That preset already exists.");
            return;
        }
        _app.Settings.Current.CustomPresets.Add(new TimePreset { On = on, Off = off });
        _app.Settings.SaveSoon();
        RebuildPresets();
        _app.Toasts.Success($"Saved preset {on:0.0} / {off:0.0} s.");
    }

    [RelayCommand]
    private void RemovePreset(TimePresetItem? preset)
    {
        if (preset is not { IsCustom: true }) return;
        _app.Settings.Current.CustomPresets.RemoveAll(p => Math.Abs(p.On - preset.On) < 0.01 && Math.Abs(p.Off - preset.Off) < 0.01);
        _app.Settings.SaveSoon();
        RebuildPresets();
    }

    // ---- commands --------------------------------------------------------------------------

    private bool CanAct() => CanEdit;
    private bool CanApply() => CanEdit && IsDirty && OnSeconds is >= 0.1 and <= 9999.9 && OffSeconds is >= 0.1 and <= 9999.9;

    [RelayCommand(CanExecute = nameof(CanApply))]
    private void Apply()
    {
        var d = _device;
        if (d is null) return;
        double on = CommandSpec.RoundTenth(OnSeconds), off = CommandSpec.RoundTenth(OffSeconds);
        var entry = _app.Commands.Run(CommandSpec.SetTimes(CommandTarget.ByMac(d.Mac), on, off), $"Apply ON {on:0.0} s / OFF {off:0.0} s", d.DisplayName, d.Mac);
        LastApply = entry;
        entry.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(CommandEntry.Status) && entry.Status == EntryStatus.Ok && ReferenceEquals(LastApply, entry))
                IsDirty = false;
        };
    }

    [RelayCommand(CanExecute = nameof(CanAct))]
    private void Rename()
    {
        var d = _device;
        if (d is null) return;
        var name = RenameText.Trim();
        if (name.Length is < 1 or > 9 || name.Any(c => c < ' ' || c > '~'))
        {
            _app.Toasts.Warning("Names are 1-9 printable ASCII characters.");
            return;
        }
        LastOtherCommand = _app.Commands.Run(CommandSpec.Rename(CommandTarget.ByMac(d.Mac), name), $"Rename to \"{name}\"", d.DisplayName, d.Mac, toastOnSuccess: true);
    }

    [RelayCommand(CanExecute = nameof(CanAct))]
    private void ApplyStartPhase()
    {
        var d = _device;
        if (d is null) return;
        var phase = StartPhaseIndex == 1 ? CyclePhase.On : CyclePhase.Off;
        LastOtherCommand = _app.Commands.Run(CommandSpec.StartPhase(CommandTarget.ByMac(d.Mac), phase),
            $"Start in the {(phase == CyclePhase.On ? "ON" : "OFF")} phase after power-up", d.DisplayName, d.Mac, toastOnSuccess: true);
    }

    [RelayCommand(CanExecute = nameof(CanAct))]
    private void MoveChannel()
    {
        var d = _device;
        if (d is null) return;
        int ch = TargetChannel;
        if (ch == d.Channel)
        {
            _app.Toasts.Info($"{d.DisplayName} is already on channel {ch}.");
            return;
        }
        if (!_app.Dialogs.Confirm($"Move {d.DisplayName} to channel {ch}?",
                $"Only this timer changes its home channel. The remote keeps talking on channel {_app.Fleet.HomeChannel}, so {d.DisplayName} will drop off the dashboard until you move the whole system to channel {ch} " +
                "(Channels page) or use \"Find lost timers\". Only do this for bench work.",
                "Move timer", destructive: true))
            return;
        LastOtherCommand = _app.Commands.Run(CommandSpec.Channel(CommandTarget.ByMac(d.Mac), ch), $"Move to channel {ch}", d.DisplayName, d.Mac, toastOnSuccess: true);
    }

    [RelayCommand(CanExecute = nameof(CanAct))]
    private void Identify()
    {
        var d = _device;
        if (d is null) return;
        LastOtherCommand = _app.Commands.Run(CommandSpec.Identify(CommandTarget.ByMac(d.Mac)), "Identify (blinks the LED for 5 s)", d.DisplayName, d.Mac, toastOnSuccess: true);
    }

    [RelayCommand(CanExecute = nameof(CanAct))]
    private void Reboot()
    {
        var d = _device;
        if (d is null) return;
        if (!_app.Dialogs.Confirm($"Reboot {d.DisplayName}?",
                "The timer restarts. Its output is off while it boots and the cycle starts again in the configured start phase.", "Reboot", destructive: true))
            return;
        LastOtherCommand = _app.Commands.Run(CommandSpec.Reboot(CommandTarget.ByMac(d.Mac)), "Reboot", d.DisplayName, d.Mac, toastOnSuccess: true);
    }

    [RelayCommand(CanExecute = nameof(CanAct))]
    private void FactoryReset()
    {
        var d = _device;
        if (d is null) return;
        if (!_app.Dialogs.Confirm($"Factory reset {d.DisplayName}?",
                "This wipes the stored settings of the timer (durations, name, start phase, channel). It stays paired with the remote but must be set up again.",
                "Factory reset", destructive: true))
            return;
        LastOtherCommand = _app.Commands.Run(CommandSpec.FactoryReset(CommandTarget.ByMac(d.Mac)), "Factory reset", d.DisplayName, d.Mac, toastOnSuccess: true);
    }

    [RelayCommand]
    private void Back() => _app.Navigate(NavPage.Dashboard);

    // ---- recent commands -------------------------------------------------------------------

    private void OnHistoryChanged(object? sender, NotifyCollectionChangedEventArgs e) => RebuildRecent();

    private void RebuildRecent()
    {
        RecentCommands.Clear();
        if (_device is null) return;
        foreach (var e in _app.Commands.ForDevice(_device.Mac).Take(12)) RecentCommands.Add(e);
    }
}
