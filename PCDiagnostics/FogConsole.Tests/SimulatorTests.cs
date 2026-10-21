using System.Text.Json.Nodes;
using FogConsole.Protocol;
using FogConsole.Simulator;
using Xunit;

namespace FogConsole.Tests;

public class SimulatorTests
{
    private static SimHarness Started(SimOptions? opt = null)
    {
        var h = new SimHarness(opt);
        h.StartSession();
        return h;
    }

    // ---- basics ---------------------------------------------------------------------------

    [Fact]
    public void Hello_describes_a_remote()
    {
        var h = new SimHarness();
        var info = h.Ok(Requests.HelloReq()).Hello!;
        Assert.Equal(DeviceKind.Remote, info.Kind);
        Assert.Equal(2, info.Proto);
        Assert.Equal(10, info.MaxDevs);
        Assert.Equal(6, info.Channel);
        Assert.Equal("AA:BB:CC:00:11:22", info.Mac);
        Assert.Contains(info.ResetReason, new[] { "poweron", "ext", "sw", "panic", "wdt", "deepsleep", "brownout", "usb", "other" });
    }

    [Fact]
    public void Ping_returns_the_device_clock()
    {
        var h = new SimHarness();
        long a = h.Ok(Requests.Ping()).DeviceMs!.Value;
        h.Advance(1000);
        long b = h.Ok(Requests.Ping()).DeviceMs!.Value;
        Assert.InRange(b - a, 950, 1100);
    }

    [Fact]
    public void Bad_requests_get_proper_error_codes()
    {
        var h = new SimHarness();
        var r = h.Request(new JsonObject { ["t"] = "doesNotExist" })!;
        Assert.False(r.Ok);
        Assert.Equal("unknownType", r.Err);

        h.Dev.HandleLine("""{"t":"ping"}""");
        h.Pump();
        var noId = h.Of<ResponseMessage>().Last();
        Assert.Equal("badRequest", noId.Err);

        h.Dev.HandleLine("""{"id":4}""");
        h.Pump();
        Assert.Equal("badRequest", h.Of<ResponseMessage>().Last().Err);

        Assert.Equal("badArg", h.Request(new JsonObject { ["t"] = "get", ["what"] = "nonsense" })!.Err);
    }

    [Fact]
    public void Garbage_lines_are_counted_but_do_not_break_the_session()
    {
        var h = new SimHarness();
        h.Dev.HandleLine("not json at all");
        h.Dev.HandleLine("{\"t\":");
        h.Pump();
        Assert.Empty(h.Received);
        var stats = h.Ok(Requests.GetStats()).Stats!;
        Assert.Equal(2, stats.Self.Pc.BadJson);
        Assert.True(h.Ok(Requests.Ping()).Ok);
    }

    [Fact]
    public void Events_stream_only_while_the_pc_is_present()
    {
        var h = Started();
        Assert.NotEmpty(h.Of<FleetMessage>());
        Assert.NotEmpty(h.Of<RemoteMessage>());

        h.Clear();
        h.Dev.Advance(TimeSpan.FromSeconds(8)); // PC silent
        h.Pump();
        Assert.NotEmpty(h.Of<FleetMessage>()); // the first 6 s are still streamed
        var lastMs = h.Of<FleetMessage>().Count();

        h.Clear();
        h.Dev.Advance(TimeSpan.FromSeconds(5));
        h.Pump();
        Assert.Empty(h.Received);
        Assert.True(lastMs is >= 8 and <= 30);

        h.Ok(Requests.Ping()); // PC is back: stream resumes immediately
        h.Dev.Advance(TimeSpan.FromMilliseconds(200));
        h.Pump();
        Assert.NotEmpty(h.Of<FleetMessage>());
    }

    [Fact]
    public void Fleet_event_lists_the_paired_timers()
    {
        var h = Started();
        var f = h.LastFleet;
        Assert.Equal(7, f.Devs.Count);
        Assert.Equal(6, f.Channel);
        Assert.Equal(Enumerable.Range(0, 7), f.Devs.Select(d => d.Index));
        Assert.All(f.Devs, d =>
        {
            Assert.True(d.Online);
            Assert.StartsWith("AA:BB:CC:DD:EE:", d.Mac);
            Assert.StartsWith("Fog", d.Name);
            Assert.InRange(d.Rssi, -98, -30);
            Assert.InRange(d.Ton, 0.1, 9999.9);
            Assert.Equal(6, d.Channel);
            Assert.Equal("2.1.0", d.Fw);
        });
        Assert.Equal(7, f.Devs.Select(d => d.SlotId).Distinct().Count());
        Assert.Equal(0, f.Active);
    }

    [Fact]
    public void Fleet_events_come_twice_a_second()
    {
        var h = Started();
        h.Clear();
        h.Advance(5000);
        int n = h.Of<FleetMessage>().Count();
        Assert.InRange(n, 9, 24);
        var seqs = h.Of<FleetMessage>().Select(f => f.Seq).ToList();
        Assert.Equal(seqs.OrderBy(s => s), seqs);
    }

    // ---- cycle engine and commands --------------------------------------------------------

    [Fact]
    public void The_cycle_alternates_off_and_on_with_the_configured_durations()
    {
        var h = Started();
        h.Cmd(CommandSpec.Reset(CommandTarget.Dev(0)), 300); // ton 1.5 / toff 10
        Assert.Equal(CyclePhase.Off, h.Fleet(0).Phase);
        h.Advance(10_300);
        Assert.Equal(CyclePhase.On, h.Fleet(0).Phase);
        Assert.InRange(h.Fleet(0).Elapsed, 0, 1.5);
        h.Advance(1500);
        Assert.Equal(CyclePhase.Off, h.Fleet(0).Phase);
    }

