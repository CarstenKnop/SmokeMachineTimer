using FogConsole.Hub;
using FogConsole.Protocol;

namespace FogConsole.Services;

/// <summary>
/// "Hold to fog": the output is on only while a button is physically held. The first <c>mode forceOn</c> (lease 1000 ms) is
/// sent with confirmation, refreshes go out every 300 ms as fire-and-forget (<c>confirm:false</c>), and the release sends a
/// confirmed <c>mode auto</c> (or <c>paused</c> if the timer was paused before), retried when it is not confirmed.
/// The dead-man lease in the timer is the second line of defence: even if the PC dies the output turns off within ~1 s.
/// </summary>
public sealed class HoldController : IDisposable
{
    public const int LeaseMs = 1000;
    public static readonly TimeSpan RefreshInterval = TimeSpan.FromMilliseconds(300);
    private const int ReleaseRetries = 3;

    private readonly object _lock = new();
    private readonly Func<CommandSpec, CommandHandle> _send;
    private readonly TimeProvider _time;
    private readonly Action<string>? _onProblem;
    private readonly Func<string, string> _nameOf;
    private readonly Dictionary<string, HeldTimer> _held = new(StringComparer.OrdinalIgnoreCase);
    private ITimer? _refreshTimer;
    private bool _disposed;

    private sealed class HeldTimer
    {
        public required TimerMode PriorMode;
        public int Refreshes;
        public DateTimeOffset Since;
    }

    public HoldController(Func<CommandSpec, CommandHandle> send, TimeProvider? time = null, Action<string>? onProblem = null, Func<string, string>? nameOf = null)
    {
        _send = send;
        _time = time ?? TimeProvider.System;
        _onProblem = onProblem;
        _nameOf = nameOf ?? (mac => mac);
    }

    public int HeldCount
    {
        get { lock (_lock) return _held.Count; }
    }

    public bool IsHolding(string mac)
    {
        lock (_lock) return _held.ContainsKey(mac);
    }

    /// <summary>Starts fogging. <paramref name="priorMode"/> is restored on release when it was Paused, otherwise Auto.</summary>
    public bool Begin(string mac, TimerMode priorMode)
    {
        lock (_lock)
        {
            if (_disposed || _held.ContainsKey(mac)) return false;
            _held[mac] = new HeldTimer { PriorMode = priorMode == TimerMode.Paused ? TimerMode.Paused : TimerMode.Auto, Since = _time.GetUtcNow() };
            _refreshTimer ??= _time.CreateTimer(_ => Refresh(), null, RefreshInterval, RefreshInterval);
        }
        // first forceOn: with confirmation so a failure is visible
        var handle = _send(CommandSpec.Mode(CommandTarget.ByMac(mac), TimerMode.ForceOn, LeaseMs));
        bool reported = false;

        void Check(CommandHandle h)
        {
            if (reported || !h.IsFinished) return;
            if (h.State is CommandState.Rejected or CommandState.Failed or CommandState.TimedOut || h.Results.Any(r => r.Result != CmdOutcome.Ok))
            {
                reported = true;
                _onProblem?.Invoke(CommandReport.Describe(h, r => _nameOf(r.Mac)).Problem ?? "Hold to fog was not confirmed.");
            }
        }

        handle.Changed += Check;
        Check(handle);
        return true;
    }

    private void Refresh()
    {
        string[] macs;
        lock (_lock)
        {
            if (_disposed) return;
            macs = _held.Keys.ToArray();
            foreach (var m in macs) _held[m].Refreshes++;
        }
        foreach (var mac in macs)
        {
            try { _send(CommandSpec.Mode(CommandTarget.ByMac(mac), TimerMode.ForceOn, LeaseMs).WithoutConfirm()); }
            catch { /* disconnected: the lease will expire by itself */ }
        }
    }

    /// <summary>Stops fogging on one timer and makes sure its mode goes back.</summary>
    public void End(string mac)
    {
        TimerMode restore;
        lock (_lock)
        {
            if (!_held.Remove(mac, out var held)) return;
            restore = held.PriorMode;
            if (_held.Count == 0)
            {
                _refreshTimer?.Dispose();
                _refreshTimer = null;
            }
        }
        SendRelease(mac, restore, attempt: 1);
    }

    private void SendRelease(string mac, TimerMode mode, int attempt)
    {
        CommandHandle handle;
        try
        {
            handle = _send(CommandSpec.Mode(CommandTarget.ByMac(mac), mode));
        }
        catch (Exception ex)
        {
            _onProblem?.Invoke($"Could not release the fog button ({ex.Message}). The timer switches off by itself within about a second.");
            return;
        }

        bool acted = false;

        void Check(CommandHandle h)
        {
            if (!h.IsFinished || acted) return;
            bool ok = h.State == CommandState.Completed && h.Results.Count > 0 && h.Results.All(r => r.Result == CmdOutcome.Ok);
            if (ok) { acted = true; return; }
            acted = true;
            if (attempt < ReleaseRetries && !_disposed)
            {
                _time.CreateTimer(_ => SendRelease(mac, mode, attempt + 1), null, TimeSpan.FromMilliseconds(250), Timeout.InfiniteTimeSpan);
            }
            else
            {
                _onProblem?.Invoke("The release of the fog button was not confirmed. The output turns off by itself within about a second (dead-man lease).");
            }
        }

        handle.Changed += Check;
        Check(handle);
    }

    /// <summary>Releases every held timer (window deactivated, connection lost, app closing).</summary>
    public void EndAll()
    {
        string[] macs;
        lock (_lock) macs = _held.Keys.ToArray();
        foreach (var m in macs) End(m);
    }

    public void Dispose()
    {
        EndAll();
        lock (_lock)
        {
            _disposed = true;
            _refreshTimer?.Dispose();
            _refreshTimer = null;
        }
    }
}
