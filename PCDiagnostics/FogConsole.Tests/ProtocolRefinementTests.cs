using System.Text.Json.Nodes;
using FogConsole.Hub;
using FogConsole.Protocol;
using FogConsole.Simulator;
using Xunit;

namespace FogConsole.Tests;

/// <summary>Protocol details added in the second revision: confirm:false, devStat events, 4.5 s online window, mV battery calibration.</summary>
public class ProtocolRefinementTests
{
    private static SimHarness Started(SimOptions? opt = null)
    {
        var h = new SimHarness(opt);
        h.StartSession();
        return h;
    }

    // ---- confirm:false --------------------------------------------------------------------

    [Fact]
    public void Confirm_false_is_only_written_when_requested()
    {
        var normal = JsonNode.Parse(Requests.Serialize(Requests.Cmd(CommandSpec.Mode(CommandTarget.Dev(0), TimerMode.ForceOn, 1000)), 1))!.AsObject();
        Assert.False(normal.ContainsKey("confirm"));

        var refresh = JsonNode.Parse(Requests.Serialize(Requests.Cmd(CommandSpec.Mode(CommandTarget.Dev(0), TimerMode.ForceOn, 1000).WithoutConfirm()), 2))!.AsObject();
        Assert.False(refresh["confirm"]!.GetValue<bool>());
        Assert.Equal("forceOn", refresh["mode"]!.GetValue<string>());
        Assert.Equal(1000, refresh["leaseMs"]!.GetValue<int>());
    }

    [Fact]
    public async Task A_fire_and_forget_command_completes_on_the_response_without_any_result()
    {
        var hub = new HubClient(new HubOptions { RequestTimeout = TimeSpan.FromMilliseconds(300), CommandTimeout = TimeSpan.FromMilliseconds(600), KeepAlive = false });
        var t = new FakeTransport
        {
            Responder = FakeTransport.Healthy(req => req["t"]!.GetValue<string>() == "cmd"
                ? new[] { FakeTransport.Ok(req["id"]!.GetValue<int>(), "\"queued\":1") }
                : null),
        };
        await hub.ConnectAsync(() => t);

        var sw = System.Diagnostics.Stopwatch.StartNew();
        var handle = await hub.SendCommandAsync(CommandSpec.Mode(CommandTarget.Dev(1), TimerMode.ForceOn, 1000).WithoutConfirm());

        Assert.True(sw.ElapsedMilliseconds < 400, "must not wait for a cmdResult that will never come");
        Assert.Equal(CommandState.Completed, handle.State);
        Assert.Empty(handle.Results);
        Assert.False(t.WrittenOfType("cmd").Single()["confirm"]!.GetValue<bool>());
        await hub.DisconnectAsync();
    }

    [Fact]
    public void The_simulator_applies_a_fire_and_forget_command_but_sends_no_cmdResult()
    {
        var h = Started();
        h.Cmd(CommandSpec.Reset(CommandTarget.Dev(1)), 400);
        h.Clear();

        var res = h.Cmd(CommandSpec.Mode(CommandTarget.Dev(1), TimerMode.ForceOn, 1000).WithoutConfirm(), 300);
        Assert.Equal(1, res.Queued);
        Assert.Empty(h.Of<CmdResultMessage>());
        Assert.Equal(TimerMode.ForceOn, h.Fleet(1).Mode);
        Assert.True(h.Fleet(1).On);

        // a "hold" is a stream of fire-and-forget refreshes ...
        for (int i = 0; i < 8; i++) h.Cmd(CommandSpec.Mode(CommandTarget.Dev(1), TimerMode.ForceOn, 1000).WithoutConfirm(), 300);
        Assert.Equal(TimerMode.ForceOn, h.Fleet(1).Mode);
        Assert.Empty(h.Of<CmdResultMessage>());

        // ... and the release is a normal, confirmed command
        h.Cmd(CommandSpec.Mode(CommandTarget.Dev(1), TimerMode.Auto), 400);
        Assert.Equal(TimerMode.Auto, h.Fleet(1).Mode);
        var r = Assert.Single(h.Of<CmdResultMessage>());
        Assert.Equal(CmdOutcome.Ok, r.Result);
        Assert.Equal("mode", r.Op);
    }

    [Fact]
    public void Fire_and_forget_commands_do_not_count_as_link_commands()
    {
        var h = Started();
        long before = h.GetStats().Self.Link.Cmds;
        for (int i = 0; i < 5; i++) h.Cmd(CommandSpec.Mode(CommandTarget.Dev(0), TimerMode.ForceOn, 1000).WithoutConfirm(), 100);
        Assert.Equal(before, h.GetStats().Self.Link.Cmds);
    }

    // ---- devStat events -------------------------------------------------------------------

