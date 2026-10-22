namespace FogConsole.Ota;

public enum OtaPhase
{
    NotStarted,
    Beginning,
    Erasing,
    Streaming,
    Verifying,
    Activating,
    Restarting,
    Done,
    Failed,
    Cancelled,
}

/// <summary>Timings and sizes of the sender (docs/OTA_UPDATE.md, 4.3). All times in milliseconds.</summary>
public sealed class OtaTransferOptions
{
    public int Window { get; init; } = 24;
    public int ResendAfterMs { get; init; } = 150;
    public int PollIdleMs { get; init; } = 400;
    public int BeginRetryMs { get; init; } = 400;
    public int BeginTimeoutMs { get; init; } = 10_000;
    public int ErasePollMs { get; init; } = 500;
    public int EraseTimeoutMs { get; init; } = 60_000;
    public int VerifyRetryMs { get; init; } = 500;
    public int VerifyTimeoutMs { get; init; } = 30_000;
    public int ActivateRetryMs { get; init; } = 500;
    public int ActivateTimeoutMs { get; init; } = 8_000;
    public int RestartTimeoutMs { get; init; } = 60_000;

    /// <summary>No State from an online timer for this long during the transfer: give up.</summary>
    public int StallTimeoutMs { get; init; } = 30_000;

    /// <summary>The timer is known to be offline (power cut) for this long: give up.</summary>
    public int OfflineGiveUpMs { get; init; } = 120_000;

    public int MaxRestarts { get; init; } = 3;

    /// <summary>
    /// A restart of the timer is noticed twice: by its state report (idle, no session) and, a moment later, by the boot counter in the fleet list.
    /// The second sign within this time after the transfer was started again is the same restart and is ignored.
    /// </summary>
    public int RestartDedupMs { get; init; } = 15_000;

    // ---- the estimate of the time left (the speed shown in the UI uses a short window; the estimate needs a steadier one) ----

    /// <summary>The first seconds of the transfer are not used for the estimate: the pipe is still filling and looks far slower than it is.</summary>
    public int EtaRampMs { get; init; } = 3_000;

    /// <summary>The estimate uses the progress of the last this-many milliseconds (reports from the timer arrive in steps, a short window jumps).</summary>
    public int EtaWindowMs { get; init; } = 30_000;

    /// <summary>No estimate until the window spans at least this long.</summary>
    public int EtaMinSpanMs { get; init; } = 3_000;

    /// <summary>What follows the last chunk: the timer checks the image, switches over and restarts (measured on a real timer: 4 to 8 s). Added to the estimate.</summary>
    public int TailSeconds { get; init; } = 6;
}

/// <summary>A snapshot of the transfer for the UI.</summary>
public sealed record OtaProgress(
    OtaPhase Phase, string Stage, double Fraction, long BytesDone, long BytesTotal, double SpeedBytesPerSecond, double? EtaSeconds,
    int Resent, int Restarts, int FramesSent, string? Message)
{
    public bool IsFinished => Phase is OtaPhase.Done or OtaPhase.Failed or OtaPhase.Cancelled;
}

/// <summary>
/// The sender for ONE timer: a state machine without any I/O of its own. The caller feeds it the timer's frames
/// (<see cref="OnPayload"/>), what the fleet list says about the timer (<see cref="OnTimerBoot"/>, <see cref="OnTimerOnline"/>)
/// and the time (<see cref="Tick"/>); it hands OTA frame payloads to the <c>send</c> delegate (which returns false when the link
/// is full - the frame is then offered again on a later tick). Not thread-safe: serialise the calls.
/// </summary>
public sealed class OtaTransfer
{
    private readonly OtaImage _image;
    private readonly byte[] _key;
    private readonly OtaTransferOptions _o;
    private readonly Func<byte[], bool> _send;
    private readonly Func<byte> _newSession;
    private readonly Action<string>? _log;

    private byte _session;
    private bool[] _acked = Array.Empty<bool>();
    private long[] _sentAt = Array.Empty<long>();
    private OtaStateReport? _report;
    private int _contig, _ackedFrom;
    private int _maxAcked = -1;
    private long _phaseStart, _lastSend, _lastState, _lastPoll;
    private int _restarts, _resent, _frames;
    private readonly HashSet<byte> _ownSessions = new();
    private long _lastRestartAt = long.MinValue / 2;
    private int _eraseProgress, _verifyProgress;
    private bool _bootKnown;
    private int _bootBaseline;
    private long _offlineSince = -1;
    private string? _message;
    private readonly Queue<(long At, long Bytes)> _samples = new();
    private long _lastSample = long.MinValue / 2;
    private readonly Queue<(long At, long Bytes)> _etaSamples = new();
    private long _streamStart = -1;

