using System.Collections.Concurrent;
using System.Threading.Channels;
using FogConsole.Hub;
using FogConsole.Protocol;

namespace FogConsole.Ota;

/// <summary>How OTA frames get to a timer and back (through the remote, or straight over a timer's own USB cable).</summary>
public interface IOtaLink
{
    /// <summary>Queues one OTA frame payload for the timer. False when the queue is full (offer it again shortly).</summary>
    bool TrySend(CommandTarget target, byte[] payload);

    /// <summary>An OTA frame arrived from a timer. Raised on a background thread.</summary>
    event Action<OtaRxMessage>? PayloadReceived;

    /// <summary>Asks the remote to keep the radio free (see <c>otaHold</c>). Returns false when it did not accept.</summary>
    Task<bool> HoldAsync(bool on, CancellationToken ct = default);
}

/// <summary>What the PC knows about the timers' state (boot counter, firmware version, online).</summary>
public interface IOtaFleet
{
    FleetDevice? Find(string mac);
}

/// <summary>
/// <see cref="IOtaLink"/> over the hub: every frame is an <c>otaTx</c> request, up to <see cref="MaxOutstanding"/> at a time to
/// hide the USB round trip; the response (after the radio finished) tells whether the timer's MAC acknowledged. A <c>busy</c>
/// answer (the remote's transmit queue is full) is retried after a short pause.
/// </summary>
public sealed class HubOtaLink : IOtaLink, IDisposable
{
    public const int MaxOutstanding = 6;

    /// <summary>
    /// Frames waiting for a free request slot. Kept small on purpose: the sender starts its re-send timer when it hands a frame over, so
    /// a long queue would make it re-send chunks that are merely waiting their turn.
    /// </summary>
    public const int QueueLimit = 4;

    private readonly HubClient _hub;
    private readonly Channel<(CommandTarget Target, byte[] Payload)> _queue =
        Channel.CreateBounded<(CommandTarget, byte[])>(new BoundedChannelOptions(QueueLimit) { FullMode = BoundedChannelFullMode.Wait, SingleReader = true });
    private readonly SemaphoreSlim _slots = new(MaxOutstanding, MaxOutstanding);
    private readonly CancellationTokenSource _cts = new();
    private readonly Task _worker;
    private long _sent, _acked, _notAcked, _busy, _timeouts, _dropped;
    private int _pending;
    private volatile bool _faulted;

    public HubOtaLink(HubClient hub)
    {
        _hub = hub;
        _hub.MessageReceived += OnMessage;
        _worker = Task.Run(() => WorkerAsync(_cts.Token));
    }

    public event Action<OtaRxMessage>? PayloadReceived;

    /// <summary>The connection broke or the remote refused to forward (reason in the argument).</summary>
    public event Action<string>? Faulted;

    public long FramesSent => Interlocked.Read(ref _sent);
    public long MacAcked => Interlocked.Read(ref _acked);
    public long MacNotAcked => Interlocked.Read(ref _notAcked);
    public long BusyRetries => Interlocked.Read(ref _busy);
    public long Timeouts => Interlocked.Read(ref _timeouts);
    public long Dropped => Interlocked.Read(ref _dropped);

    public bool TrySend(CommandTarget target, byte[] payload)
    {
        if (_faulted) return false;
        if (payload.Length is 0 or > Requests.OtaMaxPayload) return false;
        Interlocked.Increment(ref _pending);
        if (_queue.Writer.TryWrite((target, payload))) return true;
        Interlocked.Decrement(ref _pending);
        return false;
    }

    private void OnMessage(Message m)
    {
        if (m is OtaRxMessage rx)
        {
            try { PayloadReceived?.Invoke(rx); }
            catch { /* a subscriber bug must not kill the hub's reader thread */ }
        }
    }

    private async Task WorkerAsync(CancellationToken ct)
    {
        try
        {
            await foreach (var (target, payload) in _queue.Reader.ReadAllAsync(ct).ConfigureAwait(false))
            {
                await _slots.WaitAsync(ct).ConfigureAwait(false);
                _ = SendOneAsync(target, payload, ct);
            }
        }
        catch (OperationCanceledException)
        {
            // closing
        }
    }

