using System.Globalization;
using FogConsole.Hub;
using FogConsole.Protocol;
using FogConsole.Services;
using FogConsole.ViewModels;
using Xunit;

namespace FogConsole.Tests;

/// <summary>
/// End-to-end: the real HubClient and view models against the simulated remote (real time, quiet world).
/// Everything the UI does goes through exactly the code paths the application uses.
/// </summary>
public class IntegrationTests
{
    public IntegrationTests() => CultureInfo.CurrentCulture = CultureInfo.InvariantCulture;

    [Fact]
    public async Task Connecting_to_the_simulator_fills_the_dashboard_and_the_remote_chip()
    {
        using var app = new TestApp();
        await app.ConnectSimAsync();
        await app.PumpUntil(() => app.Fleet.Remote.HasData && app.Main.Settings.HasLoaded);

        Assert.Equal(HubState.Connected, app.Main.State);
        Assert.True(app.Main.IsSimulated);
        Assert.True(app.Fleet.IsLive);
        Assert.Equal(DeviceKind.Remote, app.Fleet.Remote.Kind);
        Assert.Equal("Remote", app.Fleet.Remote.Name);
        Assert.Equal(7, app.Fleet.Devices.Count);
        Assert.All(app.Fleet.Devices, d => Assert.True(d.Online));
        Assert.InRange(app.Fleet.Remote.Battery, 50, 100);
        Assert.True(app.Fleet.Remote.UsbPower);
        Assert.Equal(6, app.Fleet.HomeChannel);
        Assert.Equal(-90, app.Fleet.Remote.RssiLow);       // from get settings
        Assert.Equal("Simulated", app.Main.ConnectionText);      // the chip says what it is while the simulator runs
        Assert.True(app.Main.Dashboard.ShowGrid);
        Assert.False(app.Main.Dashboard.ShowConnectPrompt);
        Assert.Contains("FogSerial/2", app.Main.ProtocolText);
        Assert.NotNull(app.Fleet.Selected);
    }

    [Fact]
    public async Task A_card_action_is_confirmed_recorded_and_reported()
    {
        using var app = new TestApp();
        await app.ConnectSimAsync();
        var dev = app.Fleet.Devices[0];
        app.Toasts.Toasts.Clear();                          // the "connected" toast is not what we are looking at

        dev.ToggleCommand.Execute(null);
        await app.PumpUntil(() => app.Services.Commands.History.Count > 0 && app.Services.Commands.History[0].Status != EntryStatus.Pending);

        var entry = app.Services.Commands.History[0];
        Assert.Equal(EntryStatus.Ok, entry.Status);
        Assert.Equal("Toggle", entry.Title);
        Assert.Equal("Fog1", entry.DeviceName);
        Assert.Contains("confirmed", entry.Detail);
        Assert.Contains("Fog1: Toggle", app.Toasts.StatusText);
        Assert.Empty(app.Toasts.Toasts);                    // successes are quiet: no toast, only the status bar
    }

    [Fact]
    public async Task Hold_to_fog_forces_the_output_on_only_while_held_and_then_returns_to_auto()
    {
        using var app = new TestApp();
        await app.ConnectSimAsync();
        var dev = app.Fleet.Devices[1];
        int historyBefore = app.Services.Commands.History.Count;

        dev.HoldStartCommand.Execute(null);
        await app.PumpUntil(() => dev.Mode == TimerMode.ForceOn, 3000);
        Assert.True(app.Fleet.AnyForceOn);
        Assert.Contains("Fog2", app.Main.ForceOnText);
        Assert.True(dev.IsHeld);

        await app.Pause(1700);                               // longer than the 1 s lease: only the refreshes keep it on
        Assert.Equal(TimerMode.ForceOn, dev.Mode);
        Assert.True(dev.OutputOn);

        dev.HoldEndCommand.Execute(null);
        await app.PumpUntil(() => dev.Mode == TimerMode.Auto, 3000);
        Assert.False(app.Fleet.AnyForceOn);
        Assert.False(dev.IsHeld);
        Assert.Equal(historyBefore, app.Services.Commands.History.Count);   // refreshes do not spam the command history

        await app.Pause(1500);
        Assert.Equal(TimerMode.Auto, dev.Mode);              // never latched
    }

