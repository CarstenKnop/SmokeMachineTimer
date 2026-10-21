using FogConsole.Hub;
using FogConsole.Ota;
using FogConsole.Protocol;
using FogConsole.Simulator;
using Xunit;

namespace FogConsole.Tests;

/// <summary>A hub connected to the simulated remote (real time, the simulator's clock runs a few times faster).</summary>
internal sealed class OtaHubRig : IAsyncDisposable
{
    public SimDevice Device { get; }
    public HubClient Hub { get; }
    public HubOtaLink Link { get; }
    public HubFleetWatcher Fleet { get; }

    private OtaHubRig(SimDevice device, HubClient hub)
    {
        Device = device;
        Hub = hub;
        Link = new HubOtaLink(hub);
        Fleet = new HubFleetWatcher(hub);
    }

    public static SimOptions Options(SimKind kind = SimKind.Remote, string? key = null, int noSlot = 4, int legacy = 6, double speed = 4) => new()
    {
        Seed = 11, Chaos = false, Demo = false, Kind = kind, PairedTimers = 7, TotalTimers = 10, Speed = speed,
        OtaKey = key, OtaNoSlotIndex = noSlot, OtaLegacyIndex = legacy,
    };

    public static async Task<OtaHubRig> StartAsync(SimOptions? options = null)
    {
        var device = new SimDevice(options ?? Options());
        var hub = new HubClient(new HubOptions { KeepAlive = false });
        await hub.ConnectAsync(() => new SimTransport(device));
        var rig = new OtaHubRig(device, hub);
        await rig.Fleet.EnsureAsync();
        return rig;
    }

    public OtaUpdater NewUpdater(string? key = null, OtaUpdaterOptions? options = null) => new(Link, Fleet, key, options);

    public OtaTimerTarget Target(int index)
    {
        var d = Fleet.Find(Mac(index)) ?? throw new InvalidOperationException("timer not in the fleet");
        return new OtaTimerTarget(d.Mac, d.Name, Hub.Hello!.Kind == DeviceKind.Timer ? CommandTarget.Dev(0) : CommandTarget.ByMac(d.Mac));
    }

    public static string Mac(int index) => $"AA:BB:CC:DD:EE:{index + 1:X2}";

    public async Task WaitAsync(Func<bool> condition, int ms = 8000)
    {
        for (int i = 0; i < ms / 20 && !condition(); i++) await Task.Delay(20);
        Assert.True(condition(), "timed out waiting for a condition");
    }

    public async ValueTask DisposeAsync()
    {
        Fleet.Dispose();
        Link.Dispose();
        await Hub.DisposeAsync();
    }
}

public class OtaHubTests
{
    private static OtaImage Image(int size = 16_000, string version = "2.2.0") => OtaTestImages.Create(size, version);

    [Fact]
    public async Task The_probe_tells_every_kind_of_timer_apart()
    {
        await using var rig = await OtaHubRig.StartAsync();
        using var updater = rig.NewUpdater();
        var seen = new List<(string, OtaSupport)>();
        updater.SupportChecked += (t, s) => { lock (seen) seen.Add((t.Name, s)); };

        var result = await updater.ProbeAsync(Enumerable.Range(0, 7).Select(rig.Target).ToList(), CancellationToken.None);

        Assert.Equal(OtaSupport.Capable, result[OtaHubRig.Mac(0)]);
        Assert.Equal(OtaSupport.Capable, result[OtaHubRig.Mac(5)]);
        Assert.Equal(OtaSupport.NeedsUsbUpdate, result[OtaHubRig.Mac(4)]);       // one application slot
        Assert.Equal(OtaSupport.NoOtaFirmware, result[OtaHubRig.Mac(6)]);        // old firmware: no answer at all
        Assert.Contains(("Fog1", OtaSupport.Probing), seen);                     // every timer is announced as being checked
        Assert.False(rig.Device.OtaHoldActive, "the hold is released when the check is over");
    }

