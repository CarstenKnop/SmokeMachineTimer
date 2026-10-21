using System.Collections.ObjectModel;
using System.Diagnostics;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using FogConsole.Hub;
using FogConsole.Ota;
using FogConsole.Protocol;
using FogConsole.Services;

namespace FogConsole.ViewModels;

/// <summary>One timer in the over-the-air list: tick box, what is known about it and the progress of its update.</summary>
public sealed partial class OtaTimerRow : ObservableObject
{
    public OtaTimerRow(string mac)
    {
        Mac = mac;
    }

    public string Mac { get; }

    [ObservableProperty] private int _index;
    [ObservableProperty] private string _name = "";
    [ObservableProperty] private string _version = "-";
    [ObservableProperty] private bool _online;
    [ObservableProperty] private bool _isChecked;
    [ObservableProperty] private OtaSupport _support = OtaSupport.Unknown;

    [ObservableProperty] private string _stage = "";
    [ObservableProperty] private double _fraction;
    [ObservableProperty] private bool _indeterminate;
    [ObservableProperty] private string _speedText = "";
    [ObservableProperty] private string _etaText = "";
    [ObservableProperty] private string _resentText = "";
    [ObservableProperty] private string _resultText = "";
    [ObservableProperty] private ChipLevel _resultLevel = ChipLevel.Neutral;
    [ObservableProperty] private bool _isRunning;

    public bool HasResult => ResultText.Length > 0;
    public bool ShowProgress => IsRunning || Stage.Length > 0;
    public string OnlineText => Online ? "online" : "offline";
    public ChipLevel OnlineLevel => Online ? ChipLevel.Ok : ChipLevel.Warn;

    public string SupportText => Support switch
    {
        OtaSupport.Capable => "ready for OTA",
        OtaSupport.NeedsUsbUpdate => "needs a USB update first",
        OtaSupport.NoOtaFirmware => "no OTA firmware",
        OtaSupport.Busy => "busy with another update",
        OtaSupport.Probing => "checking...",
        _ => "not checked",
    };

    public string SupportTooltip => Support switch
    {
        OtaSupport.Capable => "The timer answered the probe: it can receive a firmware over the air.",
        OtaSupport.NeedsUsbUpdate => OtaText.Error(OtaError.NoSlot),
        OtaSupport.NoOtaFirmware => "The timer did not answer: its firmware has no over-the-air support (or it is out of range). Update it once over its USB cable.",
        OtaSupport.Busy => OtaText.Error(OtaError.Busy),
        _ => "Press \"Check support\" to ask the timer.",
    };

    public ChipLevel SupportLevel => Support switch
    {
        OtaSupport.Capable => ChipLevel.Ok,
        OtaSupport.NeedsUsbUpdate or OtaSupport.NoOtaFirmware or OtaSupport.Busy => ChipLevel.Warn,
        _ => ChipLevel.Neutral,
    };

    partial void OnSupportChanged(OtaSupport value)
    {
        OnPropertyChanged(nameof(SupportText));
        OnPropertyChanged(nameof(SupportTooltip));
        OnPropertyChanged(nameof(SupportLevel));
    }

    partial void OnOnlineChanged(bool value)
    {
        OnPropertyChanged(nameof(OnlineText));
        OnPropertyChanged(nameof(OnlineLevel));
    }

    partial void OnResultTextChanged(string value) => OnPropertyChanged(nameof(HasResult));
    partial void OnStageChanged(string value) => OnPropertyChanged(nameof(ShowProgress));
    partial void OnIsRunningChanged(bool value) => OnPropertyChanged(nameof(ShowProgress));

    public void ClearProgress()
    {
        Stage = "";
        Fraction = 0;
        Indeterminate = false;
        SpeedText = EtaText = ResentText = "";
        ResultText = "";
        ResultLevel = ChipLevel.Neutral;
        IsRunning = false;
    }
}

/// <summary>
/// Over-the-air update of the timers through the connected remote (or of a timer on USB) - the second route of the Firmware page.
/// The firmware is chosen in the shared list; the ticked timers are updated one after another.
/// </summary>
public sealed partial class OtaViewModel : ObservableObject
{
    private const int MaxLogLines = 600;

