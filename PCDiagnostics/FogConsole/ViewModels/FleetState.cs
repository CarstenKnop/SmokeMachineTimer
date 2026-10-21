using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using FogConsole.Protocol;
using FogConsole.Services;

namespace FogConsole.ViewModels;

/// <summary>The remote (or directly connected timer) itself: battery, channel, firmware ...</summary>
public sealed partial class RemoteInfoViewModel : ObservableObject
{
    [ObservableProperty] private DeviceKind _kind = DeviceKind.Unknown;
    [ObservableProperty] private string _name = "";
    [ObservableProperty] private string _firmware = "";
    [ObservableProperty] private string _mac = "";
    [ObservableProperty] private int _protocol;
    [ObservableProperty] private string _build = "";
    [ObservableProperty] private string _resetReason = "";
    [ObservableProperty] private int _boots;
    [ObservableProperty] private int _maxDevs = 10;

    [ObservableProperty] private int _battery = -1;
    [ObservableProperty] private int _millivolts;
    [ObservableProperty] private bool _charging;
    [ObservableProperty] private bool _usbPower;
    [ObservableProperty] private int _channel;
    [ObservableProperty] private int _net;
    [ObservableProperty] private long _uptimeSeconds;
    [ObservableProperty] private long _heap;
    [ObservableProperty] private string _menu = "";
    [ObservableProperty] private bool _displayBlank;
    [ObservableProperty] private bool _scanning;
    [ObservableProperty] private int _txQueue;
    [ObservableProperty] private bool _hasData;

    [ObservableProperty] private int _rssiLow = -90;
    [ObservableProperty] private int _rssiHigh = -50;

    public bool IsRemote => Kind == DeviceKind.Remote;
    public bool IsTimer => Kind == DeviceKind.Timer;
    public bool HasBattery => Battery >= 0;
    public string BatteryText => Battery >= 0 ? $"{Battery}%" : "";
    public string ChannelText => Channel > 0 ? $"ch {Channel}" : "";
    public string KindText => Kind switch { DeviceKind.Remote => "Remote", DeviceKind.Timer => "Timer (USB)", _ => "" };

    /// <summary>Segoe Fluent Icons glyph of the battery level.</summary>
    public string BatteryGlyph
    {
        get
        {
            if (Battery < 0) return "";
            int level = Math.Clamp((int)Math.Round(Battery / 10.0), 0, 10);
            if (Charging) return level >= 10 ? "" : ((char)(0xE85A + Math.Min(level, 9))).ToString();
            return level >= 10 ? "" : ((char)(0xE850 + Math.Min(level, 9))).ToString();
        }
    }

    public bool IsLowBattery => Battery is >= 0 and <= 15 && !Charging;

    partial void OnKindChanged(DeviceKind value)
    {
        OnPropertyChanged(nameof(IsRemote));
        OnPropertyChanged(nameof(IsTimer));
        OnPropertyChanged(nameof(KindText));
    }

    partial void OnBatteryChanged(int value)
    {
        OnPropertyChanged(nameof(HasBattery));
        OnPropertyChanged(nameof(BatteryText));
        OnPropertyChanged(nameof(BatteryGlyph));
        OnPropertyChanged(nameof(IsLowBattery));
    }

    partial void OnChargingChanged(bool value)
    {
        OnPropertyChanged(nameof(BatteryGlyph));
        OnPropertyChanged(nameof(IsLowBattery));
    }

    partial void OnChannelChanged(int value) => OnPropertyChanged(nameof(ChannelText));

    public void Apply(HelloInfo h)
    {
        Kind = h.Kind;
        Name = h.Name;
        Firmware = h.Fw;
        Mac = h.Mac;
        Protocol = h.Proto;
        Build = h.Build;
        ResetReason = h.ResetReason;
        Boots = h.Boots;
        MaxDevs = h.MaxDevs;
        if (Channel == 0) Channel = h.Channel;
        Net = h.Net;
    }

    public void Apply(RemoteMessage r)
    {
        Battery = r.Bat;
        Millivolts = r.Mv;
        Charging = r.Charging;
        UsbPower = r.Usb;
        Channel = r.Channel;
        Net = r.Net;
        UptimeSeconds = r.UpSeconds;
        Heap = r.Heap;
        Menu = r.Menu;
        DisplayBlank = r.Blank;
        Scanning = r.Scan;
        TxQueue = r.Txq;
        HasData = true;
    }

    public void Clear()
    {
        Kind = DeviceKind.Unknown;
        Battery = -1;
        Channel = 0;
        HasData = false;
        Scanning = false;
    }
}

/// <summary>
/// The model behind the dashboard: the list of paired timers (stable view models, matched by MAC), the remote's own
/// state and the selection. Updated from the hub's event stream on the UI thread.
/// </summary>
public sealed partial class FleetState : ObservableObject, IMessageSink
{
    private readonly AppServices _app;
    private double _userSelectedAt = double.MinValue;