    [Fact]
    public async Task Two_timers_are_updated_one_after_the_other_and_the_fleet_shows_the_new_version()
    {
        await using var rig = await OtaHubRig.StartAsync();
        using var updater = rig.NewUpdater();
        var image = Image(40_000);
        var progress = new List<OtaTimerProgress>();
        var finished = new List<OtaTimerResult>();
        updater.ProgressChanged += p => { lock (progress) progress.Add(p); };
        updater.TimerFinished += r => finished.Add(r);

        var results = await updater.UpdateAsync(new[] { rig.Target(0), rig.Target(1) }, image, CancellationToken.None);

        Assert.All(results, r => Assert.True(r.Succeeded, r.Message));
        Assert.Equal(new[] { "Fog1", "Fog2" }, finished.Select(r => r.Timer.Name).ToArray());
        Assert.Equal("2.1.0", results[0].OldVersion);
        Assert.Equal(image.Data, rig.Device.OtaOf(0)!.SlotBytes);
        Assert.Equal(image.Data, rig.Device.OtaOf(1)!.SlotBytes);
        await rig.WaitAsync(() => rig.Fleet.Find(OtaHubRig.Mac(0))?.Fw == "2.2.0" && rig.Fleet.Find(OtaHubRig.Mac(1))?.Fw == "2.2.0");
        Assert.Equal("2.1.0", rig.Fleet.Find(OtaHubRig.Mac(2))!.Fw);              // the others were not touched
        Assert.Contains(progress, p => p.Phase == OtaPhase.Streaming && p.Fraction > 0.05);
        Assert.Contains(progress, p => p.Phase == OtaPhase.Done);
        Assert.False(rig.Device.OtaHoldActive);
        Assert.True(rig.Link.FramesSent > image.Chunks * 2, "both transfers went through the tunnel");
    }

    [Fact]
    public async Task The_radio_hold_is_renewed_while_a_transfer_runs_and_refuses_conflicting_activities()
    {
        await using var rig = await OtaHubRig.StartAsync();
        // slow the tunnel down so that the transfer outlasts the remote's 15 s (simulated) memory of the hold
        using var updater = rig.NewUpdater(options: new OtaUpdaterOptions { HoldRefreshMs = 150 });
        var task = updater.UpdateAsync(new[] { rig.Target(0) }, Image(60_000), CancellationToken.None);

        await rig.WaitAsync(() => rig.Device.OtaHoldActive, 4000);
        var res = await rig.Hub.RequestAsync(Requests.Survey());
        Assert.False(res.Ok);
        Assert.Equal("busy", res.Err);
        Assert.Contains("firmware update", res.Msg);
        var disc = await rig.Hub.RequestAsync(Requests.Discover(true, false));
        Assert.Equal("busy", disc.Err);
        Assert.True((await rig.Hub.RequestAsync(Requests.Discover(false, false))).Ok);          // switching it off is always allowed

        var results = await task;
        Assert.True(results[0].Succeeded, results[0].Message);
        Assert.False(rig.Device.OtaHoldActive);
        Assert.True((await rig.Hub.RequestAsync(Requests.Survey())).Ok);                        // free again
    }

    [Fact]
    public async Task A_power_cut_in_the_middle_ends_that_transfer_and_it_starts_again()
    {
        await using var rig = await OtaHubRig.StartAsync();
        using var updater = rig.NewUpdater();
        var task = updater.UpdateAsync(new[] { rig.Target(0) }, Image(40_000), CancellationToken.None);

        await rig.WaitAsync(() => rig.Device.OtaOf(0)!.Contig > 60, 15000);
        Assert.True(rig.Device.CutPower(0, TimeSpan.FromSeconds(9)));

        var r = (await task)[0];
        Assert.True(r.Succeeded, r.Message);
        Assert.True(r.Restarts >= 1, "the transfer must have been started again");
        Assert.Equal(OtaOutcome.Updated, r.Outcome);
    }

    [Fact]
    public async Task A_wrong_key_is_reported_as_such_and_the_timer_keeps_its_firmware()
    {
        await using var rig = await OtaHubRig.StartAsync(OtaHubRig.Options(key: "the-timers-key"));
        using var updater = rig.NewUpdater(key: "another-key");
        var r = (await updater.UpdateAsync(new[] { rig.Target(0) }, Image(), CancellationToken.None))[0];
        Assert.Equal(OtaOutcome.Failed, r.Outcome);
        Assert.Contains("key", r.Message);
        Assert.Equal("2.1.0", rig.Fleet.Find(OtaHubRig.Mac(0))!.Fw);
    }

    [Fact]
    public async Task The_same_custom_key_works()
    {
        await using var rig = await OtaHubRig.StartAsync(OtaHubRig.Options(key: "the-timers-key"));
        using var updater = rig.NewUpdater(key: "the-timers-key");
        var r = (await updater.UpdateAsync(new[] { rig.Target(2) }, Image(), CancellationToken.None))[0];
        Assert.True(r.Succeeded, r.Message);
    }