    [Fact]
    public void SetTimes_is_confirmed_and_reflected_in_the_fleet()
    {
        var h = Started();
        var res = h.Cmd(CommandSpec.SetTimes(CommandTarget.Dev(2), 3.0, 6.5), 400);
        Assert.Equal(1, res.Queued);
        var r = h.Of<CmdResultMessage>().Last(c => c.Id == h.LastRequestId);
        Assert.Equal(CmdOutcome.Ok, r.Result);
        Assert.Equal("setTimes", r.Op);
        Assert.Equal(2, r.Dev);
        Assert.InRange(r.Attempts, 1, 8);
        Assert.Equal(3.0, h.Fleet(2).Ton);
        Assert.Equal(6.5, h.Fleet(2).Toff);
    }

    [Theory]
    [InlineData(0.0, 5.0)]
    [InlineData(0.05, 5.0)]
    [InlineData(5.0, 10000.0)]
    [InlineData(-1.0, 5.0)]
    public void SetTimes_out_of_range_is_rejected_with_badArg(double on, double off)
    {
        var h = Started();
        var req = Requests.Cmd(CommandSpec.SetTimes(CommandTarget.Dev(0), 1, 1));
        req["on"] = on;
        req["off"] = off;
        var r = h.Request(req)!;
        Assert.False(r.Ok);
        Assert.Equal("badArg", r.Err);
    }

    [Fact]
    public void Missing_arguments_are_badArg()
    {
        var h = Started();
        Assert.Equal("badArg", h.Request(new JsonObject { ["t"] = "cmd", ["dev"] = 0, ["op"] = "setTimes", ["on"] = 1.0 })!.Err);
        Assert.Equal("badArg", h.Request(new JsonObject { ["t"] = "cmd", ["dev"] = 0, ["op"] = "mode", ["mode"] = "turbo" })!.Err);
        Assert.Equal("badArg", h.Request(new JsonObject { ["t"] = "cmd", ["dev"] = 0, ["op"] = "bogus" })!.Err);
        Assert.Equal("badArg", h.Request(new JsonObject { ["t"] = "cmd", ["dev"] = 0, ["op"] = "channel", ["ch"] = 14 })!.Err);
        Assert.Equal("badArg", h.Request(new JsonObject { ["t"] = "cmd", ["dev"] = 0, ["op"] = "name", ["name"] = "WayTooLongName" })!.Err);
        Assert.Equal("badArg", h.Request(new JsonObject { ["t"] = "cmd", ["dev"] = 0, ["op"] = "mode", ["mode"] = "forceOn", ["leaseMs"] = 50 })!.Err);
    }

    [Fact]
    public void Unknown_devices_are_noDevice()
    {
        var h = Started();
        Assert.Equal("noDevice", h.Request(Requests.Cmd(CommandSpec.Toggle(CommandTarget.Dev(9))))!.Err);
        Assert.Equal("noDevice", h.Request(Requests.Cmd(CommandSpec.Toggle(CommandTarget.ByMac("AA:BB:CC:DD:EE:99"))))!.Err);
        Assert.Equal("noDevice", h.Request(Requests.Select(7))!.Err);
        Assert.Equal("noDevice", h.Request(Requests.Unpair(-1))!.Err);
        Assert.True(h.Request(Requests.Cmd(CommandSpec.Toggle(CommandTarget.ByMac("aa:bb:cc:dd:ee:03"))))!.Ok);
    }

    [Fact]
    public void ForceOn_lasts_only_as_long_as_the_lease_is_refreshed()
    {
        var h = Started();
        h.Cmd(CommandSpec.Reset(CommandTarget.Dev(1)), 300);
        Assert.False(h.Fleet(1).On);

        h.Cmd(CommandSpec.Mode(CommandTarget.Dev(1), TimerMode.ForceOn, 600), 300);
        var f = h.Fleet(1);
        Assert.Equal(TimerMode.ForceOn, f.Mode);
        Assert.True(f.On);                       // output on although the cycle is in its OFF phase
        Assert.Equal(CyclePhase.Off, f.Phase);

        // refreshing every 300 ms keeps it alive well beyond one lease
        for (int i = 0; i < 6; i++) h.Cmd(CommandSpec.Mode(CommandTarget.Dev(1), TimerMode.ForceOn, 600), 300);
        Assert.Equal(TimerMode.ForceOn, h.Fleet(1).Mode);

        // stop refreshing: the dead-man lease expires
        h.Advance(1200);
        f = h.Fleet(1);
        Assert.Equal(TimerMode.Auto, f.Mode);
        Assert.False(f.On);
    }

    [Fact]
    public void Releasing_with_mode_auto_ends_force_on_immediately()
    {
        var h = Started();
        h.Cmd(CommandSpec.Mode(CommandTarget.Dev(0), TimerMode.ForceOn, 5000), 300);
        Assert.Equal(TimerMode.ForceOn, h.Fleet(0).Mode);
        h.Cmd(CommandSpec.Mode(CommandTarget.Dev(0), TimerMode.Auto), 300);
        Assert.Equal(TimerMode.Auto, h.Fleet(0).Mode);
    }

    [Fact]
    public void Paused_freezes_the_cycle_and_switches_the_output_off()
    {
        var h = Started();
        h.Cmd(CommandSpec.Mode(CommandTarget.Dev(4), TimerMode.Paused), 600);
        var a = h.Fleet(4);
        Assert.Equal(TimerMode.Paused, a.Mode);
        Assert.False(a.On);
        h.Advance(3000);
        var b = h.Fleet(4);
        Assert.Equal(a.Elapsed, b.Elapsed, 1);
        Assert.Equal(a.Phase, b.Phase);
        h.Cmd(CommandSpec.Mode(CommandTarget.Dev(4), TimerMode.Auto), 2000);
        Assert.NotEqual(a.Elapsed, h.Fleet(4).Elapsed);
    }

    [Fact]
    public void Toggle_reset_and_setPhase()
    {
        var h = Started();
        h.Cmd(CommandSpec.Reset(CommandTarget.Dev(0)), 300);
        Assert.Equal(CyclePhase.Off, h.Fleet(0).Phase);

        h.Cmd(CommandSpec.Toggle(CommandTarget.Dev(0)), 300);
        Assert.Equal(CyclePhase.On, h.Fleet(0).Phase);
        Assert.InRange(h.Fleet(0).Elapsed, 0, 0.8);

        h.Cmd(CommandSpec.SetPhase(CommandTarget.Dev(0), CyclePhase.Off, 4.0), 300);
        var f = h.Fleet(0);
        Assert.Equal(CyclePhase.Off, f.Phase);
        Assert.InRange(f.Elapsed, 4.0, 4.8);

        h.Cmd(CommandSpec.Reset(CommandTarget.Dev(0)), 300);
        Assert.InRange(h.Fleet(0).Elapsed, 0, 0.8);
    }

