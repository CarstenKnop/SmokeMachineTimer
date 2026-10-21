using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using FogConsole.Hub;
using FogConsole.Protocol;

namespace FogConsole.Services;

/// <summary>One line of the command history (also used for the inline status next to an Apply button).</summary>
public sealed partial class CommandEntry : ObservableObject
{
    public CommandEntry(string title, string deviceName, string? mac)
    {
        Title = title;
        DeviceName = deviceName;
        Mac = mac;
        Time = DateTime.Now;
    }

    public DateTime Time { get; }
    public string Title { get; }
    public string DeviceName { get; }
    public string? Mac { get; }
    public CommandHandle? Handle { get; internal set; }

    [ObservableProperty] private EntryStatus _status = EntryStatus.Pending;
    [ObservableProperty] private string _detail = "sending...";
    [ObservableProperty] private int _attempts;
    [ObservableProperty] private int _ms;

    public bool IsFinished => Status != EntryStatus.Pending;
    public string TimeText => Time.ToString("HH:mm:ss");

    partial void OnStatusChanged(EntryStatus value) => OnPropertyChanged(nameof(IsFinished));
}

/// <summary>
/// Sends commands through the hub, tracks them in a history and turns their outcome into toasts / status bar text
/// ("Fog3 did not answer - it may be powered off while heating."). UI thread only.
/// </summary>
public sealed class CommandCenter
{
    private readonly HubClient _hub;
    private readonly IUiDispatcher _ui;
    private readonly ToastService _toasts;
    private readonly Func<string, string?> _nameOfMac;
    private readonly Func<int, string?> _nameOfIndex;

    public CommandCenter(HubClient hub, IUiDispatcher ui, ToastService toasts, Func<string, string?> nameOfMac, Func<int, string?> nameOfIndex)
    {
        _hub = hub;
        _ui = ui;
        _toasts = toasts;
        _nameOfMac = nameOfMac;
        _nameOfIndex = nameOfIndex;
    }

    public ObservableCollection<CommandEntry> History { get; } = new();

    public int MaxHistory { get; set; } = 300;

    /// <summary>Name shown for a result: nickname by MAC, else by index, else the MAC.</summary>
    public string NameOf(CmdResultMessage r) =>
        (r.Mac.Length > 0 ? _nameOfMac(r.Mac) : null) ?? _nameOfIndex(r.Dev) ?? (r.Mac.Length > 0 ? r.Mac : $"timer {r.Dev}");

    /// <summary>
    /// Starts a command. The returned entry updates itself (pending, confirmed, failed ...).
    /// <paramref name="quiet"/> suppresses the toast for failures (used for repeated refreshes that report themselves).
    /// </summary>
    public CommandEntry Run(CommandSpec spec, string title, string deviceName = "", string? mac = null, bool quiet = false, bool toastOnSuccess = false, bool record = true)
    {
        var entry = new CommandEntry(title, deviceName, mac);
        if (record) AddToHistory(entry);

        CommandHandle handle;
        try
        {
            handle = _hub.BeginCommand(spec);
        }
        catch (Exception ex)
        {
            entry.Status = EntryStatus.Failed;
            entry.Detail = ex.Message;
            return entry;
        }
        entry.Handle = handle;
        bool finalHandled = false;

        void Apply(CommandHandle h)
        {
            var rep = CommandReport.Describe(h, NameOf);
            entry.Status = rep.Status;
            entry.Detail = rep.Detail;
            entry.Attempts = rep.Attempts;
            entry.Ms = rep.Ms;
            if (!h.IsFinished && h.State != CommandState.TimedOut) return;
            if (finalHandled && h.State != CommandState.Completed) return;
            bool first = !finalHandled;
            finalHandled = true;
            if (!first && rep.Status == EntryStatus.Ok) return;

            string who = string.IsNullOrEmpty(deviceName) ? "" : deviceName + ": ";
            if (rep.Status is EntryStatus.Failed or EntryStatus.Warning)
            {
                if (!quiet) _toasts.Show(rep.Status == EntryStatus.Failed ? ToastKind.Error : ToastKind.Warning, rep.Problem ?? $"{who}{title}: {rep.Detail}");
                else _toasts.Status(rep.Problem ?? $"{who}{title}: {rep.Detail}", ToastKind.Warning);
            }
            else if (toastOnSuccess)
            {
                _toasts.Show(ToastKind.Success, $"{who}{title}: {rep.Detail}");
            }
            else
            {
                _toasts.Status($"{who}{title}: {rep.Detail}", ToastKind.Success);
            }
        }

        handle.Changed += h => _ui.Post(() => Apply(h));
        _ui.Post(() => Apply(handle));
        return entry;
    }

    /// <summary>
    /// Runs several commands (or one with dev:"all") and reports them as one operation:
    /// "7/8 confirmed, Fog3 timed out".
    /// </summary>
    public async Task<(EntryStatus Status, string Summary)> RunBulkAsync(IReadOnlyList<CommandSpec> specs, string title, bool toastOnSuccess = true)
    {
        var entry = new CommandEntry(title, "all timers", null);
        AddToHistory(entry);
        var handles = specs.Select(s => _hub.BeginCommand(s)).ToList();
        entry.Handle = handles.Count == 1 ? handles[0] : null;
        foreach (var h in handles)
            h.Changed += _ => _ui.Post(() => entry.Detail = "waiting for confirmation...");

        try
        {
            await Task.WhenAll(handles.Select(h => h.Completion)).ConfigureAwait(true);
        }
        catch
        {
            // Completion never faults
        }

        var (status, summary) = CommandReport.Aggregate(handles, NameOf);
        entry.Status = status;
        entry.Detail = summary;
        var kind = status switch { EntryStatus.Ok => ToastKind.Success, EntryStatus.Warning => ToastKind.Warning, _ => ToastKind.Error };
        if (status != EntryStatus.Ok || toastOnSuccess) _toasts.Show(kind, $"{title}: {summary}");
        else _toasts.Status($"{title}: {summary}", kind);
        return (status, summary);
    }

    private void AddToHistory(CommandEntry entry)
    {
        History.Insert(0, entry);
        while (History.Count > MaxHistory) History.RemoveAt(History.Count - 1);
    }

    public IEnumerable<CommandEntry> ForDevice(string mac) =>
        History.Where(e => string.Equals(e.Mac, mac, StringComparison.OrdinalIgnoreCase));
}
