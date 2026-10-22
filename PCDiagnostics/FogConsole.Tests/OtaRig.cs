using FogConsole.Flashing;
using FogConsole.Ota;
using FogConsole.Simulator;

namespace FogConsole.Tests;

/// <summary>A valid timer image of about the given size (ESP header, chip id, identity string, checksum, SHA-256) for tests.</summary>
internal static class OtaTestImages
{
    public static OtaImage Create(int approxSize = 24_000, string version = "2.2.0", int seed = 1, int chunk = OtaFrames.DefaultChunk) =>
        new(DemoFirmware.CreateImage(FirmwareKind.Timer, version, approxSize, seed: seed), version, chunk);
}

/// <summary>
/// Sender and timer model in virtual time with a link in between that can lose, duplicate, delay and reorder frames in both directions.
/// Everything is deterministic for a given seed. The "fleet" the sender watches (boot counter, firmware version, online) follows the timer.
/// </summary>
internal sealed class OtaRig
{
    private readonly PriorityQueue<(bool ToTimer, byte[] Payload), (long Due, long Seq)> _wire = new();
    private readonly Random _rng;
    private long _seq;

    public OtaRig(OtaImage image, string? senderKey = null, string? timerKey = null, OtaTransferOptions? options = null, int seed = 1,
        bool hasSlot = true, bool supported = true, string fw = "2.1.0")
    {
        Image = image;
        _rng = new Random(seed);
        Rx = new SimOtaReceiver(timerKey, hasSlot: hasSlot, supported: supported) { EraseMsPerSector = 20 };
        Fw = fw;
        Tr = new OtaTransfer(image, OtaAuth.KeyBytes(senderKey), Send, options ?? new OtaTransferOptions(),
            newSession: () => (byte)(0x40 + _sessions++), log: l => Log.Add($"{Now,7} ms  {l}"));
        Tr.OnTimerBoot(Boot, Fw, 0);
        Tr.OnTimerOnline(true, 0);
    }

    private int _sessions;

    public OtaImage Image { get; }
    public SimOtaReceiver Rx { get; }
    public OtaTransfer Tr { get; }
    public List<string> Log { get; } = new();
    public long Now { get; private set; }

    // the link
    public double LossToTimer { get; set; }
    public double LossToPc { get; set; }
    public double DuplicateRate { get; set; }
    public int LatencyMs { get; set; } = 8;
    public int JitterMs { get; set; }
    public bool Blackhole { get; set; }
    public Func<byte[], byte[]>? Mangle { get; set; }              // lets a test damage a frame on its way to the timer
    public int SendLimit { get; set; } = int.MaxValue;               // frames the link accepts per Tick (backpressure)

    // the timer's life
    public int Boot { get; private set; } = 7;
    public string Fw { get; set; }
    public bool Powered { get; private set; } = true;
    private long _poweredAt;
    private string? _pendingFw;
    public string? FwAfterActivate { get; set; }                       // null: the new version; otherwise what the "rebooted" timer reports (a rollback)
    /// <summary>The fleet list learns of a new boot counter this long after the timer restarted (the state report of the timer is faster).</summary>
    public int BootReportDelayMs { get; set; }
    private readonly List<(long At, int Boot)> _bootLog = new() { (long.MinValue / 2, 7) };
    public int FramesToTimer { get; private set; }
    public int FramesToPc { get; private set; }
    private int _sentThisTick;

    private bool Send(byte[] payload)
    {
        if (_sentThisTick >= SendLimit) return false;
        _sentThisTick++;
        FramesToTimer++;
        Deliver(true, payload, LossToTimer);
        return true;
    }

    private void Deliver(bool toTimer, byte[] payload, double loss)
    {
        if (Blackhole || _rng.NextDouble() < loss) return;
        if (toTimer && Mangle is not null) payload = Mangle(payload);
        int copies = _rng.NextDouble() < DuplicateRate ? 2 : 1;
        for (int i = 0; i < copies; i++)
        {
            long due = Now + LatencyMs + (JitterMs > 0 ? _rng.Next(0, JitterMs + 1) : 0) + i * 3;
            _wire.Enqueue((toTimer, payload), (due, _seq++));
        }
    }

    /// <summary>The timer loses its power for <paramref name="offMs"/>: RAM state gone, flash kept, offline until it is back (boot + 1).</summary>
    public void PowerCut(long offMs)
    {
        Powered = false;
        Rx.PowerLoss();
        _poweredAt = Now + offMs;
        _pendingFw = null;
        Tr.OnTimerOnline(false, Now);
    }

    /// <summary>Runs virtual time until the transfer ends or <paramref name="maxMs"/> passed. Step 5 ms.</summary>
    public void Run(long maxMs, Action<OtaRig>? each = null)
    {
        long end = Now + maxMs;
        if (Tr.Phase == OtaPhase.NotStarted) Tr.Start(Now);
        while (!Tr.IsFinished && Now < end)
        {
            Step();
            each?.Invoke(this);
        }
    }

    public void Step()
    {
        Now += 5;
        _sentThisTick = 0;

        if (!Powered && Now >= _poweredAt)
        {
            Powered = true;
            Boot++;
            _bootLog.Add((Now, Boot));
            if (_pendingFw is not null) { Fw = _pendingFw; _pendingFw = null; }
            Tr.OnTimerOnline(true, Now);
        }

        while (_wire.TryPeek(out var item, out var key) && key.Due <= Now)
        {
            _wire.Dequeue();
            if (item.ToTimer)
            {
                if (!Powered) continue;
                foreach (var reply in Rx.OnFrame(item.Payload, Now)) { FramesToPc++; Deliver(false, reply, LossToPc); }
            }
            else
            {
                Tr.OnPayload(item.Payload, Now);
            }
        }

        if (Powered)
        {
            foreach (var reply in Rx.Tick(Now)) { FramesToPc++; Deliver(false, reply, LossToPc); }
            if (Rx.TakePendingActivation() is { } v)
            {
                // the timer reboots into the new image: gone for a moment, then boot + 1
                string version = FwAfterActivate ?? $"{v.Major}.{v.Minor}.{v.Patch}";
                Powered = false;
                _poweredAt = Now + 2500;
                _pendingFw = version;
                Tr.OnTimerOnline(false, Now);
            }
        }

        int reportedBoot = _bootLog.Last(b => b.At <= Now - BootReportDelayMs).Boot;
        Tr.OnTimerBoot(reportedBoot, Fw, Now);
        Tr.Tick(Now);
    }
}
