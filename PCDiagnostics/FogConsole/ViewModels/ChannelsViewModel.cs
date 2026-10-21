using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using FogConsole.Controls;
using FogConsole.Hub;
using FogConsole.Protocol;
using FogConsole.Services;

namespace FogConsole.ViewModels;

public enum MigrationStatus { Pending, Confirmed, Failed }

public sealed partial class MigrationItemViewModel : ObservableObject
{
    public MigrationItemViewModel(string name, string mac)
    {
        Name = name;
        Mac = mac;
    }

    public string Name { get; }
    public string Mac { get; }

    [ObservableProperty] private MigrationStatus _status = MigrationStatus.Pending;

    public string StatusText => Status switch
    {
        MigrationStatus.Confirmed => "confirmed, hopping to the new channel",
        MigrationStatus.Failed => "did not confirm - stays on the old channel",
        _ => "waiting for confirmation...",
    };

    partial void OnStatusChanged(MigrationStatus value) => OnPropertyChanged(nameof(StatusText));
}

public sealed class LinkTestResultViewModel
{
    public required DateTime Time { get; init; }
    public required string DeviceName { get; init; }
    public required int Channel { get; init; }
    public required int Sent { get; init; }
    public required double MacAckPercent { get; init; }
    public required double EchoPercent { get; init; }
    public required double RttAvg { get; init; }
    public required double RttMax { get; init; }
    public required int Rssi { get; init; }

    public string TimeText => Time.ToString("HH:mm:ss");
    public string ChannelText => $"ch {Channel}";
    public string SuccessText => $"{EchoPercent:0}%";
    public string AckText => $"{MacAckPercent:0}%";
    public string RttText => RttAvg > 0 ? $"{RttAvg:0.0} / {RttMax:0} ms" : "-";
    public string RssiText => Rssi == 0 ? "-" : $"{Rssi} dBm";
}

/// <summary>Channel survey, system channel move, lost-timer recovery and the Link Lab.</summary>
public sealed partial class ChannelsViewModel : ObservableObject, IPageViewModel, IMessageSink
{
    private readonly AppServices _app;
    private readonly SurveyChMessage?[] _survey = new SurveyChMessage?[14];
    private IReadOnlyList<int> _best = Array.Empty<int>();
    private double _surveyStartedAt;
    private double _recoverStartedAt;
    private int _testId;
    private bool _waitingForTestId;
    private DeviceViewModel? _testDevice;
    private int _testChannel;