    public OtaTransfer(OtaImage image, byte[] key, Func<byte[], bool> send, OtaTransferOptions? options = null,
        Func<byte>? newSession = null, Action<string>? log = null)
    {
        _image = image;
        _key = key;
        _send = send;
        _o = options ?? new OtaTransferOptions();
        _newSession = newSession ?? RandomSession;
        _log = log;
    }

    public OtaPhase Phase { get; private set; } = OtaPhase.NotStarted;

    /// <summary>Why the transfer failed (null unless <see cref="Phase"/> is <see cref="OtaPhase.Failed"/>).</summary>
    public string? FailureMessage { get; private set; }

    /// <summary>The error the timer reported, when that was the reason.</summary>
    public OtaError? TimerError { get; private set; }

    public bool Succeeded => Phase == OtaPhase.Done;
    public bool IsFinished => Phase is OtaPhase.Done or OtaPhase.Failed or OtaPhase.Cancelled;
    public byte Session => _session;
    public int Restarts => _restarts;
    public int Resent => _resent;
    public int FramesSent => _frames;
    public OtaStateReport? LastReport => _report;

    /// <summary>The timer reported that it is in this firmware's slot and verified (used by tests).</summary>
    public int Contig => _contig;

    public event Action<OtaProgress>? Changed;

    private static byte RandomSession() => (byte)Random.Shared.Next(1, 256);

    // ---- control --------------------------------------------------------------------------------------------------

    public void Start(long now)
    {
        if (Phase != OtaPhase.NotStarted) throw new InvalidOperationException("already started");
        BeginSession(now);
    }

    public void Cancel(long now)
    {
        if (IsFinished) return;
        if (Phase != OtaPhase.NotStarted) _send(OtaFrames.Abort(_session));      // polite; the timer drops the session by itself after 30 s anyway
        Phase = OtaPhase.Cancelled;
        _message = "Cancelled.";
        Raise(now);
    }

    private void BeginSession(long now)
    {
        _session = _newSession();
        if (_session == 0) _session = 1;
        _ownSessions.Add(_session);
        int total = _image.Chunks;
        _acked = new bool[total];
        _sentAt = new long[total];
        Array.Fill(_sentAt, -1L);
        _contig = 0;
        _ackedFrom = 0;
        _maxAcked = -1;
        _report = null;
        _eraseProgress = _verifyProgress = 0;
        _samples.Clear();
        _etaSamples.Clear();
        _streamStart = -1;
        _lastSample = long.MinValue / 2;
        Phase = OtaPhase.Beginning;
        _phaseStart = now;
        _lastState = now;
        _lastPoll = now;
        SendBegin(now);
        Raise(now);
    }

    private void SendBegin(long now)
    {
        var begin = new OtaBegin(_session, (uint)_image.Size, _image.Sha256, _image.Chunk, _image.Major, _image.Minor, _image.Patch);
        if (Send(OtaFrames.Begin(begin, _key))) _lastSend = now;
    }

    private bool Send(byte[] payload)
    {
        if (!_send(payload)) return false;
        _frames++;
        return true;
    }

    private void Fail(string message, long now, OtaError? timerError = null)
    {
        if (IsFinished) return;
        Phase = OtaPhase.Failed;
        FailureMessage = message;
        TimerError = timerError;
        _message = message;
        _log?.Invoke(message);
        Raise(now);
    }

    private void Succeed(long now, string message)
    {
        if (IsFinished) return;
        Phase = OtaPhase.Done;
        _message = message;
        _log?.Invoke(message);
        Raise(now);
    }

    private void Restart(string reason, long now)
    {
        if (_restarts >= _o.MaxRestarts)
        {
            Fail($"{reason} The transfer was started again {_o.MaxRestarts} times without success.", now);
            return;
        }
        _restarts++;
        _lastRestartAt = now;
        _log?.Invoke($"{reason} Starting the transfer again ({_restarts} of {_o.MaxRestarts}).");
        BeginSession(now);
    }

    // ---- what the fleet list says -----------------------------------------------------------------------------------

