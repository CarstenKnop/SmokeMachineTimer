using System.Diagnostics;
using FogConsole.Hub;
using FogConsole.Protocol;

namespace FogConsole.Ota;

/// <summary>One timer to update: its MAC (how the fleet list knows it), its name and how the frames are addressed.</summary>
public sealed record OtaTimerTarget(string Mac, string Name, CommandTarget Target);

public enum OtaOutcome { Updated, Failed, Cancelled, NeedsUsbUpdate, NoOtaFirmware, Busy, Skipped }

public sealed record OtaTimerResult(OtaTimerTarget Timer, OtaOutcome Outcome, string Message, string? OldVersion, string? NewVersion,
    TimeSpan Duration, int Resent, int Restarts)
{
    public bool Succeeded => Outcome == OtaOutcome.Updated;
}

public sealed record OtaTimerProgress(OtaTimerTarget Timer, OtaPhase? Phase, string Stage, double Fraction, double SpeedBytesPerSecond,
    double? EtaSeconds, int Resent, int Restarts);

public sealed class OtaUpdaterOptions
{
    public OtaTransferOptions Transfer { get; init; } = new();

    /// <summary>The remote forgets the hold after 15 s; it is renewed this often while anything runs.</summary>
    public int HoldRefreshMs { get; init; } = 5000;

    /// <summary>Loop period of the sender (the radio side is paced by the link, this only drives the timers).</summary>
    public int TickMs { get; init; } = 10;
}

/// <summary>
/// Updates the chosen timers one after another over the air (through the remote, or over a timer's own USB cable):
/// probes each timer, runs an <see cref="OtaTransfer"/> and reports per-timer results. Keeps the remote's radio free with
/// <c>otaHold</c> while it works. Everything that is not a timer's own firmware stays untouched.
/// </summary>
public sealed class OtaUpdater : IDisposable
{
    private readonly IOtaLink _link;
    private readonly IOtaFleet _fleet;
    private readonly byte[] _key;
    private readonly OtaUpdaterOptions _o;
    private readonly Stopwatch _clock = Stopwatch.StartNew();
    private readonly object _gate = new();
    private readonly SemaphoreSlim _holdLock = new(1, 1);
    private Action<OtaRxMessage>? _rx;
    private int _holders;
    private CancellationTokenSource? _holdCts;
    private Task? _holdTask;

    public OtaUpdater(IOtaLink link, IOtaFleet fleet, string? key = null, OtaUpdaterOptions? options = null)
    {
        _link = link;
        _fleet = fleet;
        _key = OtaAuth.KeyBytes(key);
        _o = options ?? new OtaUpdaterOptions();
        _link.PayloadReceived += OnPayload;
    }

    public event Action<OtaTimerProgress>? ProgressChanged;
    public event Action<OtaTimerResult>? TimerFinished;
    public event Action<OtaTimerTarget, OtaSupport>? SupportChecked;
    public event Action<string>? Log;

    private long NowMs => _clock.ElapsedMilliseconds;

    private void OnPayload(OtaRxMessage m)
    {
        lock (_gate) _rx?.Invoke(m);
    }

    private static bool Matches(OtaRxMessage m, OtaTimerTarget t)
    {
        if (!string.IsNullOrEmpty(m.Mac) && !string.IsNullOrEmpty(t.Mac)) return string.Equals(m.Mac, t.Mac, StringComparison.OrdinalIgnoreCase);
        return t.Target.Index is { } i && m.Dev == i;
    }

    // ---- the hold on the remote's radio -------------------------------------------------------------------------------

    private async Task AcquireHoldAsync()
    {
        await _holdLock.WaitAsync().ConfigureAwait(false);
        try
        {
            if (_holders++ > 0) return;
            if (!await _link.HoldAsync(true).ConfigureAwait(false)) Log?.Invoke("The remote did not accept the radio hold (older firmware?); going on without it.");
            _holdCts = new CancellationTokenSource();
            var token = _holdCts.Token;
            _holdTask = Task.Run(async () =>
            {
                try
                {
                    while (!token.IsCancellationRequested)
                    {
                        await Task.Delay(_o.HoldRefreshMs, token).ConfigureAwait(false);
                        await _link.HoldAsync(true, token).ConfigureAwait(false);
                    }
                }
                catch (OperationCanceledException)
                {
                    // released
                }
            }, CancellationToken.None);
        }
        finally
        {
            _holdLock.Release();
        }
    }