    [Fact]
    public async Task Losing_the_connection_while_holding_never_leaves_the_output_forced_on()
    {
        using var app = new TestApp();
        await app.ConnectSimAsync();
        var dev = app.Fleet.Devices[2];
        dev.HoldStartCommand.Execute(null);
        await app.PumpUntil(() => dev.Mode == TimerMode.ForceOn, 3000);

        await app.Hub.DisconnectAsync();
        await app.PumpUntil(() => app.Main.State == HubState.Disconnected);
        Assert.Equal(0, app.Services.Hold.HeldCount);        // released locally
        Assert.False(app.Fleet.AnyForceOn);

        // nothing refreshes the lease any more: the timer's dead-man lease switches the output off by itself
        app.Device.Advance(TimeSpan.FromSeconds(2));
        Assert.Equal(TimerMode.Auto, app.Device.TestRead(l => l[2].Mode));
    }

    [Fact]
    public async Task A_command_to_a_heating_timer_fails_with_a_friendly_message()
    {
        using var app = new TestApp();
        await app.ConnectSimAsync();
        app.Device.CutPower(2, TimeSpan.FromSeconds(90));
        await app.PumpUntil(() => !app.Fleet.Devices[2].Online, 9000);
        var dev = app.Fleet.Devices[2];
        Assert.False(dev.ToggleCommand.CanExecute(null));    // the card's buttons are disabled

        var entry = app.Services.Commands.Run(CommandSpec.Toggle(CommandTarget.ByMac(dev.Mac)), "Toggle", dev.DisplayName, dev.Mac);
        await app.PumpUntil(() => entry.Status != EntryStatus.Pending, 7000);

        Assert.Equal(EntryStatus.Failed, entry.Status);
        Assert.Contains(app.Toasts.Toasts, t => t.Message == "Fog3 did not answer — it may be powered off while heating." && t.Kind == ToastKind.Error);
    }

    [Fact]
    public async Task A_bulk_action_reports_one_aggregated_result()
    {
        using var app = new TestApp();
        await app.ConnectSimAsync();
        app.Device.CutPower(3, TimeSpan.FromSeconds(90));
        await app.PumpUntil(() => !app.Fleet.Devices[3].Online, 9000);

        _ = ((CommunityToolkit.Mvvm.Input.IAsyncRelayCommand)app.Main.Dashboard.PauseAllCommand).ExecuteAsync(null);
        await app.PumpUntil(() => app.Toasts.Toasts.Any(t => t.Message.StartsWith("Pause all")), 8000);

        var toast = app.Toasts.Toasts.Single(t => t.Message.StartsWith("Pause all"));
        Assert.Equal("Pause all: 6/7 confirmed, Fog4 timed out", toast.Message);
        Assert.Equal(ToastKind.Warning, toast.Kind);
        await app.PumpUntil(() => app.Fleet.Devices.Count(d => d.Mode == TimerMode.Paused) == 6, 3000);
        Assert.Equal(EntryStatus.Warning, app.Services.Commands.History[0].Status);
    }

    [Fact]
    public async Task Pause_all_without_problems_is_quiet_and_resume_undoes_it()
    {
        using var app = new TestApp();
        await app.ConnectSimAsync();
        app.Toasts.Toasts.Clear();
        _ = ((CommunityToolkit.Mvvm.Input.IAsyncRelayCommand)app.Main.Dashboard.PauseAllCommand).ExecuteAsync(null);
        await app.PumpUntil(() => app.Fleet.Devices.All(d => d.Mode == TimerMode.Paused), 5000);
        Assert.Contains("7/7 confirmed", app.Toasts.StatusText);
        Assert.Empty(app.Toasts.Toasts);

        _ = ((CommunityToolkit.Mvvm.Input.IAsyncRelayCommand)app.Main.Dashboard.ResumeAllCommand).ExecuteAsync(null);
        await app.PumpUntil(() => app.Fleet.Devices.All(d => d.Mode == TimerMode.Auto), 5000);
    }