    [Fact]
    public void Rename_and_channel_ops()
    {
        var h = Started();
        h.Cmd(CommandSpec.Rename(CommandTarget.Dev(3), "Smoky"), 400);
        Assert.Equal("Smoky", h.Fleet(3).Name);

        // moving one timer to another channel makes it unreachable until recover/migrate
        h.Cmd(CommandSpec.Channel(CommandTarget.Dev(3), 11), 400);
        h.Advance(5000);
        Assert.False(h.Fleet(3).Online);
    }

    [Fact]
    public void A_command_to_all_gets_one_result_per_timer()
    {
        var h = Started();
        var res = h.Cmd(CommandSpec.Toggle(CommandTarget.All), 800);
        Assert.Equal(7, res.Queued);
        var results = h.Of<CmdResultMessage>().Where(c => c.Id == h.LastRequestId).ToList();
        Assert.Equal(7, results.Count);
        Assert.Equal(Enumerable.Range(0, 7), results.Select(r => r.Dev).OrderBy(x => x));
        Assert.All(results, r => Assert.Equal(CmdOutcome.Ok, r.Result));
    }

    [Fact]
    public void A_command_to_all_with_nobody_paired_queues_zero()
    {
        var h = Started(SimHarness.Quiet(paired: 0));
        Assert.Equal(0, h.Cmd(CommandSpec.Toggle(CommandTarget.All), 500).Queued);
        Assert.Empty(h.Of<CmdResultMessage>());
        Assert.Empty(h.LastFleet.Devs);
    }

    [Fact]
    public void A_command_to_an_unpowered_timer_ends_in_timeout()
    {
        var h = Started();
        h.Dev.TestPowerCut(3, 60_000, false);
        h.Advance(5200);
        var f = h.Fleet(3);
        Assert.False(f.Online);
        Assert.True(f.AgeMs > 4500);

        h.Cmd(CommandSpec.Toggle(CommandTarget.Dev(3)));
        int id = h.LastRequestId;
        h.Advance(500);
        Assert.Equal(1, h.Fleet(3).Pending);                         // queued, not done yet
        Assert.DoesNotContain(h.Of<CmdResultMessage>(), c => c.Id == id);
        h.Advance(2400);
        var r = h.Of<CmdResultMessage>().Single(c => c.Id == id);
        Assert.Equal(CmdOutcome.Timeout, r.Result);
        Assert.Equal(2500, r.Ms);
        Assert.Equal(0, h.Fleet(3).Pending);
    }

    [Fact]
    public void Queued_commands_are_cancelled_when_the_timer_is_unpaired()
    {
        var h = Started();
        h.Dev.TestPowerCut(2, 60_000, false);
        h.Advance(5200);
        h.Cmd(CommandSpec.Toggle(CommandTarget.Dev(2)));
        int id = h.LastRequestId;
        h.Advance(300);
        h.Ok(Requests.Unpair(2), 200);
        var r = h.Of<CmdResultMessage>().Single(c => c.Id == id);
        Assert.Equal(CmdOutcome.Cancelled, r.Result);
        h.Advance(3000);
        Assert.Single(h.Of<CmdResultMessage>(), c => c.Id == id); // no second result
    }

    [Fact]
    public void A_power_cut_makes_the_timer_come_back_with_boot_plus_one()
    {
        var h = Started();
        int bootBefore = h.Fleet(1).Boot;
        long upBefore = h.Fleet(1).UpSeconds;
        Assert.True(upBefore > 60);

        h.Dev.TestPowerCut(1, 8000, brownout: false);
        h.Advance(5200);
        Assert.False(h.Fleet(1).Online);
        h.Advance(5000);
        var f = h.Fleet(1);
        Assert.True(f.Online);
        Assert.Equal(bootBefore + 1, f.Boot);
        Assert.True(f.UpSeconds < 10, $"uptime should restart, was {f.UpSeconds}");
        Assert.Equal(TimerMode.Auto, f.Mode);
    }

    [Fact]
    public void Chaos_cuts_power_to_timers_now_and_then()
    {
        var h = new SimHarness(new SimOptions { Seed = 3, Chaos = true, Demo = false, PowerCutsPerTimerPerMinute = 1.0 });
        h.StartSession();
        var boots = h.LastFleet.Devs.ToDictionary(d => d.Index, d => d.Boot);
        h.Advance(180_000);
        Assert.Contains(h.LastFleet.Devs, d => d.Boot > boots[d.Index]);
        Assert.Contains(h.Of<FleetMessage>(), f => f.Devs.Any(d => !d.Online));
    }

    [Fact]
    public void The_demo_scenario_has_an_offline_timer_and_a_scripted_reboot()
    {
        var h = new SimHarness(new SimOptions { Seed = 42, Chaos = false, Demo = true });
        h.StartSession();
        Assert.False(h.Fleet(3).Online);             // Fog4 is heating
        Assert.True(h.Fleet(3).AgeMs > 10_000);
        int boot = h.Fleet(1).Boot;
        h.Advance(20_000);
        Assert.Equal(boot + 1, h.Fleet(1).Boot);     // Fog2 lost power and came back
        Assert.True(h.Fleet(1).Online);
    }

    // ---- discovery and pairing ------------------------------------------------------------

