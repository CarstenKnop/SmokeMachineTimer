using System.Windows.Media;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using FogConsole.Controls;
using FogConsole.Protocol;
using FogConsole.Services;

namespace FogConsole.ViewModels;

public enum PillState { Offline, Off, On, ForceOn, Paused }

public enum LinkQuality { Unknown, Good, Fair, Poor }

/// <summary>
/// One paired timer as seen by the dashboard: live state from the fleet stream, values interpolated on the client between
/// the 500 ms events, link statistics and history for the sparklines / timeline / link graph.
/// </summary>
public sealed partial class DeviceViewModel : ObservableObject
{
    public static readonly string[] TagColors = { "#2EC4B6", "#4D9DE0", "#9B5DE5", "#F15BB5", "#FF9F1C", "#8AC926", "#00BBF9", "#C9B79C" };

    private const double TimelineSeconds = 70;
    private const int LinkHistoryPoints = 300;

    private readonly AppServices _app;

    // raw state from the last event
    private double _elAtEvent;
    private double _eventTime;
    private CyclePhase _phaseAtEvent;
    private double _rebootedUntil;
    private bool _hasData;
    private double _lastLinkSample = double.MinValue;
    private readonly List<TimelineSample> _timeline = new();
    private readonly List<double> _rssiSpark = new();
    private readonly List<double> _linkRssi = new(), _linkRssiT = new(), _linkLoss = new();

    public DeviceViewModel(AppServices app, string mac)
    {
        _app = app;
        Mac = mac;
        _tagColor = ColorFor(mac, 0);
        TagBrush = MakeBrush(_tagColor);
    }

    public string Mac { get; }

    // ---- state from the fleet event --------------------------------------------------------

    [ObservableProperty] private int _index;
    [ObservableProperty] private string _hubName = "";
    [ObservableProperty] private bool _online;
    [ObservableProperty] private bool _outputOn;
    [ObservableProperty] private CyclePhase _reportedPhase;
    [ObservableProperty] private TimerMode _mode;
    [ObservableProperty] private double _ton;
    [ObservableProperty] private double _toff;
    [ObservableProperty] private int _rssi;
    [ObservableProperty] private int _rssiT;
    [ObservableProperty] private int _ageMs;
    [ObservableProperty] private int _loss = -1;
    [ObservableProperty] private int _channel;
    [ObservableProperty] private int _boot;
    [ObservableProperty] private long _upSeconds;
    [ObservableProperty] private string _fw = "";
    [ObservableProperty] private int _pending;
    [ObservableProperty] private int _slotId;
    [ObservableProperty] private bool _isSelected;

    // ---- interpolated ----------------------------------------------------------------------

    [ObservableProperty] private CyclePhase _phase;
    [ObservableProperty] private double _progress;
    [ObservableProperty] private double _remaining;
    [ObservableProperty] private string _remainingText = "-";
    [ObservableProperty] private string _lastSeenText = "";
    [ObservableProperty] private bool _isRebooted;
    [ObservableProperty] private IReadOnlyList<TimelineSample> _timelineSamples = Array.Empty<TimelineSample>();
    [ObservableProperty] private double _timelineNow;
    [ObservableProperty] private IReadOnlyList<double> _rssiHistory = Array.Empty<double>();
    [ObservableProperty] private IReadOnlyList<GraphSeries> _linkSeries = Array.Empty<GraphSeries>();

    // ---- local presentation ----------------------------------------------------------------

    [ObservableProperty] private bool _isEditingName;
    [ObservableProperty] private string _editName = "";
    [ObservableProperty] private Color _tagColor;
    [ObservableProperty] private Brush _tagBrush;

    public string DisplayName => string.IsNullOrWhiteSpace(Nickname) ? HubName : Nickname!;

    public string? Nickname
    {
        get => _app.Settings.Current.Devices.TryGetValue(Mac, out var a) ? a.Nickname : null;
        set
        {
            var n = string.IsNullOrWhiteSpace(value) || value.Trim() == HubName ? null : value.Trim();
            _app.Settings.AppearanceOf(Mac).Nickname = n;
            _app.Settings.SaveSoon();
            OnPropertyChanged();
            OnPropertyChanged(nameof(DisplayName));
            OnPropertyChanged(nameof(HasNickname));
        }
    }

