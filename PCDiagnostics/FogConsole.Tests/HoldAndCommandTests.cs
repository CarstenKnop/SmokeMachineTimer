using FogConsole.Hub;
using FogConsole.Protocol;
using FogConsole.Services;
using FogConsole.ViewModels;
using Xunit;

namespace FogConsole.Tests;

public class HoldControllerTests
{
    private const string Mac = "AA:BB:CC:DD:EE:01";

    private sealed class Rig
    {
        public readonly ManualTimeProvider Time = new();
        public readonly List<CommandSpec> Sent = new();
        public readonly List<CommandHandle> Handles = new();
        public readonly List<string> Problems = new();
        public HoldController Hold;

        /// <summary>How the fake remote answers confirmed commands.</summary>
        public Func<CommandSpec, CmdOutcome?> Answer = _ => CmdOutcome.Ok;

        public Rig()
        {
            Hold = new HoldController(Send, Time, Problems.Add);
        }

        private CommandHandle Send(CommandSpec spec)
        {
            Sent.Add(spec);
            var h = new CommandHandle(spec);
            Handles.Add(h);
            if (!spec.Confirm)
            {
                h.MarkQueued(0);
                return h;
            }
            var outcome = Answer(spec);
            if (outcome is null)
            {
                h.MarkFailed("not connected");
            }
            else
            {
                h.MarkQueued(1);
                h.AddResult(new CmdResultMessage(h.Id, 0, spec.Target.Mac ?? "", spec.Op, outcome.Value, outcome.Value.ToString().ToLowerInvariant(), 1, 12));
            }
            return h;
        }

        public string? ModeOf(CommandSpec s) => s.Args is not null && s.Args.TryGetValue("mode", out var m) ? m as string : null;
    }

    [Fact]
    public void Begin_sends_a_confirmed_forceOn_with_a_one_second_lease_to_the_timer_by_mac()
    {
        var r = new Rig();
        Assert.True(r.Hold.Begin(Mac, TimerMode.Auto));

        var first = Assert.Single(r.Sent);
        Assert.Equal("mode", first.Op);
        Assert.Equal("forceOn", r.ModeOf(first));
        Assert.Equal(1000, first.Args!["leaseMs"]);
        Assert.True(first.Confirm);
        Assert.Equal(Mac, first.Target.Mac);
        Assert.True(r.Hold.IsHolding(Mac));
    }

    [Fact]
    public void Refreshes_go_out_every_300_ms_without_confirmation_while_held()
    {
        var r = new Rig();
        r.Hold.Begin(Mac, TimerMode.Auto);
        r.Time.Advance(TimeSpan.FromMilliseconds(1000));

        var refreshes = r.Sent.Skip(1).ToList();
        Assert.Equal(3, refreshes.Count);                                 // 300, 600, 900 ms
        Assert.All(refreshes, s =>
        {
            Assert.False(s.Confirm);                                      // fire-and-forget
            Assert.Equal("forceOn", r.ModeOf(s));
            Assert.Equal(1000, s.Args!["leaseMs"]);
        });
    }

    [Fact]
    public void Releasing_sends_a_confirmed_mode_auto_and_stops_the_refreshes()
    {
        var r = new Rig();
        r.Hold.Begin(Mac, TimerMode.Auto);
        r.Time.Advance(TimeSpan.FromMilliseconds(700));
        int before = r.Sent.Count;

        r.Hold.End(Mac);

        var release = r.Sent.Last();
        Assert.Equal(before + 1, r.Sent.Count);
        Assert.Equal("auto", r.ModeOf(release));
        Assert.True(release.Confirm);
        Assert.False(r.Hold.IsHolding(Mac));

        r.Time.Advance(TimeSpan.FromSeconds(5));
        Assert.Equal(before + 1, r.Sent.Count);                            // nothing more: forceOn can never stay latched
        Assert.Equal(0, r.Time.ActiveTimers);
        Assert.Empty(r.Problems);
    }

    [Fact]
    public void A_timer_that_was_paused_goes_back_to_paused_not_to_auto()
    {
        var r = new Rig();
        r.Hold.Begin(Mac, TimerMode.Paused);
        r.Hold.End(Mac);
        Assert.Equal("paused", r.ModeOf(r.Sent.Last()));

        r.Hold.Begin(Mac, TimerMode.ForceOn);                               // anything else restores to auto
        r.Hold.End(Mac);
        Assert.Equal("auto", r.ModeOf(r.Sent.Last()));
    }

    [Fact]
    public void Beginning_twice_does_nothing_and_releasing_something_not_held_does_nothing()
    {
        var r = new Rig();
        Assert.True(r.Hold.Begin(Mac, TimerMode.Auto));
        Assert.False(r.Hold.Begin(Mac, TimerMode.Auto));
        Assert.Single(r.Sent);
        r.Hold.End("AA:BB:CC:DD:EE:99");
        Assert.Single(r.Sent);
    }