    [Fact]
    public void Discovery_reports_timers_and_sweep_finds_the_one_on_another_channel()
    {
        var h = Started();
        h.Clear();
        h.Ok(Requests.Discover(true, false));
        h.Advance(2500);
        var found = h.Of<FoundMessage>().ToList();
        Assert.Contains(found, f => f.Paired == 0);                   // paired timers are reported with their index
        var unpaired = found.Where(f => f.Paired == -1).Select(f => f.Mac).Distinct().ToList();
        Assert.Equal(2, unpaired.Count);                              // two unpaired timers on the home channel
        Assert.All(found, f => Assert.Equal(6, f.Channel));

        h.Clear();
        h.Ok(Requests.Discover(true, true));
        h.Advance(6000);
        var other = h.Of<FoundMessage>().Where(f => f.Channel != 6).ToList();
        Assert.NotEmpty(other);
        Assert.All(other, f => Assert.Equal(-1, f.Paired));

        h.Ok(Requests.Discover(false, false));
        h.Clear();
        h.Advance(3000);
        Assert.Empty(h.Of<FoundMessage>());
    }

    [Fact]
    public void Pairing_adds_a_timer_and_unpairing_shifts_the_indices()
    {
        var h = Started();
        var res = h.Ok(Requests.Pair("AA:BB:CC:DD:EE:08", "Garage"), 1500);
        Assert.Equal(7, h.Dev.PairedCount - 1);
        var f = h.LastFleet;
        Assert.Equal(8, f.Devs.Count);
        Assert.Equal("Garage", f.Devs[7].Name);
        Assert.Equal("AA:BB:CC:DD:EE:08", f.Devs[7].Mac);
        Assert.True(f.Devs[7].Online);

        // pairing the same timer twice is refused
        Assert.Equal("badArg", h.Request(Requests.Pair("AA:BB:CC:DD:EE:08", null))!.Err);
        Assert.Equal("noDevice", h.Request(Requests.Pair("AA:BB:CC:DD:EE:77", null))!.Err);
        Assert.Equal("badArg", h.Request(Requests.Pair("AA:BB:CC:DD:EE:09", "ThisNameIsTooLong"))!.Err);

        string nameOfNext = f.Devs[3].Name;
        h.Ok(Requests.Unpair(2), 700);
        f = h.LastFleet;
        Assert.Equal(7, f.Devs.Count);
        Assert.Equal(nameOfNext, f.Devs[2].Name);                     // indices above the removed one shifted down
        Assert.Equal(Enumerable.Range(0, 7), f.Devs.Select(d => d.Index));
        _ = res;
    }

    [Fact]
    public void Pairing_a_timer_on_another_channel_moves_it_to_the_home_channel()
    {
        var h = Started();
        h.Ok(Requests.Pair("AA:BB:CC:DD:EE:0A", null), 3000); // the 10th timer sits on another channel
        var f = h.LastFleet;
        Assert.Equal(8, f.Devs.Count);
        Assert.True(f.Devs[7].Online);
        Assert.Equal(6, f.Devs[7].Channel);
    }

    [Fact]
    public void The_fleet_list_holds_at_most_ten_timers()
    {
        var h = Started(new SimOptions { Seed = 1, Chaos = false, Demo = false, PairedTimers = 10, TotalTimers = 12 });
        Assert.Equal(10, h.LastFleet.Devs.Count);
        var r = h.Request(Requests.Pair("AA:BB:CC:DD:EE:0B", null))!;
        Assert.False(r.Ok);
        Assert.Equal("full", r.Err);
    }

    [Fact]
    public void Select_changes_the_active_timer()
    {
        var h = Started();
        h.Ok(Requests.Select(4), 700);
        Assert.Equal(4, h.LastFleet.Active);
    }

    // ---- survey, channel change, recover, link test ---------------------------------------

    [Fact]
    public void Survey_emits_thirteen_channels_and_a_recommendation()
    {
        var h = Started();
        h.Clear();
        h.Ok(Requests.Survey());
        Assert.Equal("busy", h.Request(Requests.Survey())!.Err);
        h.Advance(3600);
        var chans = h.Of<SurveyChMessage>().ToList();
        Assert.Equal(13, chans.Count);
        Assert.Equal(Enumerable.Range(1, 13), chans.Select(c => c.Channel));
        Assert.All(chans, c => Assert.InRange(c.Score, 0, 100));
        var done = Assert.Single(h.Of<SurveyDoneMessage>());
        Assert.Equal(6, done.Current);
        Assert.Equal(3, done.Best.Count);
        var scores = chans.ToDictionary(c => c.Channel, c => c.Score);
        Assert.Equal(done.Best.Select(b => scores[b]).OrderBy(s => s), done.Best.Select(b => scores[b])); // best first
        Assert.DoesNotContain(6, done.Best);                  // the home channel is the noisiest one in the simulated neighbourhood
        Assert.True(scores[6] > scores[done.Best[0]]);
        Assert.True(chans.Single(c => c.Channel == 6).Aps >= 4);
    }

    [Fact]
    public void A_survey_can_be_repeated_after_it_finished()
    {
        var h = Started();
        h.Ok(Requests.Survey(), 3800);
        Assert.True(h.Request(Requests.Survey())!.Ok);
    }

    [Fact]
    public void Channel_migration_moves_all_online_timers()
    {
        var h = Started();
        h.Clear();
        h.Ok(Requests.Channel(11, false));
        h.Advance(6000);
        var m = h.Of<MigrateMessage>().ToList();
        Assert.Equal("start", m[0].Stage);
        Assert.Equal(7, m[0].Targets);
        Assert.Equal(7, m.Count(x => x.Stage == "dev" && x.DevOk == true));
        var done = m.Single(x => x.Stage == "done");
        Assert.True(done.Committed);
        Assert.Equal(7, done.OkCount);
        Assert.Equal(0, done.Failed);
        Assert.Equal("done", m[^1].Stage);

        Assert.Equal(11, h.LastFleet.Channel);
        h.Advance(3000);
        Assert.All(h.LastFleet.Devs, d => Assert.True(d.Online));
        Assert.All(h.LastFleet.Devs, d => Assert.Equal(11, d.Channel));
    }

    [Fact]
    public void Channel_change_is_validated()
    {
        var h = Started();
        Assert.Equal("badArg", h.Request(Requests.Channel(0, false))!.Err);
        Assert.Equal("badArg", h.Request(Requests.Channel(14, false))!.Err);
        Assert.Equal("badArg", h.Request(Requests.Channel(6, false))!.Err); // already there
    }

