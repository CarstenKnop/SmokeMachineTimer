using FogConsole.Ota;
using FogConsole.Simulator;
using Xunit;

namespace FogConsole.Tests;

/// <summary>The sender (OtaTransfer) against the C# model of the timer in virtual time, with a bad link in between.</summary>
public class OtaTransferTests
{
    private static OtaRig Rig(int size = 24_000, string? senderKey = null, string? timerKey = null, OtaTransferOptions? options = null,
        int seed = 1, bool hasSlot = true, bool supported = true, int chunk = OtaFrames.DefaultChunk, string version = "2.2.0") =>
        new(OtaTestImages.Create(size, version, chunk: chunk), senderKey, timerKey, options, seed, hasSlot, supported);

    private static void AssertImageInSlot(OtaRig r)
    {
        Assert.Equal(r.Image.Data, r.Rx.SlotBytes);
    }

    // ---- the happy path -----------------------------------------------------------------------------------------------

    [Fact]
    public void A_clean_link_transfers_verifies_activates_and_sees_the_new_version()
    {
        var r = Rig();
        r.Run(120_000);

        Assert.Equal(OtaPhase.Done, r.Tr.Phase);
        Assert.True(r.Tr.Succeeded);
        AssertImageInSlot(r);
        Assert.Equal("2.2.0", r.Fw);
        Assert.Equal(8, r.Boot);                                        // the timer restarted once
        Assert.Equal(0, r.Tr.Restarts);
        Assert.Equal(r.Image.Chunks, r.Rx.AcceptedData);
        Assert.Equal(0, r.Rx.DuplicateData);
        Assert.Contains(r.Log, l => l.Contains("ready to receive"));
        Assert.Contains(r.Log, l => l.Contains("verified"));
    }

    [Fact]
    public void The_stages_follow_the_documented_order()
    {
        var r = Rig();
        var phases = new List<OtaPhase>();
        r.Run(120_000, rig => { if (phases.Count == 0 || phases[^1] != rig.Tr.Phase) phases.Add(rig.Tr.Phase); });
        Assert.Equal(new[] { OtaPhase.Beginning, OtaPhase.Erasing, OtaPhase.Streaming, OtaPhase.Verifying, OtaPhase.Activating, OtaPhase.Restarting, OtaPhase.Done }, phases);
    }

    [Fact]
    public void Progress_speed_and_eta_are_reported_while_streaming()
    {
        var r = Rig(size: 1_500_000);
        r.SendLimit = 1;                                   // a slow link: 200 frames per virtual second, so that the transfer takes a while
        OtaProgress? mid = null;
        r.Run(900_000, rig =>
        {
            var s = rig.Tr.Snapshot(rig.Now);
            if (mid is null && s.Phase == OtaPhase.Streaming && s.Fraction > 0.5) mid = s;
        });
        Assert.NotNull(mid);
        Assert.InRange(mid!.Fraction, 0.5, 0.95);
        Assert.True(mid.SpeedBytesPerSecond > 1000, $"speed {mid.SpeedBytesPerSecond}");
        Assert.NotNull(mid.EtaSeconds);
        Assert.StartsWith("Sending", mid.Stage);
        Assert.Equal(r.Image.Size, mid.BytesTotal);
    }

    /// <summary>The estimate of the time left as the progress panel shows it, against what really remained.</summary>
    private static (List<(long Now, double Eta)> Shown, long StreamEnd, long First) RecordEstimates(OtaRig r, long maxMs)
    {
        var shown = new List<(long, double)>();
        long first = -1, end = -1;
        r.Run(maxMs, rig =>
        {
            var s = rig.Tr.Snapshot(rig.Now);
            if (s.Phase == OtaPhase.Streaming)
            {
                if (first < 0) first = rig.Now;
                if (s.EtaSeconds is { } e) shown.Add((rig.Now, e));
            }
            else if (first >= 0 && end < 0) end = rig.Now;
        });
        return (shown, end, first);
    }

    [Theory]
    [InlineData(0.0, 0.0, 0.10)]
    [InlineData(0.10, 0.10, 0.25)]
    public void The_time_left_stays_close_to_the_truth_and_is_not_shown_while_the_pipe_fills(double toTimer, double toPc, double tolerance)
    {
        var r = Rig(size: 1_500_000, options: new OtaTransferOptions { TailSeconds = 0 }, seed: 7);
        r.SendLimit = 1;
        r.LossToTimer = toTimer;
        r.LossToPc = toPc;
        var (shown, end, first) = RecordEstimates(r, 900_000);

        Assert.True(r.Tr.Succeeded, r.Tr.FailureMessage);
        Assert.True(end > first + 20_000, "the transfer must last long enough to be worth an estimate");
        Assert.NotEmpty(shown);
        Assert.True(shown[0].Now - first >= 5_000, $"the first estimate came after {shown[0].Now - first} ms: the start-up ramp must not be extrapolated");
        foreach (var (now, eta) in shown)
        {
            double truth = (end - now) / 1000.0;
            if (truth < 3) continue;
            Assert.True(Math.Abs(eta - truth) <= truth * tolerance + 1, $"{(now - first) / 1000.0:0.0} s into the transfer: shown {eta:0.0} s left, really {truth:0.0} s");
        }
    }