    public bool HasNickname => !string.IsNullOrWhiteSpace(Nickname);

    partial void OnHubNameChanged(string value)
    {
        OnPropertyChanged(nameof(DisplayName));
    }

    // ---- derived ---------------------------------------------------------------------------

    public bool CanControl => Online && _app.Fleet.IsLive;
    public bool IsForceOn => Mode == TimerMode.ForceOn && Online;
    public bool IsPaused => Mode == TimerMode.Paused;

    public PillState PillState => !Online ? PillState.Offline
        : Mode == TimerMode.ForceOn ? PillState.ForceOn
        : Mode == TimerMode.Paused ? PillState.Paused
        : Phase == CyclePhase.On ? PillState.On : PillState.Off;

    public string PillText => PillState switch
    {
        PillState.Offline => "OFFLINE",
        PillState.ForceOn => "FORCE ON",
        PillState.Paused => "PAUSED",
        PillState.On => "ON",
        _ => "OFF",
    };

    public string ModeText => Mode switch { TimerMode.ForceOn => "FORCE ON", TimerMode.Paused => "PAUSED", _ => "AUTO" };

    public string UntilText => Mode == TimerMode.Paused ? "paused" : Phase == CyclePhase.On ? "until OFF" : "until ON";

    public string PauseButtonText => IsPaused ? "Resume" : "Pause";
    public string PauseButtonGlyph => IsPaused ? "" : "";

    public int SignalBars
    {
        get
        {
            if (!Online || Rssi == 0) return 0;
            var s = _app.Fleet.Remote;
            int low = s.RssiLow, high = s.RssiHigh;
            if (high <= low) high = low + 5;
            return (int)Math.Round(Math.Clamp((Rssi - low) / (double)(high - low), 0, 1) * 6);
        }
    }

    public LinkQuality Quality
    {
        get
        {
            if (!Online || Rssi == 0) return LinkQuality.Unknown;
            int bars = SignalBars;
            if (Loss >= 20 || bars <= 1) return LinkQuality.Poor;
            if (Loss >= 8 || bars <= 3) return LinkQuality.Fair;
            return LinkQuality.Good;
        }
    }

    public string QualityText => Quality switch { LinkQuality.Good => "good link", LinkQuality.Fair => "fair link", LinkQuality.Poor => "poor link", _ => "no link" };

    public string SignalText => !Online ? "no signal" : Rssi == 0 ? "- dBm" : $"{Rssi} dBm";
    public string SignalTimerText => RssiT == 0 ? "-" : $"{RssiT} dBm";
    public string LossText => !Online || Loss < 0 ? "loss -" : $"loss {Loss}%";
    public string LossValueText => Loss < 0 ? "-" : $"{Loss}%";

    /// <summary>Number of samples the link graph spans (grows with the history up to 5 minutes).</summary>
    public double LinkWindowPoints => Math.Clamp(_linkRssi.Count, 60, 300);
    public string BootText => $"boot {Boot}";
    public string UptimeText => Fmt.Duration(UpSeconds);
    public string PendingText => Pending == 1 ? "1 pending" : $"{Pending} pending";
    public bool HasPending => Pending > 0;
    public string TimesText => $"ON {Fmt.Seconds(Ton)} s · OFF {Fmt.Seconds(Toff)} s";
    public string RebootedText => "REBOOTED";

    /// <summary>Derived from the configured durations (what the timer does when left alone).</summary>
    public string CycleLengthText => $"{Fmt.Seconds(Ton + Toff)} s";
    public string DutyText => Ton + Toff <= 0 ? "-" : string.Format(System.Globalization.CultureInfo.CurrentCulture, "{0:0.#} %", Ton / (Ton + Toff) * 100.0);
    public string FogPerMinuteText => Ton + Toff <= 0 ? "-" : $"{Fmt.Seconds(Ton * 60.0 / (Ton + Toff))} s";