    [Fact]
    public void A_migration_without_any_online_timer_is_abandoned_unless_forced()
    {
        var h = Started();
        for (int i = 0; i < 7; i++) h.Dev.TestPowerCut(i, 120_000, false);
        h.Advance(5500);
        h.Clear();
        h.Ok(Requests.Channel(11, false));
        h.Advance(2500);
        var done = h.Of<MigrateMessage>().Single(x => x.Stage == "done");
        Assert.False(done.Committed);
        Assert.Equal(6, h.LastFleet.Channel);

        h.Clear();
        h.Ok(Requests.Channel(11, true));
        h.Advance(2500);
        Assert.True(h.Of<MigrateMessage>().Single(x => x.Stage == "done").Committed);
        Assert.Equal(11, h.LastFleet.Channel);
    }

    [Fact]
    public void A_timer_that_missed_the_migration_is_stranded_and_recover_finds_it()
    {
        var h = Started();
        h.Dev.TestPowerCut(4, 12_000, false);              // heating while the system moves
        h.Advance(5200);
        h.Ok(Requests.Channel(11, false));
        h.Advance(6000);
        Assert.Equal(11, h.LastFleet.Channel);
        h.Advance(12_000);                                  // power returns: but on the old channel
        Assert.False(h.Fleet(4).Online);
        Assert.Equal(6, h.Fleet(4).Channel);

        h.Clear();
        h.Ok(Requests.Recover());
        h.Advance(5500);
        Assert.Contains(h.Of<FoundMessage>(), f => f.Paired == 4 && f.Channel == 6);
        h.Advance(3000);
        var f4 = h.Fleet(4);
        Assert.True(f4.Online);
        Assert.Equal(11, f4.Channel);
    }

    [Fact]
    public void Radio_tools_report_busy_while_the_radio_is_occupied()
    {
        var h = Started();
        h.Ok(Requests.Recover());
        Assert.Equal("busy", h.Request(Requests.Survey())!.Err);
        Assert.Equal("busy", h.Request(Requests.Channel(11, false))!.Err);
        Assert.Equal("busy", h.Request(Requests.Recover())!.Err);
    }

    [Fact]
    public void Link_test_reports_progress_and_a_final_result()
    {
        var h = Started();
        h.Clear();
        h.Ok(Requests.LinkTest(0, 40, 25));
        int id = h.LastRequestId;
        Assert.Equal("busy", h.Request(Requests.LinkTest(1, 10, 25))!.Err);
        h.Advance(1500);
        var tests = h.Of<LinkTestMessage>().Where(t => t.Id == id).ToList();
        Assert.True(tests.Count >= 3);
        Assert.Equal(tests.Select(t => t.Sent).OrderBy(x => x), tests.Select(t => t.Sent));
        var last = tests[^1];
        Assert.True(last.Done);
        Assert.Equal(40, last.Sent);
        Assert.InRange(last.MacAck, 30, 40);
        Assert.True(last.Echo <= last.MacAck);
        Assert.InRange(last.RttMs, 1, 40);
        Assert.True(last.RttMax >= last.RttMs);
        Assert.Single(tests, t => t.Done);
    }

    [Fact]
    public void Link_test_on_an_unpowered_timer_gets_no_answers_and_arguments_are_validated()
    {
        var h = Started();
        h.Dev.TestPowerCut(2, 60_000, false);
        h.Advance(5200);
        h.Ok(Requests.LinkTest(2, 20, 20));
        int id = h.LastRequestId;
        h.Advance(900);
        var last = h.Of<LinkTestMessage>().Last(t => t.Id == id);
        Assert.True(last.Done);
        Assert.Equal(20, last.Sent);
        Assert.Equal(0, last.MacAck);
        Assert.Equal(0, last.Echo);

        Assert.Equal("badArg", h.Request(Requests.LinkTest(0, 201, 40))!.Err);
        Assert.Equal("badArg", h.Request(Requests.LinkTest(0, 0, 40))!.Err);
        Assert.Equal("noDevice", h.Request(Requests.LinkTest(9, 10, 40))!.Err);
    }

    [Fact]
    public void A_weak_link_loses_more_than_a_strong_one_and_a_quieter_channel_helps()
    {
        var h = Started();
        h.Advance(300_000);
        var s = h.GetStats();
        double LossOf(int i) => s.Devs[i].Link!.Lost * 100.0 / Math.Max(1, s.Devs[i].Link!.Expected);
        Assert.True(LossOf(5) > LossOf(4) + 5, $"weak {LossOf(5):0.0}% vs strong {LossOf(4):0.0}%");   // -79 dBm vs -49 dBm
        Assert.True(LossOf(5) > 8);
        Assert.True(LossOf(4) < 7);
        Assert.True(s.Devs[5].Link!.RssiAvg < s.Devs[4].Link!.RssiAvg);
        Assert.True(s.Devs[5].Link!.MacFailPercent > s.Devs[4].Link!.MacFailPercent);
    }

    // ---- settings, logging, sniffer -------------------------------------------------------

    [Fact]
    public void Settings_can_be_read_and_changed()
    {
        var h = new SimHarness();
        var s = h.Ok(Requests.GetSettings()).Settings!;
        Assert.Equal(80, s.TxPower);
        Assert.Equal(20.0, s.TxPowerDbm);
        Assert.Equal(new[] { 3300, 3750, 4150 }, s.BatCal);

        var changed = h.Ok(Requests.Set(new JsonObject { ["brightness"] = 99, ["autoOff"] = 0, ["unknownKey"] = 1 })).Settings!;
        Assert.Equal(99, changed.Brightness);
        Assert.Equal(0, changed.AutoOff);
        Assert.Equal(80, changed.TxPower);
        Assert.Equal(99, h.Ok(Requests.GetSettings()).Settings!.Brightness);
    }