    [Fact]
    public async Task The_application_survives_the_remote_rebooting_and_reconnects_by_itself()
    {
        using var app = new TestApp(new HubOptions { ReconnectInterval = TimeSpan.FromMilliseconds(400) });
        await app.ConnectSimAsync();
        await app.PumpUntil(() => app.Fleet.Remote.HasData);
        int boots = app.Fleet.Remote.Boots;

        await app.Hub.RequestAsync(Requests.Reboot());
        await app.PumpUntil(() => app.Main.State == HubState.WaitingForDevice, 4000);
        Assert.True(app.Main.IsWaiting);
        Assert.False(app.Fleet.IsLive);
        Assert.NotEmpty(app.Fleet.Devices);                  // the cards stay (disabled) while we wait
        Assert.False(app.Fleet.Devices[0].CanControl);
        Assert.Contains(app.Toasts.Toasts, t => t.Message.Contains("asleep or unplugged"));
        Assert.Contains("waiting", app.Main.WaitingText, StringComparison.OrdinalIgnoreCase);

        await app.PumpUntil(() => app.Main.State == HubState.Connected, 15000);
        await app.PumpUntil(() => app.Fleet.Remote.Boots == boots + 1, 3000);
        Assert.True(app.Fleet.IsLive);
        Assert.True(app.Fleet.Devices[0].CanControl);

        app.Main.Console.Flush();
        Assert.Contains(app.Main.Console.RawRows, r => r.Kind == RawKind.PlainText && r.Text.StartsWith("ESP-ROM"));   // boot text is shown in the console
        Assert.Contains(app.Main.Console.LogRows, r => r.Source == "serial");
    }

    [Fact]
    public async Task The_counter_stream_runs_only_while_the_diagnostics_page_is_visible()
    {
        using var app = new TestApp();
        await app.ConnectSimAsync();
        int stats = 0;
        app.Hub.RawLineSeen += l =>
        {
            if (l.Direction == RawDirection.Rx && l.Text.Contains("\"t\":\"stats\"")) Interlocked.Increment(ref stats);
        };
        await app.Pause(1500);
        Assert.Equal(0, stats);                               // streaming is off on the dashboard

        app.Main.Navigate(NavPage.Diagnostics);
        await app.PumpUntil(() => app.Main.Diagnostics.HasData && app.Main.Diagnostics.Devices.Count == 7 && Volatile.Read(ref stats) >= 2, 8000);

        var d = app.Main.Diagnostics;
        Assert.NotEmpty(d.Radio.Items);
        Assert.Contains(d.Radio.Items, m => m.Name.StartsWith("Sent"));
        Assert.NotEmpty(d.Resets.Items);
        Assert.NotEmpty(d.RemoteChips);
        Assert.True(d.HasChannelRows);
        Assert.All(d.Devices, x => Assert.NotEmpty(x.Link.Items));
        Assert.All(d.Devices, x => Assert.NotEmpty(x.Chips));
        await app.PumpUntil(() => d.Devices.All(x => x.HasTimer), 12_000);   // the timers' own counters arrive round-robin
        Assert.All(d.Devices, x => Assert.NotEmpty(x.Timer.Items));
        Assert.Contains("Streaming", d.StatusText);

        app.Main.Navigate(NavPage.Dashboard);
        await app.Pause(500);
        int atLeave = Volatile.Read(ref stats);
        await app.Pause(2500);
        Assert.InRange(Volatile.Read(ref stats) - atLeave, 0, 1);            // switched off again (one in-flight event at most)
    }

    [Fact]
    public async Task Diagnostics_can_compare_against_a_baseline_and_export()
    {
        using var app = new TestApp();
        await app.ConnectSimAsync();
        app.Main.Navigate(NavPage.Diagnostics);
        await app.PumpUntil(() => app.Main.Diagnostics.Devices.Count == 7 && app.Main.Diagnostics.Devices.All(x => x.Link.Items.Count > 0), 8000);
        var d = app.Main.Diagnostics;

        d.SetBaselineCommand.Execute(null);
        Assert.True(d.HasBaseline);
        await app.Pause(3500);
        await app.PumpUntil(() => { d.Tick(); return d.CompareRows.Count > 0; }, 5000);
        Assert.Contains(d.CompareRows, r => r.Name == "radio.tx" && r.Delta.StartsWith("+"));

        d.CopyJsonCommand.Execute(null);
        Assert.NotNull(app.Dialogs.Clipboard);
        var json = System.Text.Json.Nodes.JsonNode.Parse(app.Dialogs.Clipboard!)!.AsObject();
        Assert.True(json.ContainsKey("stats"));
        Assert.True(json.ContainsKey("rates"));
        Assert.True(json.ContainsKey("verdicts"));
        Assert.True(json.ContainsKey("devStats"));

        app.Dialogs.SavePath = "counters.csv";
        d.ExportCsvCommand.Execute(null);
        Assert.StartsWith("time,scope,device,name,metric,value,perSecond", app.Dialogs.Saved["counters.csv"]);

        d.ClearBaselineCommand.Execute(null);
        Assert.False(d.HasBaseline);
        Assert.Empty(d.CompareRows);

        d.ResetChannelTableCommand.Execute(null);
        Assert.False(d.HasChannelRows);
    }

