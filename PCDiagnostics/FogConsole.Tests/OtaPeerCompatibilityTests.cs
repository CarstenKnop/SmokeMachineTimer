using System.Diagnostics;
using FogConsole.Ota;
using Xunit;

namespace FogConsole.Tests;

/// <summary>
/// The timer's receiver as the C++ program tools/native-tests/build/ota_peer.exe (built by tools/native-tests/run.ps1), driven over
/// stdin/stdout. The tests below are skipped silently when the program has not been built.
/// </summary>
internal sealed class OtaPeerProcess : IDisposable
{
    private readonly Process _p;

    private OtaPeerProcess(Process p) => _p = p;

    public static string? FindExecutable()
    {
        for (var dir = new DirectoryInfo(AppContext.BaseDirectory); dir is not null; dir = dir.Parent)
        {
            var candidate = Path.Combine(dir.FullName, "tools", "native-tests", "build", "ota_peer.exe");
            if (File.Exists(candidate)) return candidate;
        }
        return null;
    }

    public static OtaPeerProcess? Start(string? key = null, int? slotSize = null, bool noSlot = false, int runningSlot = 0)
    {
        var exe = FindExecutable();
        if (exe is null) return null;
        var psi = new ProcessStartInfo(exe)
        {
            RedirectStandardInput = true,
            RedirectStandardOutput = true,
            UseShellExecute = false,
            CreateNoWindow = true,
        };
        if (key is not null) { psi.ArgumentList.Add("--key"); psi.ArgumentList.Add(key); }
        if (slotSize is not null) { psi.ArgumentList.Add("--slot-size"); psi.ArgumentList.Add(slotSize.Value.ToString()); }
        if (noSlot) psi.ArgumentList.Add("--no-slot");
        psi.ArgumentList.Add("--running-slot");
        psi.ArgumentList.Add(runningSlot.ToString());
        return new OtaPeerProcess(Process.Start(psi) ?? throw new InvalidOperationException("ota_peer did not start"));
    }

    private string ReadLine()
    {
        var read = _p.StandardOutput.ReadLineAsync();
        if (!read.Wait(10_000)) throw new TimeoutException("ota_peer did not answer");
        return read.Result ?? throw new InvalidOperationException("ota_peer closed its output");
    }

    private List<byte[]> Command(string line)
    {
        _p.StandardInput.WriteLine(line);
        _p.StandardInput.Flush();
        var replies = new List<byte[]>();
        while (true)
        {
            var l = ReadLine();
            if (l == "OK") return replies;
            if (l.StartsWith("P ", StringComparison.Ordinal)) replies.Add(Convert.FromHexString(l[2..]));
            else throw new InvalidOperationException("unexpected answer of ota_peer: " + l);
        }
    }

    /// <summary>Hands one OTA payload to the receiver; returns the payloads it sends back.</summary>
    public List<byte[]> Payload(byte[] payload) => Command("P " + Convert.ToHexString(payload).ToLowerInvariant());

    /// <summary>Sets the virtual time and runs the receiver's tick.</summary>
    public List<byte[]> Tick(long ms) => Command("T " + ms);

    /// <summary>Power cut: the receiver loses its RAM state, the flash content stays.</summary>
    public void PowerCut() => Command("R");

    public (string Hash, int State, int Error, int Contig, int Total, int Slot) Inspect()
    {
        _p.StandardInput.WriteLine("H");
        _p.StandardInput.Flush();
        var h = ReadLine().Split(' ');
        var s = ReadLine().Split(' ');
        Assert.Equal("H", h[0]);
        Assert.Equal("S", s[0]);
        return (h[1], int.Parse(s[1]), int.Parse(s[2]), int.Parse(s[3]), int.Parse(s[4]), int.Parse(s[5]));
    }

    public void Dispose()
    {
        try
        {
            _p.StandardInput.WriteLine("Q");
            _p.StandardInput.Flush();
            if (!_p.WaitForExit(2000)) _p.Kill();
        }
        catch { /* gone already */ }
        _p.Dispose();
    }
}

/// <summary>The sender of Fog Console against the real receiver code, in virtual time, over a link that can lose, duplicate and reorder frames.</summary>
internal sealed class OtaPeerRig : IDisposable
{
    private readonly PriorityQueue<(bool ToTimer, byte[] Payload), (long Due, long Seq)> _wire = new();
    private readonly Random _rng;
    private long _seq;
    private int _sessions;
    private bool _powered = true;
    private long _backAt;
    private bool _rebootPending;
    private long _rebootAt;

    public OtaPeerRig(OtaPeerProcess peer, OtaImage image, string? senderKey = null, int seed = 1)
    {
        Peer = peer;
        Image = image;
        _rng = new Random(seed);
        Tr = new OtaTransfer(image, OtaAuth.KeyBytes(senderKey), Send, new OtaTransferOptions(),
            newSession: () => (byte)(0x60 + _sessions++), log: l => Log.Add($"{Now,7} ms  {l}"));
        Tr.OnTimerBoot(Boot, Fw, 0);
        Tr.OnTimerOnline(true, 0);
    }