    [Fact]
    public void Invalid_settings_are_refused_atomically()
    {
        var h = new SimHarness();
        var r = h.Request(Requests.Set(new JsonObject { ["brightness"] = 120, ["txPower"] = 200 }))!;
        Assert.Equal("badArg", r.Err);
        Assert.Equal(200, h.Ok(Requests.GetSettings()).Settings!.Brightness);   // nothing was applied

        Assert.Equal("badArg", h.Request(Requests.Set(new JsonObject { ["rssiLow"] = -60, ["rssiHigh"] = -58 }))!.Err);
        Assert.Equal("badArg", h.Request(Requests.Set(new JsonObject { ["batCal"] = new JsonArray(2600, 2240, 1850) }))!.Err);
        Assert.Equal("badArg", h.Request(Requests.Set(new JsonObject { ["batCal"] = new JsonArray(1, 2) }))!.Err);
        Assert.Equal("badArg", h.Request(Requests.Set(new JsonObject { ["brightness"] = 4 }))!.Err);
        Assert.True(h.Request(Requests.Set(new JsonObject { ["batCal"] = new JsonArray(1800, 2200, 2650) }))!.Ok);
    }

    [Fact]
    public void Log_events_follow_the_configured_level()
    {
        var h = Started();
        h.Clear();
        h.Advance(2000);
        Assert.Empty(h.Of<LogMessage>());                      // off by default

        Assert.Equal("badArg", h.Request(Requests.LogCfg("loud"))!.Err);
        h.Ok(Requests.LogCfg("debug"));
        h.Cmd(CommandSpec.Toggle(CommandTarget.Dev(0)), 500);
        var logs = h.Of<LogMessage>().ToList();
        Assert.NotEmpty(logs);
        Assert.Contains(logs, l => l.Src == "cmd");

        h.Ok(Requests.LogCfg("error"));
        h.Clear();
        h.Cmd(CommandSpec.Toggle(CommandTarget.Dev(0)), 500);
        Assert.DoesNotContain(h.Of<LogMessage>(), l => l.Level > LogLevel.Error);
    }

    [Fact]
    public void Sniffer_streams_frames_but_never_more_than_thirty_per_second()
    {
        var h = Started();
        h.Clear();
        h.Advance(2000);
        Assert.Empty(h.Of<FrameMessage>());

        h.Ok(Requests.Sniff(true));
        h.Clear();
        h.Advance(4000);
        var frames = h.Of<FrameMessage>().ToList();
        Assert.NotEmpty(frames);
        Assert.InRange(frames.Count, 5, 4 * 30 + 5);
        Assert.Contains(frames, f => f.Dir == "rx" && f.Kind == "STATUS");
        Assert.Contains(frames, f => f.Dir == "tx");
        Assert.All(frames, f => Assert.StartsWith("F6 02", f.Hex));
    }

    // ---- stats ----------------------------------------------------------------------------

    [Fact]
    public void Get_stats_returns_both_ends_and_counters_never_go_backwards()
    {
        var h = Started();
        var a = h.GetStats();
        Assert.Equal(DeviceKind.Remote, a.Self.Kind);
        Assert.Equal(7, a.Devs.Count);
        Assert.All(a.Devs, d => Assert.NotNull(d.Link));
        Assert.NotEmpty(a.Self.Chan);
        Assert.Equal(6, a.Self.Chan[0].Channel);             // home channel first
        Assert.True(a.Self.Radio.Tx >= a.Self.Radio.TxOk + a.Self.Radio.TxFail);

        h.Advance(20_000);
        var b = h.GetStats();
        Assert.True(b.Self.UpSeconds > a.Self.UpSeconds);
        Assert.True(b.Self.Radio.Tx >= a.Self.Radio.Tx);
        Assert.True(b.Self.Radio.Rx > a.Self.Radio.Rx);
        Assert.True(b.Self.Link.Cmds >= a.Self.Link.Cmds);
        Assert.True(b.Self.Pc.LinesRx > a.Self.Pc.LinesRx);
        Assert.True(b.Self.Pc.LinesTx > a.Self.Pc.LinesTx);
        Assert.True(b.Self.LoopMaxUs >= a.Self.LoopMaxUs);
        for (int i = 0; i < 7; i++)
        {
            Assert.True(b.Devs[i].Link!.Rx >= a.Devs[i].Link!.Rx);
            Assert.True(b.Devs[i].Link!.Expected >= a.Devs[i].Link!.Expected);
            Assert.True(b.Devs[i].Link!.MacOk >= a.Devs[i].Link!.MacOk);
            Assert.True(b.Devs[i].Link!.Rx <= b.Devs[i].Link!.Expected);
        }
    }

    [Fact]
    public void Stats_streaming_is_controlled_by_statsCfg()
    {
        var h = Started();
        h.Clear();
        h.Advance(3000);
        Assert.Empty(h.Of<StatsMessage>());

        // out-of-range intervals are clamped by the request builder, so build the invalid requests by hand
        Assert.Equal("badArg", h.Request(new System.Text.Json.Nodes.JsonObject { ["t"] = "statsCfg", ["intervalMs"] = 100 })!.Err);
        Assert.Equal("badArg", h.Request(new System.Text.Json.Nodes.JsonObject { ["t"] = "statsCfg", ["intervalMs"] = -5 })!.Err);
        h.Ok(Requests.StatsCfg(1000));
        h.Clear();
        h.Advance(5000);
        var stats = h.Of<StatsMessage>().ToList();
        Assert.InRange(stats.Count, 4, 6);

        h.Ok(Requests.StatsCfg(0));
        h.Clear();
        h.Advance(3000);
        Assert.Empty(h.Of<StatsMessage>());
    }

    [Fact]
    public void Timer_counters_arrive_round_robin_while_streaming()
    {
        var h = Started();
        Assert.All(h.GetStats().Devs, d => Assert.Null(d.Timer));  // nobody asked yet

        h.Ok(Requests.StatsCfg(1000));
        h.Advance(9000);
        var s = h.LatestStats();
        Assert.All(s.Devs, d =>
        {
            Assert.NotNull(d.Timer);
            Assert.InRange(d.Timer!.AgeMs, 0, 10_000);   // never older than ~10 s
            Assert.StartsWith("2.1", d.Timer.Fw);
        });
        // data of different timers was captured at different moments
        Assert.True(s.Devs.Select(d => d.Timer!.AgeMs).Distinct().Count() > 3);
    }