    // ---- one restart of the timer is one restart of the transfer ---------------------------------------------------------

    [Theory]
    [InlineData(0)]
    [InlineData(400)]
    [InlineData(1500)]
    public void A_restart_noticed_by_the_state_report_and_by_the_fleet_list_restarts_the_transfer_once(int fleetListLagMs)
    {
        var r = Rig(size: 300_000);
        r.SendLimit = 1;                                   // a link that takes some seconds for the image, as a real one does
        r.BootReportDelayMs = fleetListLagMs;
        bool cut = false;
        r.Run(300_000, rig =>
        {
            if (!cut && rig.Tr.Phase == OtaPhase.Streaming && rig.Tr.Contig > rig.Image.Chunks / 3) { cut = true; rig.PowerCut(300); }
        });

        Assert.True(cut);
        Assert.True(r.Tr.Succeeded, r.Tr.FailureMessage);
        Assert.Equal(1, r.Tr.Restarts);
        AssertImageInSlot(r);
        Assert.DoesNotContain(r.Log, l => l.Contains("busy with another"));
    }

    [Fact]
    public void A_timer_that_is_busy_with_our_own_earlier_transfer_is_carried_on_with_not_given_up_on()
    {
        var r = Rig(size: 60_000);
        bool poked = false;
        r.Run(300_000, rig =>
        {
            // a false alarm about a restart while the timer still holds the running transfer: the new Begin is answered "busy" with OUR first session
            if (!poked && rig.Tr.Phase == OtaPhase.Streaming && rig.Tr.Contig > rig.Image.Chunks / 3)
            {
                poked = true;
                rig.Tr.OnTimerBoot(rig.Boot + 5, rig.Fw, rig.Now);
            }
        });

        Assert.True(poked);
        Assert.True(r.Tr.Succeeded, r.Tr.FailureMessage);
        Assert.Contains(r.Log, l => l.Contains("carrying on with it"));
        AssertImageInSlot(r);
    }

    [Fact]
    public void The_time_left_includes_what_follows_the_last_chunk()
    {
        var r = Rig(size: 1_500_000, options: new OtaTransferOptions { TailSeconds = 8 });
        r.SendLimit = 1;
        var (shown, end, _) = RecordEstimates(r, 900_000);
        var last = shown[^1];
        Assert.InRange(last.Eta, 8, 8 + 3);                         // almost all of it sent: the tail (check, switch, restart) is what is left
        Assert.True(shown.Count > 10);
        Assert.True(end > 0);
    }

    [Theory]
    [InlineData(224)]                                  // exactly one chunk
    [InlineData(225)]                                  // one byte into the second
    [InlineData(448)]
    [InlineData(671)]
    [InlineData(3_000)]
    public void An_image_of_any_length_arrives_byte_exact(int length)
    {
        var data = OtaTestImages.Create(8_000).Data[..length];
        var img = new OtaImage(data, "2.2.0");
        var r = new OtaRig(img);
        r.Run(120_000);
        Assert.True(r.Tr.Succeeded, r.Tr.FailureMessage);
        Assert.Equal(data, r.Rx.SlotBytes);
        Assert.Equal((length + 223) / 224, img.Chunks);
    }

    // ---- bad links ----------------------------------------------------------------------------------------------------

    [Theory]
    [InlineData(0.10, 0.10, 1)]
    [InlineData(0.25, 0.25, 2)]
    [InlineData(0.40, 0.30, 3)]
    public void Loss_in_both_directions_costs_re_sends_but_the_image_arrives(double toTimer, double toPc, int seed)
    {
        var r = Rig(size: 30_000, seed: seed);
        r.LossToTimer = toTimer;
        r.LossToPc = toPc;
        r.Run(600_000);

        Assert.True(r.Tr.Succeeded, r.Tr.FailureMessage);
        AssertImageInSlot(r);
        Assert.True(r.Tr.Resent > 0);
        Assert.Equal("2.2.0", r.Fw);
    }

