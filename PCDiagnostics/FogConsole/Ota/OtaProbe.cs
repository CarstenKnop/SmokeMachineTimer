namespace FogConsole.Ota;

/// <summary>What the capability probe found out about a timer.</summary>
public enum OtaSupport
{
    Unknown,
    Probing,

    /// <summary>The timer answered and is ready for an over-the-air update.</summary>
    Capable,

    /// <summary>The timer has OTA code but no second application slot: it needs one update over USB (new partition layout).</summary>
    NeedsUsbUpdate,

    /// <summary>The timer did not answer: its firmware has no OTA support (or it is offline / out of range).</summary>
    NoOtaFirmware,

    /// <summary>The timer is in the middle of a transfer (another session).</summary>
    Busy,
}

/// <summary>
/// The capability probe: <c>Poll</c> with session 0 until the timer answers with a State. No answer after a few tries means
/// firmware without OTA support (docs/OTA_UPDATE.md, section 2). Same I/O-free style as <see cref="OtaTransfer"/>.
/// </summary>
public sealed class OtaProbe
{
    private readonly Func<byte[], bool> _send;
    private readonly int _retryMs;
    private readonly int _attempts;
    private int _sent;
    private long _lastSend;
    private long _started;

    public OtaProbe(Func<byte[], bool> send, int retryMs = 400, int attempts = 6)
    {
        _send = send;
        _retryMs = retryMs;
        _attempts = attempts;
    }

    public OtaSupport Result { get; private set; } = OtaSupport.Unknown;
    public bool IsFinished => Result is not (OtaSupport.Unknown or OtaSupport.Probing);
    public OtaStateReport? Report { get; private set; }

    public void Start(long now)
    {
        Result = OtaSupport.Probing;
        _started = now;
        _lastSend = long.MinValue / 2;
        _sent = 0;
        Tick(now);
    }

    public void Tick(long now)
    {
        if (Result != OtaSupport.Probing) return;
        if (_sent >= _attempts)
        {
            // everything was sent; allow the last answer one more interval
            if (now - _lastSend >= _retryMs) Result = OtaSupport.NoOtaFirmware;
            return;
        }
        if (now - _lastSend >= _retryMs && _send(OtaFrames.Poll(0)))
        {
            _lastSend = now;
            _sent++;
        }
    }

    public void OnPayload(ReadOnlySpan<byte> payload, long now)
    {
        if (Result != OtaSupport.Probing || !OtaStateReport.TryParse(payload, out var r)) return;
        Report = r;
        Result = r.Error == OtaError.NoSlot ? OtaSupport.NeedsUsbUpdate
               : r.State != OtaState.Idle || r.Error == OtaError.Busy ? OtaSupport.Busy
               : OtaSupport.Capable;
    }

    public long ElapsedMs(long now) => now - _started;
}