    partial void OnOnlineChanged(bool value) => RaiseStateDerived();
    partial void OnModeChanged(TimerMode value) => RaiseStateDerived();
    partial void OnPhaseChanged(CyclePhase value) => RaiseStateDerived();
    partial void OnRssiChanged(int value) { OnPropertyChanged(nameof(SignalBars)); OnPropertyChanged(nameof(SignalText)); OnPropertyChanged(nameof(Quality)); OnPropertyChanged(nameof(QualityText)); }
    partial void OnRssiTChanged(int value) => OnPropertyChanged(nameof(SignalTimerText));
    partial void OnLossChanged(int value) { OnPropertyChanged(nameof(LossText)); OnPropertyChanged(nameof(LossValueText)); OnPropertyChanged(nameof(Quality)); OnPropertyChanged(nameof(QualityText)); }
    partial void OnBootChanged(int value) => OnPropertyChanged(nameof(BootText));
    partial void OnUpSecondsChanged(long value) => OnPropertyChanged(nameof(UptimeText));
    partial void OnPendingChanged(int value) { OnPropertyChanged(nameof(PendingText)); OnPropertyChanged(nameof(HasPending)); }
    partial void OnTonChanged(double value) { OnPropertyChanged(nameof(TimesText)); RaiseDerivedTimes(); }
    partial void OnToffChanged(double value) { OnPropertyChanged(nameof(TimesText)); RaiseDerivedTimes(); }

    private void RaiseDerivedTimes()
    {
        OnPropertyChanged(nameof(CycleLengthText));
        OnPropertyChanged(nameof(DutyText));
        OnPropertyChanged(nameof(FogPerMinuteText));
    }

    private void RaiseStateDerived()
    {
        NotifyCanExecuteChanged();
        OnPropertyChanged(nameof(SignalText));
        OnPropertyChanged(nameof(LossText));
        OnPropertyChanged(nameof(PillState));
        OnPropertyChanged(nameof(PillText));
        OnPropertyChanged(nameof(ModeText));
        OnPropertyChanged(nameof(UntilText));
        OnPropertyChanged(nameof(IsForceOn));
        OnPropertyChanged(nameof(IsPaused));
        OnPropertyChanged(nameof(PauseButtonText));
        OnPropertyChanged(nameof(PauseButtonGlyph));
        OnPropertyChanged(nameof(CanControl));
        OnPropertyChanged(nameof(SignalBars));
        OnPropertyChanged(nameof(Quality));
        OnPropertyChanged(nameof(QualityText));
    }

    /// <summary>Called when the connection state of the fleet changed (enables / disables the controls).</summary>
    public void RefreshLiveness()
    {
        NotifyCanExecuteChanged();
        OnPropertyChanged(nameof(CanControl));
    }

    /// <summary>Called when the RSSI thresholds of the remote changed.</summary>
    public void RefreshThresholds()
    {
        OnPropertyChanged(nameof(SignalBars));
        OnPropertyChanged(nameof(Quality));
        OnPropertyChanged(nameof(QualityText));
    }

    // ---- updating --------------------------------------------------------------------------

    /// <summary>Applies one entry of a fleet event. <paramref name="now"/> is <see cref="UiClock.Seconds"/>.</summary>
    public void Update(FleetDevice d, double now)
    {
        if (_hasData && d.Boot > Boot)
            _rebootedUntil = now + 30;
        _hasData = true;

        Index = d.Index;
        HubName = d.Name;
        Online = d.Online;
        OutputOn = d.On;
        ReportedPhase = d.Phase;
        Mode = d.Mode;
        Ton = d.Ton;
        Toff = d.Toff;
        Rssi = d.Rssi;
        RssiT = d.RssiT;
        AgeMs = d.AgeMs;
        Loss = d.Loss;
        Channel = d.Channel;
        Boot = d.Boot;
        UpSeconds = d.UpSeconds;
        Fw = d.Fw;
        Pending = d.Pending;
        SlotId = d.SlotId;

        _elAtEvent = d.Elapsed;
        _phaseAtEvent = d.Phase;
        _eventTime = now;

        // colour tag default: by slot
        if (!_app.Settings.Current.Devices.TryGetValue(Mac, out var a) || string.IsNullOrEmpty(a.Color))
        {
            var c = ColorFor(Mac, d.SlotId);
            if (c != TagColor) SetTag(c);
        }
        else if (TryParseColor(a.Color, out var stored) && stored != TagColor)
        {
            SetTag(stored);
        }

        RecordTimeline(now);
        if (Online && d.Rssi != 0)
        {
            _rssiSpark.Add(d.Rssi);
            if (_rssiSpark.Count > 40) _rssiSpark.RemoveAt(0);
            RssiHistory = _rssiSpark.ToArray();
        }
        RecordLink(now, d);
        Tick(now);
    }