    [Fact]
    public async Task A_survey_a_confirmed_channel_move_and_the_migration_progress_work_through_the_channels_page()
    {
        using var app = new TestApp();
        await app.ConnectSimAsync();
        app.Main.Navigate(NavPage.Channels);
        var ch = app.Main.Channels;

        ch.SurveyCommand.Execute(null);
        await app.PumpUntil(() => ch.IsSurveying, 2000);
        Assert.Equal("busy", (await app.Hub.RequestAsync(Requests.Survey())).Err);   // the radio really is busy
        await app.PumpUntil(() => !ch.IsSurveying && ch.Cells.All(c => c.HasData), 9000);

        Assert.NotNull(ch.BestChannel);
        Assert.Contains("Best: channel", ch.Recommendation);
        Assert.Contains(ch.Cells, c => c.IsCurrent && c.Channel == 6);
        Assert.Equal(3, ch.Cells.Count(c => c.BestRank > 0));
        int target = ch.BestChannel!.Value;
        Assert.NotEqual(6, target);
        Assert.Equal(target, ch.SelectedChannel);              // the recommended channel is pre-selected
        Assert.Equal($"Move system to channel {target}", ch.MoveButtonText);

        // declined: nothing happens
        app.Dialogs.ConfirmAnswer = false;
        ch.MoveCommand.Execute(null);
        await app.Pause(800);
        Assert.Single(app.Dialogs.Asked);
        Assert.Contains($"channel {target}", app.Dialogs.Asked[0]);
        Assert.Equal(6, app.Fleet.HomeChannel);

        // confirmed
        app.Dialogs.ConfirmAnswer = true;
        ch.MoveCommand.Execute(null);
        await app.PumpUntil(() => ch.MigrationActive, 3000);
        await app.PumpUntil(() => !ch.MigrationActive && app.Fleet.HomeChannel == target, 12_000);
        Assert.Equal(7, ch.MigrationItems.Count);
        Assert.All(ch.MigrationItems, i => Assert.Equal(MigrationStatus.Confirmed, i.Status));
        Assert.Contains($"channel {target}", ch.MigrationSummary);
        Assert.Contains(app.Toasts.Toasts, t => t.Kind == ToastKind.Success && t.Message.Contains($"channel {target}"));
        await app.PumpUntil(() => app.Fleet.Devices.All(x => x.Online), 8000);   // everybody followed
    }

    [Fact]
    public async Task Lost_timers_are_found_again_after_a_channel_move()
    {
        using var app = new TestApp();
        await app.ConnectSimAsync();
        app.Dialogs.ConfirmAnswer = true;
        app.Device.CutPower(4, TimeSpan.FromSeconds(14));      // heating while the system moves
        await app.PumpUntil(() => !app.Fleet.Devices[4].Online, 9000);

        app.Main.Navigate(NavPage.Channels);
        var ch = app.Main.Channels;
        ch.SelectedChannel = 11;
        ch.MoveCommand.Execute(null);
        await app.PumpUntil(() => app.Fleet.HomeChannel == 11 && !ch.MigrationActive, 12_000);
        Assert.Equal(6, ch.MigrationItems.Count);               // only the online timers were asked
        await app.PumpUntil(() => app.Device.TestRead(l => l[4].Powered), 20_000);
        await app.Pause(5500);
        Assert.False(app.Fleet.Devices[4].Online);              // it came back on the old channel and is lost

        ch.RecoverCommand.Execute(null);
        await app.PumpUntil(() => ch.IsRecovering, 3000);
        await app.PumpUntil(() => { ch.Tick(); return !ch.IsRecovering; }, 14_000);     // the page's 1 Hz housekeeping ends the sweep
        Assert.Contains(ch.RecoverLog, l => l.StartsWith("Found Fog5"));
        await app.PumpUntil(() => app.Fleet.Devices[4].Online, 8000);
    }

