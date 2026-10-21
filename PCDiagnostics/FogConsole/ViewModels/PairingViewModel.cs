using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using FogConsole.Hub;
using FogConsole.Protocol;
using FogConsole.Services;

namespace FogConsole.ViewModels;

/// <summary>A timer heard during discovery.</summary>
public sealed partial class FoundDeviceViewModel : ObservableObject
{
    private readonly AppServices _app;
    private double _lastHeard;

    public FoundDeviceViewModel(AppServices app, string mac)
    {
        _app = app;
        Mac = mac;
    }

    public string Mac { get; }

    [ObservableProperty] private string _name = "";
    [ObservableProperty] private int _rssi;
    [ObservableProperty] private int _channel;
    [ObservableProperty] private int _pairedIndex = -1;
    [ObservableProperty] private double _ton;
    [ObservableProperty] private double _toff;
    [ObservableProperty] private bool _outputOn;
    [ObservableProperty] private int _boot;
    [ObservableProperty] private string _pairName = "";
    [ObservableProperty] private bool _isStale;
    [ObservableProperty] private string _ageText = "just now";

    public bool IsPaired => PairedIndex >= 0;
    public bool OnOtherChannel => Channel != 0 && Channel != _app.Fleet.HomeChannel && _app.Fleet.HomeChannel != 0;
    public string TimesText => $"ON {Ton:0.0} s · OFF {Toff:0.0} s";
    public double LastHeard => _lastHeard;

    public int SignalBars
    {
        get
        {
            if (Rssi == 0) return 0;
            int low = _app.Fleet.Remote.RssiLow, high = _app.Fleet.Remote.RssiHigh;
            if (high <= low) high = low + 5;
            return (int)Math.Round(Math.Clamp((Rssi - low) / (double)(high - low), 0, 1) * 6);
        }
    }

    partial void OnPairedIndexChanged(int value) => OnPropertyChanged(nameof(IsPaired));
    partial void OnRssiChanged(int value) => OnPropertyChanged(nameof(SignalBars));
    partial void OnChannelChanged(int value) => OnPropertyChanged(nameof(OnOtherChannel));
    partial void OnTonChanged(double value) => OnPropertyChanged(nameof(TimesText));
    partial void OnToffChanged(double value) => OnPropertyChanged(nameof(TimesText));

    public void Update(FoundMessage f, double now)
    {
        Name = f.Name;
        Rssi = f.Rssi;
        Channel = f.Channel;
        PairedIndex = f.Paired;
        Ton = f.Ton;
        Toff = f.Toff;
        OutputOn = f.On;
        Boot = f.Boot;
        _lastHeard = now;
        IsStale = false;
        AgeText = "just now";
    }

    public void Tick(double now)
    {
        double age = now - _lastHeard;
        IsStale = age > 6;
        AgeText = Fmt.Ago(age);
        OnPropertyChanged(nameof(OnOtherChannel));
    }
}

/// <summary>Discover, pair and unpair timers.</summary>
public sealed partial class PairingViewModel : ObservableObject, IPageViewModel, IMessageSink
{
    private readonly AppServices _app;