    private async Task ReleaseHoldAsync()
    {
        await _holdLock.WaitAsync().ConfigureAwait(false);
        try
        {
            if (--_holders > 0) return;
            var cts = _holdCts;
            _holdCts = null;
            try { cts?.Cancel(); } catch { /* ignore */ }
            if (_holdTask is not null) { try { await _holdTask.ConfigureAwait(false); } catch { /* ignore */ } }
            _holdTask = null;
            cts?.Dispose();
            await _link.HoldAsync(false).ConfigureAwait(false);
        }
        finally
        {
            _holdLock.Release();
        }
    }

    // ---- probing ------------------------------------------------------------------------------------------------------

    /// <summary>Asks each timer whether it can be updated over the air. Raises <see cref="SupportChecked"/> for every one.</summary>
    public async Task<IReadOnlyDictionary<string, OtaSupport>> ProbeAsync(IReadOnlyList<OtaTimerTarget> timers, CancellationToken ct)
    {
        var result = new Dictionary<string, OtaSupport>(StringComparer.OrdinalIgnoreCase);
        await AcquireHoldAsync().ConfigureAwait(false);
        try
        {
            foreach (var t in timers)
            {
                ct.ThrowIfCancellationRequested();
                SupportChecked?.Invoke(t, OtaSupport.Probing);
                var fleet = _fleet.Find(t.Mac);
                OtaSupport s = fleet is { Online: false } ? OtaSupport.Unknown : await ProbeOneAsync(t, ct).ConfigureAwait(false);
                result[t.Mac] = s;
                SupportChecked?.Invoke(t, s);
            }
        }
        finally
        {
            await ReleaseHoldAsync().ConfigureAwait(false);
        }
        return result;
    }

    private async Task<OtaSupport> ProbeOneAsync(OtaTimerTarget t, CancellationToken ct)
    {
        var probe = new OtaProbe(p => _link.TrySend(t.Target, p));
        lock (_gate) _rx = m => { if (Matches(m, t)) probe.OnPayload(m.Payload, NowMs); };
        try
        {
            lock (_gate) probe.Start(NowMs);
            while (true)
            {
                lock (_gate)
                {
                    probe.Tick(NowMs);
                    if (probe.IsFinished) return probe.Result;
                }
                await Task.Delay(_o.TickMs, ct).ConfigureAwait(false);
            }
        }
        finally
        {
            lock (_gate) _rx = null;
        }
    }

    // ---- updating -----------------------------------------------------------------------------------------------------

    /// <summary>Updates the timers in the given order. Never throws for a failing timer; a cancel ends the run (the remaining timers are reported as cancelled).</summary>
    public async Task<IReadOnlyList<OtaTimerResult>> UpdateAsync(IReadOnlyList<OtaTimerTarget> timers, OtaImage image, CancellationToken ct)
    {
        var results = new List<OtaTimerResult>();
        await AcquireHoldAsync().ConfigureAwait(false);
        try
        {
            foreach (var t in timers)
            {
                OtaTimerResult r;
                if (ct.IsCancellationRequested)
                    r = new OtaTimerResult(t, OtaOutcome.Cancelled, "Cancelled before it started.", _fleet.Find(t.Mac)?.Fw, null, TimeSpan.Zero, 0, 0);
                else
                {
                    try { r = await RunOneAsync(t, image, ct).ConfigureAwait(false); }
                    catch (OperationCanceledException)
                    {
                        r = new OtaTimerResult(t, OtaOutcome.Cancelled, "Cancelled.", _fleet.Find(t.Mac)?.Fw, null, TimeSpan.Zero, 0, 0);
                    }
                }
                results.Add(r);
                TimerFinished?.Invoke(r);
            }
        }
        finally
        {
            await ReleaseHoldAsync().ConfigureAwait(false);
        }
        return results;
    }

    private void Raise(OtaTimerTarget t, OtaProgress p) =>
        ProgressChanged?.Invoke(new OtaTimerProgress(t, p.Phase, p.Stage, p.Fraction, p.SpeedBytesPerSecond, p.EtaSeconds, p.Resent, p.Restarts));

    private void Raise(OtaTimerTarget t, string stage, double fraction = 0) =>
        ProgressChanged?.Invoke(new OtaTimerProgress(t, null, stage, fraction, 0, null, 0, 0));