    /// <summary>The boot counter and firmware version the timer reports in its heartbeats (the fleet list).</summary>
    public void OnTimerBoot(int bootCount, string? fw, long now)
    {
        if (IsFinished || Phase == OtaPhase.NotStarted) return;
        if (!_bootKnown)
        {
            _bootBaseline = bootCount;
            _bootKnown = true;
            return;
        }
        if (bootCount == _bootBaseline) return;
        _bootBaseline = bootCount;

        switch (Phase)
        {
            case OtaPhase.Activating:
            case OtaPhase.Restarting:
                if (_image.IsVersion(fw))
                    Succeed(now, $"The timer restarted and reports firmware {(string.IsNullOrWhiteSpace(fw) ? _image.Version ?? "?" : fw)}.");
                else
                    Fail($"The timer restarted but still runs firmware {fw}, not {_image.Version}. The boot loader probably went back to the old firmware " +
                         "(the new one did not start properly, or the power was cut during its first seconds). Try again.", now);
                break;
            default:
                // the state report has noticed this restart already and started the transfer again: do not start it a third time on top of that
                // (a genuine second restart is still caught: the timer then reports an idle state without a session, which restarts the transfer again)
                if (now - _lastRestartAt < _o.RestartDedupMs) break;
                Restart("The timer restarted during the transfer (a power cut?), so the data written so far is lost.", now);
                break;
        }
    }

    /// <summary>The fleet list says whether the timer is online (heard within 4.5 s).</summary>
    public void OnTimerOnline(bool online, long now)
    {
        if (online) _offlineSince = -1;
        else if (_offlineSince < 0) _offlineSince = now;
    }

    // ---- frames from the timer ----------------------------------------------------------------------------------------

    public void OnPayload(ReadOnlySpan<byte> payload, long now)
    {
        if (IsFinished || Phase == OtaPhase.NotStarted) return;
        if (!OtaStateReport.TryParse(payload, out var r)) return;

        if (r.Error == OtaError.NoSlot)
        {
            Fail(OtaText.Error(OtaError.NoSlot), now, OtaError.NoSlot);
            return;
        }
        if (r.Session != _session)
        {
            if (r.Error == OtaError.Busy && _ownSessions.Contains(r.Session) && Phase is OtaPhase.Beginning or OtaPhase.Erasing)
            {
                // the timer is still busy with a transfer WE started a moment ago (two restarts in a row): carry on with that one
                _log?.Invoke($"The timer still holds our earlier transfer (session {r.Session:X2}); carrying on with it.");
                _session = r.Session;
                _lastSend = long.MinValue / 2;
                return;
            }
            if (r.Error == OtaError.Busy && r.Session != 0 && Phase != OtaPhase.Restarting)
                Fail($"The timer is busy with another firmware transfer (session {r.Session:X2}). {OtaText.Error(OtaError.Busy)}", now, OtaError.Busy);
            else if (r.State == OtaState.Idle && r.Session == 0 && Phase is OtaPhase.Erasing or OtaPhase.Streaming or OtaPhase.Verifying)
                LostSession(r, now);
            return;                                    // a late answer of another session
        }

        _lastState = now;
        _report = r;

        if (r.State == OtaState.Failed || (r.Error != OtaError.None && r.Error != OtaError.Timeout))
        {
            Fail(OtaText.Error(r.Error == OtaError.None ? OtaError.FlashError : r.Error), now, r.Error);
            return;
        }
        // the transfer is gone from the timer: it restarted, or dropped the session. (After Activate an idle timer is the expected sign of a restart.)
        if (r.Error == OtaError.Timeout || (r.State == OtaState.Idle && Phase is OtaPhase.Erasing or OtaPhase.Streaming or OtaPhase.Verifying))
        {
            LostSession(r, now);
            return;
        }

        switch (r.State)
        {
            case OtaState.Erasing:
                if (Phase is OtaPhase.Beginning or OtaPhase.Erasing)
                {
                    if (Phase == OtaPhase.Beginning) { Phase = OtaPhase.Erasing; _phaseStart = now; }
                    _eraseProgress = r.Progress;
                }
                break;
            case OtaState.Receiving:
                if (Phase is OtaPhase.Beginning or OtaPhase.Erasing)
                {
                    Phase = OtaPhase.Streaming;
                    _phaseStart = now;
                    _log?.Invoke($"The timer is ready to receive ({r.Total} chunks, slot {r.Slot}).");
                }
                if (Phase == OtaPhase.Streaming) AbsorbReceived(r, now);
                break;
            case OtaState.Verifying:
                if (Phase is OtaPhase.Streaming or OtaPhase.Beginning or OtaPhase.Erasing or OtaPhase.Verifying)
                {
                    if (Phase != OtaPhase.Verifying) { Phase = OtaPhase.Verifying; _phaseStart = now; }
                    _verifyProgress = r.Progress;
                }
                break;
            case OtaState.Verified:
                if (Phase != OtaPhase.Activating && Phase != OtaPhase.Restarting)
                {
                    Phase = OtaPhase.Activating;
                    _phaseStart = now;
                    _log?.Invoke("The timer verified the firmware (SHA-256 matches). Switching over...");
                    SendActivate(now);
                }
                break;
            case OtaState.Activating:
                if (Phase != OtaPhase.Restarting)
                {
                    Phase = OtaPhase.Restarting;
                    _phaseStart = now;
                    _log?.Invoke("The timer is switching to the new firmware and restarts.");
                }
                break;
        }

        if (Phase == OtaPhase.Streaming) SendWindow(now, fast: true);
        Raise(now);
    }