    [Fact]
    public void Reordered_delayed_and_duplicated_frames_do_no_harm()
    {
        var r = Rig(size: 30_000, seed: 5);
        r.JitterMs = 120;                      // frames overtake each other
        r.LatencyMs = 40;
        r.DuplicateRate = 0.25;
        r.LossToTimer = 0.05;
        r.Run(600_000);

        Assert.True(r.Tr.Succeeded, r.Tr.FailureMessage);
        AssertImageInSlot(r);
        Assert.True(r.Rx.DuplicateData > 0, "the duplicates must have reached the timer");
    }

    [Fact]
    public void A_very_slow_link_still_works()
    {
        var r = Rig(size: 12_000);
        r.LatencyMs = 450;                     // far more than the re-send time: everything is sent twice, nothing breaks
        r.Run(600_000);
        Assert.True(r.Tr.Succeeded, r.Tr.FailureMessage);
        AssertImageInSlot(r);
    }

    [Fact]
    public void A_link_that_accepts_only_a_few_frames_at_a_time_is_fed_as_fast_as_it_takes()
    {
        var r = Rig(size: 24_000);
        r.SendLimit = 3;
        r.Run(300_000);
        Assert.True(r.Tr.Succeeded, r.Tr.FailureMessage);
        AssertImageInSlot(r);
    }

    [Fact]
    public void A_missing_chunk_is_sent_again_quickly_when_later_ones_arrived()
    {
        var r = Rig(size: 24_000);
        int seen = 0;
        long first = -1, again = -1;
        r.Mangle = p =>
        {
            if (p[0] == OtaOp.Data && p[2] == 10 && p[3] == 0)
            {
                if (++seen == 1) { first = r.Now; return Array.Empty<byte>(); }       // chunk 10 is lost the first time
                again = r.Now;
            }
            return p;
        };
        r.Run(120_000);
        Assert.Equal(2, seen);
        Assert.True(r.Tr.Succeeded, r.Tr.FailureMessage);
        Assert.InRange(again - first, 1, 400);              // the gap was noticed long before any stall time
        AssertImageInSlot(r);
    }

    // ---- refusals -----------------------------------------------------------------------------------------------------

    [Fact]
    public void A_wrong_key_is_refused_and_the_timer_is_not_touched()
    {
        var r = Rig(senderKey: "not-the-key");
        r.Run(60_000);

        Assert.Equal(OtaPhase.Failed, r.Tr.Phase);
        Assert.Equal(OtaError.AuthFailed, r.Tr.TimerError);
        Assert.Contains("key", r.Tr.FailureMessage);
        Assert.Equal(OtaState.Idle, r.Rx.State);
        Assert.Empty(r.Rx.SlotBytes);
    }

    [Fact]
    public void The_same_custom_key_on_both_sides_works()
    {
        var r = Rig(senderKey: "my own secret", timerKey: "my own secret");
        r.Run(120_000);
        Assert.True(r.Tr.Succeeded, r.Tr.FailureMessage);
    }

    [Fact]
    public void A_timer_without_a_second_slot_says_it_needs_a_usb_update_first()
    {
        var r = Rig(hasSlot: false);
        r.Run(60_000);
        Assert.Equal(OtaPhase.Failed, r.Tr.Phase);
        Assert.Equal(OtaError.NoSlot, r.Tr.TimerError);
        Assert.Contains("USB", r.Tr.FailureMessage);
    }

    [Fact]
    public void Firmware_without_ota_support_never_answers_and_the_message_says_what_to_check()
    {
        var r = Rig(supported: false);
        r.Run(60_000);
        Assert.Equal(OtaPhase.Failed, r.Tr.Phase);
        Assert.Contains("did not answer", r.Tr.FailureMessage);
        Assert.Contains("over-the-air support", r.Tr.FailureMessage);
    }

    [Fact]
    public void An_image_that_does_not_fit_the_slot_is_refused()
    {
        var r = new OtaRig(OtaTestImages.Create(24_000));
        var small = new SimOtaReceiver(slotSize: 10_000) { EraseMsPerSector = 20 };
        // talk to a timer whose slot is smaller than the image
        var replies = small.OnFrame(OtaFrames.Begin(new OtaBegin(7, (uint)r.Image.Size, r.Image.Sha256, 224, 2, 2, 0), OtaAuth.KeyBytes(null)), 0);
        Assert.True(OtaStateReport.TryParse(replies[0], out var rep));
        Assert.Equal(OtaError.TooBig, rep.Error);
        Assert.Equal(OtaState.Idle, small.State);
    }