    private async Task<OtaTimerResult> RunOneAsync(OtaTimerTarget t, OtaImage image, CancellationToken ct)
    {
        var started = Stopwatch.StartNew();
        string? oldFw = _fleet.Find(t.Mac)?.Fw;                       // the version before this update
        OtaTimerResult Result(OtaOutcome outcome, string message, string? now = null, int resent = 0, int restarts = 0) =>
            new(t, outcome, message, oldFw, now, started.Elapsed, resent, restarts);

        var info = _fleet.Find(t.Mac);
        if (info is null) return Result(OtaOutcome.Skipped, "The timer is not in the remote's fleet list.");
        if (!info.Online)
            return Result(OtaOutcome.Skipped, "The timer is offline (a machine that is heating up has no power). Try again when it is back.");
        Log?.Invoke($"{t.Name}: firmware {(string.IsNullOrEmpty(oldFw) ? "?" : oldFw)}, updating to {image.Version ?? "the chosen image"}.");

        // 1. can it do it?
        Raise(t, "Checking over-the-air support...");
        var support = await ProbeOneAsync(t, ct).ConfigureAwait(false);
        SupportChecked?.Invoke(t, support);
        switch (support)
        {
            case OtaSupport.NeedsUsbUpdate:
                return Result(OtaOutcome.NeedsUsbUpdate, OtaText.Error(OtaError.NoSlot));
            case OtaSupport.NoOtaFirmware:
                return Result(OtaOutcome.NoOtaFirmware,
                    "The timer did not answer the over-the-air probe: its firmware has no OTA support (or it is out of range). Update it once over its USB cable.");
            case OtaSupport.Busy:
                return Result(OtaOutcome.Busy, OtaText.Error(OtaError.Busy));
        }

        // 2. transfer
        var tr = new OtaTransfer(image, _key, p => _link.TrySend(t.Target, p), _o.Transfer, log: s => Log?.Invoke($"{t.Name}: {s}"));
        tr.Changed += p => Raise(t, p);
        lock (_gate)
        {
            _rx = m => { if (Matches(m, t)) tr.OnPayload(m.Payload, NowMs); };
            long now = NowMs;
            tr.OnTimerBoot(info.Boot, info.Fw, now);
            tr.OnTimerOnline(info.Online, now);
            tr.Start(now);
        }
        try
        {
            while (true)
            {
                if (ct.IsCancellationRequested)
                {
                    lock (_gate) tr.Cancel(NowMs);
                    await Task.Delay(120, CancellationToken.None).ConfigureAwait(false);
                    _link.TrySend(t.Target, OtaFrames.Abort(tr.Session));          // once more, in case the first was lost
                    return Result(OtaOutcome.Cancelled, "Cancelled. The timer keeps its old firmware.", resent: tr.Resent, restarts: tr.Restarts);
                }
                bool gone = false;
                lock (_gate)
                {
                    long now = NowMs;
                    var fi = _fleet.Find(t.Mac);
                    if (fi is null) gone = true;
                    else
                    {
                        tr.OnTimerOnline(fi.Online, now);
                        tr.OnTimerBoot(fi.Boot, fi.Fw, now);
                    }
                    if (!gone) tr.Tick(now);
                    if (tr.IsFinished) Raise(t, tr.Snapshot(now));
                }
                if (gone)
                {
                    lock (_gate) tr.Cancel(NowMs);
                    return Result(OtaOutcome.Failed, "The timer is no longer in the fleet list.", resent: tr.Resent, restarts: tr.Restarts);
                }
                if (tr.IsFinished) break;
                await Task.Delay(_o.TickMs, CancellationToken.None).ConfigureAwait(false);      // a cancel is handled at the top of the loop (it sends the Abort)
            }
        }
        finally
        {
            lock (_gate) _rx = null;
        }

        string? newFw = _fleet.Find(t.Mac)?.Fw;
        string extra = tr.Resent > 0 || tr.Restarts > 0
            ? $" ({tr.Resent} chunk{(tr.Resent == 1 ? "" : "s")} re-sent{(tr.Restarts > 0 ? $", started again {tr.Restarts}x" : "")})"
            : "";
        if (tr.Succeeded)
            return Result(OtaOutcome.Updated, $"Updated {(string.IsNullOrEmpty(oldFw) ? "" : oldFw + " → ")}{newFw} in {started.Elapsed.TotalSeconds:0} s{extra}.", newFw, tr.Resent, tr.Restarts);
        if (tr.Phase == OtaPhase.Cancelled)
            return Result(OtaOutcome.Cancelled, "Cancelled.", resent: tr.Resent, restarts: tr.Restarts);
        if (tr.TimerError == OtaError.NoSlot)
            return Result(OtaOutcome.NeedsUsbUpdate, OtaText.Error(OtaError.NoSlot), resent: tr.Resent, restarts: tr.Restarts);
        return Result(OtaOutcome.Failed, (tr.FailureMessage ?? "The transfer failed.") + " The timer keeps its old firmware.", resent: tr.Resent, restarts: tr.Restarts);
    }

    public void Dispose()
    {
        _link.PayloadReceived -= OnPayload;
        try { _holdCts?.Cancel(); } catch { /* ignore */ }
    }
}