    [Fact]
    public async Task Timers_that_cannot_be_updated_are_reported_with_the_right_advice()
    {
        await using var rig = await OtaHubRig.StartAsync();
        using var updater = rig.NewUpdater();
        var results = await updater.UpdateAsync(new[] { rig.Target(4), rig.Target(6), rig.Target(0) }, Image(), CancellationToken.None);

        Assert.Equal(OtaOutcome.NeedsUsbUpdate, results[0].Outcome);
        Assert.Contains("USB", results[0].Message);
        Assert.Equal(OtaOutcome.NoOtaFirmware, results[1].Outcome);
        Assert.Contains("no OTA support", results[1].Message);
        Assert.True(results[2].Succeeded, results[2].Message);                 // the refusals did not stop the run
    }

    [Fact]
    public async Task A_timer_that_is_offline_is_skipped_not_waited_for()
    {
        await using var rig = await OtaHubRig.StartAsync();
        rig.Device.CutPower(1, TimeSpan.FromSeconds(60));
        await rig.WaitAsync(() => rig.Fleet.Find(OtaHubRig.Mac(1))?.Online == false, 8000);
        using var updater = rig.NewUpdater();
        var results = await updater.UpdateAsync(new[] { rig.Target(1), rig.Target(0) }, Image(), CancellationToken.None);
        Assert.Equal(OtaOutcome.Skipped, results[0].Outcome);
        Assert.Contains("offline", results[0].Message);
        Assert.True(results[1].Succeeded, results[1].Message);
    }

    [Fact]
    public async Task A_timer_that_is_not_in_the_fleet_list_is_skipped()
    {
        await using var rig = await OtaHubRig.StartAsync();
        using var updater = rig.NewUpdater();
        var ghost = new OtaTimerTarget("11:22:33:44:55:66", "Ghost", CommandTarget.ByMac("11:22:33:44:55:66"));
        var r = (await updater.UpdateAsync(new[] { ghost }, Image(), CancellationToken.None))[0];
        Assert.Equal(OtaOutcome.Skipped, r.Outcome);
        Assert.Contains("fleet list", r.Message);
    }

    [Fact]
    public async Task Cancelling_stops_the_run_leaves_the_timer_idle_and_releases_the_hold()
    {
        await using var rig = await OtaHubRig.StartAsync();
        using var updater = rig.NewUpdater();
        using var cts = new CancellationTokenSource();
        var task = updater.UpdateAsync(new[] { rig.Target(0), rig.Target(1) }, Image(60_000), cts.Token);

        await rig.WaitAsync(() => rig.Device.OtaOf(0)!.Contig > 20, 15000);
        cts.Cancel();
        var results = await task;

        Assert.Equal(OtaOutcome.Cancelled, results[0].Outcome);
        Assert.Equal(OtaOutcome.Cancelled, results[1].Outcome);               // never started
        await rig.WaitAsync(() => rig.Device.OtaOf(0)!.State == OtaState.Idle, 3000);
        Assert.False(rig.Device.OtaHoldActive);
        Assert.Equal("2.1.0", rig.Fleet.Find(OtaHubRig.Mac(0))!.Fw);
    }

    [Fact]
    public async Task A_lossy_radio_costs_re_sends_but_every_timer_gets_the_image()
    {
        await using var rig = await OtaHubRig.StartAsync();
        rig.Device.ExtraLossPercent = 35;
        using var updater = rig.NewUpdater();
        var image = Image(20_000);
        var r = (await updater.UpdateAsync(new[] { rig.Target(2) }, image, CancellationToken.None))[0];
        Assert.True(r.Succeeded, r.Message);
        Assert.True(r.Resent > 0);
        Assert.Equal(image.Data, rig.Device.OtaOf(2)!.SlotBytes);
    }

    [Fact]
    public async Task A_timer_on_usb_is_updated_through_its_own_cable_without_a_remote()
    {
        await using var rig = await OtaHubRig.StartAsync(OtaHubRig.Options(kind: SimKind.Timer));
        Assert.Equal(DeviceKind.Timer, rig.Hub.Hello!.Kind);
        Assert.True(rig.Hub.Hello.SupportsOta);
        using var updater = rig.NewUpdater();
        var timer = new OtaTimerTarget("AA:BB:CC:DD:EE:01", "FogTimer", CommandTarget.Dev(0));
        Assert.True(rig.Fleet.Find(timer.Mac) is not null, "the fleet of one");

        var image = Image();
        var r = (await updater.UpdateAsync(new[] { timer }, image, CancellationToken.None))[0];
        Assert.True(r.Succeeded, r.Message);
        Assert.Equal(image.Data, rig.Device.OtaOf(0)!.SlotBytes);
    }