    [Fact]
    public void An_unconfirmed_release_is_retried_three_times_then_reported()
    {
        var r = new Rig { Answer = s => r_Mode(s) == "auto" ? CmdOutcome.Timeout : CmdOutcome.Ok };
        string? r_Mode(CommandSpec s) => s.Args is not null && s.Args.TryGetValue("mode", out var m) ? m as string : null;

        r.Hold.Begin(Mac, TimerMode.Auto);
        r.Hold.End(Mac);
        Assert.Equal(1, r.Sent.Count(s => r.ModeOf(s) == "auto"));
        Assert.Empty(r.Problems);

        r.Time.Advance(TimeSpan.FromMilliseconds(300));
        Assert.Equal(2, r.Sent.Count(s => r.ModeOf(s) == "auto"));
        r.Time.Advance(TimeSpan.FromMilliseconds(300));
        Assert.Equal(3, r.Sent.Count(s => r.ModeOf(s) == "auto"));
        r.Time.Advance(TimeSpan.FromSeconds(2));
        Assert.Equal(3, r.Sent.Count(s => r.ModeOf(s) == "auto"));          // gives up after three attempts

        var problem = Assert.Single(r.Problems);
        Assert.Contains("dead-man", problem);                               // explains that the lease protects anyway
    }

    [Fact]
    public void A_release_while_the_connection_is_gone_is_reported_without_throwing()
    {
        var r = new Rig();
        r.Hold.Begin(Mac, TimerMode.Auto);
        r.Answer = _ => null; // handle fails: not connected
        r.Hold.End(Mac);
        r.Time.Advance(TimeSpan.FromSeconds(3));
        Assert.Single(r.Problems);
    }

    [Fact]
    public void A_failed_first_forceOn_is_reported()
    {
        var r = new Rig { Answer = _ => CmdOutcome.Timeout };
        r.Hold.Begin(Mac, TimerMode.Auto);
        Assert.NotEmpty(r.Problems);
        Assert.Contains("did not answer", string.Join(" ", r.Problems));
    }

    [Fact]
    public void EndAll_releases_every_held_timer_and_Dispose_never_leaves_a_timer_running()
    {
        var r = new Rig();
        r.Hold.Begin("AA:BB:CC:DD:EE:01", TimerMode.Auto);
        r.Hold.Begin("AA:BB:CC:DD:EE:02", TimerMode.Auto);
        Assert.Equal(2, r.Hold.HeldCount);
        r.Time.Advance(TimeSpan.FromMilliseconds(350));
        Assert.Equal(2, r.Sent.Count(s => !s.Confirm));                     // one refresh per timer

        r.Hold.EndAll();
        Assert.Equal(0, r.Hold.HeldCount);
        Assert.Equal(2, r.Sent.Count(s => s.Confirm && r.ModeOf(s) == "auto"));

        r.Hold.Begin(Mac, TimerMode.Auto);
        r.Hold.Dispose();
        Assert.Equal(0, r.Time.ActiveTimers);
        Assert.False(r.Hold.Begin(Mac, TimerMode.Auto));                    // disposed: no new holds
    }

    [Fact]
    public void Two_timers_are_refreshed_independently_and_one_can_be_released_first()
    {
        var r = new Rig();
        r.Hold.Begin("AA:BB:CC:DD:EE:01", TimerMode.Auto);
        r.Hold.Begin("AA:BB:CC:DD:EE:02", TimerMode.Auto);
        r.Hold.End("AA:BB:CC:DD:EE:01");
        r.Sent.Clear();
        r.Time.Advance(TimeSpan.FromMilliseconds(650));
        var refreshed = r.Sent.Select(s => s.Target.Mac).Distinct().ToList();
        Assert.Equal(new[] { "AA:BB:CC:DD:EE:02" }, refreshed);
    }
}

public class CommandReportTests
{
    private static CommandHandle Done(CommandSpec spec, int expected, params (int dev, string mac, CmdOutcome outcome, int attempts, int ms)[] results)
    {
        var h = new CommandHandle(spec) { Id = 5 };
        h.MarkQueued(expected);
        foreach (var r in results)
            h.AddResult(new CmdResultMessage(5, r.dev, r.mac, spec.Op, r.outcome, r.outcome.ToString().ToLowerInvariant(), r.attempts, r.ms));
        return h;
    }

    private static string Name(CmdResultMessage r) => r.Dev switch { 0 => "Fog1", 1 => "Fog2", 2 => "Fog3", _ => "Fog?" };

    [Fact]
    public void A_confirmed_command_reports_attempts_and_time()
    {
        var h = Done(CommandSpec.SetTimes(CommandTarget.Dev(0), 1.5, 10), 1, (0, "A", CmdOutcome.Ok, 2, 180));
        var rep = CommandReport.Describe(h, Name);
        Assert.Equal(EntryStatus.Ok, rep.Status);
        Assert.Contains("180 ms", rep.Detail);
        Assert.Contains("2 attempts", rep.Detail);
        Assert.Null(rep.Problem);
    }

    [Fact]
    public void A_timeout_says_the_machine_may_be_heating_up()
    {
        var h = Done(CommandSpec.Toggle(CommandTarget.Dev(2)), 1, (2, "C", CmdOutcome.Timeout, 8, 2500));
        var rep = CommandReport.Describe(h, Name);
        Assert.Equal(EntryStatus.Failed, rep.Status);
        Assert.Equal("Fog3 did not answer — it may be powered off while heating.", rep.Problem);
    }