    private TimelineState CurrentTimelineState => !Online ? TimelineState.Offline
        : Mode == TimerMode.ForceOn ? TimelineState.ForceOn
        : Mode == TimerMode.Paused ? TimelineState.Paused
        : OutputOn || _phaseAtEvent == CyclePhase.On && Mode == TimerMode.Auto ? TimelineState.On : TimelineState.Off;

    private void RecordTimeline(double now)
    {
        var state = CurrentTimelineState;
        if (_timeline.Count == 0 || _timeline[^1].State != state)
        {
            // a phase flip happened "elapsed" seconds ago; place the boundary there when that is plausible
            double t = state is TimelineState.On or TimelineState.Off && _elAtEvent < 1.0 ? now - _elAtEvent : now;
            if (_timeline.Count > 0) t = Math.Max(t, _timeline[^1].T);
            _timeline.Add(new TimelineSample(t, state));
        }
        // drop what is far outside the window
        while (_timeline.Count > 2 && _timeline[1].T < now - TimelineSeconds) _timeline.RemoveAt(0);
        TimelineSamples = _timeline.ToArray();
    }

    /// <summary>Adds a boundary when the interpolated phase flips between events so the strip stays accurate.</summary>
    private void NoteInterpolatedFlip(double now)
    {
        if (!Online || Mode != TimerMode.Auto) return;
        var state = Phase == CyclePhase.On ? TimelineState.On : TimelineState.Off;
        if (_timeline.Count > 0 && _timeline[^1].State != state && (_timeline[^1].State is TimelineState.On or TimelineState.Off))
        {
            _timeline.Add(new TimelineSample(now, state));
            TimelineSamples = _timeline.ToArray();
        }
    }

    private void RecordLink(double now, FleetDevice d)
    {
        if (now - _lastLinkSample < 1.0) return;
        _lastLinkSample = now;
        void Add(List<double> l, double v)
        {
            l.Add(v);
            if (l.Count > LinkHistoryPoints) l.RemoveAt(0);
        }
        Add(_linkRssi, d.Online && d.Rssi != 0 ? d.Rssi : double.NaN);
        Add(_linkRssiT, d.Online && d.RssiT != 0 ? d.RssiT : double.NaN);
        Add(_linkLoss, d.Online && d.Loss >= 0 ? d.Loss : double.NaN);
        LinkSeries = new GraphSeries[]
        {
            new("Signal at remote", "Fog.Accent", _linkRssi.ToArray()),
            new("Signal at timer", "Fog.Ok", _linkRssiT.ToArray(), thickness: 1.4),
            new("Loss %", "Fog.Warn", _linkLoss.ToArray(), rightAxis: true),
        };
        OnPropertyChanged(nameof(LinkWindowPoints));
    }

    /// <summary>Client side interpolation between the 500 ms events (called ~20x per second).</summary>
    public void Tick(double now)
    {
        double dt = now - _eventTime;
        var phase = _phaseAtEvent;
        double elapsed = _elAtEvent;
        double dur = (phase == CyclePhase.On ? Ton : Toff);

        if (Online && Mode != TimerMode.Paused && dt > 0)
        {
            elapsed += dt;
            for (int guard = 0; guard < 4 && dur > 0 && elapsed >= dur; guard++)
            {
                elapsed -= dur;
                phase = phase == CyclePhase.On ? CyclePhase.Off : CyclePhase.On;
                dur = phase == CyclePhase.On ? Ton : Toff;
            }
            if (dur > 0 && elapsed > dur) elapsed = dur;
        }

        Phase = phase;
        double remaining = dur > 0 ? Math.Max(0, dur - elapsed) : 0;
        double progress = dur > 0 ? Math.Clamp(elapsed / dur, 0, 1) : 0;
        if (Math.Abs(progress - Progress) > 0.0005) Progress = progress;
        if (Math.Abs(remaining - Remaining) > 0.04)
        {
            Remaining = remaining;
            RemainingText = Online || Mode == TimerMode.Paused ? Fmt.Seconds(remaining) : "-";
        }
        else if (!Online && RemainingText != "-")
        {
            RemainingText = "-";
        }

        string seen = Online ? "" : _hasData ? $"last seen {Fmt.Ago(AgeMs / 1000.0 + Math.Max(0, dt))}" : "never seen";
        if (seen != LastSeenText) LastSeenText = seen;

        bool rebooted = now < _rebootedUntil;
        if (rebooted != IsRebooted) IsRebooted = rebooted;

        TimelineNow = now;
        NoteInterpolatedFlip(now);
    }