    [Fact]
    public async Task The_simulated_remote_and_timers_advertise_ota_and_the_new_firmware_version()
    {
        await using var rig = await OtaHubRig.StartAsync();
        Assert.True(rig.Hub.Hello!.SupportsOta);
        Assert.Equal("2.1.0", rig.Hub.Hello.Fw);
        Assert.Equal("2.1.0", rig.Fleet.Find(OtaHubRig.Mac(0))!.Fw);
        Assert.Equal("2.0.0", rig.Fleet.Find(OtaHubRig.Mac(6))!.Fw);              // the legacy one
    }

    [Fact]
    public async Task Ota_tx_to_an_unknown_timer_or_with_bad_data_is_refused_with_a_reason()
    {
        await using var rig = await OtaHubRig.StartAsync();
        var noDev = await rig.Hub.RequestAsync(Requests.OtaTx(CommandTarget.Dev(42), new byte[] { 3, 0 }));
        Assert.Equal("noDevice", noDev.Err);
        var bad = Requests.OtaTx(CommandTarget.Dev(0), new byte[] { 3, 0 });
        bad["d"] = "###";
        Assert.Equal("badArg", (await rig.Hub.RequestAsync(bad)).Err);
        var empty = await rig.Hub.RequestAsync(Requests.OtaTx(CommandTarget.Dev(0), ReadOnlySpan<byte>.Empty));
        Assert.Equal("badArg", empty.Err);
        var big = await rig.Hub.RequestAsync(Requests.OtaTx(CommandTarget.Dev(0), new byte[243]));
        Assert.Equal("badArg", big.Err);
        var ok = await rig.Hub.RequestAsync(Requests.OtaTx(CommandTarget.Dev(0), OtaFrames.Poll(0)));
        Assert.True(ok.Ok);
        Assert.NotNull(ok.Acked);
    }
}

/// <summary>The hub plumbing for a stream of otaTx requests.</summary>
public class HubOtaLinkTests
{
    private static (HubClient Hub, FakeTransport Transport) Connect(Func<System.Text.Json.Nodes.JsonObject, IEnumerable<string>?>? onOta)
    {
        var t = new FakeTransport("COM9") { Responder = FakeTransport.Healthy(onOta) };
        var hub = new HubClient(new HubOptions { KeepAlive = false });
        hub.ConnectAsync(() => t).GetAwaiter().GetResult();
        return (hub, t);
    }

    private static int IdOf(System.Text.Json.Nodes.JsonObject req) => req["id"]!.GetValue<int>();

    [Fact]
    public async Task At_most_six_requests_are_outstanding_at_a_time()
    {
        var (hub, t) = Connect(_ => null);                            // never answers
        using var link = new HubOtaLink(hub);
        for (int i = 0; i < 8; i++)
            for (int tries = 0; tries < 50 && !link.TrySend(CommandTarget.Dev(0), new byte[] { 3, 0 }); tries++) await Task.Delay(2);
        await Task.Delay(300);
        Assert.Equal(HubOtaLink.MaxOutstanding, t.WrittenOfType("otaTx").Count);
        await hub.DisposeAsync();
    }

    [Fact]
    public async Task Every_answer_frees_a_slot_for_the_next_frame_and_acked_false_is_counted()
    {
        int n = 0;
        var (hub, t) = Connect(req => req["t"]!.GetValue<string>() == "otaTx"
            ? new[] { FakeTransport.Ok(IdOf(req), (++n % 3 == 0) ? "\"acked\":false" : "\"acked\":true") } : null);
        using var link = new HubOtaLink(hub);
        int accepted = 0;
        for (int i = 0; i < 30; i++)
        {
            while (!link.TrySend(CommandTarget.Dev(0), new byte[] { 3, 0 })) await Task.Delay(2);
            accepted++;
        }
        for (int i = 0; i < 200 && link.FramesSent < 30; i++) await Task.Delay(10);
        Assert.Equal(30, link.FramesSent);
        Assert.Equal(10, link.MacNotAcked);
        Assert.Equal(20, link.MacAcked);
        await hub.DisposeAsync();
    }

    [Fact]
    public async Task A_busy_answer_is_retried_after_a_short_pause()
    {
        int n = 0;
        var (hub, t) = Connect(req => req["t"]!.GetValue<string>() == "otaTx"
            ? new[] { ++n <= 3 ? $$"""{"t":"res","id":{{IdOf(req)}},"ok":false,"err":"busy","msg":"transmit queue full"}""" : FakeTransport.Ok(IdOf(req), "\"acked\":true") }
            : null);
        using var link = new HubOtaLink(hub);
        Assert.True(link.TrySend(CommandTarget.Dev(0), new byte[] { 3, 0 }));
        for (int i = 0; i < 200 && link.FramesSent < 1; i++) await Task.Delay(10);
        Assert.Equal(1, link.FramesSent);
        Assert.Equal(3, link.BusyRetries);
        Assert.Equal(4, t.WrittenOfType("otaTx").Count);              // the same frame four times
        await hub.DisposeAsync();
    }