    [Fact]
    public async Task The_link_lab_runs_a_test_and_keeps_a_history_with_the_channel()
    {
        using var app = new TestApp();
        await app.ConnectSimAsync();
        app.Main.Navigate(NavPage.Channels);
        var ch = app.Main.Channels;
        Assert.NotNull(ch.LabDevice);
        ch.LabCount = 20;
        ch.LabGapMs = 20;

        ch.RunLinkTestCommand.Execute(null);
        await app.PumpUntil(() => ch.IsTesting, 2000);
        await app.PumpUntil(() => !ch.IsTesting && ch.LabHistory.Count == 1, 6000);

        var r = ch.LabHistory[0];
        Assert.Equal(6, r.Channel);
        Assert.Equal(20, r.Sent);
        Assert.InRange(r.EchoPercent, 50, 100);
        Assert.True(r.RttAvg > 0);
        Assert.Equal(1.0, ch.LiveProgress);
        Assert.Contains("answered", ch.LabStatus);
    }

    [Fact]
    public async Task Pairing_discover_pair_and_unpair_roundtrip()
    {
        using var app = new TestApp();
        await app.ConnectSimAsync();
        app.Dialogs.ConfirmAnswer = true;
        app.Main.Navigate(NavPage.Pairing);
        var p = app.Main.Pairing;

        p.DiscoverOn = true;
        await app.PumpUntil(() => p.Found.Count(f => !f.IsPaired) >= 2, 6000);
        Assert.Equal(7, p.Found.Count(f => f.IsPaired));
        var fresh = p.Found.First(f => !f.IsPaired);
        Assert.Equal("Fog8", fresh.PairName);                  // the first free name
        Assert.Equal(2, p.Found.Where(f => !f.IsPaired).Select(f => f.PairName).Distinct().Count());

        fresh.PairName = "Garage";
        p.PairCommand.Execute(fresh);
        await app.PumpUntil(() => app.Fleet.Devices.Count == 8, 5000);
        Assert.Equal("Garage", app.Fleet.Devices[7].HubName);
        await app.PumpUntil(() => fresh.IsPaired, 3000);
        Assert.Contains(app.Toasts.Toasts, t => t.Message.Contains("Paired Garage"));

        p.UnpairCommand.Execute(app.Fleet.Devices[2]);
        await app.PumpUntil(() => app.Fleet.Devices.Count == 7, 5000);
        Assert.DoesNotContain(app.Fleet.Devices, d => d.HubName == "Fog3");
        Assert.Equal(Enumerable.Range(0, 7), app.Fleet.Devices.Select(d => d.Index));

        app.Main.Navigate(NavPage.Dashboard);                  // leaving the page stops discovery
        Assert.False(p.DiscoverOn);
        await app.PumpUntil(() => !app.Fleet.Remote.Scanning, 3000);
    }

    [Fact]
    public async Task Pairing_a_declined_unpair_does_nothing()
    {
        using var app = new TestApp();
        await app.ConnectSimAsync();
        app.Dialogs.ConfirmAnswer = false;
        app.Main.Pairing.UnpairCommand.Execute(app.Fleet.Devices[0]);
        await app.Pause(600);
        Assert.Equal(7, app.Fleet.Devices.Count);
        Assert.Single(app.Dialogs.Asked);
    }