    public PairingViewModel(AppServices app)
    {
        _app = app;
        app.Fleet.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(FleetState.IsLive))
            {
                OnPropertyChanged(nameof(CanDiscover));
                if (!app.Fleet.IsLive) DiscoverOn = false;
            }
        };
    }

    public NavPage Page => NavPage.Pairing;
    public FleetState Fleet => _app.Fleet;
    public ObservableCollection<FoundDeviceViewModel> Found { get; } = new();
    public ObservableCollection<DeviceViewModel> Paired => _app.Fleet.Devices;

    [ObservableProperty] private bool _discoverOn;
    [ObservableProperty] private bool _sweep;
    [ObservableProperty] private bool _busy;

    public bool CanDiscover => _app.Fleet.IsLive && !_app.Fleet.Remote.IsTimer;
    public bool IsDirectTimer => _app.Fleet.Remote.IsTimer;
    public bool HasFound => Found.Count > 0;
    public bool ListFull => _app.Fleet.Devices.Count >= Math.Max(1, _app.Fleet.Remote.MaxDevs);
    public string CapacityText => $"{_app.Fleet.Devices.Count} of {Math.Max(1, _app.Fleet.Remote.MaxDevs)} timers paired";

    public void Activate()
    {
        OnPropertyChanged(nameof(CanDiscover));
        OnPropertyChanged(nameof(IsDirectTimer));
        OnPropertyChanged(nameof(CapacityText));
    }

    public void Deactivate()
    {
        // discovery occupies the radio: never leave it running unattended
        if (DiscoverOn) DiscoverOn = false;
    }

    public void OnMessage(Message message)
    {
        if (message is FoundMessage f)
        {
            var vm = Found.FirstOrDefault(x => x.Mac == f.Mac);
            if (vm is null)
            {
                vm = new FoundDeviceViewModel(_app, f.Mac);
                Found.Add(vm);
                OnPropertyChanged(nameof(HasFound));
            }
            vm.Update(f, _app.Clock.Seconds);
            // only timers that can be paired get a suggested name
            if (!vm.IsPaired && string.IsNullOrEmpty(vm.PairName)) vm.PairName = SuggestName();
        }
        else if (message is FleetMessage)
        {
            // pairing state of discovered timers follows the fleet list
            foreach (var f2 in Found)
            {
                var paired = _app.Fleet.ByMac(f2.Mac);
                f2.PairedIndex = paired?.Index ?? -1;
                if (paired is null && string.IsNullOrEmpty(f2.PairName)) f2.PairName = SuggestName();
            }
            OnPropertyChanged(nameof(ListFull));
            OnPropertyChanged(nameof(CapacityText));
        }
    }

    public void Tick()
    {
        double now = _app.Clock.Seconds;
        foreach (var f in Found) f.Tick(now);
        for (int i = Found.Count - 1; i >= 0; i--)
            if (now - Found[i].LastHeard > 90) Found.RemoveAt(i);
        OnPropertyChanged(nameof(HasFound));
    }

    public void ClearSession()
    {
        Found.Clear();
        DiscoverOn = false;
        OnPropertyChanged(nameof(HasFound));
    }

    private string SuggestName()
    {
        var used = new HashSet<string>(_app.Fleet.Devices.Select(d => d.HubName), StringComparer.OrdinalIgnoreCase);
        foreach (var f in Found)
            if (!string.IsNullOrEmpty(f.PairName)) used.Add(f.PairName);
        for (int i = 1; i <= 99; i++)
            if (!used.Contains($"Fog{i}")) return $"Fog{i}";
        return "Fog";
    }

    // ---- discovery -------------------------------------------------------------------------

    partial void OnDiscoverOnChanged(bool value) => _ = SendDiscoverAsync();
    partial void OnSweepChanged(bool value)
    {
        if (DiscoverOn) _ = SendDiscoverAsync();
    }

    private async Task SendDiscoverAsync()
    {
        if (!_app.IsConnected) return;
        try
        {
            var res = await _app.Hub.RequestAsync(Requests.Discover(DiscoverOn, Sweep), retries: 1).ConfigureAwait(true);
            if (!res.Ok)
            {
                _app.Toasts.Warning($"Discovery: {res.Describe()}");
                if (DiscoverOn) DiscoverOn = false;
            }
        }
        catch (Exception ex)
        {
            _app.Toasts.Warning($"Discovery failed: {ex.Message}");
        }
    }

    /// <summary>Re-sends the discovery setting after a re-connection.</summary>
    public void ReapplyAfterReconnect()
    {
        if (DiscoverOn) _ = SendDiscoverAsync();
    }

    // ---- pair / unpair ---------------------------------------------------------------------

    [RelayCommand]
    private async Task Pair(FoundDeviceViewModel? found)
    {
        if (found is null || !_app.IsConnected) return;
        string name = found.PairName.Trim();
        if (name.Length > 0 && (name.Length > 9 || name.Any(c => c < ' ' || c > '~')))
        {
            _app.Toasts.Warning("Names are 1-9 printable ASCII characters.");
            return;
        }
        Busy = true;
        try
        {
            var res = await _app.Hub.RequestAsync(Requests.Pair(found.Mac, name.Length == 0 ? null : name)).ConfigureAwait(true);
            if (res.Ok)
            {
                _app.Toasts.Success(found.OnOtherChannel
                    ? $"Paired {(name.Length > 0 ? name : found.Name)}. It was on channel {found.Channel} and is being moved to channel {_app.Fleet.HomeChannel}."
                    : $"Paired {(name.Length > 0 ? name : found.Name)}.");
                found.PairName = SuggestName();
            }
            else
            {
                _app.Toasts.Error(res.Err switch
                {
                    "full" => "The remote's list is full (10 timers). Unpair one first.",
                    "noDevice" => "The remote does not know that timer any more - keep discovery running and try again.",
                    _ => $"Pairing failed: {res.Describe()}",
                });
            }
        }
        catch (HubException ex)
        {
            _app.Toasts.Error($"Pairing failed: {ex.Message}");
        }
        finally
        {
            Busy = false;
        }
    }

    [RelayCommand]
    private async Task Unpair(DeviceViewModel? device)
    {
        if (device is null || !_app.IsConnected) return;
        if (!_app.Dialogs.Confirm($"Unpair {device.DisplayName}?",
                "The timer is removed from the remote's list and from this dashboard. The timer itself keeps its settings and can be paired again.",
                "Unpair", destructive: true))
            return;
        await UnpairCore(device.Index, device.DisplayName);
    }

    [RelayCommand]
    private async Task UnpairFound(FoundDeviceViewModel? found)
    {
        if (found is null || !found.IsPaired) return;
        var d = _app.Fleet.ByMac(found.Mac);
        if (d is not null) await Unpair(d);
    }

    private async Task UnpairCore(int index, string name)
    {
        Busy = true;
        try
        {
            var res = await _app.Hub.RequestAsync(Requests.Unpair(index)).ConfigureAwait(true);
            if (res.Ok) _app.Toasts.Success($"{name} was unpaired.");
            else _app.Toasts.Error($"Unpair failed: {res.Describe()}");
        }
        catch (HubException ex)
        {
            _app.Toasts.Error($"Unpair failed: {ex.Message}");
        }
        finally
        {
            Busy = false;
        }
    }
}