    [Fact]
    public async Task A_remote_that_cannot_forward_makes_the_link_fail_with_its_reason()
    {
        var (hub, _) = Connect(req => req["t"]!.GetValue<string>() == "otaTx"
            ? new[] { $$"""{"t":"res","id":{{IdOf(req)}},"ok":false,"err":"unknownType","msg":"unknown request type 'otaTx'"}""" } : null);
        using var link = new HubOtaLink(hub);
        string? why = null;
        link.Faulted += w => why = w;
        link.TrySend(CommandTarget.Dev(0), new byte[] { 3, 0 });
        for (int i = 0; i < 100 && why is null; i++) await Task.Delay(10);
        Assert.Contains("unknownType", why);
        Assert.False(link.TrySend(CommandTarget.Dev(0), new byte[] { 3, 0 }), "a failed link takes no more frames");
        await hub.DisposeAsync();
    }

    [Fact]
    public async Task Timeouts_are_counted_and_do_not_kill_the_link()
    {
        var (hub, _) = Connect(_ => null);
        using var link = new HubOtaLink(hub);
        link.TrySend(CommandTarget.Dev(0), new byte[] { 3, 0 });
        for (int i = 0; i < 300 && link.Timeouts < 1; i++) await Task.Delay(10);
        Assert.Equal(1, link.Timeouts);
        Assert.True(link.TrySend(CommandTarget.Dev(0), new byte[] { 3, 0 }));
        await hub.DisposeAsync();
    }

    [Fact]
    public async Task Frames_from_the_timers_reach_the_subscriber()
    {
        var (hub, t) = Connect(_ => null);
        using var link = new HubOtaLink(hub);
        var got = new List<OtaRxMessage>();
        link.PayloadReceived += got.Add;
        t.Feed("""{"t":"otaRx","dev":2,"mac":"aa:bb:cc:dd:ee:03","rssi":-60,"d":"gUICAAUABQAAAGQXAAE="}""");
        t.Feed("""{"t":"fleet","seq":1,"active":0,"ch":6,"devs":[]}""");
        for (int i = 0; i < 100 && got.Count < 1; i++) await Task.Delay(10);
        var rx = Assert.Single(got);
        Assert.Equal(2, rx.Dev);
        Assert.Equal(14, rx.Payload.Length);
        await hub.DisposeAsync();
    }

    [Fact]
    public async Task The_hold_request_is_sent_and_its_answer_is_returned()
    {
        var (hub, t) = Connect(req => req["t"]!.GetValue<string>() == "otaHold" ? new[] { FakeTransport.Ok(IdOf(req)) } : null);
        using var link = new HubOtaLink(hub);
        Assert.True(await link.HoldAsync(true));
        Assert.True(t.WrittenOfType("otaHold")[0]["on"]!.GetValue<bool>());
        await hub.DisposeAsync();

        var (hub2, _) = Connect(req => req["t"]!.GetValue<string>() == "otaHold" ? new[] { $$"""{"t":"res","id":{{IdOf(req)}},"ok":false,"err":"unknownType"}""" } : null);
        using var link2 = new HubOtaLink(hub2);
        Assert.False(await link2.HoldAsync(true));                    // an old remote: the update goes on without a hold
        await hub2.DisposeAsync();
    }

    [Fact]
    public async Task The_fleet_watcher_follows_the_fleet_events()
    {
        var (hub, t) = Connect(_ => null);
        using var watcher = new HubFleetWatcher(hub);
        Assert.Null(watcher.Find("AA:BB:CC:DD:EE:01"));
        t.Feed("""{"t":"fleet","seq":1,"active":0,"ch":6,"devs":[{"i":0,"mac":"aa:bb:cc:dd:ee:01","name":"Fog1","online":1,"boot":5,"fw":"2.1","ph":"off","mode":"auto","ton":1,"toff":2,"el":0}]}""");
        for (int i = 0; i < 100 && watcher.Updates == 0; i++) await Task.Delay(10);
        var d = watcher.Find("aa:bb:cc:dd:ee:01");
        Assert.NotNull(d);
        Assert.Equal(5, d!.Boot);
        Assert.Equal("2.1", d.Fw);
        Assert.True(d.Online);
        await hub.DisposeAsync();
    }
}