    [Fact]
    public void Timer_counters_are_monotonic_between_reboots_and_restart_at_zero_after_a_power_cut()
    {
        var h = Started();
        h.Ok(Requests.StatsCfg(1000));
        h.Advance(10_000);
        var before = h.LatestStats().Devs[1].Timer!;
        h.Advance(5_000);
        var later = h.LatestStats().Devs[1].Timer!;
        Assert.True(later.Tx >= before.Tx);
        Assert.True(later.Rx >= before.Rx);
        Assert.True(later.Beats >= before.Beats);
        Assert.Equal(before.Boots, later.Boots);

        h.Dev.TestPowerCut(1, 4000, brownout: true);
        h.Advance(20_000);
        var after = h.LatestStats().Devs[1].Timer!;
        Assert.Equal(before.Boots + 1, after.Boots);
        Assert.True(after.UpSeconds < 25);
        Assert.True(after.Tx < before.Tx);                                // counters count since boot
        Assert.Equal(before.Rst.Brownout + 1, after.Rst.Brownout);        // lifetime reset causes survive
        Assert.Equal("brownout", after.RstReason);
    }

    [Fact]
    public void Plain_power_cuts_count_as_poweron_not_brownout()
    {
        var h = Started();
        h.Ok(Requests.StatsCfg(1000));
        h.Advance(8_000);
        var before = h.LatestStats().Devs[0].Timer!;
        h.Dev.TestPowerCut(0, 3000, brownout: false);
        h.Advance(20_000);
        var after = h.LatestStats().Devs[0].Timer!;
        Assert.Equal(before.Rst.PowerOn + 1, after.Rst.PowerOn);
        Assert.Equal(before.Rst.Brownout, after.Rst.Brownout);
        Assert.Equal("poweron", after.RstReason);
    }

    [Fact]
    public void Link_counters_see_offline_periods_and_reboots()
    {
        var h = Started();
        var before = h.GetStats().Devs[2].Link!;
        h.Dev.TestPowerCut(2, 15_000, false);
        h.Advance(30_000);
        var after = h.GetStats().Devs[2].Link!;
        Assert.Equal(before.OfflineEvents + 1, after.OfflineEvents);
        Assert.InRange(after.OfflineMs - before.OfflineMs, 8_000, 16_000);
        Assert.Equal(before.Reboots + 1, after.Reboots);
        Assert.True(after.MacFail > before.MacFail);               // frames sent to a dead timer are never acknowledged
    }

    [Fact]
    public void Chaos_produces_rxDrop_bursts_and_loop_stalls()
    {
        var h = new SimHarness(new SimOptions { Seed = 11, Chaos = true, Demo = false });
        h.StartSession();
        h.Ok(Requests.StatsCfg(1000));
        h.Advance(900_000); // 15 virtual minutes
        var s = h.LatestStats();
        Assert.True(s.Self.Radio.RxDrop > 0);
        Assert.True(s.Self.LoopMaxUs > 20_000);
        Assert.Contains(s.Devs, d => d.Timer is { RxDrop: > 0 });
        Assert.True(s.Self.Radio.TxFail > 0);
        Assert.Contains(h.Of<DevStatMessage>(), m => m.Entry.Timer is { Rst.Brownout: > 0 });
    }

    [Fact]
    public void Channel_counters_include_other_channels_after_a_sweep()
    {
        var h = Started();
        h.Ok(Requests.Recover(), 4800);
        var s = h.GetStats();
        Assert.Contains(s.Self.Chan, c => c.Channel == 6 && c.Tx > 0 && c.Rx > 0);
        Assert.Contains(s.Self.Chan, c => c.Channel != 6);
        Assert.True(s.Self.Link.Recovers >= 1);
    }

    [Fact]
    public void DevStats_asks_the_timer_and_answers_with_its_counters()
    {
        var h = Started();
        var r = h.Request(Requests.GetDevStats(CommandTarget.Dev(0)), 1000)!;
        Assert.True(r.Ok);
        Assert.NotNull(r.DevStat);
        Assert.Equal(0, r.DevStat!.Index);
        Assert.NotNull(r.DevStat.Link);
        Assert.NotNull(r.DevStat.Timer);
        Assert.Equal("2.1.0", r.DevStat.Timer!.Fw);
        Assert.InRange(r.DevStat.Timer.AgeMs, 0, 500);
    }

    [Fact]
    public void DevStats_for_a_timer_that_does_not_answer_returns_null_after_three_seconds()
    {
        var h = Started();
        h.Dev.TestPowerCut(4, 60_000, false);
        h.Advance(5200);
        int before = h.Received.Count;
        h.Request(Requests.GetDevStats(CommandTarget.Dev(4)), 0);
        int id = h.LastRequestId;
        h.Advance(2800);
        Assert.DoesNotContain(h.Of<ResponseMessage>(), x => x.Id == id);
        h.Advance(400);
        var res = h.Of<ResponseMessage>().Single(x => x.Id == id);
        Assert.True(res.Ok);
        Assert.NotNull(res.DevStat);
        Assert.NotNull(res.DevStat!.Link);
        Assert.Null(res.DevStat.Timer);
        _ = before;
    }

    [Fact]
    public void DevStats_validates_the_device()
    {
        var h = Started();
        Assert.Equal("noDevice", h.Request(Requests.GetDevStats(CommandTarget.Dev(30)))!.Err);
    }

    // ---- remote housekeeping --------------------------------------------------------------

    [Fact]
    public void The_battery_drains_slowly()
    {
        var h = Started();
        int start = h.Of<RemoteMessage>().Last().Bat;
        h.Advance(1_200_000);
        int end = h.Of<RemoteMessage>().Last().Bat;
        Assert.InRange(start - end, 5, 25);
        var r = h.Of<RemoteMessage>().Last();
        Assert.True(r.Usb);
        Assert.InRange(r.Mv, 3300, 4200);
    }