    [Fact]
    public async Task Remote_settings_are_read_validated_and_saved()
    {
        using var app = new TestApp();
        await app.ConnectSimAsync();
        var s = app.Main.Settings;
        await app.PumpUntil(() => s.HasLoaded);
        Assert.Equal(80, s.TxPower);
        Assert.Equal("20 dBm  (≈ 100 mW)", s.TxPowerText);
        Assert.Equal(200, s.Brightness);
        Assert.Equal(60, s.AutoOff);
        Assert.Equal(3300, s.BatCal0);
        Assert.Equal(4150, s.BatCal100);

        s.Brightness = 123;
        s.RssiLow = -95;
        s.ApplyCommand.Execute(null);
        await app.PumpUntil(() => app.Toasts.Toasts.Any(t => t.Message.Contains("Remote settings saved")), 4000);
        Assert.Equal(-95, app.Fleet.Remote.RssiLow);                         // the dashboard's signal bars use it right away
        var res = await app.Hub.RequestAsync(Requests.GetSettings());
        Assert.Equal(123, res.Settings!.Brightness);
        Assert.Equal(-95, res.Settings.RssiLow);

        // invalid combinations never reach the device
        app.Toasts.Toasts.Clear();
        s.RssiHigh = s.RssiLow + 2;
        s.ApplyCommand.Execute(null);
        await app.Pause(400);
        Assert.Contains(app.Toasts.Toasts, t => t.Kind == ToastKind.Warning && t.Message.Contains("at least 5"));
        Assert.Equal(-50, (await app.Hub.RequestAsync(Requests.GetSettings())).Settings!.RssiHigh);

        // live battery voltage can be adopted as a calibration point
        await app.PumpUntil(() => app.Fleet.Remote.Millivolts > 0);
        s.UseLiveAsCommand.Execute("50");
        Assert.InRange(s.BatCal50, app.Fleet.Remote.Millivolts - 1, app.Fleet.Remote.Millivolts + 1);
    }

    [Fact]
    public async Task A_timer_plugged_in_directly_looks_like_a_fleet_of_one_and_restricts_the_remote_only_pages()
    {
        using var app = new TestApp(sim: new Simulator.SimOptions { Seed = 3, Kind = Simulator.SimKind.Timer, Chaos = false, Demo = false });
        await app.ConnectSimAsync();
        await app.PumpUntil(() => app.Main.Settings.HasLoaded);

        Assert.Single(app.Fleet.Devices);
        Assert.Equal(DeviceKind.Timer, app.Fleet.Remote.Kind);
        Assert.True(app.Main.Dashboard.IsDirectTimer);
        Assert.True(app.Main.Settings.IsTimer);
        Assert.Equal("FogTimer", app.Main.Settings.TimerName);
        Assert.False(app.Fleet.Remote.HasBattery);
        Assert.False(app.Main.Pairing.CanDiscover);
        Assert.False(app.Main.Channels.CanUseTools);

        // the card works like any other
        var dev = app.Fleet.Devices[0];
        dev.ToggleCommand.Execute(null);
        await app.PumpUntil(() => app.Services.Commands.History.Count > 0 && app.Services.Commands.History[0].Status == EntryStatus.Ok);

        // diagnostics shows the timer's own counters
        app.Main.Navigate(NavPage.Diagnostics);
        await app.PumpUntil(() => app.Main.Diagnostics.HasSelfTimer, 6000);
        Assert.True(app.Main.Diagnostics.IsDirectTimer);
        Assert.Equal("Timer connected by USB", app.Main.Diagnostics.RemoteTitle);
        Assert.NotEmpty(app.Main.Diagnostics.SelfTimer.Items);
        Assert.Empty(app.Main.Diagnostics.Devices);
    }

    [Fact]
    public async Task The_raw_console_adds_an_id_and_shows_both_directions()
    {
        using var app = new TestApp();
        await app.ConnectSimAsync();
        var c = app.Main.Console;
        c.RawInput = "{\"t\":\"get\",\"what\":\"fleet\"}";
        Assert.Equal("valid JSON object", c.RawValidation);

        c.SendRawCommand.Execute(null);
        await app.PumpUntil(() =>
        {
            c.Flush();
            return c.RawRows.Any(r => r.Direction == RawDirection.Rx && r.Text.Contains("\"t\":\"res\"") && r.Text.Contains("1100000"));
        }, 4000);

        var sent = c.RawRows.Last(r => r.Direction == RawDirection.Tx && r.Text.Contains("\"what\":\"fleet\""));
        Assert.Contains("\"id\":1100000", sent.Text);
        Assert.StartsWith("{\"t\":\"get\",\"id\":", sent.Text);              // t and id first
        Assert.Equal("", c.RawInput);

        c.HistoryUp();
        Assert.Equal("{\"t\":\"get\",\"what\":\"fleet\"}", c.RawInput);      // the history remembers what was typed
        c.RawInput = "not json";
        Assert.Contains("not valid JSON", c.RawValidation);
    }