    [Fact]
    public void Another_transfer_in_progress_makes_the_timer_busy()
    {
        var r = Rig();
        // somebody else got there first
        var other = new OtaBegin(0x77, (uint)r.Image.Size, r.Image.Sha256, 224, 2, 2, 0);
        r.Rx.OnFrame(OtaFrames.Begin(other, OtaAuth.KeyBytes(null)), 0);
        r.Run(60_000);
        Assert.Equal(OtaPhase.Failed, r.Tr.Phase);
        Assert.Equal(OtaError.Busy, r.Tr.TimerError);
        Assert.Contains("busy", r.Tr.FailureMessage, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void A_damaged_transfer_is_caught_by_the_sha_check_and_the_old_firmware_stays()
    {
        var r = Rig(size: 20_000);
        r.Mangle = p =>
        {
            // a bit flips inside chunk 12 on the way (the radio's CRC would normally catch this; here it must be the SHA-256)
            if (p[0] == OtaOp.Data && p[2] == 12 && p[3] == 0) { p = (byte[])p.Clone(); p[40] ^= 0x01; }
            return p;
        };
        r.Run(120_000);
        Assert.Equal(OtaPhase.Failed, r.Tr.Phase);
        Assert.Equal(OtaError.HashMismatch, r.Tr.TimerError);
        Assert.Contains("SHA-256", r.Tr.FailureMessage);
        Assert.Equal("2.1.0", r.Fw);                       // nothing was activated
        Assert.Equal(7, r.Boot);
    }

    [Fact]
    public void An_image_that_is_not_for_the_timer_is_refused_before_it_is_written()
    {
        // an ESP32-S3 image (chip id 9): chunk 0 reveals it
        var data = Flashing.DemoFirmware.CreateImage(Flashing.FirmwareKind.Timer, "2.2.0", 20_000, chipId: 9);
        var img = new OtaImage(data, "2.2.0");
        var r = new OtaRig(img);
        r.Run(60_000);
        Assert.Equal(OtaPhase.Failed, r.Tr.Phase);
        Assert.Equal(OtaError.BadImage, r.Tr.TimerError);
        Assert.Contains("not a firmware for the ESP32-C3 timer", r.Tr.FailureMessage);
    }

    [Fact]
    public void A_remote_firmware_cannot_be_put_on_a_timer()
    {
        var data = Flashing.DemoFirmware.CreateImage(Flashing.FirmwareKind.Remote, "2.2.0", 20_000);
        var r = new OtaRig(new OtaImage(data, "2.2.0"));
        r.Run(120_000);
        Assert.Equal(OtaPhase.Failed, r.Tr.Phase);
        Assert.Equal(OtaError.BadImage, r.Tr.TimerError);
        Assert.NotEqual("2.2.0", r.Fw);
    }

    // ---- cancel ---------------------------------------------------------------------------------------------------------

    [Fact]
    public void Cancelling_sends_an_abort_and_the_timer_returns_to_idle()
    {
        var r = Rig(size: 60_000);
        r.Run(120_000, rig =>
        {
            if (rig.Rx.State == OtaState.Receiving && rig.Rx.Contig > 40 && rig.Tr.Phase == OtaPhase.Streaming) rig.Tr.Cancel(rig.Now);
        });
        for (int i = 0; i < 20; i++) r.Step();               // let the abort travel
        Assert.Equal(OtaPhase.Cancelled, r.Tr.Phase);
        Assert.Equal(OtaState.Idle, r.Rx.State);
        Assert.Equal("2.1.0", r.Fw);
    }

    [Fact]
    public void Cancelling_a_finished_transfer_changes_nothing()
    {
        var r = Rig();
        r.Run(120_000);
        r.Tr.Cancel(r.Now);
        Assert.Equal(OtaPhase.Done, r.Tr.Phase);
    }

    // ---- power cuts -------------------------------------------------------------------------------------------------------

    [Fact]
    public void A_power_cut_in_the_middle_loses_the_transfer_and_it_is_started_again_when_the_timer_is_back()
    {
        var r = Rig(size: 40_000);
        bool cut = false;
        r.Run(600_000, rig =>
        {
            if (!cut && rig.Rx.Contig > 60) { cut = true; rig.PowerCut(8_000); }
        });
        Assert.True(cut);
        Assert.True(r.Tr.Succeeded, r.Tr.FailureMessage);
        Assert.Equal(1, r.Tr.Restarts);
        AssertImageInSlot(r);
        Assert.Equal("2.2.0", r.Fw);
        Assert.Equal(9, r.Boot);                       // power cut + the reboot into the new image
        Assert.Contains(r.Log, l => l.Contains("restarted during the transfer"));
    }

    [Fact]
    public void A_timer_that_keeps_losing_power_ends_the_transfer_after_three_new_starts()
    {
        var r = Rig(size: 40_000, options: new OtaTransferOptions { MaxRestarts = 3 });
        int cuts = 0;
        r.Run(2_000_000, rig =>
        {
            if (rig.Powered && rig.Rx.State == OtaState.Receiving && rig.Rx.Contig > 20 && cuts < 6) { cuts++; rig.PowerCut(3_000); }
        });
        Assert.Equal(OtaPhase.Failed, r.Tr.Phase);
        Assert.Equal(3, r.Tr.Restarts);
        Assert.Contains("3 times", r.Tr.FailureMessage);
    }

    [Fact]
    public void A_timer_that_stays_offline_too_long_ends_the_transfer_with_an_explanation()
    {
        var r = Rig(size: 40_000, options: new OtaTransferOptions { OfflineGiveUpMs = 10_000 });
        bool cut = false;
        r.Run(600_000, rig => { if (!cut && rig.Rx.Contig > 30) { cut = true; rig.PowerCut(600_000); } });
        Assert.Equal(OtaPhase.Failed, r.Tr.Phase);
        Assert.Contains("offline", r.Tr.FailureMessage);
        Assert.Contains("heats up", r.Tr.FailureMessage);
    }

    [Fact]
    public void A_dead_link_for_longer_than_the_timers_patience_makes_it_drop_the_session_and_the_sender_starts_over()
    {
        var r = Rig(size: 30_000, options: new OtaTransferOptions { StallTimeoutMs = 120_000, MaxRestarts = 2 });
        bool cut = false, back = false;
        r.Run(900_000, rig =>
        {
            if (!cut && rig.Rx.Contig > 40) { cut = true; rig.Blackhole = true; }
            if (cut && !back && rig.Now > 0 && rig.Rx.State == OtaState.Idle) { back = true; rig.Blackhole = false; }   // the timer gave up after 30 s of silence
        });
        Assert.True(cut && back, "the timer must have dropped the session");
        Assert.True(r.Tr.Succeeded, r.Tr.FailureMessage);
        Assert.Equal(1, r.Tr.Restarts);
        AssertImageInSlot(r);
    }

    [Fact]
    public void An_online_timer_that_stops_answering_fails_the_transfer_after_the_stall_time()
    {
        var r = Rig(size: 30_000, options: new OtaTransferOptions { StallTimeoutMs = 8_000 });
        bool cut = false;
        r.Run(900_000, rig => { if (!cut && rig.Rx.Contig > 30) { cut = true; rig.Blackhole = true; } });
        Assert.Equal(OtaPhase.Failed, r.Tr.Phase);
        Assert.Contains("stopped answering", r.Tr.FailureMessage);
    }

    // ---- the end -------------------------------------------------------------------------------------------------------

    [Fact]
    public void A_lost_activate_is_repeated()
    {
        var r = Rig();
        int swallowed = 0;
        r.Mangle = p =>
        {
            if (p[0] == OtaOp.Activate && swallowed < 2) { swallowed++; return Array.Empty<byte>(); }
            return p;
        };
        r.Run(120_000);
        Assert.True(swallowed >= 1);
        Assert.True(r.Tr.Succeeded, r.Tr.FailureMessage);
    }

    [Fact]
    public void A_timer_that_comes_back_with_the_old_version_is_reported_as_rolled_back()
    {
        var r = Rig();
        r.FwAfterActivate = "2.1.0";                   // the boot loader went back to the old image
        r.Run(120_000);
        Assert.Equal(OtaPhase.Failed, r.Tr.Phase);
        Assert.Contains("went back to the old firmware", r.Tr.FailureMessage);
        Assert.Contains("2.1.0", r.Tr.FailureMessage);
    }

    [Fact]
    public void The_real_remote_reports_only_major_and_minor_which_is_enough()
    {
        var r = Rig();
        r.FwAfterActivate = "2.2";
        r.Run(120_000);
        Assert.True(r.Tr.Succeeded, r.Tr.FailureMessage);
    }

    [Fact]
    public void A_transfer_cannot_be_started_twice()
    {
        var r = Rig();
        r.Tr.Start(0);
        Assert.Throws<InvalidOperationException>(() => r.Tr.Start(0));
    }

    [Fact]
    public void Late_answers_of_an_older_session_are_ignored()
    {
        var r = Rig();
        r.Tr.Start(0);
        var stale = new OtaStateReport((byte)(r.Tr.Session + 5), OtaState.Receiving, OtaError.None, 5, 0, 100, 23, 1).ToBytes();
        r.Tr.OnPayload(stale, 10);
        Assert.Equal(OtaPhase.Beginning, r.Tr.Phase);
        Assert.Null(r.Tr.LastReport);
    }
}

/// <summary>The C# model of the timer's receiver (docs/OTA_UPDATE.md, 6.1) on its own.</summary>
public class SimOtaReceiverTests
{
    private static OtaImage Image(int size = 12_000) => OtaTestImages.Create(size);

    private static byte[] Begin(OtaImage img, byte session = 7, string? key = null) =>
        OtaFrames.Begin(new OtaBegin(session, (uint)img.Size, img.Sha256, img.Chunk, img.Major, img.Minor, img.Patch), OtaAuth.KeyBytes(key));

    private static OtaStateReport Only(List<byte[]> replies)
    {
        Assert.Single(replies);
        Assert.True(OtaStateReport.TryParse(replies[0], out var r));
        return r;
    }

    private static SimOtaReceiver Ready(OtaImage img, out long now)
    {
        var rx = new SimOtaReceiver { EraseMsPerSector = 10 };
        now = 0;
        rx.OnFrame(Begin(img), now);
        while (rx.State == OtaState.Erasing) { now += 10; rx.Tick(now); }
        return rx;
    }

    [Fact]
    public void The_probe_of_an_idle_timer_says_ready()
    {
        var rx = new SimOtaReceiver();
        var r = Only(rx.OnFrame(OtaFrames.Poll(0), 0));
        Assert.Equal(OtaState.Idle, r.State);
        Assert.Equal(OtaError.None, r.Error);
        Assert.Equal(0, r.Session);
    }

    [Fact]
    public void The_probe_of_a_timer_without_a_second_slot_says_no_slot_and_firmware_without_ota_says_nothing()
    {
        Assert.Equal(OtaError.NoSlot, Only(new SimOtaReceiver(hasSlot: false).OnFrame(OtaFrames.Poll(0), 0)).Error);
        Assert.Empty(new SimOtaReceiver(supported: false).OnFrame(OtaFrames.Poll(0), 0));
    }

    [Fact]
    public void Begin_starts_erasing_and_reports_progress_until_it_is_ready()
    {
        var img = Image(20_000);
        var rx = new SimOtaReceiver { EraseMsPerSector = 10 };
        var first = Only(rx.OnFrame(Begin(img), 0));
        Assert.Equal(OtaState.Erasing, first.State);
        Assert.Equal(img.Chunks, first.Total);
        Assert.Equal(1, first.Slot);                          // running slot 0: the other one is written

        var mid = rx.Tick(30);                               // 3 of 5 sectors
        Assert.Empty(mid);
        Assert.Equal(60, Only(rx.OnFrame(OtaFrames.Poll(7), 31)).Progress);

        var done = rx.Tick(60);
        Assert.Equal(OtaState.Receiving, Only(done).State);  // announced on its own
    }

    [Fact]
    public void A_repeated_begin_of_the_same_session_only_reports_and_keeps_what_was_received()
    {
        var img = Image();
        var rx = Ready(img, out long now);
        rx.OnFrame(OtaFrames.Data(7, 0, img.ChunkBytes(0)), now);
        rx.OnFrame(OtaFrames.Data(7, 1, img.ChunkBytes(1)), now);
        var r = Only(rx.OnFrame(Begin(img), now + 5));
        Assert.Equal(OtaState.Receiving, r.State);
        Assert.Equal(2, r.Contig);
    }

    [Fact]
    public void Another_begin_while_a_transfer_runs_is_busy()
    {
        var img = Image();
        var rx = Ready(img, out long now);
        var r = Only(rx.OnFrame(Begin(img, session: 9), now));
        Assert.Equal(OtaError.Busy, r.Error);
        Assert.Equal(7, r.Session);                           // the session that holds the timer
        Assert.Equal(OtaState.Receiving, rx.State);
    }

    [Fact]
    public void A_wrong_key_changes_nothing_on_the_timer()
    {
        var img = Image();
        var rx = new SimOtaReceiver();
        var r = Only(rx.OnFrame(Begin(img, key: "bad"), 0));
        Assert.Equal(OtaError.AuthFailed, r.Error);
        Assert.Equal(OtaState.Idle, rx.State);

        // ... also while a legitimate transfer is running
        var ok = Ready(img, out long now);
        Only(ok.OnFrame(Begin(img, session: 9, key: "bad"), now));
        Assert.Equal(OtaState.Receiving, ok.State);
        Assert.Equal(7, ok.Session);
    }

    [Fact]
    public void Chunks_arrive_in_any_order_and_the_report_tells_the_gaps()
    {
        var img = Image();
        var rx = Ready(img, out long now);
        foreach (int seq in new[] { 0, 1, 3, 4, 7 }) rx.OnFrame(OtaFrames.Data(7, seq, img.ChunkBytes(seq)), now);
        var r = Only(rx.OnFrame(OtaFrames.Poll(7), now));
        Assert.Equal(2, r.Contig);
        Assert.True(r.Has(3) && r.Has(4) && r.Has(7));
        Assert.False(r.Has(2) || r.Has(5) || r.Has(6));
        Assert.Equal(5, rx.ReceivedChunks);
    }

    [Fact]
    public void A_state_is_sent_after_eight_frames_and_when_a_chunk_arrives_out_of_order_but_not_more_often_than_every_20_ms()
    {
        var img = Image();
        var rx = Ready(img, out long now);
        now += 100;
        int states = 0;
        for (int seq = 0; seq < 7; seq++) states += rx.OnFrame(OtaFrames.Data(7, seq, img.ChunkBytes(seq)), now).Count;
        Assert.Equal(0, states);                              // 7 in order: nothing yet
        states += rx.OnFrame(OtaFrames.Data(7, 7, img.ChunkBytes(7)), now).Count;
        Assert.Equal(1, states);                              // the 8th
        Assert.Empty(rx.OnFrame(OtaFrames.Data(7, 20, img.ChunkBytes(20)), now + 5));            // out of order, but within 20 ms of the last report
        var late = rx.Tick(now + 25);                         // ... it follows on the next tick
        Assert.Single(late);
    }

    [Fact]
    public void Chunk_zero_must_be_an_esp32_c3_image()
    {
        var img = Image();
        var rx = Ready(img, out long now);
        var bad = (byte[])img.ChunkBytes(0).ToArray().Clone();
        bad[0] = 0x00;
        var r = Only(rx.OnFrame(OtaFrames.Data(7, 0, bad), now));
        Assert.Equal(OtaState.Failed, r.State);
        Assert.Equal(OtaError.BadImage, r.Error);
    }

    [Fact]
    public void Data_with_a_wrong_length_or_number_is_ignored()
    {
        var img = Image();
        var rx = Ready(img, out long now);
        Assert.Empty(rx.OnFrame(OtaFrames.Data(7, 0, img.ChunkBytes(0)[..100]), now));
        Assert.Empty(rx.OnFrame(OtaFrames.Data(7, 500, img.ChunkBytes(0)), now));
        Assert.Equal(0, rx.ReceivedChunks);
    }

    [Fact]
    public void Verify_before_everything_arrived_is_a_bad_request_and_nothing_breaks()
    {
        var img = Image();
        var rx = Ready(img, out long now);
        rx.OnFrame(OtaFrames.Data(7, 0, img.ChunkBytes(0)), now);
        var r = Only(rx.OnFrame(OtaFrames.Verify(7), now));
        Assert.Equal(OtaError.BadRequest, r.Error);
        Assert.Equal(OtaState.Receiving, rx.State);
        Assert.Equal(OtaError.None, rx.Error);
    }

    [Fact]
    public void Activate_is_only_valid_after_a_successful_check_and_needs_its_magic()
    {
        var img = Image();
        var rx = Ready(img, out long now);
        Assert.Equal(OtaError.BadRequest, Only(rx.OnFrame(OtaFrames.Activate(7), now)).Error);

        for (int s = 0; s < img.Chunks; s++) rx.OnFrame(OtaFrames.Data(7, s, img.ChunkBytes(s)), now);
        rx.OnFrame(OtaFrames.Verify(7), now);
        for (int i = 0; i < 40 && rx.State == OtaState.Verifying; i++) rx.Tick(now += 5);
        Assert.Equal(OtaState.Verified, rx.State);

        Assert.Equal(OtaError.BadRequest, Only(rx.OnFrame(new byte[] { OtaOp.Activate, 7, 0x00 }, now)).Error);     // wrong magic
        Assert.Equal(OtaState.Verified, rx.State);
        Assert.Equal(OtaState.Activating, Only(rx.OnFrame(OtaFrames.Activate(7), now)).State);
        Assert.Null(rx.PendingActivation);                    // the reboot follows a moment later
        rx.Tick(now + 400);
        Assert.Equal((2, 2, 0), rx.TakePendingActivation() is { } v ? (v.Major, v.Minor, v.Patch) : (0, 0, 0));
        Assert.Equal(OtaState.Idle, rx.State);
    }

    [Fact]
    public void Thirty_seconds_of_silence_drop_the_session_and_the_next_answer_says_so()
    {
        var img = Image();
        var rx = Ready(img, out long now);
        rx.OnFrame(OtaFrames.Data(7, 0, img.ChunkBytes(0)), now);
        rx.Tick(now + 29_000);
        Assert.Equal(OtaState.Receiving, rx.State);
        rx.Tick(now + 30_000);
        Assert.Equal(OtaState.Idle, rx.State);
        var r = Only(rx.OnFrame(OtaFrames.Poll(7), now + 31_000));
        Assert.Equal(OtaError.Timeout, r.Error);
        Assert.Equal(OtaError.None, Only(rx.OnFrame(OtaFrames.Poll(7), now + 31_100)).Error);   // said once
    }

    [Fact]
    public void A_power_loss_forgets_the_transfer_but_not_the_flash()
    {
        var img = Image();
        var rx = Ready(img, out long now);
        for (int s = 0; s < 5; s++) rx.OnFrame(OtaFrames.Data(7, s, img.ChunkBytes(s)), now);
        rx.PowerLoss();
        Assert.Equal(OtaState.Idle, rx.State);
        Assert.Equal(0, rx.Total);
        Assert.Equal(OtaState.Idle, Only(rx.OnFrame(OtaFrames.Poll(7), now)).State);
    }

    [Fact]
    public void Abort_returns_to_idle()
    {
        var img = Image();
        var rx = Ready(img, out long now);
        Only(rx.OnFrame(OtaFrames.Abort(7), now));
        Assert.Equal(OtaState.Idle, rx.State);
        Assert.Equal(OtaState.Idle, Only(rx.OnFrame(OtaFrames.Abort(7), now)).State);            // harmless twice
    }

    [Fact]
    public void Frames_of_a_wrong_session_are_answered_with_the_session_that_holds_the_timer()
    {
        var img = Image();
        var rx = Ready(img, out long now);
        var r = Only(rx.OnFrame(OtaFrames.Data(9, 0, img.ChunkBytes(0)), now));
        Assert.Equal(OtaError.Busy, r.Error);
        Assert.Equal(7, r.Session);
        Assert.Equal(0, rx.ReceivedChunks);
    }

    [Fact]
    public void The_running_slot_decides_which_slot_is_written()
    {
        var img = Image();
        var rx = new SimOtaReceiver { RunningSlot = 1 };
        Assert.Equal(0, Only(rx.OnFrame(Begin(img), 0)).Slot);
    }
}

public class OtaProbeTests
{
    private static (OtaProbe Probe, SimOtaReceiver Rx, List<long> Sent) Setup(SimOtaReceiver rx)
    {
        var sent = new List<long>();
        long clock = 0;
        var probe = new OtaProbe(p => { sent.Add(clock); return true; });
        return (probe, rx, sent);
    }

    private static OtaSupport Run(SimOtaReceiver rx, out int polls, double loss = 0, int seed = 1)
    {
        var rng = new Random(seed);
        int count = 0;
        long now = 0;
        OtaProbe? probe = null;
        probe = new OtaProbe(p =>
        {
            count++;
            if (rng.NextDouble() >= loss)
                foreach (var reply in rx.OnFrame(p, now))
                    if (rng.NextDouble() >= loss) probe!.OnPayload(reply, now);
            return true;
        });
        probe.Start(now);
        while (!probe.IsFinished && now < 20_000) { now += 10; probe.Tick(now); }
        polls = count;
        return probe.Result;
    }

    [Fact]
    public void A_ready_timer_is_capable_after_one_poll()
    {
        Assert.Equal(OtaSupport.Capable, Run(new SimOtaReceiver(), out int polls));
        Assert.Equal(1, polls);
    }

    [Fact]
    public void A_timer_without_a_second_slot_needs_a_usb_update()
    {
        Assert.Equal(OtaSupport.NeedsUsbUpdate, Run(new SimOtaReceiver(hasSlot: false), out _));
    }

    [Fact]
    public void Silence_means_no_ota_firmware_after_six_tries()
    {
        Assert.Equal(OtaSupport.NoOtaFirmware, Run(new SimOtaReceiver(supported: false), out int polls));
        Assert.Equal(6, polls);
    }

    [Fact]
    public void A_busy_timer_is_reported_busy()
    {
        var rx = new SimOtaReceiver { EraseMsPerSector = 10 };
        var img = OtaTestImages.Create();
        rx.OnFrame(OtaFrames.Begin(new OtaBegin(7, (uint)img.Size, img.Sha256, 224, 2, 2, 0), OtaAuth.KeyBytes(null)), 0);
        Assert.Equal(OtaSupport.Busy, Run(rx, out _));
    }

    [Fact]
    public void A_lossy_link_is_survived_by_the_retries()
    {
        Assert.Equal(OtaSupport.Capable, Run(new SimOtaReceiver(), out int polls, loss: 0.4, seed: 4));
        Assert.True(polls >= 1);
    }
}