    private void LostSession(OtaStateReport r, long now)
    {
        string why = r.Error == OtaError.Timeout
            ? "The timer dropped the transfer after 30 s of silence."
            : "The timer no longer knows the transfer (it restarted, or it was reset).";
        Restart(why, now);
    }

    private void AbsorbReceived(OtaStateReport r, long now)
    {
        _contig = Math.Max(_contig, Math.Min(r.Contig, _image.Chunks));
        for (; _ackedFrom < _contig; _ackedFrom++) _acked[_ackedFrom] = true;
        for (int bit = 0; bit < 32; bit++)
        {
            int seq = r.Contig + 1 + bit;
            if (seq >= _image.Chunks) break;
            if ((r.Mask & (1u << bit)) != 0) { _acked[seq] = true; _maxAcked = Math.Max(_maxAcked, seq); }
        }
        _maxAcked = Math.Max(_maxAcked, _contig - 1);

        if (_contig >= _image.Chunks)
        {
            Phase = OtaPhase.Verifying;
            _phaseStart = now;
            _log?.Invoke($"All {_image.Chunks} chunks were received ({_resent} re-sent). Asking the timer to check the firmware.");
            if (Send(OtaFrames.Verify(_session))) _lastSend = now;
        }
    }

    // ---- time -----------------------------------------------------------------------------------------------------

    public void Tick(long now)
    {
        if (IsFinished || Phase == OtaPhase.NotStarted) return;

        if (_offlineSince >= 0)
        {
            if (now - _offlineSince > _o.OfflineGiveUpMs)
            {
                Fail($"The timer is offline since {(now - _offlineSince) / 1000} s (power cut while its machine heats up, or out of range) and did not come back. " +
                     "Try again when it is back; the update starts from the beginning.", now);
            }
            return;                                              // nothing to send to a timer that cannot hear us
        }

        switch (Phase)
        {
            case OtaPhase.Beginning:
                if (now - _phaseStart > _o.BeginTimeoutMs)
                    Fail("The timer did not answer the start request within 10 s. Is it online, in range, and running firmware with over-the-air support?", now);
                else if (now - _lastSend >= _o.BeginRetryMs) SendBegin(now);
                break;

            case OtaPhase.Erasing:
                if (now - _phaseStart > _o.EraseTimeoutMs) Fail("The timer needed more than 60 s to prepare its flash. Try again.", now);
                else if (now - _lastSend >= _o.ErasePollMs) PollNow(now);
                break;

            case OtaPhase.Streaming:
                if (now - _lastState > _o.StallTimeoutMs)
                {
                    Fail($"The timer stopped answering during the transfer ({(now - _lastState) / 1000} s without a report).", now);
                    break;
                }
                if (now - Math.Max(_lastState, _lastPoll) >= _o.PollIdleMs) PollNow(now);
                SendWindow(now, fast: false);
                break;

            case OtaPhase.Verifying:
                if (now - _phaseStart > _o.VerifyTimeoutMs) Fail("The timer needed more than 30 s to check the firmware.", now);
                else if (now - _lastSend >= _o.VerifyRetryMs)
                {
                    if (_report is { State: OtaState.Verifying } ) PollNow(now);
                    else if (Send(OtaFrames.Verify(_session))) _lastSend = now;
                }
                break;

            case OtaPhase.Activating:
                if (now - _phaseStart > _o.ActivateTimeoutMs) Fail("The timer did not acknowledge the switch to the new firmware.", now);
                else if (now - _lastSend >= _o.ActivateRetryMs) SendActivate(now);
                break;

            case OtaPhase.Restarting:
                if (now - _phaseStart > _o.RestartTimeoutMs)
                    Fail("The timer did not report a restart with the new firmware within 60 s. It may still be restarting, or have gone back to the old firmware; check its version.", now);
                break;
        }

        if (!IsFinished) SampleSpeed(now);
        if (!IsFinished) Raise(now);
    }

    private void PollNow(long now)
    {
        if (Send(OtaFrames.Poll(_session))) { _lastSend = now; _lastPoll = now; }
    }