    public FleetState(AppServices app)
    {
        _app = app;
        Remote = new RemoteInfoViewModel();
    }

    public ObservableCollection<DeviceViewModel> Devices { get; } = new();
    public RemoteInfoViewModel Remote { get; }

    [ObservableProperty] private DeviceViewModel? _selected;
    [ObservableProperty] private int _activeIndex = -1;
    [ObservableProperty] private int _homeChannel;
    [ObservableProperty] private bool _isLive;
    [ObservableProperty] private bool _hasFleetData;
    [ObservableProperty] private bool _anyForceOn;

    public bool IsEmpty => Devices.Count == 0;

    public DeviceViewModel? ByMac(string mac) =>
        Devices.FirstOrDefault(d => string.Equals(d.Mac, mac, StringComparison.OrdinalIgnoreCase));

    public DeviceViewModel? ByIndex(int index) => Devices.FirstOrDefault(d => d.Index == index);

    public string? NameOfMac(string mac) => ByMac(mac)?.DisplayName;
    public string? NameOfIndex(int index) => ByIndex(index)?.DisplayName;

    public void OnMessage(Message message)
    {
        switch (message)
        {
            case FleetMessage f: ApplyFleet(f); break;
            case RemoteMessage r: Remote.Apply(r); break;
        }
    }

    public void ApplyFleet(FleetMessage f)
    {
        double now = _app.Clock.Seconds;
        HomeChannel = f.Channel;
        if (f.Channel > 0) Remote.Channel = f.Channel;

        // remove timers that are no longer paired
        foreach (var gone in Devices.Where(d => f.Devs.All(x => !string.Equals(x.Mac, d.Mac, StringComparison.OrdinalIgnoreCase))).ToList())
        {
            if (ReferenceEquals(Selected, gone)) Selected = null;
            _app.Hold.End(gone.Mac);
            Devices.Remove(gone);
        }

        for (int i = 0; i < f.Devs.Count; i++)
        {
            var fd = f.Devs[i];
            var vm = ByMac(fd.Mac);
            if (vm is null)
            {
                vm = new DeviceViewModel(_app, fd.Mac);
                Devices.Insert(Math.Min(i, Devices.Count), vm);
            }
            else
            {
                int at = Devices.IndexOf(vm);
                if (at != i && i < Devices.Count) Devices.Move(at, i);
            }
            vm.Update(fd, now);
        }

        ActiveIndex = f.Active;
        bool userRecent = now - _userSelectedAt < 2.0;
        if (!userRecent)
        {
            var active = f.Active >= 0 ? ByIndex(f.Active) : null;
            if (active is not null) Selected = active;
            else if (Selected is null && Devices.Count > 0) Selected = Devices[0];
        }
        foreach (var d in Devices) d.IsSelected = ReferenceEquals(d, Selected);

        HasFleetData = true;
        AnyForceOn = Devices.Any(d => d.IsForceOn);
        OnPropertyChanged(nameof(IsEmpty));
    }

    /// <summary>Makes a timer the remote's active device and the one shown on the Device page.</summary>
    public void SelectDevice(DeviceViewModel device)
    {
        _userSelectedAt = _app.Clock.Seconds;
        Selected = device;
        foreach (var d in Devices) d.IsSelected = ReferenceEquals(d, device);
        if (!_app.IsConnected || Remote.IsTimer) return;
        int index = device.Index;
        _ = SendSelectAsync(index, device.DisplayName);
    }

    private async Task SendSelectAsync(int index, string name)
    {
        try
        {
            var res = await _app.Hub.RequestAsync(Requests.Select(index), retries: 1).ConfigureAwait(true);
            if (!res.Ok) _app.Toasts.Warning($"Could not select {name}: {res.Describe()}");
        }
        catch (Exception ex)
        {
            _app.Toasts.Warning($"Could not select {name}: {ex.Message}");
        }
    }

    public void Tick()
    {
        double now = _app.Clock.Seconds;
        foreach (var d in Devices) d.Tick(now);
    }

    /// <summary>Called when the link state changes. Cards stay visible but disabled while the link is down.</summary>
    public void SetLive(bool live, bool clear)
    {
        IsLive = live;
        if (clear)
        {
            Devices.Clear();
            Selected = null;
            HasFleetData = false;
            AnyForceOn = false;
            Remote.Clear();
            OnPropertyChanged(nameof(IsEmpty));
        }
        else
        {
            foreach (var d in Devices) d.RefreshLiveness();
        }
    }

    public void RefreshThresholds()
    {
        foreach (var d in Devices) d.RefreshThresholds();
    }
}
