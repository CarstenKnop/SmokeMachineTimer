using System.Security.Cryptography;
using System.Text;
using FogConsole.Ota;

namespace FogConsole.Simulator;

/// <summary>
/// The receiving side of an over-the-air update as the timer firmware implements it (docs/OTA_UPDATE.md, section 6.1), in C#:
/// used by the simulated timers and as the reference for the sender's tests. It has no radio and no clock of its own:
/// frames go in through <see cref="OnFrame"/>, time through <see cref="Tick"/>, and what the timer would send comes out as payloads.
/// </summary>
public sealed class SimOtaReceiver
{
    public const int SectorBytes = 4096;
    private const int SilenceMs = 30_000;
    private const int SelfStateGapMs = 20;
    private static readonly byte[] TimerMarker = Encoding.ASCII.GetBytes("FOGFW|timer|");

    private readonly byte[] _key;
    private readonly int _slotSize;

    public SimOtaReceiver(string? key = null, int slotSize = 0x1E0000, bool hasSlot = true, bool supported = true)
    {
        _key = OtaAuth.KeyBytes(key);
        _slotSize = slotSize;
        HasSlot = hasSlot;
        Supported = supported;
    }

    /// <summary>False: the timer runs firmware without OTA code and ignores every OTA frame.</summary>
    public bool Supported { get; set; }

    /// <summary>False: the flash layout has only one application slot.</summary>
    public bool HasSlot { get; set; }

    /// <summary>The slot the running firmware boots from (the other one is written).</summary>
    public int RunningSlot { get; set; }

    /// <summary>Simulated time to erase one 4 KiB sector including the pause between sectors.</summary>
    public int EraseMsPerSector { get; set; } = 65;

    /// <summary>Bytes the hash check covers per <see cref="Tick"/> (the real timer does a slice per loop pass).</summary>
    public int VerifyBytesPerTick { get; set; } = 96 * 1024;

    public OtaState State { get; private set; } = OtaState.Idle;
    public OtaError Error { get; private set; }
    public byte Session { get; private set; }
    public int Total { get; private set; }
    public int Contig { get; private set; }
    public int ReceivedChunks { get; private set; }
    public uint ImageSize { get; private set; }

    /// <summary>The bytes written to the other slot so far (its length is the image size).</summary>
    public byte[] SlotBytes => _slot;

    /// <summary>Set when Activate was accepted: the new firmware's version (the timer reboots into it).</summary>
    public (byte Major, byte Minor, byte Patch)? PendingActivation { get; private set; }

    /// <summary>Returns the version of a firmware that was just activated (once) - the caller reboots the timer into it.</summary>
    public (byte Major, byte Minor, byte Patch)? TakePendingActivation()
    {
        var p = PendingActivation;
        PendingActivation = null;
        return p;
    }

    /// <summary>Frames that were accepted as new chunks (tests).</summary>
    public int AcceptedData { get; private set; }

    /// <summary>Frames that were duplicates of chunks already written (tests).</summary>
    public int DuplicateData { get; private set; }

    private int TargetSlot => 1 - RunningSlot;

    private byte[] _slot = Array.Empty<byte>();
    private bool[] _received = Array.Empty<bool>();
    private OtaBegin? _begin;
    private int _chunk;
    private int _erasedSectors, _sectors;
    private long _lastEraseAt;
    private int _verifyPos;
    private long _lastFrameAt;
    private long _lastSelfState = long.MinValue / 2;
    private int _sinceState;
    private bool _stateWanted;
    private bool _timeoutPending;
    private long _activateAt = -1;
    private IncrementalHash? _hash;

    /// <summary>The timer lost its power: RAM state (the whole transfer) is gone, the flash keeps what was written.</summary>
    public void PowerLoss()
    {
        ResetSession();
        _timeoutPending = false;
        PendingActivation = null;
        _activateAt = -1;
    }

    private void ResetSession()
    {
        State = OtaState.Idle;
        Error = OtaError.None;
        Session = 0;
        Total = 0;
        Contig = 0;
        ReceivedChunks = 0;
        _received = Array.Empty<bool>();
        _begin = null;
        _stateWanted = false;
        _sinceState = 0;
        _hash?.Dispose();
        _hash = null;
    }

    // ---- frames in ------------------------------------------------------------------------------------------------------