    public OtaPeerProcess Peer { get; }
    public OtaImage Image { get; }
    public OtaTransfer Tr { get; }
    public List<string> Log { get; } = new();
    public long Now { get; private set; }
    public int Boot { get; private set; } = 7;
    public string Fw { get; private set; } = "2.1.0";

    public double LossToTimer { get; set; }
    public double LossToPc { get; set; }
    public double DuplicateRate { get; set; }
    public int LatencyMs { get; set; } = 8;
    public int JitterMs { get; set; }

    private bool Send(byte[] payload)
    {
        Deliver(true, payload, LossToTimer);
        return true;
    }

    private void Deliver(bool toTimer, byte[] payload, double loss)
    {
        if (_rng.NextDouble() < loss) return;
        int copies = _rng.NextDouble() < DuplicateRate ? 2 : 1;
        for (int i = 0; i < copies; i++)
            _wire.Enqueue((toTimer, payload), (Now + LatencyMs + (JitterMs > 0 ? _rng.Next(0, JitterMs + 1) : 0) + i * 3, _seq++));
    }

    private void FromTimer(IEnumerable<byte[]> replies)
    {
        foreach (var reply in replies)
        {
            Deliver(false, reply, LossToPc);
            // the real timer reboots a moment after it reported that the new image is selected
            if (OtaStateReport.TryParse(reply, out var report) && report.State == OtaState.Activating && !_rebootPending)
            {
                _rebootPending = true;
                _rebootAt = Now + 400;
            }
        }
    }

    /// <summary>Power cut for <paramref name="offMs"/>: the receiver's RAM is gone (the flash content stays), the timer is offline until it is back.</summary>
    public void PowerCut(long offMs)
    {
        _powered = false;
        _rebootPending = false;
        _backAt = Now + offMs;
        Peer.PowerCut();
        Tr.OnTimerOnline(false, Now);
    }

    public void Run(long maxMs, Action<OtaPeerRig>? each = null)
    {
        long end = Now + maxMs;
        if (Tr.Phase == OtaPhase.NotStarted) Tr.Start(Now);
        while (!Tr.IsFinished && Now < end)
        {
            Step();
            each?.Invoke(this);
        }
    }

    /// <summary>Lets time pass without regard to the transfer (frames still on the wire arrive).</summary>
    public void Settle(long ms)
    {
        for (long end = Now + ms; Now < end;) Step();
    }

    private void Step()
    {
        Now += 5;
        if (!_powered && Now >= _backAt)
        {
            _powered = true;
            Boot++;
            Tr.OnTimerOnline(true, Now);
        }

        if (_powered)
        {
            FromTimer(Peer.Tick(Now));
            if (_rebootPending && Now >= _rebootAt)
            {
                // reboot into the new image: gone for a moment, then boot + 1 with the new version
                _rebootPending = false;
                _powered = false;
                _backAt = Now + 2500;
                Peer.PowerCut();
                Fw = Image.Version ?? Fw;
                Tr.OnTimerOnline(false, Now);
            }
        }

        while (_wire.TryPeek(out var item, out var key) && key.Due <= Now)
        {
            _wire.Dequeue();
            if (item.ToTimer)
            {
                if (_powered) FromTimer(Peer.Payload(item.Payload));
            }
            else
            {
                Tr.OnPayload(item.Payload, Now);
            }
        }

        Tr.OnTimerBoot(Boot, Fw, Now);
        Tr.Tick(Now);
    }

    public void Dispose() => Peer.Dispose();
}

public class OtaPeerCompatibilityTests
{
    private static OtaImage Image(int size = 24_000) => OtaTestImages.Create(size, "2.2.0");

    private static string Sha(OtaImage image) => Convert.ToHexString(image.Sha256).ToLowerInvariant();

    [Fact]
    public void The_receiver_program_takes_the_image_over_a_clean_link()
    {
        using var peer = OtaPeerProcess.Start();
        if (peer is null) return;                                       // the program has not been built
        using var rig = new OtaPeerRig(peer, Image());
        rig.Run(120_000);

        Assert.True(rig.Tr.Succeeded, rig.Tr.FailureMessage);
        var state = peer.Inspect();
        Assert.Equal(Sha(rig.Image), state.Hash);
        Assert.Equal("2.2.0", rig.Fw);
        Assert.Equal(8, rig.Boot);
        Assert.Equal(0, rig.Tr.Restarts);
    }

    [Theory]
    [InlineData(0)]
    [InlineData(1)]
    public void The_slot_that_is_not_running_receives_the_image(int runningSlot)
    {
        using var peer = OtaPeerProcess.Start(runningSlot: runningSlot);
        if (peer is null) return;
        using var rig = new OtaPeerRig(peer, Image(9_000));
        rig.Run(120_000);

        Assert.True(rig.Tr.Succeeded, rig.Tr.FailureMessage);
        Assert.Equal(Sha(rig.Image), peer.Inspect().Hash);
    }