    [Fact]
    public void Every_stats_event_is_followed_by_one_devStat_per_paired_timer()
    {
        var h = Started();
        h.Ok(Requests.StatsCfg(1000));
        h.Clear();
        h.Advance(4200);

        var kinds = h.Received.Where(m => m is StatsMessage or DevStatMessage).ToList();
        int stats = kinds.Count(m => m is StatsMessage);
        Assert.InRange(stats, 3, 5);
        Assert.Equal(stats * 7, kinds.Count(m => m is DevStatMessage));

        // order: stats, then devStat 0..6, then the next stats
        for (int i = 0; i < kinds.Count; i += 8)
        {
            Assert.IsType<StatsMessage>(kinds[i]);
            for (int k = 1; k <= 7 && i + k < kinds.Count; k++)
            {
                var d = Assert.IsType<DevStatMessage>(kinds[i + k]);
                Assert.Equal(k - 1, d.Entry.Index);
                Assert.NotNull(d.Entry.Link);
            }
        }
    }

    [Fact]
    public void The_stats_event_no_longer_nests_the_timers()
    {
        var h = Started();
        h.Ok(Requests.StatsCfg(1000));
        h.Advance(2500);
        var line = h.RawLines.First(l => l.Contains("\"t\":\"stats\""));
        var o = JsonNode.Parse(line)!.AsObject();
        Assert.True(o.ContainsKey("self"));
        Assert.False(o.ContainsKey("devs"));
    }

    [Fact]
    public void Get_stats_answers_with_the_self_part_and_then_emits_the_devStat_events()
    {
        var h = Started();
        h.Clear();
        int id = 0;
        var res = h.Ok(Requests.GetStats());
        id = res.Id;
        Assert.NotNull(res.Stats);
        Assert.Equal(DeviceKind.Remote, res.Stats!.Self.Kind);
        Assert.Empty(res.Stats.Devs);

        var raw = h.RawLines.First(l => l.Contains($"\"id\":{id}"));
        var body = JsonNode.Parse(raw)!.AsObject();
        Assert.True(body["stats"]!.AsObject().ContainsKey("radio"));      // res.stats is the self object

        int resIdx = h.Received.FindIndex(m => m is ResponseMessage r && r.Id == id);
        var after = h.Received.Skip(resIdx + 1).OfType<DevStatMessage>().ToList();
        Assert.Equal(7, after.Count);
    }

    [Fact]
    public void A_timer_connected_directly_has_no_devStat_events()
    {
        var h = Started(SimHarness.Quiet(kind: SimKind.Timer));
        h.Ok(Requests.StatsCfg(1000));
        h.Advance(3000);
        Assert.NotEmpty(h.Of<StatsMessage>());
        Assert.Empty(h.Of<DevStatMessage>());
        Assert.NotNull(h.Of<StatsMessage>().Last().Snapshot.Self.Timer);
    }

    // ---- 4.5 s online window --------------------------------------------------------------

    [Fact]
    public void A_silent_timer_is_still_online_after_three_seconds_and_offline_after_five()
    {
        var h = Started();
        h.Dev.CutPower(2, TimeSpan.FromSeconds(60));
        h.Advance(3000);
        Assert.True(h.Fleet(2).Online);
        h.Advance(2300);
        Assert.False(h.Fleet(2).Online);
        Assert.True(h.Fleet(2).AgeMs > 4500);
        Assert.Equal(4500, SimDevice.OnlineWindowMs);
    }

    // ---- per-channel counters exclude searches --------------------------------------------

    [Fact]
    public void Channel_searches_do_not_show_up_in_the_per_channel_counters()
    {
        var h = Started();
        h.Dev.CutPower(4, TimeSpan.FromSeconds(12));
        h.Advance(5200);
        h.Ok(Requests.Channel(11, false), 6000);
        h.Advance(12_000);                             // power returns on the old channel: stranded

        var before = h.GetStats().Self.Chan.ToDictionary(c => c.Channel);
        h.Ok(Requests.Recover(), 5000);                // sweeps all channels with unicast probes
        var after = h.GetStats().Self.Chan.ToDictionary(c => c.Channel);

        foreach (var ch in new[] { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 13 })
        {
            if (!before.TryGetValue(ch, out var b)) { Assert.False(after.ContainsKey(ch) && after[ch].Tx > 0); continue; }
            Assert.Equal(b.Tx, after[ch].Tx);
            Assert.Equal(b.Fail, after[ch].Fail);
        }
        Assert.True(after[11].Tx > before[11].Tx);     // normal traffic on the new home channel keeps counting
    }

    [Fact]
    public void Home_channel_counters_follow_a_channel_change()
    {
        var h = Started();
        var a = h.GetStats().Self.Chan.ToDictionary(c => c.Channel);
        h.Ok(Requests.Channel(11, false), 6000);
        h.Advance(10_000);
        var b = h.GetStats().Self.Chan.ToDictionary(c => c.Channel);
        Assert.True(b[11].Tx > 0);
        Assert.True(b[11].Rx > 0);
        Assert.True(b[6].Tx - a[6].Tx < 100);          // channel 6 stopped counting after the move
    }