    [Fact]
    public void Reboot_closes_the_port_and_counts_a_boot()
    {
        var h = Started();
        int bootsBefore = h.Ok(Requests.HelloReq()).Hello!.Boots;
        TimeSpan? down = null;
        h.Dev.RebootRequested += d => down = d;
        h.Ok(Requests.Reboot());
        h.Advance(500);
        Assert.NotNull(down);
        Assert.InRange(down!.Value.TotalSeconds, 1, 10);

        var hello = h.Ok(Requests.HelloReq()).Hello!;
        Assert.Equal(bootsBefore + 1, hello.Boots);
        Assert.Equal("sw", hello.ResetReason);
        Assert.True(h.Dev.ConsumeBootBanner());
        Assert.False(h.Dev.ConsumeBootBanner());
    }

    [Fact]
    public void Factory_reset_needs_confirmation_and_clears_the_pairings()
    {
        var h = Started();
        var noConfirm = Requests.FactoryReset("remote");
        noConfirm["confirm"] = false;
        Assert.Equal("badArg", h.Request(noConfirm)!.Err);
        Assert.Equal("badArg", h.Request(Requests.FactoryReset("everything"))!.Err);
        Assert.Equal(7, h.Dev.PairedCount);

        h.Ok(Requests.FactoryReset("remote"), 500);
        Assert.Equal(0, h.Dev.PairedCount);
    }

    // ---- determinism ----------------------------------------------------------------------

    private static List<string> Script(int seed, bool chaos)
    {
        var h = new SimHarness(new SimOptions { Seed = seed, Chaos = chaos, Demo = true });
        h.StartSession();
        h.Ok(Requests.StatsCfg(1000));
        h.Ok(Requests.LogCfg("debug"));
        h.Ok(Requests.Sniff(true));
        h.Cmd(CommandSpec.SetTimes(CommandTarget.Dev(0), 2.0, 7.0), 700);
        h.Cmd(CommandSpec.Toggle(CommandTarget.All), 1000);
        h.Ok(Requests.Discover(true, true), 6000);
        h.Ok(Requests.Survey(), 4000);
        h.Ok(Requests.LinkTest(1, 30, 30), 1500);
        h.Advance(30_000);
        return h.RawLines;
    }

    [Fact]
    public void The_same_seed_gives_byte_identical_output()
    {
        var a = Script(42, chaos: true);
        var b = Script(42, chaos: true);
        Assert.True(a.Count > 500);
        Assert.Equal(a, b);
    }

    [Fact]
    public void Different_seeds_behave_differently()
    {
        Assert.NotEqual(Script(1, true), Script(2, true));
    }

    // ---- a timer plugged in directly ------------------------------------------------------

    private static SimHarness DirectTimer()
    {
        var h = new SimHarness(SimHarness.Quiet(kind: SimKind.Timer));
        h.StartSession();
        return h;
    }

    [Fact]
    public void A_direct_timer_looks_like_a_fleet_of_one()
    {
        var h = DirectTimer();
        Assert.Equal(DeviceKind.Timer, h.Ok(Requests.HelloReq()).Hello!.Kind);
        var f = h.LastFleet;
        var d = Assert.Single(f.Devs);
        Assert.Equal(0, d.Index);
        Assert.True(d.Online);
        var r = h.Of<RemoteMessage>().Last();
        Assert.Equal(-1, r.Bat);                              // timers have no battery
        Assert.True(r.Heap > 0);
    }

    [Fact]
    public void A_direct_timer_rejects_fleet_management_as_unsupported()
    {
        var h = DirectTimer();
        foreach (var req in new[]
                 {
                     Requests.Pair("AA:BB:CC:DD:EE:02", null), Requests.Unpair(0), Requests.Discover(true, false), Requests.Survey(),
                     Requests.Channel(11, false), Requests.LinkTest(0, 10, 40), Requests.Recover(), Requests.Select(0),
                     Requests.GetDevStats(CommandTarget.Dev(0)),
                 })
        {
            var r = h.Request(req)!;
            Assert.False(r.Ok);
            Assert.Equal("unsupported", r.Err);
        }
    }

    [Fact]
    public void A_direct_timer_executes_commands_on_dev_zero_only()
    {
        var h = DirectTimer();
        h.Cmd(CommandSpec.SetTimes(CommandTarget.Dev(0), 2.0, 4.0), 300);
        var r = h.Of<CmdResultMessage>().Last();
        Assert.Equal(CmdOutcome.Ok, r.Result);
        Assert.Equal(2.0, h.Fleet(0).Ton);
        Assert.Equal("noDevice", h.Request(Requests.Cmd(CommandSpec.Toggle(CommandTarget.Dev(1))))!.Err);
        h.Cmd(CommandSpec.Mode(CommandTarget.Dev(0), TimerMode.ForceOn, 500), 300);
        Assert.True(h.Fleet(0).On);
    }

    [Fact]
    public void A_direct_timer_has_timer_settings_and_stats_without_devs()
    {
        var h = DirectTimer();
        var s = h.Ok(Requests.GetSettings()).Settings!;
        Assert.Equal("off", s.StartPhase);
        Assert.NotNull(s.Ton);
        Assert.Null(s.TxPower);

        var set = h.Ok(Requests.Set(new JsonObject { ["name"] = "Bench", ["startPhase"] = "on", ["ton"] = 2.5, ["toff"] = 5.0, ["led"] = false })).Settings!;
        Assert.Equal("Bench", set.Name);
        Assert.Equal("on", set.StartPhase);
        Assert.Equal(false, set.Led);
        Assert.Equal("badArg", h.Request(Requests.Set(new JsonObject { ["ton"] = 0.0 }))!.Err);
        Assert.Equal("badArg", h.Request(Requests.Set(new JsonObject { ["startPhase"] = "maybe" }))!.Err);

        var stats = h.GetStats();
        Assert.Equal(DeviceKind.Timer, stats.Self.Kind);
        Assert.Empty(stats.Devs);
        Assert.NotNull(stats.Self.Timer);
        Assert.True(stats.Self.Timer!.Beats > 0);
        Assert.DoesNotContain(h.Of<DevStatMessage>(), _ => true);

        h.Ok(Requests.StatsCfg(1000));
        h.Advance(3000);
        Assert.NotEmpty(h.Of<StatsMessage>());
    }
}