    private readonly AppServices _app;
    private readonly Func<FirmwareChoice?> _selectedFirmware;
    private readonly Dictionary<string, OtaTimerRow> _byMac = new(StringComparer.OrdinalIgnoreCase);
    private CancellationTokenSource? _cts;

    public OtaViewModel(AppServices app, Func<FirmwareChoice?> selectedFirmware)
    {
        _app = app;
        _selectedFirmware = selectedFirmware;
        Rows.CollectionChanged += (_, _) => OnPropertyChanged(nameof(HasRows));
    }

    /// <summary>Opens the link to the timers. Replaceable so that tests can use their own transport.</summary>
    public Func<HubClient, IOtaLink> LinkFactory { get; set; } = hub => new HubOtaLink(hub);

    /// <summary>Timings of the sender (tests and the screenshot script shorten them).</summary>
    public OtaTransferOptions TransferOptions { get; set; } = new();

    public ObservableCollection<OtaTimerRow> Rows { get; } = new();
    public ObservableCollection<string> LogLines { get; } = new();

    [ObservableProperty] private bool _isBusy;
    [ObservableProperty] private string _blockedText = "";
    [ObservableProperty] private string _summaryText = "";
    [ObservableProperty] private ChipLevel _summaryLevel = ChipLevel.Neutral;
    [ObservableProperty] private string _stageText = "";

    public bool HasRows => Rows.Count > 0;
    public bool HasLog => LogLines.Count > 0;
    public bool HasSummary => SummaryText.Length > 0;
    public bool IsBlocked => BlockedText.Length > 0;
    public string LogText => string.Join(Environment.NewLine, LogLines);

    /// <summary>The remote (or timer) behind the hub lists the <c>ota</c> capability.</summary>
    public bool IsAvailable => _app.IsConnected && _app.Hub.Hello is { SupportsOta: true };

    public bool CanUpdate => !IsBusy && !IsBlocked;
    public bool CanCheck => !IsBusy && IsAvailable && Rows.Count > 0;
    public string UpdateButtonText => IsBusy ? "Updating..." : "Update selected timers";

    public int CheckedCount => Rows.Count(r => r.IsChecked);

    partial void OnIsBusyChanged(bool value)
    {
        OnPropertyChanged(nameof(CanUpdate));
        OnPropertyChanged(nameof(CanCheck));
        OnPropertyChanged(nameof(UpdateButtonText));
        UpdateCommand.NotifyCanExecuteChanged();
        CheckSupportCommand.NotifyCanExecuteChanged();
        CancelCommand.NotifyCanExecuteChanged();
    }

    partial void OnBlockedTextChanged(string value)
    {
        OnPropertyChanged(nameof(IsBlocked));
        OnPropertyChanged(nameof(CanUpdate));
        UpdateCommand.NotifyCanExecuteChanged();
    }

    partial void OnSummaryTextChanged(string value) => OnPropertyChanged(nameof(HasSummary));

    // ---- the list of timers ---------------------------------------------------------------------------------------