    [Theory]
    [InlineData(224)]
    [InlineData(225)]
    [InlineData(4096)]
    [InlineData(4097)]
    public void Images_of_awkward_lengths_arrive_byte_exact(int length)
    {
        using var peer = OtaPeerProcess.Start();
        if (peer is null) return;
        var data = OtaTestImages.Create(12_000).Data[..length];
        using var rig = new OtaPeerRig(peer, new OtaImage(data, "2.2.0"));
        rig.Run(120_000);

        Assert.True(rig.Tr.Succeeded, rig.Tr.FailureMessage);
        Assert.Equal(Sha(rig.Image), peer.Inspect().Hash);
    }

    [Fact]
    public void Loss_duplicates_and_reordering_in_both_directions_are_survived()
    {
        using var peer = OtaPeerProcess.Start();
        if (peer is null) return;
        using var rig = new OtaPeerRig(peer, Image(40_000), seed: 5)
        {
            LossToTimer = 0.2, LossToPc = 0.2, DuplicateRate = 0.1, JitterMs = 40,
        };
        rig.Run(300_000);

        Assert.True(rig.Tr.Succeeded, rig.Tr.FailureMessage);
        Assert.True(rig.Tr.Resent > 0);
        Assert.Equal(Sha(rig.Image), peer.Inspect().Hash);
    }

    [Fact]
    public void A_power_cut_while_streaming_is_followed_by_a_new_session_and_a_complete_image()
    {
        using var peer = OtaPeerProcess.Start();
        if (peer is null) return;
        using var rig = new OtaPeerRig(peer, Image(60_000));
        bool cut = false;
        rig.Run(300_000, r =>
        {
            if (!cut && r.Tr.Contig > 40) { cut = true; r.PowerCut(6_000); }
        });

        Assert.True(cut);
        Assert.True(rig.Tr.Succeeded, rig.Tr.FailureMessage);
        Assert.True(rig.Tr.Restarts >= 1);
        Assert.Equal(Sha(rig.Image), peer.Inspect().Hash);
    }

    [Fact]
    public void A_different_key_is_refused_and_nothing_is_written()
    {
        using var peer = OtaPeerProcess.Start(key: "another-key");
        if (peer is null) return;
        using var rig = new OtaPeerRig(peer, Image());
        rig.Run(60_000);

        Assert.Equal(OtaPhase.Failed, rig.Tr.Phase);
        Assert.Equal(OtaError.AuthFailed, rig.Tr.TimerError);
        Assert.NotEqual(Sha(rig.Image), peer.Inspect().Hash);
    }

    [Fact]
    public void The_same_custom_key_on_both_sides_works()
    {
        using var peer = OtaPeerProcess.Start(key: "workshop-key");
        if (peer is null) return;
        using var rig = new OtaPeerRig(peer, Image(9_000), senderKey: "workshop-key");
        rig.Run(120_000);
        Assert.True(rig.Tr.Succeeded, rig.Tr.FailureMessage);
    }

    [Fact]
    public void A_timer_without_a_second_slot_is_reported_and_refuses_the_image()
    {
        using var peer = OtaPeerProcess.Start(noSlot: true);
        if (peer is null) return;
        using var rig = new OtaPeerRig(peer, Image());
        rig.Run(60_000);

        Assert.Equal(OtaPhase.Failed, rig.Tr.Phase);
        Assert.Equal(OtaError.NoSlot, rig.Tr.TimerError);
    }

    [Fact]
    public void An_image_that_does_not_fit_the_slot_is_refused()
    {
        using var peer = OtaPeerProcess.Start(slotSize: 16_384);
        if (peer is null) return;
        using var rig = new OtaPeerRig(peer, Image(24_000));
        rig.Run(60_000);

        Assert.Equal(OtaPhase.Failed, rig.Tr.Phase);
        Assert.Equal(OtaError.TooBig, rig.Tr.TimerError);
    }

    [Fact]
    public void Cancelling_sends_an_abort_that_returns_the_receiver_to_idle()
    {
        using var peer = OtaPeerProcess.Start();
        if (peer is null) return;
        using var rig = new OtaPeerRig(peer, Image(60_000));
        bool cancelled = false;
        rig.Run(60_000, r =>
        {
            if (!cancelled && r.Tr.Contig > 30) { cancelled = true; r.Tr.Cancel(r.Now); }
        });
        rig.Settle(200);                                                 // let the abort frame arrive

        Assert.Equal(OtaPhase.Cancelled, rig.Tr.Phase);
        Assert.Equal((int)OtaState.Idle, peer.Inspect().State);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void The_probe_tells_a_capable_timer_from_one_that_needs_a_usb_update(bool noSlot)
    {
        using var peer = OtaPeerProcess.Start(noSlot: noSlot);
        if (peer is null) return;
        long now = 0;
        OtaProbe? probe = null;
        probe = new OtaProbe(p =>
        {
            foreach (var reply in peer.Payload(p)) probe!.OnPayload(reply, now);
            return true;
        });
        probe.Start(now);
        while (!probe.IsFinished && now < 10_000) { now += 10; peer.Tick(now); probe.Tick(now); }

        Assert.Equal(noSlot ? OtaSupport.NeedsUsbUpdate : OtaSupport.Capable, probe.Result);
    }
}