    // ---- colour tag ------------------------------------------------------------------------

    private static Color ColorFor(string mac, int slot)
    {
        int h = Math.Abs(slot >= 0 ? slot : mac.GetHashCode());
        return ParseColor(TagColors[h % TagColors.Length]);
    }

    private static Color ParseColor(string hex) => (Color)ColorConverter.ConvertFromString(hex);

    private static bool TryParseColor(string? hex, out Color c)
    {
        try { c = ParseColor(hex!); return true; }
        catch { c = default; return false; }
    }

    private static Brush MakeBrush(Color c)
    {
        var b = new SolidColorBrush(c);
        b.Freeze();
        return b;
    }

    private void SetTag(Color c)
    {
        TagColor = c;
        TagBrush = MakeBrush(c);
    }

    [RelayCommand]
    private void CycleColor()
    {
        int i = Array.FindIndex(TagColors, h => ParseColor(h) == TagColor);
        var next = ParseColor(TagColors[(i + 1) % TagColors.Length]);
        SetTag(next);
        _app.Settings.AppearanceOf(Mac).Color = TagColors[(i + 1) % TagColors.Length];
        _app.Settings.SaveSoon();
    }

    // ---- name editing ----------------------------------------------------------------------

    [RelayCommand]
    private void StartEditName()
    {
        EditName = DisplayName;
        IsEditingName = true;
    }

    [RelayCommand]
    private void CommitName()
    {
        if (!IsEditingName) return;
        IsEditingName = false;
        Nickname = EditName;
    }

    [RelayCommand]
    private void CancelEditName() => IsEditingName = false;

    // ---- commands --------------------------------------------------------------------------

    private CommandTarget Target => CommandTarget.ByMac(Mac);

    private void Run(CommandSpec spec, string title, bool toastOnSuccess = false) =>
        _app.Commands.Run(spec, title, DisplayName, Mac, toastOnSuccess: toastOnSuccess);

    [RelayCommand]
    private void Select() => _app.Fleet.SelectDevice(this);

    [RelayCommand]
    private void OpenDetails()
    {
        _app.Fleet.SelectDevice(this);
        _app.Navigate(NavPage.Device);
    }

    [RelayCommand(CanExecute = nameof(CanControl))]
    private void Toggle() => Run(CommandSpec.Toggle(Target), "Toggle");

    [RelayCommand(CanExecute = nameof(CanControl))]
    private void Reset() => Run(CommandSpec.Reset(Target), "Reset cycle");

    [RelayCommand(CanExecute = nameof(CanControl))]
    private void PauseResume() =>
        Run(CommandSpec.Mode(Target, IsPaused ? TimerMode.Auto : TimerMode.Paused), IsPaused ? "Resume" : "Pause");

    [RelayCommand(CanExecute = nameof(CanControl))]
    private void Identify() => Run(CommandSpec.Identify(Target), "Identify (blinks the timer's LED for 5 s)", toastOnSuccess: true);

    [RelayCommand(CanExecute = nameof(CanControl))]
    private void HoldStart()
    {
        if (!_app.Hold.Begin(Mac, Mode)) return;
        OnPropertyChanged(nameof(IsHeld));
    }

    [RelayCommand]
    private void HoldEnd()
    {
        _app.Hold.End(Mac);
        OnPropertyChanged(nameof(IsHeld));
    }

    public bool IsHeld => _app.Hold.IsHolding(Mac);

    public void NotifyCanExecuteChanged()
    {
        ToggleCommand.NotifyCanExecuteChanged();
        ResetCommand.NotifyCanExecuteChanged();
        PauseResumeCommand.NotifyCanExecuteChanged();
        IdentifyCommand.NotifyCanExecuteChanged();
        HoldStartCommand.NotifyCanExecuteChanged();
    }
}