    private void SendActivate(long now)
    {
        if (Send(OtaFrames.Activate(_session))) _lastSend = now;
    }

    private void SendWindow(long now, bool fast)
    {
        int end = Math.Min(_image.Chunks, _contig + _o.Window);
        for (int seq = _contig; seq < end; seq++)
        {
            if (_acked[seq]) continue;
            long sent = _sentAt[seq];
            if (sent >= 0)
            {
                // re-send after the timeout, or sooner when the timer already has later chunks (a gap = this one was lost)
                bool gap = fast && seq < _maxAcked && now - sent >= 40;
                if (!gap && now - sent < _o.ResendAfterMs) continue;
            }
            if (!Send(OtaFrames.Data(_session, seq, _image.ChunkBytes(seq)))) return;       // the link is full: try again next tick
            if (sent >= 0) _resent++;
            _sentAt[seq] = now;
        }
    }

    // ---- progress -------------------------------------------------------------------------------------------------------

    private long BytesDone()
    {
        if (Phase is OtaPhase.Verifying or OtaPhase.Activating or OtaPhase.Restarting or OtaPhase.Done) return _image.Size;
        long chunks = _contig;
        if (_report is { } r && Phase == OtaPhase.Streaming) chunks = r.ReceivedCount;
        return Math.Min(_image.Size, chunks * _image.Chunk);
    }

    private void SampleSpeed(long now)
    {
        if (now - _lastSample < 250) return;
        _lastSample = now;
        long done = BytesDone();
        _samples.Enqueue((now, done));
        while (_samples.Count > 2 && now - _samples.Peek().At > 5000) _samples.Dequeue();

        if (Phase != OtaPhase.Streaming) return;
        if (_streamStart < 0) _streamStart = now;
        if (now - _streamStart < _o.EtaRampMs) return;
        _etaSamples.Enqueue((now, done));
        while (_etaSamples.Count > 2 && now - _etaSamples.Peek().At > _o.EtaWindowMs) _etaSamples.Dequeue();
    }

    /// <summary>Seconds until the timer runs the new firmware, or null while there is not yet enough to tell.</summary>
    private double? EstimateSecondsLeft(long done)
    {
        if (Phase != OtaPhase.Streaming || _etaSamples.Count < 2) return null;
        var first = _etaSamples.Peek();
        var last = _etaSamples.Last();
        if (last.At - first.At < _o.EtaMinSpanMs || last.Bytes <= first.Bytes) return null;
        double speed = (last.Bytes - first.Bytes) * 1000.0 / (last.At - first.At);
        return Math.Max(0, _image.Size - done) / speed + _o.TailSeconds;
    }

    public OtaProgress Snapshot(long now)
    {
        long done = BytesDone();
        double speed = 0;
        if (_samples.Count >= 2)
        {
            var first = _samples.Peek();
            var last = _samples.Last();
            if (last.At > first.At) speed = Math.Max(0, (last.Bytes - first.Bytes) * 1000.0 / (last.At - first.At));
        }
        double? eta = EstimateSecondsLeft(done);

        (string stage, double fraction) = Phase switch
        {
            OtaPhase.NotStarted => ("Waiting", 0.0),
            OtaPhase.Beginning => ("Contacting the timer...", 0.0),
            OtaPhase.Erasing => ($"Preparing the timer's flash ({_eraseProgress} %)", _eraseProgress / 100.0),
            OtaPhase.Streaming => ("Sending the firmware", _image.Size == 0 ? 0 : (double)done / _image.Size),
            OtaPhase.Verifying => ($"The timer checks the firmware ({_verifyProgress} %)", _verifyProgress / 100.0),
            OtaPhase.Activating => ("Switching to the new firmware...", 1.0),
            OtaPhase.Restarting => ("Waiting for the timer to restart...", 1.0),
            OtaPhase.Done => ("Done", 1.0),
            OtaPhase.Failed => ("Failed", _image.Size == 0 ? 0 : (double)done / _image.Size),
            _ => ("Cancelled", _image.Size == 0 ? 0 : (double)done / _image.Size),
        };
        return new OtaProgress(Phase, stage, fraction, done, _image.Size, speed, eta, _resent, _restarts, _frames, _message);
    }

    private long _lastRaise = long.MinValue / 2;

    private void Raise(long now)
    {
        // progress changes at most about 5 times per second (and always when the transfer ends)
        if (!IsFinished && now - _lastRaise < 200) return;
        _lastRaise = now;
        Changed?.Invoke(Snapshot(now));
    }
}