    [Fact]
    public async Task Log_and_packet_streams_follow_their_switches()
    {
        using var app = new TestApp();
        await app.ConnectSimAsync();
        var c = app.Main.Console;

        c.CaptureFrames = true;
        await app.PumpUntil(() => { c.Flush(); return c.PacketRows.Count > 10; }, 5000);
        Assert.Contains(c.PacketRows, p => p.Kind == "STATUS" && p.Dir == "rx");
        c.PacketFilter = "ping";
        Assert.All(c.PacketRows, p => Assert.Equal("PING", p.Kind));
        c.PacketFilter = "";
        c.CaptureFrames = false;
        await app.Pause(800);
        c.Flush();
        int count = c.PacketRing.TotalCount;
        await app.Pause(1200);
        c.Flush();
        Assert.InRange(c.PacketRing.TotalCount - count, 0, 40);              // the stream stopped (a few in-flight frames at most)

        c.DeviceLogLevel = "debug";
        app.Fleet.Devices[0].ToggleCommand.Execute(null);
        await app.PumpUntil(() => { c.Flush(); return c.LogRows.Any(r => r.Source == "cmd"); }, 5000);
        c.LevelFilterIndex = 3;                                               // errors only
        Assert.DoesNotContain(c.LogRows, r => r.Level != LogLevel.Error);
    }

    [Fact]
    public async Task Disconnecting_clears_the_dashboard_and_disables_everything()
    {
        using var app = new TestApp();
        await app.ConnectSimAsync();
        await app.Main.DisconnectAsync();
        await app.PumpUntil(() => app.Main.State == HubState.Disconnected && app.Fleet.Devices.Count == 0);

        Assert.True(app.Main.Dashboard.ShowConnectPrompt);
        Assert.False(app.Main.Dashboard.ShowGrid);
        Assert.False(app.Fleet.IsLive);
        Assert.False(app.Main.Dashboard.PauseAllCommand.CanExecute(null));
        Assert.False(app.Main.IsSimulated);
        Assert.Equal("Connect", app.Main.ConnectButtonText);
        Assert.Equal("Not connected", app.Main.ConnectionText);
    }

    [Fact]
    public async Task The_simulator_demo_controls_work_through_the_panel()
    {
        using var app = new TestApp();
        app.Sim.Chaos = false;
        app.Sim.DemoScenario = false;
        await app.Main.StartSimulatorAsync();
        await app.PumpUntil(() => app.Fleet.Devices.Count == 7, 5000);
        var panel = app.Main.SimPanel;

        app.Sim.LossPercent = 40;
        Assert.Equal(40, app.Sim.Device!.ExtraLossPercent);

        panel.CutTarget = app.Fleet.Devices[0];
        panel.CutPowerCommand.Execute(null);
        await app.PumpUntil(() => !app.Fleet.Devices[0].Online, 9000);
        Assert.Contains(app.Toasts.Toasts, t => t.Message.Contains("cut for 30 s"));

        panel.InterfereCommand.Execute(null);
        Assert.Contains(app.Toasts.Toasts, t => t.Message.Contains("jammer"));

        panel.TogglePauseCommand.Execute(null);
        Assert.True(app.Sim.Device.TimePaused);
        Assert.True(app.Services.Clock.Paused);
        Assert.Equal("Resume time", panel.PauseText);
        panel.TogglePauseCommand.Execute(null);
        Assert.False(app.Sim.Device.TimePaused);
        Assert.False(app.Services.Clock.Paused);

        // restart with another timer count and seed
        app.Sim.TimerCount = 3;
        app.Sim.Seed = 99;
        await panel.RestartCommand.ExecuteAsync(null);
        await app.PumpUntil(() => app.Fleet.Devices.Count == 3, 6000);
        Assert.Equal(3, app.Sim.Device!.PairedCount);

        await app.Main.ToggleSimulatorCommand.ExecuteAsync(null);
        await app.PumpUntil(() => app.Main.State == HubState.Disconnected);
        Assert.False(app.Main.IsSimulated);
    }
}