    /// <summary>Handles one OTA payload from the remote; returns the payloads the timer sends back.</summary>
    public List<byte[]> OnFrame(ReadOnlySpan<byte> p, long now)
    {
        var replies = new List<byte[]>();
        if (!Supported || p.Length < 2) return replies;
        byte op = p[0], session = p[1];

        switch (op)
        {
            case OtaOp.Poll:
                if (State == OtaState.Idle)
                    replies.Add(HasSlot ? Report(session, now) : Fault(session, OtaError.NoSlot, now));   // the capability probe: ready, or no second slot
                else if (session == 0) replies.Add(Report(Session, now));                              // busy with a transfer: say so
                else if (!IsActive(session)) replies.Add(WrongSession(session, now));
                else { Touch(now); replies.Add(Report(session, now)); }
                break;

            case OtaOp.Begin:
                HandleBegin(p, now, replies);
                break;

            case OtaOp.Data:
                HandleData(p, now, replies);
                break;

            case OtaOp.Verify:
                if (!IsActive(session)) { replies.Add(WrongSession(session, now)); break; }
                Touch(now);
                if (State == OtaState.Receiving && Contig >= Total)
                {
                    State = OtaState.Verifying;
                    _verifyPos = 0;
                    _hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
                }
                else if (State is not (OtaState.Verifying or OtaState.Verified or OtaState.Activating))
                {
                    Error = OtaError.BadRequest;               // not all chunks are here
                    replies.Add(Report(session, now));
                    Error = OtaError.None;
                    break;
                }
                replies.Add(Report(session, now));
                break;

            case OtaOp.Activate:
                if (!IsActive(session)) { replies.Add(WrongSession(session, now)); break; }
                Touch(now);
                if (State == OtaState.Verified && p.Length >= 3 && p[2] == OtaOp.ActivateMagic)
                {
                    State = OtaState.Activating;
                    _activateAt = now + 300;                       // the reboot follows a moment after the answer left
                }
                else if (State != OtaState.Activating)
                {
                    Error = OtaError.BadRequest;
                    replies.Add(Report(session, now));
                    Error = OtaError.None;
                    break;
                }
                replies.Add(Report(session, now));
                break;

            case OtaOp.Abort:
                if (IsActive(session)) ResetSession();
                replies.Add(Report(session, now));
                break;
        }
        return replies;
    }

    private bool IsActive(byte session) => State != OtaState.Idle && session == Session;

    private void Touch(long now) => _lastFrameAt = now;

    private void HandleBegin(ReadOnlySpan<byte> p, long now, List<byte[]> replies)
    {
        if (!OtaFrames.TryParseBegin(p, out var b, out var auth)) { replies.Add(Fault(p[1], OtaError.BadRequest, now)); return; }
        if (!HasSlot) { replies.Add(Fault(b.Session, OtaError.NoSlot, now)); return; }
        if (!OtaAuth.Verify(_key, b, auth)) { replies.Add(Fault(b.Session, OtaError.AuthFailed, now)); return; }

        if (State is OtaState.Idle or OtaState.Failed)
        {
            if (b.Size == 0 || b.Size > _slotSize) { replies.Add(Fault(b.Session, OtaError.TooBig, now)); return; }
            if (b.Chunk is < OtaFrames.MinChunk or > OtaFrames.MaxChunk || b.Session == 0) { replies.Add(Fault(b.Session, OtaError.BadRequest, now)); return; }

            ResetSession();
            Session = b.Session;
            _begin = b;
            ImageSize = b.Size;
            _chunk = b.Chunk;
            Total = (int)((b.Size + (uint)_chunk - 1) / (uint)_chunk);
            _slot = new byte[b.Size];
            _received = new bool[Total];
            _sectors = (int)((b.Size + SectorBytes - 1) / SectorBytes);
            _erasedSectors = 0;
            _lastEraseAt = now;
            State = OtaState.Erasing;
            Touch(now);
            replies.Add(Report(Session, now));
            return;
        }

        if (_begin is { } cur && cur.Session == b.Session && cur.Size == b.Size && cur.Sha256.AsSpan().SequenceEqual(b.Sha256) && cur.Chunk == b.Chunk)
        {
            Touch(now);
            replies.Add(Report(Session, now));                     // the same transfer again: only report (resume after a restart of the sender)
            return;
        }
        replies.Add(Report(Session, now, OtaError.Busy));
    }