    private async Task SendOneAsync(CommandTarget target, byte[] payload, CancellationToken ct)
    {
        try
        {
            for (int attempt = 0; attempt < 8; attempt++)
            {
                var res = await _hub.RequestAsync(Requests.OtaTx(target, payload), TimeSpan.FromSeconds(1.5), 0, ct).ConfigureAwait(false);
                if (res.Ok)
                {
                    Interlocked.Increment(ref _sent);
                    if (res.Acked == false) Interlocked.Increment(ref _notAcked); else Interlocked.Increment(ref _acked);
                    return;
                }
                if (res.Err == "busy")
                {
                    Interlocked.Increment(ref _busy);
                    await Task.Delay(15, ct).ConfigureAwait(false);
                    continue;
                }
                // noDevice / badArg / unknownType: the remote cannot forward this
                _faulted = true;
                Faulted?.Invoke(res.Describe());
                return;
            }
            Interlocked.Increment(ref _dropped);
        }
        catch (HubTimeoutException)
        {
            Interlocked.Increment(ref _timeouts);     // the transfer's own timers send it again
        }
        catch (HubDisconnectedException ex)
        {
            _faulted = true;
            Faulted?.Invoke(ex.Message);
        }
        catch (OperationCanceledException)
        {
            // closing
        }
        finally
        {
            Interlocked.Decrement(ref _pending);
            _slots.Release();
        }
    }

    /// <summary>Waits (briefly) until every frame handed over so far has been answered, so that a release cannot overtake a late frame.</summary>
    private async Task DrainAsync(CancellationToken ct)
    {
        long until = Environment.TickCount64 + 2500;
        while (Volatile.Read(ref _pending) > 0 && !_faulted && Environment.TickCount64 < until)
            await Task.Delay(5, ct).ConfigureAwait(false);
    }

    public async Task<bool> HoldAsync(bool on, CancellationToken ct = default)
    {
        try
        {
            if (!on) await DrainAsync(ct).ConfigureAwait(false);       // the remote renews its hold with every frame it forwards
            var res = await _hub.RequestAsync(Requests.OtaHold(on), TimeSpan.FromSeconds(1.5), 1, ct).ConfigureAwait(false);
            return res.Ok;
        }
        catch (HubException)
        {
            return false;
        }
    }

    public void Dispose()
    {
        _hub.MessageReceived -= OnMessage;
        try { _cts.Cancel(); } catch { /* ignore */ }
        _queue.Writer.TryComplete();
        try { _worker.Wait(500); } catch { /* ignore */ }
        _cts.Dispose();
    }
}

/// <summary>Keeps the latest fleet list from the hub (boot counters, versions, online flags) for the update logic, which runs off the UI thread.</summary>
public sealed class HubFleetWatcher : IOtaFleet, IDisposable
{
    private readonly HubClient _hub;
    private volatile Dictionary<string, FleetDevice> _map = new(StringComparer.OrdinalIgnoreCase);
    private long _updates;

    public HubFleetWatcher(HubClient hub)
    {
        _hub = hub;
        _hub.MessageReceived += OnMessage;
    }

    /// <summary>Number of fleet events seen (0 = no picture of the fleet yet).</summary>
    public long Updates => Interlocked.Read(ref _updates);

    public void Seed(IEnumerable<FleetDevice> devices)
    {
        var map = new Dictionary<string, FleetDevice>(StringComparer.OrdinalIgnoreCase);
        foreach (var d in devices) map[d.Mac] = d;
        _map = map;
    }

    private void OnMessage(Message m)
    {
        if (m is not FleetMessage f) return;
        var map = new Dictionary<string, FleetDevice>(StringComparer.OrdinalIgnoreCase);
        foreach (var d in f.Devs) map[d.Mac] = d;
        _map = map;
        Interlocked.Increment(ref _updates);
    }

    public FleetDevice? Find(string mac) => _map.TryGetValue(mac, out var d) ? d : null;

    /// <summary>Asks the device for a fleet event and waits (briefly) until it arrives.</summary>
    public async Task EnsureAsync(CancellationToken ct = default)
    {
        long before = Updates;
        try { await _hub.RequestAsync(Requests.GetFleet(), TimeSpan.FromSeconds(1.5), 1, ct).ConfigureAwait(false); }
        catch (HubException) { return; }
        for (int i = 0; i < 40 && Updates == before; i++) await Task.Delay(25, ct).ConfigureAwait(false);
    }

    public void Dispose() => _hub.MessageReceived -= OnMessage;
}