    public ChannelsViewModel(AppServices app)
    {
        _app = app;
        _cells = BuildCells();
        app.Fleet.Devices.CollectionChanged += (_, _) =>
        {
            if (LabDevice is null || !app.Fleet.Devices.Contains(LabDevice))
                LabDevice = app.Fleet.Selected ?? app.Fleet.Devices.FirstOrDefault();
        };
        app.Fleet.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName is nameof(FleetState.HomeChannel) or nameof(FleetState.IsLive))
            {
                Rebuild();
                RefreshCommands();
            }
        };
    }

    public NavPage Page => NavPage.Channels;
    public FleetState Fleet => _app.Fleet;

    public void Activate()
    {
        if (SelectedChannel == 0 && _app.Fleet.HomeChannel > 0) SelectedChannel = BestChannel ?? _app.Fleet.HomeChannel;
        Rebuild();
        RefreshCommands();
        if (LabDevice is null || !_app.Fleet.Devices.Contains(LabDevice)) LabDevice = _app.Fleet.Selected ?? _app.Fleet.Devices.FirstOrDefault();
    }

    public void Deactivate() { }

    // ---- survey ----------------------------------------------------------------------------

    [ObservableProperty] private IReadOnlyList<ChannelCell> _cells;
    [ObservableProperty] private int _selectedChannel;
    [ObservableProperty] private bool _isSurveying;
    [ObservableProperty] private int _surveyDone;
    [ObservableProperty] private string _surveyStatus = "No survey yet. A survey listens on all 13 channels for about 3 seconds.";
    [ObservableProperty] private string _recommendation = "";
    [ObservableProperty] private bool _forceMove;

    public bool CanUseTools => _app.Fleet.IsLive && !_app.Fleet.Remote.IsTimer;
    public bool IsDirectTimer => _app.Fleet.Remote.IsTimer;
    public int? BestChannel => _best.Count > 0 ? _best[0] : null;
    public int HomeChannel => _app.Fleet.HomeChannel;

    public string MoveButtonText => SelectedChannel > 0 ? $"Move system to channel {SelectedChannel}" : "Move system to channel...";

    partial void OnSelectedChannelChanged(int value)
    {
        OnPropertyChanged(nameof(MoveButtonText));
        MoveCommand.NotifyCanExecuteChanged();
    }

    private IReadOnlyList<ChannelCell> BuildCells()
    {
        var list = new List<ChannelCell>(13);
        for (int c = 1; c <= 13; c++)
        {
            var s = _survey[c];
            list.Add(new ChannelCell
            {
                Channel = c,
                HasData = s is not null,
                Score = s?.Score ?? 0,
                Aps = s?.Aps ?? 0,
                ApRssi = s?.ApRssi ?? 0,
                IsCurrent = c == _app.Fleet.HomeChannel,
                BestRank = Array.IndexOf(_best.ToArray(), c) is var i and >= 0 ? i + 1 : 0,
            });
        }
        return list;
    }

    private void Rebuild()
    {
        Cells = BuildCells();
        OnPropertyChanged(nameof(HomeChannel));
        OnPropertyChanged(nameof(CanUseTools));
        OnPropertyChanged(nameof(IsDirectTimer));
    }

    private void RefreshCommands()
    {
        SurveyCommand.NotifyCanExecuteChanged();
        MoveCommand.NotifyCanExecuteChanged();
        RecoverCommand.NotifyCanExecuteChanged();
        RunLinkTestCommand.NotifyCanExecuteChanged();
        OnPropertyChanged(nameof(CanUseTools));
    }

    private bool CanTool() => CanUseTools && !IsSurveying && !MigrationActive && !IsRecovering;

    [RelayCommand(CanExecute = nameof(CanTool))]
    private async Task Survey()
    {
        try
        {
            var res = await _app.Hub.RequestAsync(Requests.Survey()).ConfigureAwait(true);
            if (!res.Ok)
            {
                _app.Toasts.Warning(res.Err == "busy" ? "The radio is busy right now. Try again in a few seconds." : $"Survey failed: {res.Describe()}");
                return;
            }
            Array.Clear(_survey);
            _best = Array.Empty<int>();
            Recommendation = "";
            SurveyDone = 0;
            IsSurveying = true;
            _surveyStartedAt = _app.Clock.Seconds;
            SurveyStatus = "Measuring the 2.4 GHz band... (the radio is busy for about 3 seconds)";
            Rebuild();
            RefreshCommands();
        }
        catch (HubException ex)
        {
            _app.Toasts.Error($"Survey failed: {ex.Message}");
        }
    }

    // ---- move system -----------------------------------------------------------------------

    [ObservableProperty] private bool _migrationActive;
    [ObservableProperty] private string _migrationSummary = "";
    public ObservableCollection<MigrationItemViewModel> MigrationItems { get; } = new();
    private int _migrationTarget;

    private bool CanMove() => CanTool() && SelectedChannel is >= 1 and <= 13 && SelectedChannel != _app.Fleet.HomeChannel;

    [RelayCommand(CanExecute = nameof(CanMove))]
    private async Task Move()
    {
        int ch = SelectedChannel;
        int online = _app.Fleet.Devices.Count(d => d.Online);
        int offline = _app.Fleet.Devices.Count - online;
        string message =
            $"The whole system moves from channel {_app.Fleet.HomeChannel} to channel {ch}.\n\n" +
            $"1.  The remote asks the {online} online timer(s) to confirm the new channel.\n" +
            "2.  When they have confirmed, all of them hop together and the remote follows.\n" +
            "3.  Control is interrupted for a few seconds.\n\n" +
            (offline > 0
                ? $"{offline} timer(s) are offline right now (heating?). They will not hear the command, come back on channel {_app.Fleet.HomeChannel} and stay lost until you press \"Find lost timers\".\n\n"
                : "") +
            (ForceMove
                ? "FORCE is on: the remote moves even if no timer confirms. That can leave you without any connection to your timers."
                : "If no timer confirms, the change is abandoned and nothing moves.");
        if (!_app.Dialogs.Confirm($"Move the system to channel {ch}?", message, "Move system", destructive: true)) return;

        try
        {
            var res = await _app.Hub.RequestAsync(Requests.Channel(ch, ForceMove)).ConfigureAwait(true);
            if (!res.Ok)
            {
                _app.Toasts.Error(res.Err == "busy" ? "The radio is busy right now. Try again in a few seconds." : $"Channel change refused: {res.Describe()}");
                return;
            }
            MigrationItems.Clear();
            foreach (var d in _app.Fleet.Devices.Where(d => d.Online).OrderBy(d => d.Index))
                MigrationItems.Add(new MigrationItemViewModel(d.DisplayName, d.Mac));
            _migrationTarget = ch;
            MigrationSummary = $"Moving to channel {ch}...";
            MigrationActive = true;
            RefreshCommands();
        }
        catch (HubException ex)
        {
            _app.Toasts.Error($"Channel change failed: {ex.Message}");
        }
    }

    partial void OnMigrationActiveChanged(bool value) => RefreshCommands();
    partial void OnIsSurveyingChanged(bool value) => RefreshCommands();

    // ---- recover ---------------------------------------------------------------------------

    [ObservableProperty] private bool _isRecovering;
    public ObservableCollection<string> RecoverLog { get; } = new();

    [RelayCommand(CanExecute = nameof(CanTool))]
    private async Task Recover()
    {
        try
        {
            var res = await _app.Hub.RequestAsync(Requests.Recover()).ConfigureAwait(true);
            if (!res.Ok)
            {
                _app.Toasts.Warning(res.Err == "busy" ? "The radio is busy right now. Try again in a few seconds." : $"Recover failed: {res.Describe()}");
                return;
            }
            RecoverLog.Clear();
            RecoverLog.Add("Sweeping channels 1-13 for timers that went quiet...");
            IsRecovering = true;
            _recoverStartedAt = _app.Clock.Seconds;
            RefreshCommands();
        }
        catch (HubException ex)
        {
            _app.Toasts.Error($"Recover failed: {ex.Message}");
        }
    }

    partial void OnIsRecoveringChanged(bool value) => RefreshCommands();

    // ---- link lab --------------------------------------------------------------------------

    [ObservableProperty] private DeviceViewModel? _labDevice;
    [ObservableProperty] private int _labCount = 50;
    [ObservableProperty] private int _labGapMs = 40;
    [ObservableProperty] private bool _isTesting;
    [ObservableProperty] private int _liveSent;
    [ObservableProperty] private double _liveAckPercent;
    [ObservableProperty] private double _liveEchoPercent;
    [ObservableProperty] private double _liveRttAvg;
    [ObservableProperty] private double _liveRttMax;
    [ObservableProperty] private int _liveRssi;
    [ObservableProperty] private string _labStatus = "Pick a timer and press Run. The remote sends unicast pings and counts how many arrive.";
    public ObservableCollection<LinkTestResultViewModel> LabHistory { get; } = new();
    public IReadOnlyList<int> CountChoices { get; } = new[] { 20, 50, 100, 200 };
    public IReadOnlyList<int> GapChoices { get; } = new[] { 20, 40, 100, 250 };

    public double LiveProgress => LabCount > 0 ? Math.Clamp(LiveSent / (double)LabCount, 0, 1) : 0;
    public string LiveEchoText => LiveSent == 0 ? "-" : $"{LiveEchoPercent:0}%";
    public string LiveAckText => LiveSent == 0 ? "-" : $"{LiveAckPercent:0}%";
    public string LiveRttText => LiveRttAvg > 0 ? $"{LiveRttAvg:0.0} ms avg · {LiveRttMax:0} ms max" : "-";
    public string LiveRssiText => LiveRssi == 0 ? "-" : $"{LiveRssi} dBm";

    partial void OnLiveSentChanged(int value) { OnPropertyChanged(nameof(LiveProgress)); OnPropertyChanged(nameof(LiveEchoText)); OnPropertyChanged(nameof(LiveAckText)); }
    partial void OnLiveEchoPercentChanged(double value) => OnPropertyChanged(nameof(LiveEchoText));
    partial void OnLiveAckPercentChanged(double value) => OnPropertyChanged(nameof(LiveAckText));
    partial void OnLiveRttAvgChanged(double value) => OnPropertyChanged(nameof(LiveRttText));
    partial void OnLiveRttMaxChanged(double value) => OnPropertyChanged(nameof(LiveRttText));
    partial void OnLiveRssiChanged(int value) => OnPropertyChanged(nameof(LiveRssiText));
    partial void OnLabCountChanged(int value) => OnPropertyChanged(nameof(LiveProgress));
    partial void OnIsTestingChanged(bool value) => RefreshCommands();
    partial void OnLabDeviceChanged(DeviceViewModel? value) => RunLinkTestCommand.NotifyCanExecuteChanged();

    private bool CanRunTest() => CanUseTools && !IsTesting && !IsSurveying && !MigrationActive && LabDevice is { Online: true };

    [RelayCommand(CanExecute = nameof(CanRunTest))]
    private async Task RunLinkTest()
    {
        var d = LabDevice;
        if (d is null) return;
        try
        {
            LiveSent = 0;
            LiveAckPercent = LiveEchoPercent = LiveRttAvg = LiveRttMax = 0;
            LiveRssi = 0;
            _testDevice = d;
            _testChannel = _app.Fleet.HomeChannel;
            _testId = 0;
            _waitingForTestId = true;
            IsTesting = true;
            LabStatus = $"Testing the link to {d.DisplayName} on channel {_testChannel}...";
            var res = await _app.Hub.RequestAsync(Requests.LinkTest(d.Index, LabCount, LabGapMs)).ConfigureAwait(true);
            if (!res.Ok)
            {
                IsTesting = false;
                _waitingForTestId = false;
                LabStatus = res.Err == "busy" ? "The radio is busy right now. Try again in a few seconds." : $"Link test refused: {res.Describe()}";
                _app.Toasts.Warning(LabStatus);
                return;
            }
            if (_waitingForTestId) { _testId = res.Id; _waitingForTestId = false; }
        }
        catch (HubException ex)
        {
            IsTesting = false;
            _waitingForTestId = false;
            LabStatus = $"Link test failed: {ex.Message}";
            _app.Toasts.Error(LabStatus);
        }
    }

    [RelayCommand]
    private void ClearLabHistory() => LabHistory.Clear();

    // ---- messages --------------------------------------------------------------------------

    public void OnMessage(Message message)
    {
        switch (message)
        {
            case SurveyChMessage s when s.Channel is >= 1 and <= 13:
                _survey[s.Channel] = s;
                SurveyDone = _survey.Count(x => x is not null);
                Rebuild();
                break;

            case SurveyDoneMessage done:
                _best = done.Best;
                IsSurveying = false;
                SurveyDone = 13;
                if (SelectedChannel == 0 || SelectedChannel == _app.Fleet.HomeChannel) SelectedChannel = done.Best.Count > 0 ? done.Best[0] : SelectedChannel;
                var cur = _survey[Math.Clamp(done.Current, 1, 13)];
                if (done.Best.Count > 0)
                {
                    var best = _survey[Math.Clamp(done.Best[0], 1, 13)];
                    Recommendation = $"Best: channel {string.Join(", ", done.Best)}.";
                    if (cur is not null && best is not null && done.Best[0] != done.Current)
                        Recommendation += cur.Score - best.Score >= 10
                            ? $" The current channel {done.Current} (score {cur.Score}) is noticeably busier than channel {done.Best[0]} (score {best.Score})."
                            : $" Channel {done.Current} (score {cur.Score}) is about as good as channel {done.Best[0]} (score {best.Score}); moving is optional.";
                    else Recommendation += " The current channel is already the quietest one.";
                }
                SurveyStatus = "Survey finished. Lower score = quieter. Click a column to choose a target channel.";
                Rebuild();
                break;

            case MigrateMessage m:
                OnMigrate(m);
                break;

            case LinkTestMessage t:
                OnLinkTest(t);
                break;

            case FoundMessage f when IsRecovering && f.Paired >= 0:
                var name = _app.Fleet.ByIndex(f.Paired)?.DisplayName ?? f.Name;
                RecoverLog.Add(f.Channel != _app.Fleet.HomeChannel
                    ? $"Found {name} on channel {f.Channel}. It is being moved back to channel {_app.Fleet.HomeChannel}."
                    : $"{name} answered on channel {f.Channel}.");
                break;
        }
    }

    private void OnMigrate(MigrateMessage m)
    {
        switch (m.Stage)
        {
            case "start":
                if (!MigrationActive || MigrationItems.Count == 0)
                {
                    MigrationItems.Clear();
                    foreach (var d in _app.Fleet.Devices.Where(d => d.Online).OrderBy(d => d.Index))
                        MigrationItems.Add(new MigrationItemViewModel(d.DisplayName, d.Mac));
                    MigrationActive = true;
                }
                _migrationTarget = m.Channel;
                MigrationSummary = m.Targets == 0 ? $"Moving to channel {m.Channel}: no timer is online to confirm." : $"Moving to channel {m.Channel}: asking {m.Targets} timer(s) to confirm...";
                break;

            case "dev":
                var dev = _app.Fleet.ByIndex(m.Dev);
                var item = dev is null ? null : MigrationItems.FirstOrDefault(i => i.Mac == dev.Mac);
                if (item is null)
                {
                    item = new MigrationItemViewModel(dev?.DisplayName ?? $"timer {m.Dev}", dev?.Mac ?? "");
                    MigrationItems.Add(item);
                }
                item.Status = m.DevOk == true ? MigrationStatus.Confirmed : MigrationStatus.Failed;
                break;

            case "done":
                foreach (var pending in MigrationItems.Where(i => i.Status == MigrationStatus.Pending))
                    pending.Status = MigrationStatus.Failed;
                MigrationActive = false;
                if (m.Committed)
                {
                    MigrationSummary = $"System is now on channel {m.Channel}: {m.OkCount} timer(s) moved" + (m.Failed > 0 ? $", {m.Failed} did not confirm (offline timers stay behind - use \"Find lost timers\")." : ".");
                    _app.Toasts.Show(m.Failed > 0 ? ToastKind.Warning : ToastKind.Success, MigrationSummary);
                }
                else
                {
                    MigrationSummary = $"Channel change to {m.Channel} was abandoned: no timer confirmed. Nothing moved.";
                    _app.Toasts.Warning(MigrationSummary);
                }
                break;
        }
    }

    private void OnLinkTest(LinkTestMessage t)
    {
        if (_waitingForTestId && IsTesting) { _testId = t.Id; _waitingForTestId = false; }
        if (t.Id != _testId) return;

        LiveSent = t.Sent;
        LiveAckPercent = t.Sent > 0 ? 100.0 * t.MacAck / t.Sent : 0;
        LiveEchoPercent = t.Sent > 0 ? 100.0 * t.Echo / t.Sent : 0;
        LiveRttAvg = t.RttMs;
        LiveRttMax = t.RttMax;
        LiveRssi = t.Rssi;
        if (!t.Done) return;

        IsTesting = false;
        var name = _testDevice?.DisplayName ?? $"timer {t.Dev}";
        LabHistory.Insert(0, new LinkTestResultViewModel
        {
            Time = DateTime.Now,
            DeviceName = name,
            Channel = _testChannel,
            Sent = t.Sent,
            MacAckPercent = LiveAckPercent,
            EchoPercent = LiveEchoPercent,
            RttAvg = t.RttMs,
            RttMax = t.RttMax,
            Rssi = t.Rssi,
        });
        while (LabHistory.Count > 30) LabHistory.RemoveAt(LabHistory.Count - 1);
        LabStatus = $"{name} on channel {_testChannel}: {LiveEchoPercent:0}% of {t.Sent} pings answered, {LiveAckPercent:0}% acknowledged by the radio.";
    }

    /// <summary>Housekeeping: watchdogs for operations whose end event might get lost.</summary>
    public void Tick()
    {
        double now = _app.Clock.Seconds;
        if (IsSurveying && now - _surveyStartedAt > 10)
        {
            IsSurveying = false;
            SurveyStatus = "The survey did not finish (no result from the remote).";
        }
        if (IsRecovering && now - _recoverStartedAt > 3 && !_app.Fleet.Remote.Scanning || IsRecovering && now - _recoverStartedAt > 12)
        {
            IsRecovering = false;
            RecoverLog.Add(RecoverLog.Count <= 1 ? "Sweep finished: no lost timers were found." : "Sweep finished.");
        }
        if (MigrationActive && now - _surveyStartedAt > 0 && MigrationItems.Count == 0 && !_app.Fleet.Remote.Scanning && _migrationTarget == _app.Fleet.HomeChannel)
            MigrationActive = false;
    }

    public void ClearSession()
    {
        IsSurveying = false;
        IsRecovering = false;
        IsTesting = false;
        MigrationActive = false;
    }
}