    private void HandleData(ReadOnlySpan<byte> p, long now, List<byte[]> replies)
    {
        if (!OtaFrames.TryParseData(p, out byte session, out int seq, out var bytes)) return;
        if (!IsActive(session)) { replies.Add(WrongSession(session, now)); return; }
        Touch(now);
        if (State != OtaState.Receiving) return;                    // data only while receiving
        if (seq >= Total) return;
        int expected = seq == Total - 1 ? (int)(ImageSize - (uint)(seq * _chunk)) : _chunk;
        if (bytes.Length != expected) return;

        if (seq == 0 && (bytes.Length < 14 || bytes[0] != 0xE9 || (bytes[12] | (bytes[13] << 8)) != 5))
        {
            State = OtaState.Failed;
            Error = OtaError.BadImage;
            replies.Add(Report(session, now));
            return;
        }

        if (_received[seq])
        {
            DuplicateData++;
        }
        else
        {
            bytes.CopyTo(_slot.AsSpan(seq * _chunk));
            _received[seq] = true;
            ReceivedChunks++;
            AcceptedData++;
            while (Contig < Total && _received[Contig]) Contig++;
        }

        _sinceState++;
        bool gap = seq > Contig;                                    // arrived out of order: tell the sender what is missing
        if (Contig >= Total) _stateWanted = true;
        if ((_sinceState >= 8 || gap || _stateWanted) && now - _lastSelfState >= SelfStateGapMs)
        {
            replies.Add(Report(Session, now));
            _stateWanted = false;
        }
        else if (_sinceState >= 8 || gap)
        {
            _stateWanted = true;                                    // send it with the next tick
        }
    }

    // ---- time ----------------------------------------------------------------------------------------------------------

    /// <summary>The main loop pass: erases, hashes, drops silent sessions. Returns the payloads the timer sends on its own.</summary>
    public List<byte[]> Tick(long now)
    {
        var replies = new List<byte[]>();
        if (!Supported) return replies;

        if (State is OtaState.Erasing or OtaState.Receiving or OtaState.Verifying or OtaState.Verified && now - _lastFrameAt >= SilenceMs)
        {
            ResetSession();
            _timeoutPending = true;
            return replies;
        }

        switch (State)
        {
            case OtaState.Erasing:
            {
                long due = (now - _lastEraseAt) / Math.Max(1, EraseMsPerSector);
                if (due > 0)
                {
                    _erasedSectors = (int)Math.Min(_sectors, _erasedSectors + due);
                    _lastEraseAt += due * EraseMsPerSector;
                }
                if (_erasedSectors >= _sectors)
                {
                    State = OtaState.Receiving;
                    replies.Add(Report(Session, now));
                }
                break;
            }
            case OtaState.Verifying:
            {
                int n = Math.Min(VerifyBytesPerTick, _slot.Length - _verifyPos);
                _hash!.AppendData(_slot, _verifyPos, n);
                _verifyPos += n;
                if (_verifyPos >= _slot.Length)
                {
                    var digest = _hash.GetHashAndReset();
                    if (!digest.AsSpan().SequenceEqual(_begin!.Sha256)) { State = OtaState.Failed; Error = OtaError.HashMismatch; }
                    else if (_slot.AsSpan().IndexOf(TimerMarker) < 0) { State = OtaState.Failed; Error = OtaError.BadImage; }
                    else State = OtaState.Verified;
                    replies.Add(Report(Session, now));
                }
                break;
            }
            case OtaState.Receiving:
                if (_stateWanted && now - _lastSelfState >= SelfStateGapMs)
                {
                    replies.Add(Report(Session, now));
                    _stateWanted = false;
                }
                break;
            case OtaState.Activating:
                if (_activateAt >= 0 && now >= _activateAt)
                {
                    _activateAt = -1;
                    PendingActivation = (_begin!.FwMajor, _begin.FwMinor, _begin.FwPatch);
                    ResetSession();                              // the timer is rebooting
                }
                break;
        }
        return replies;
    }

    // ---- reports ------------------------------------------------------------------------------------------------------

    private byte[] Report(byte session, long now, OtaError? error = null)
    {
        _lastSelfState = now;
        _sinceState = 0;
        var e = error ?? Error;
        if (_timeoutPending && State == OtaState.Idle) { e = OtaError.Timeout; _timeoutPending = false; }
        int progress = State switch
        {
            OtaState.Erasing => _sectors == 0 ? 100 : _erasedSectors * 100 / _sectors,
            OtaState.Verifying => _slot.Length == 0 ? 100 : _verifyPos * 100 / _slot.Length,
            _ => 100,
        };
        uint mask = 0;
        for (int bit = 0; bit < 32; bit++)
        {
            int c = Contig + 1 + bit;
            if (c < Total && _received[c]) mask |= 1u << bit;
        }
        return new OtaStateReport(session, State, e, Contig, mask, progress, Total, TargetSlot).ToBytes();
    }

    private byte[] Fault(byte session, OtaError error, long now)
    {
        // the timer's own state does not change (an attacker must not be able to disturb a running transfer)
        _lastSelfState = now;
        return new OtaStateReport(session, State == OtaState.Idle ? OtaState.Idle : State, error, Contig, 0, 100, Total, TargetSlot).ToBytes();
    }

    private byte[] WrongSession(byte session, long now)
    {
        if (State == OtaState.Idle) return Report(session, now);          // "nothing here": Idle, error none
        return new OtaStateReport(Session, State, OtaError.Busy, Contig, 0, 100, Total, TargetSlot).ToBytes();
    }
}