    /// <summary>Rebuilds the list from the fleet (names, versions, online) and re-evaluates why updating might not be possible. Cheap; called about once a second.</summary>
    public void Refresh()
    {
        OnPropertyChanged(nameof(IsAvailable));
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var d in _app.Fleet.Devices)
        {
            if (string.IsNullOrEmpty(d.Mac)) continue;
            seen.Add(d.Mac);
            if (!_byMac.TryGetValue(d.Mac, out var row))
            {
                row = new OtaTimerRow(d.Mac);
                row.PropertyChanged += (_, e) =>
                {
                    if (e.PropertyName == nameof(OtaTimerRow.IsChecked)) OnRowTickChanged();
                };
                _byMac[d.Mac] = row;
                Rows.Add(row);
            }
            row.Index = d.Index;
            row.Name = d.DisplayName;
            row.Version = string.IsNullOrEmpty(d.Fw) ? "-" : d.Fw;
            row.Online = d.Online;
        }
        foreach (var gone in _byMac.Keys.Where(k => !seen.Contains(k)).ToList())
        {
            Rows.Remove(_byMac[gone]);
            _byMac.Remove(gone);
        }
        if (!IsBusy) ComputeBlocked();
        CheckSupportCommand.NotifyCanExecuteChanged();
        OnPropertyChanged(nameof(CanCheck));
    }

    private void OnRowTickChanged()
    {
        OnPropertyChanged(nameof(CheckedCount));
        if (!IsBusy) ComputeBlocked();
    }

    /// <summary>Called when the firmware choice of the page changes.</summary>
    public void OnFirmwareChanged()
    {
        if (!IsBusy) ComputeBlocked();
    }

    private void ComputeBlocked()
    {
        BlockedText = WhyNot() ?? "";
    }

    /// <summary>The reason an update cannot start now, or null.</summary>
    private string? WhyNot()
    {
        if (!_app.IsConnected)
            return "Over-the-air updates go through the remote: connect to it first (or try the offline demo).";
        var hello = _app.Hub.Hello;
        if (hello is null) return "Waiting for the device to answer.";
        if (!hello.SupportsOta)
            return hello.Kind == DeviceKind.Timer
                ? $"This timer's firmware ({hello.Fw}) cannot receive updates over its USB cable (no OTA support). Update it with the USB cable route."
                : $"The remote's firmware ({hello.Fw}) cannot forward firmware updates to timers. Update the remote over USB first (firmware 2.1.0 or newer).";

        var choice = _selectedFirmware();
        if (choice is null) return "Choose the timer firmware in the list above.";
        if (choice.HasProblems) return "The chosen firmware cannot be used: " + choice.ProblemText;
        if (choice.Set.Kind == Flashing.FirmwareKind.Remote)
            return $"{choice.Title} is firmware for the remote; the timers need timer firmware. Choose a timer firmware in the list above.";
        if (choice.Set.Kind == Flashing.FirmwareKind.Unknown)
            return "This file does not say that it is timer firmware (its identity string is missing), and the timers would refuse it. Choose a timer firmware.";
        if (choice.Set.AppInfo.ChipId != Flashing.Esp32C3.ChipId)
            return "The chosen image is not built for the ESP32-C3.";

        if (Rows.Count == 0) return "No timers are paired yet.";
        if (!Rows.Any(r => r.IsChecked)) return "Tick the timers to update.";
        var offline = Rows.Where(r => r.IsChecked && !r.Online).Select(r => r.Name).ToList();
        if (offline.Count == Rows.Count(r => r.IsChecked))
            return $"{string.Join(", ", offline)} {(offline.Count == 1 ? "is" : "are")} offline (a machine that is heating up has no power). An offline timer cannot be updated.";
        return null;
    }

    [RelayCommand]
    private void SelectAll()
    {
        foreach (var r in Rows) r.IsChecked = true;
    }

    [RelayCommand]
    private void SelectNone()
    {
        foreach (var r in Rows) r.IsChecked = false;
    }

    /// <summary>Ticks every online timer whose firmware is older than the chosen image (what you usually want).</summary>
    [RelayCommand]
    private void SelectOutdated()
    {
        var version = _selectedFirmware()?.Set.Version;
        foreach (var r in Rows)
            r.IsChecked = r.Online && (version is null || r.Version == "-" || Flashing.Versions.Compare(r.Version, version) < 0);
    }

    // ---- running ------------------------------------------------------------------------------------------------------

    private OtaTimerTarget TargetOf(OtaTimerRow r)
    {
        bool direct = _app.Hub.Hello?.Kind == DeviceKind.Timer;
        return new OtaTimerTarget(r.Mac, r.Name, direct ? CommandTarget.Dev(0) : CommandTarget.ByMac(r.Mac));
    }

    private HubFleetWatcher NewWatcher()
    {
        var w = new HubFleetWatcher(_app.Hub);
        w.Seed(_app.Fleet.Devices.Select(d => new FleetDevice { Index = d.Index, Mac = d.Mac, Name = d.DisplayName, Online = d.Online, Boot = d.Boot, Fw = d.Fw }));
        return w;
    }

    private OtaUpdater NewUpdater(IOtaLink link, HubFleetWatcher watcher)
    {
        var updater = new OtaUpdater(link, watcher, _app.Settings.Current.OtaKey, new OtaUpdaterOptions { Transfer = TransferOptions });
        updater.Log += line => _app.Ui.Post(() => AppendLog(line));
        updater.SupportChecked += (t, s) => _app.Ui.Post(() => { if (_byMac.TryGetValue(t.Mac, out var row)) row.Support = s; });
        updater.ProgressChanged += p => _app.Ui.Post(() => ApplyProgress(p));
        updater.TimerFinished += r => _app.Ui.Post(() => ApplyResult(r));
        return updater;
    }

    private void ApplyProgress(OtaTimerProgress p)
    {
        if (!_byMac.TryGetValue(p.Timer.Mac, out var row) || !row.IsRunning) return;
        row.Stage = p.Stage;
        row.Fraction = p.Fraction;
        row.Indeterminate = p.Phase is null or OtaPhase.Beginning or OtaPhase.Restarting or OtaPhase.Activating;
        row.SpeedText = p.SpeedBytesPerSecond > 1 ? $"{p.SpeedBytesPerSecond / 1024:0.0} KB/s" : "";
        row.EtaText = p.EtaSeconds is { } eta && p.Phase == OtaPhase.Streaming ? $"about {Fmt.Duration((long)Math.Ceiling(eta))} left" : "";
        row.ResentText = p.Resent > 0 ? $"{p.Resent} re-sent" : "";
    }

    private void ApplyResult(OtaTimerResult r)
    {
        if (!_byMac.TryGetValue(r.Timer.Mac, out var row)) return;
        row.IsRunning = false;
        row.Indeterminate = false;
        row.Stage = r.Outcome == OtaOutcome.Updated ? "Done" : r.Outcome == OtaOutcome.Cancelled ? "Cancelled" : r.Outcome == OtaOutcome.Skipped ? "Skipped" : "Failed";
        row.Fraction = r.Outcome == OtaOutcome.Updated ? 1 : row.Fraction;
        row.SpeedText = row.EtaText = "";
        row.ResentText = r.Resent > 0 ? $"{r.Resent} re-sent" : "";
        row.ResultText = r.Message;
        row.ResultLevel = r.Outcome switch
        {
            OtaOutcome.Updated => ChipLevel.Ok,
            OtaOutcome.Cancelled or OtaOutcome.Skipped => ChipLevel.Warn,
            _ => ChipLevel.Bad,
        };
        if (r.Outcome == OtaOutcome.NeedsUsbUpdate) row.Support = OtaSupport.NeedsUsbUpdate;
        if (r.Outcome == OtaOutcome.NoOtaFirmware) row.Support = OtaSupport.NoOtaFirmware;
        AppendLog($"{r.Timer.Name}: {r.Message}");
    }

    [RelayCommand(CanExecute = nameof(CanUpdate))]
    private async Task Update()
    {
        var choice = _selectedFirmware();
        if (choice is null || IsBusy) return;
        ComputeBlocked();
        if (IsBlocked) return;

        var chosen = Rows.Where(r => r.IsChecked).ToList();
        var skippedOffline = chosen.Where(r => !r.Online).ToList();
        var targets = chosen.Where(r => r.Online).ToList();
        if (targets.Count == 0) return;

        OtaImage image;
        try
        {
            image = new OtaImage(choice.Set.App, choice.Set.Version);
        }
        catch (ArgumentException ex)
        {
            _app.Toasts.Error($"That firmware cannot be sent: {ex.Message}");
            return;
        }

        LogLines.Clear();
        OnPropertyChanged(nameof(HasLog));
        OnPropertyChanged(nameof(LogText));
        SummaryText = "";
        foreach (var r in Rows) r.ClearProgress();
        foreach (var r in targets) { r.IsRunning = true; r.Stage = "Waiting"; r.Indeterminate = true; }
        foreach (var r in skippedOffline)
        {
            r.Stage = "Skipped";
            r.ResultText = "The timer is offline (a machine that is heating up has no power). Try again when it is back.";
            r.ResultLevel = ChipLevel.Warn;
        }
        StageText = "Starting...";
        IsBusy = true;
        _cts = new CancellationTokenSource();
        AppendLog($"Updating {targets.Count} timer{(targets.Count == 1 ? "" : "s")} with {choice.Title} ({image.Size / 1024:N0} KB, {image.Chunks:N0} chunks).");

        IReadOnlyList<OtaTimerResult> results = Array.Empty<OtaTimerResult>();
        HubOtaLinkHolder? holder = null;
        try
        {
            holder = new HubOtaLinkHolder(LinkFactory(_app.Hub));
            using var watcher = NewWatcher();
            await watcher.EnsureAsync(_cts.Token).ConfigureAwait(true);
            using var updater = NewUpdater(holder.Link, watcher);
            results = await updater.UpdateAsync(targets.Select(TargetOf).ToList(), image, _cts.Token).ConfigureAwait(true);
        }
        catch (Exception ex) when (ex is not OperationCanceledException)
        {
            CrashLog.Write("Over-the-air update", ex);
            AppendLog($"Unexpected problem: {ex.Message}");
            SummaryText = $"Unexpected problem: {ex.Message}";
            SummaryLevel = ChipLevel.Bad;
        }
        finally
        {
            holder?.Dispose();
        }

        _app.Ui.Post(() => { });                          // let the last progress and result updates land first
        foreach (var r in Rows) r.IsRunning = false;
        int ok = results.Count(r => r.Succeeded);
        if (SummaryText.Length == 0)
        {
            bool cancelled = results.Any(r => r.Outcome == OtaOutcome.Cancelled);
            SummaryText = $"{ok} of {results.Count} timer{(results.Count == 1 ? "" : "s")} updated" +
                          (cancelled ? " (cancelled)" : results.Count - ok > 0 ? $"; {results.Count - ok} did not get the new firmware - see below" : "") + ".";
            SummaryLevel = ok == results.Count && results.Count > 0 ? ChipLevel.Ok : ok > 0 ? ChipLevel.Warn : cancelled ? ChipLevel.Warn : ChipLevel.Bad;
        }
        AppendLog(SummaryText);
        StageText = "";
        _cts?.Dispose();
        _cts = null;
        IsBusy = false;
        ComputeBlocked();
        if (SummaryLevel == ChipLevel.Ok) _app.Toasts.Success(SummaryText);
        else if (SummaryLevel == ChipLevel.Warn) _app.Toasts.Warning(SummaryText);
        else _app.Toasts.Error(SummaryText);
    }

    /// <summary>Asks the ticked timers (or all when none is ticked) whether they can be updated over the air.</summary>
    [RelayCommand(CanExecute = nameof(CanCheck))]
    private async Task CheckSupport()
    {
        if (IsBusy || !IsAvailable) return;
        var rows = Rows.Any(r => r.IsChecked) ? Rows.Where(r => r.IsChecked).ToList() : Rows.ToList();
        if (rows.Count == 0) return;

        StageText = "Checking over-the-air support...";
        IsBusy = true;
        _cts = new CancellationTokenSource();
        HubOtaLinkHolder? holder = null;
        try
        {
            holder = new HubOtaLinkHolder(LinkFactory(_app.Hub));
            using var watcher = NewWatcher();
            await watcher.EnsureAsync(_cts.Token).ConfigureAwait(true);
            using var updater = NewUpdater(holder.Link, watcher);
            await updater.ProbeAsync(rows.Select(TargetOf).ToList(), _cts.Token).ConfigureAwait(true);
            int ready = rows.Count(r => r.Support == OtaSupport.Capable);
            AppendLog($"{ready} of {rows.Count} timer{(rows.Count == 1 ? "" : "s")} can be updated over the air.");
        }
        catch (OperationCanceledException)
        {
            AppendLog("Check cancelled.");
        }
        catch (Exception ex)
        {
            CrashLog.Write("Over-the-air probe", ex);
            _app.Toasts.Warning($"The check failed: {ex.Message}");
        }
        finally
        {
            holder?.Dispose();
            _app.Ui.Post(() => { });
            StageText = "";
            _cts?.Dispose();
            _cts = null;
            IsBusy = false;
        }
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

    /// <summary>Cancels a running update (the window is closing). The timers keep their old firmware.</summary>
    public void CancelForShutdown() => _cts?.Cancel();

    private sealed class HubOtaLinkHolder : IDisposable
    {
        public HubOtaLinkHolder(IOtaLink link) => Link = link;
        public IOtaLink Link { get; }
        public void Dispose() => (Link as IDisposable)?.Dispose();
    }
}