    // ---- battery calibration in millivolts ------------------------------------------------

    [Fact]
    public void Battery_millivolts_follow_the_calibration_points()
    {
        var h = Started();
        var cal = h.Ok(Requests.GetSettings()).Settings!.BatCal!;
        Assert.Equal(new[] { 3300, 3750, 4150 }, cal);
        h.Advance(2000);
        var r = h.Of<RemoteMessage>().Last();
        double expected = r.Bat <= 50 ? 3300 + 450 * r.Bat / 50.0 : 3750 + 400 * (r.Bat - 50) / 50.0;
        Assert.InRange(r.Mv, expected - 15, expected + 15);

        h.Ok(Requests.Set(new JsonObject { ["batCal"] = new JsonArray(3000, 3500, 4000) }));
        h.Advance(2000);
        r = h.Of<RemoteMessage>().Last();
        expected = r.Bat <= 50 ? 3000 + 500 * r.Bat / 50.0 : 3500 + 500 * (r.Bat - 50) / 50.0;
        Assert.InRange(r.Mv, expected - 15, expected + 15);
    }

    // ---- demo controls --------------------------------------------------------------------

    [Fact]
    public void Cutting_power_by_hand_behaves_like_a_heating_machine()
    {
        var h = Started();
        int boot = h.Fleet(3).Boot;
        Assert.True(h.Dev.CutPower(3, TimeSpan.FromSeconds(30)));
        Assert.False(h.Dev.CutPower(99, TimeSpan.FromSeconds(30)));
        h.Advance(8000);
        Assert.False(h.Fleet(3).Online);
        h.Advance(30_000);
        Assert.True(h.Fleet(3).Online);
        Assert.Equal(boot + 1, h.Fleet(3).Boot);
    }

    [Fact]
    public void Extra_packet_loss_makes_the_links_worse()
    {
        var quiet = Started();
        quiet.Advance(120_000);
        var lossy = Started();
        lossy.Dev.ExtraLossPercent = 40;
        lossy.Advance(120_000);
        Assert.Equal(40, lossy.Dev.ExtraLossPercent);

        // loss % over the last 32 frames, averaged over the last 20 status events and all timers
        double Loss(SimHarness h) => h.Of<FleetMessage>().TakeLast(20).SelectMany(f => f.Devs).Average(d => Math.Max(0, d.Loss));
        Assert.True(Loss(lossy) > Loss(quiet) + 10, $"quiet {Loss(quiet):0.0}% lossy {Loss(lossy):0.0}%");
        Assert.Contains(lossy.Of<FleetMessage>().Last().Devs, d => d.Loss > 20);
    }

    [Fact]
    public void Interference_on_the_current_channel_shows_in_the_survey_and_hurts_the_link_until_it_ends()
    {
        var h = Started();
        h.Ok(Requests.Survey(), 3800);
        int scoreBefore = h.Of<SurveyChMessage>().Last(c => c.Channel == 6).Score;

        Assert.Equal(6, h.Dev.InjectInterference(TimeSpan.FromSeconds(20)));
        h.Clear();
        h.Ok(Requests.Survey(), 3800);
        var during = h.Of<SurveyChMessage>().Single(c => c.Channel == 6);
        Assert.True(during.Score >= scoreBefore);
        Assert.True(during.ApRssi > -45);
        Assert.True(h.Of<SurveyDoneMessage>().Single().Best.All(b => b != 6));

        h.Advance(25_000);
        h.Clear();
        h.Ok(Requests.Survey(), 3800);
        var after = h.Of<SurveyChMessage>().Single(c => c.Channel == 6);
        Assert.True(after.ApRssi < during.ApRssi);
    }

    [Fact]
    public void Interference_raises_the_loss_of_every_link_while_it_lasts()
    {
        var h = Started();
        h.Advance(60_000);
        var calm = h.LastFleet.Devs.Average(d => Math.Max(0, d.Loss));
        h.Dev.InjectInterference(TimeSpan.FromSeconds(120));
        h.Advance(60_000);
        var jammed = h.LastFleet.Devs.Average(d => Math.Max(0, d.Loss));
        Assert.True(jammed > calm + 8, $"calm {calm:0.0}% jammed {jammed:0.0}%");
    }

    [Fact]
    public void The_number_of_timers_is_a_start_option()
    {
        foreach (int n in new[] { 1, 4, 10 })
        {
            var h = Started(new SimOptions { Seed = 5, Chaos = false, Demo = false, PairedTimers = n, TotalTimers = Math.Min(13, n + 3) });
            Assert.Equal(n, h.LastFleet.Devs.Count);
            Assert.Equal(n, h.Dev.PairedCount);
        }
    }
}