    [Fact]
    public void A_rejected_command_names_the_timer_and_the_op()
    {
        var h = Done(CommandSpec.Toggle(CommandTarget.Dev(1)), 1, (1, "B", CmdOutcome.Rejected, 1, 30));
        var rep = CommandReport.Describe(h, Name);
        Assert.Equal(EntryStatus.Failed, rep.Status);
        Assert.Contains("Fog2 rejected", rep.Problem);
        Assert.Contains("toggle", rep.Problem);
    }

    [Fact]
    public void Partial_success_for_several_timers_is_summarised()
    {
        var h = Done(CommandSpec.Toggle(CommandTarget.All), 3,
            (0, "A", CmdOutcome.Ok, 1, 10), (1, "B", CmdOutcome.Ok, 1, 12), (2, "C", CmdOutcome.Timeout, 8, 2500));
        var rep = CommandReport.Describe(h, Name);
        Assert.Equal(EntryStatus.Warning, rep.Status);
        Assert.Equal("2/3 confirmed, Fog3 timed out", rep.Detail);
    }

    [Fact]
    public void All_confirmed_for_several_timers()
    {
        var h = Done(CommandSpec.Toggle(CommandTarget.All), 2, (0, "A", CmdOutcome.Ok, 1, 10), (1, "B", CmdOutcome.Ok, 2, 90));
        var rep = CommandReport.Describe(h, Name);
        Assert.Equal(EntryStatus.Ok, rep.Status);
        Assert.Equal("2/2 confirmed", rep.Detail);
    }

    [Fact]
    public void Refused_failed_and_unanswered_requests()
    {
        var rejected = new CommandHandle(CommandSpec.Toggle(CommandTarget.Dev(9)));
        rejected.MarkRejected(new ResponseMessage { Ok = false, Err = "noDevice", Msg = "no such timer" });
        var rep = CommandReport.Describe(rejected, Name);
        Assert.Equal(EntryStatus.Failed, rep.Status);
        Assert.Contains("noDevice", rep.Problem);

        var failed = new CommandHandle(CommandSpec.Toggle(CommandTarget.Dev(0)));
        failed.MarkFailed("Not connected");
        Assert.Contains("not sent", CommandReport.Describe(failed, Name).Problem);

        var timedOut = new CommandHandle(CommandSpec.Toggle(CommandTarget.Dev(0)));
        timedOut.MarkQueued(1);
        timedOut.MarkTimedOut();
        Assert.Equal(EntryStatus.Failed, CommandReport.Describe(timedOut, Name).Status);
    }

    [Fact]
    public void Fire_and_forget_and_empty_targets()
    {
        var ff = new CommandHandle(CommandSpec.Mode(CommandTarget.Dev(0), TimerMode.ForceOn).WithoutConfirm());
        ff.MarkQueued(0);
        Assert.Equal(EntryStatus.Ok, CommandReport.Describe(ff, Name).Status);

        var nobody = new CommandHandle(CommandSpec.Toggle(CommandTarget.All));
        nobody.MarkQueued(0);
        var rep = CommandReport.Describe(nobody, Name);
        Assert.Equal(EntryStatus.Warning, rep.Status);
        Assert.Contains("No timers", rep.Problem);
    }

    [Fact]
    public void A_bulk_of_several_handles_is_aggregated_like_the_spec_example()
    {
        var handles = new List<CommandHandle>();
        for (int i = 0; i < 8; i++)
        {
            var h = new CommandHandle(CommandSpec.SetPhase(CommandTarget.Dev(i), CyclePhase.Off, i)) { Id = 10 + i };
            h.MarkQueued(1);
            h.AddResult(new CmdResultMessage(h.Id, i, $"M{i}", "setPhase", i == 2 ? CmdOutcome.Timeout : CmdOutcome.Ok, i == 2 ? "timeout" : "ok", 1, 20));
            handles.Add(h);
        }
        var (status, summary) = CommandReport.Aggregate(handles, r => $"Fog{r.Dev + 1}");
        Assert.Equal(EntryStatus.Warning, status);
        Assert.Equal("7/8 confirmed, Fog3 timed out", summary);

    }

    [Fact]
    public void Stagger_plan_spreads_the_timers_evenly_over_their_off_time()
    {
        var plan = DashboardViewModel.StaggerPlan(new[] { ("A", 10.0), ("B", 10.0), ("C", 10.0), ("D", 10.0), ("E", 20.0) });
        Assert.Equal(5, plan.Count);
        Assert.All(plan, p => Assert.Equal("setPhase", p.Op));
        Assert.All(plan, p => Assert.Equal("off", p.Args!["phase"]));
        var elapsed = plan.Select(p => Convert.ToDouble(p.Args!["elapsed"])).ToArray();
        Assert.Equal(new[] { 0.0, 2.0, 4.0, 6.0, 16.0 }, elapsed);          // toff * k / n, each timer with its own toff
        Assert.Equal(new[] { "A", "B", "C", "D", "E" }, plan.Select(p => p.Target.Mac));
    }
}
