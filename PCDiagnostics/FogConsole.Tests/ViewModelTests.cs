using System.Globalization;
using FogConsole.Controls;
using FogConsole.Protocol;
using FogConsole.Services;
using FogConsole.ViewModels;
using Xunit;

namespace FogConsole.Tests;

public class DeviceViewModelTests : IDisposable
{
    private readonly TestApp _app = new();

    public DeviceViewModelTests()
    {
        CultureInfo.CurrentCulture = CultureInfo.InvariantCulture;
        _app.Fleet.SetLive(true, clear: false);
    }

    public void Dispose() => _app.Dispose();

    private static FleetDevice Fd(CyclePhase ph = CyclePhase.On, double el = 0.2, double ton = 1.5, double toff = 10, bool online = true,
        TimerMode mode = TimerMode.Auto, int boot = 5, int rssi = -60, int loss = 3, int index = 0, string mac = "AA:BB:CC:DD:EE:01", bool on = false, int ageMs = 300) =>
        new()
        {
            Index = index, Mac = mac, Name = "Fog1", Online = online, On = on || (ph == CyclePhase.On && mode == TimerMode.Auto), Phase = ph, Mode = mode,
            Ton = ton, Toff = toff, Elapsed = el, Rssi = rssi, RssiT = rssi + 2, AgeMs = ageMs, Loss = loss, Channel = 6, Boot = boot, UpSeconds = 100, Fw = "2.0.0",
        };

    private DeviceViewModel Vm() => new(_app.Services, "AA:BB:CC:DD:EE:01");

    [Fact]
    public void The_countdown_is_interpolated_between_fleet_events()
    {
        var vm = Vm();
        vm.Update(Fd(CyclePhase.On, el: 0.2), now: 100);
        Assert.Equal(CyclePhase.On, vm.Phase);
        Assert.Equal(1.3, vm.Remaining, 1);

        vm.Tick(100.5);                                   // half a second later, no event yet
        Assert.Equal(0.8, vm.Remaining, 1);
        Assert.Equal(0.7 / 1.5, vm.Progress, 2);
        Assert.Equal("0.8", vm.RemainingText);

        vm.Tick(100.9);
        Assert.Equal(0.4, vm.Remaining, 1);
    }

    [Fact]
    public void Cycle_figures_follow_the_configured_durations()
    {
        var vm = Vm();
        vm.Update(Fd(ton: 1.5, toff: 10), 100);
        Assert.Equal("11.5 s", vm.CycleLengthText);
        Assert.Equal("13 %", vm.DutyText);
        Assert.Equal("7.8 s", vm.FogPerMinuteText);

        vm.Update(Fd(ton: 2, toff: 8), 101);                // the texts follow a change of durations
        Assert.Equal("10.0 s", vm.CycleLengthText);
        Assert.Equal("20 %", vm.DutyText);
        Assert.Equal("12.0 s", vm.FogPerMinuteText);
    }

    [Fact]
    public void The_phase_flips_on_the_client_when_the_time_is_up_before_the_next_event()
    {
        var vm = Vm();
        vm.Update(Fd(CyclePhase.On, el: 1.4), 100);
        vm.Tick(100.3);                                    // 1.7 s elapsed in a 1.5 s phase -> OFF for 0.2 s
        Assert.Equal(CyclePhase.Off, vm.Phase);
        Assert.Equal(PillState.Off, vm.PillState);
        Assert.Equal(9.8, vm.Remaining, 1);
        Assert.Equal("until ON", vm.UntilText);

        vm.Tick(111.0);                                    // a long gap: never runs away, stays within the phase length
        Assert.InRange(vm.Progress, 0, 1);
        Assert.InRange(vm.Remaining, 0, 10);
    }

    [Fact]
    public void A_paused_timer_stands_still()
    {
        var vm = Vm();
        vm.Update(Fd(CyclePhase.Off, el: 3.0, mode: TimerMode.Paused), 50);
        double before = vm.Remaining;
        vm.Tick(58);
        Assert.Equal(before, vm.Remaining, 2);
        Assert.Equal(PillState.Paused, vm.PillState);
        Assert.True(vm.IsPaused);
        Assert.Equal("PAUSED", vm.ModeText);
        Assert.Equal("Resume", vm.PauseButtonText);
    }

    [Fact]
    public void An_offline_timer_shows_no_countdown_and_how_long_ago_it_was_seen()
    {
        var vm = Vm();
        vm.Update(Fd(CyclePhase.Off, el: 3.0, online: false, ageMs: 12_000), 50);
        vm.Tick(52);
        Assert.Equal("-", vm.RemainingText);
        Assert.Equal(PillState.Offline, vm.PillState);
        Assert.Equal("OFFLINE", vm.PillText);
        Assert.StartsWith("last seen 14 s ago", vm.LastSeenText);
        Assert.False(vm.CanControl);
        Assert.Equal(0, vm.SignalBars);
        Assert.Equal(LinkQuality.Unknown, vm.Quality);
        Assert.Equal("no signal", vm.SignalText);
    }

    [Fact]
    public void A_boot_counter_increase_marks_the_timer_as_rebooted_for_thirty_seconds()
    {
        var vm = Vm();
        vm.Update(Fd(boot: 5), 10);
        Assert.False(vm.IsRebooted);                       // the first sight is not a reboot
        vm.Update(Fd(boot: 5), 11);
        Assert.False(vm.IsRebooted);
        vm.Update(Fd(boot: 6), 12);
        Assert.True(vm.IsRebooted);
        vm.Tick(40);
        Assert.True(vm.IsRebooted);
        vm.Tick(43);
        Assert.False(vm.IsRebooted);
        Assert.Equal("boot 6", vm.BootText);
    }

    [Theory]
    [InlineData(-50, 6)]
    [InlineData(-90, 0)]
    [InlineData(-70, 3)]
    [InlineData(-30, 6)]
    [InlineData(-110, 0)]
    public void Signal_bars_follow_the_thresholds_of_the_remote(int rssi, int bars)
    {
        var vm = Vm();
        vm.Update(Fd(rssi: rssi), 1);
        Assert.Equal(bars, vm.SignalBars);
    }

    [Fact]
    public void Signal_bars_use_the_thresholds_read_from_the_remote_settings()
    {
        var vm = Vm();
        _app.Fleet.Remote.RssiLow = -100;
        _app.Fleet.Remote.RssiHigh = -60;
        vm.Update(Fd(rssi: -80), 1);
        Assert.Equal(3, vm.SignalBars);
        _app.Fleet.Remote.RssiLow = -80;
        _app.Fleet.Remote.RssiHigh = -70;
        vm.RefreshThresholds();
        Assert.Equal(0, vm.SignalBars);
    }

    [Theory]
    [InlineData(-50, 2, LinkQuality.Good)]
    [InlineData(-50, 12, LinkQuality.Fair)]
    [InlineData(-50, 30, LinkQuality.Poor)]
    [InlineData(-88, 2, LinkQuality.Poor)]
    [InlineData(-75, 2, LinkQuality.Fair)]
    public void Link_quality_combines_signal_and_loss(int rssi, int loss, LinkQuality expected)
    {
        var vm = Vm();
        vm.Update(Fd(rssi: rssi, loss: loss), 1);
        Assert.Equal(expected, vm.Quality);
    }

    [Fact]
    public void Pill_states_cover_force_on_paused_on_and_off()
    {
        var vm = Vm();
        vm.Update(Fd(CyclePhase.Off, mode: TimerMode.ForceOn, on: true), 1);
        Assert.Equal(PillState.ForceOn, vm.PillState);
        Assert.True(vm.IsForceOn);
        Assert.Equal("FORCE ON", vm.PillText);

        vm.Update(Fd(CyclePhase.On, el: 0.1), 2);
        Assert.Equal(PillState.On, vm.PillState);
        Assert.Equal("until OFF", vm.UntilText);

        vm.Update(Fd(CyclePhase.Off, el: 2), 3);
        Assert.Equal(PillState.Off, vm.PillState);
    }

    [Fact]
    public void The_nickname_is_local_persistent_and_falls_back_to_the_hub_name()
    {
        var vm = Vm();
        vm.Update(Fd(), 1);
        Assert.Equal("Fog1", vm.DisplayName);
        Assert.False(vm.HasNickname);

        vm.Nickname = "  Stage left ";
        Assert.Equal("Stage left", vm.DisplayName);
        Assert.True(vm.HasNickname);
        Assert.Equal("Stage left", _app.Settings.Current.Devices["AA:BB:CC:DD:EE:01"].Nickname);

        vm.Nickname = "Fog1";                              // same as the name on the device: no nickname needed
        Assert.Equal("Fog1", vm.DisplayName);
        Assert.False(vm.HasNickname);
    }

    [Fact]
    public void Inline_rename_commits_with_enter_and_cancels_with_escape()
    {
        var vm = Vm();
        vm.Update(Fd(), 1);
        vm.StartEditNameCommand.Execute(null);
        Assert.True(vm.IsEditingName);
        Assert.Equal("Fog1", vm.EditName);
        vm.EditName = "Smoky";
        vm.CommitNameCommand.Execute(null);
        Assert.False(vm.IsEditingName);
        Assert.Equal("Smoky", vm.DisplayName);

        vm.StartEditNameCommand.Execute(null);
        vm.EditName = "Nope";
        vm.CancelEditNameCommand.Execute(null);
        Assert.Equal("Smoky", vm.DisplayName);
    }

    [Fact]
    public void The_colour_tag_cycles_and_is_remembered()
    {
        var vm = Vm();
        vm.Update(Fd(), 1);
        var first = vm.TagColor;
        vm.CycleColorCommand.Execute(null);
        Assert.NotEqual(first, vm.TagColor);
        Assert.False(string.IsNullOrEmpty(_app.Settings.Current.Devices["AA:BB:CC:DD:EE:01"].Color));

        var again = new DeviceViewModel(_app.Services, "AA:BB:CC:DD:EE:01");
        again.Update(Fd(), 2);
        Assert.Equal(vm.TagColor, again.TagColor);
    }

    [Fact]
    public void The_timeline_records_state_changes_and_keeps_a_bounded_history()
    {
        var vm = Vm();
        vm.Update(Fd(CyclePhase.Off, el: 3), 100);
        vm.Update(Fd(CyclePhase.On, el: 0.1), 103);
        vm.Update(Fd(CyclePhase.Off, el: 0.1), 105);
        vm.Update(Fd(CyclePhase.Off, el: 0.6, online: false), 106);
        var states = vm.TimelineSamples.Select(s => s.State).ToList();
        Assert.Equal(new[] { TimelineState.Off, TimelineState.On, TimelineState.Off, TimelineState.Offline }, states);
        Assert.Equal(vm.TimelineSamples.Select(s => s.T).OrderBy(t => t), vm.TimelineSamples.Select(s => s.T));

        for (int i = 0; i < 400; i++)
            vm.Update(Fd(i % 2 == 0 ? CyclePhase.On : CyclePhase.Off, el: 0.1), 200 + i);
        Assert.True(vm.TimelineSamples.Count < 100);       // old history is dropped
    }

    [Fact]
    public void The_link_history_is_sampled_once_a_second_and_capped_at_five_minutes()
    {
        var vm = Vm();
        for (int i = 0; i < 1000; i++) vm.Update(Fd(rssi: -60 - i % 5), 100 + i * 0.5);
        var rssi = vm.LinkSeries.Single(s => s.Name == "Signal at remote");
        Assert.Equal(300, rssi.Values.Count);
        Assert.Equal(300, vm.LinkWindowPoints);
        Assert.Equal(3, vm.LinkSeries.Count);
        Assert.True(vm.LinkSeries.Single(s => s.Name == "Loss %").RightAxis);
        Assert.InRange(vm.RssiHistory.Count, 1, 40);        // the card sparkline keeps ~20 s
    }

    [Fact]
    public void Controls_are_disabled_while_the_link_is_down_or_the_timer_is_offline()
    {
        var vm = Vm();
        vm.Update(Fd(), 1);
        Assert.True(vm.CanControl);
        Assert.True(vm.ToggleCommand.CanExecute(null));
        _app.Fleet.SetLive(false, clear: false);
        Assert.False(vm.CanControl);
        Assert.False(vm.ToggleCommand.CanExecute(null));
        Assert.False(vm.HoldStartCommand.CanExecute(null));
        _app.Fleet.SetLive(true, clear: false);
        vm.Update(Fd(online: false), 2);
        Assert.False(vm.IsForceOn);
        Assert.False(vm.ResetCommand.CanExecute(null));
    }
}

public class FleetStateTests : IDisposable
{
    private readonly TestApp _app = new();

    public FleetStateTests() => _app.Fleet.SetLive(true, clear: false);
    public void Dispose() => _app.Dispose();

    private static FleetMessage Msg(int active, params (int i, string mac, string name)[] devs) =>
        new(1, active, 6, devs.Select(d => new FleetDevice { Index = d.i, Mac = d.mac, Name = d.name, Online = true, Phase = CyclePhase.Off, Ton = 1, Toff = 5, Rssi = -60, Channel = 6, SlotId = d.i }).ToList());

    [Fact]
    public void Devices_follow_the_fleet_list_in_order_and_keep_their_identity_when_indices_shift()
    {
        _app.Fleet.ApplyFleet(Msg(0, (0, "A", "Fog1"), (1, "B", "Fog2"), (2, "C", "Fog3")));
        Assert.Equal(new[] { "A", "B", "C" }, _app.Fleet.Devices.Select(d => d.Mac));
        var b = _app.Fleet.ByMac("B")!;
        b.Nickname = "Back";

        // Fog1 is unpaired: B becomes index 0 but it is still the same object with the same nickname
        _app.Fleet.ApplyFleet(Msg(0, (0, "B", "Fog2"), (1, "C", "Fog3")));
        Assert.Equal(new[] { "B", "C" }, _app.Fleet.Devices.Select(d => d.Mac));
        Assert.Same(b, _app.Fleet.ByMac("B"));
        Assert.Equal("Back", b.DisplayName);
        Assert.Equal(0, b.Index);
        Assert.Null(_app.Fleet.ByMac("A"));
        Assert.Same(b, _app.Fleet.ByIndex(0));
        Assert.Equal("Back", _app.Fleet.NameOfMac("B"));
        Assert.Equal("Fog3", _app.Fleet.NameOfIndex(1));
    }

    [Fact]
    public void A_new_timer_is_inserted_at_its_list_position()
    {
        _app.Fleet.ApplyFleet(Msg(0, (0, "A", "Fog1"), (1, "C", "Fog3")));
        _app.Fleet.ApplyFleet(Msg(0, (0, "A", "Fog1"), (1, "B", "Fog2"), (2, "C", "Fog3")));
        Assert.Equal(new[] { "A", "B", "C" }, _app.Fleet.Devices.Select(d => d.Mac));
    }

    [Fact]
    public void The_selection_follows_the_remotes_active_timer()
    {
        _app.Fleet.ApplyFleet(Msg(1, (0, "A", "Fog1"), (1, "B", "Fog2")));
        Assert.Equal("B", _app.Fleet.Selected!.Mac);
        Assert.True(_app.Fleet.ByMac("B")!.IsSelected);
        Assert.False(_app.Fleet.ByMac("A")!.IsSelected);

        // somebody presses the remote's buttons: the active timer changes
        _app.Fleet.ApplyFleet(Msg(0, (0, "A", "Fog1"), (1, "B", "Fog2")));
        Assert.Equal("A", _app.Fleet.Selected!.Mac);
    }

    [Fact]
    public void A_click_on_the_dashboard_wins_for_a_moment_over_the_next_fleet_event()
    {
        _app.Fleet.ApplyFleet(Msg(0, (0, "A", "Fog1"), (1, "B", "Fog2")));
        _app.Fleet.SelectDevice(_app.Fleet.ByMac("B")!);
        Assert.Equal("B", _app.Fleet.Selected!.Mac);
        _app.Fleet.ApplyFleet(Msg(0, (0, "A", "Fog1"), (1, "B", "Fog2")));   // the remote has not caught up yet
        Assert.Equal("B", _app.Fleet.Selected!.Mac);
    }

    [Fact]
    public void An_unpaired_selection_is_cleared_and_the_force_on_flag_follows_the_modes()
    {
        _app.Fleet.ApplyFleet(Msg(1, (0, "A", "Fog1"), (1, "B", "Fog2")));
        Assert.False(_app.Fleet.AnyForceOn);
        var forced = Msg(1, (0, "A", "Fog1"), (1, "B", "Fog2"));
        var devs = (List<FleetDevice>)forced.Devs;
        devs[1] = devs[1] with { Mode = TimerMode.ForceOn, On = true };
        _app.Fleet.ApplyFleet(forced);
        Assert.True(_app.Fleet.AnyForceOn);

        _app.Fleet.ApplyFleet(Msg(0, (0, "A", "Fog1")));
        Assert.False(_app.Fleet.AnyForceOn);
        Assert.Equal("A", _app.Fleet.Selected!.Mac);
    }

    [Fact]
    public void Clearing_the_fleet_on_disconnect_empties_the_dashboard()
    {
        _app.Fleet.ApplyFleet(Msg(0, (0, "A", "Fog1")));
        Assert.False(_app.Fleet.IsEmpty);
        _app.Fleet.SetLive(false, clear: true);
        Assert.True(_app.Fleet.IsEmpty);
        Assert.Null(_app.Fleet.Selected);
        Assert.False(_app.Fleet.HasFleetData);
    }

    [Fact]
    public void Remote_messages_update_battery_and_channel_info()
    {
        _app.Fleet.OnMessage(new RemoteMessage { Bat = 17, Mv = 3500, Usb = true, Charging = false, Channel = 11, Net = 1, Menu = "main" });
        var r = _app.Fleet.Remote;
        Assert.Equal(17, r.Battery);
        Assert.Equal("17%", r.BatteryText);
        Assert.True(r.HasBattery);
        Assert.False(r.IsLowBattery);
        Assert.Equal("ch 11", r.ChannelText);

        _app.Fleet.OnMessage(new RemoteMessage { Bat = 10, Charging = false });
        Assert.True(r.IsLowBattery);
        _app.Fleet.OnMessage(new RemoteMessage { Bat = 10, Charging = true });
        Assert.False(r.IsLowBattery);
        _app.Fleet.OnMessage(new RemoteMessage { Bat = -1 });
        Assert.False(r.HasBattery);                          // a timer connected directly has no battery
    }
}

public class StartupOptionsTests
{
    [Fact]
    public void The_spec_command_line_is_understood()
    {
        var o = StartupOptions.Parse(new[] { "--sim", "--seed", "42", "--page", "Channels", "--select", "3", "--shot", "out.png", "--delay", "2.5", "--exit", "--theme", "dark" });
        Assert.Empty(o.Problems);
        Assert.True(o.Sim);
        Assert.Equal(42, o.Seed);
        Assert.Equal(NavPage.Channels, o.Page);
        Assert.Equal(3, o.Select);
        Assert.Equal("out.png", o.Shot);
        Assert.Equal(2.5, o.Delay);
        Assert.True(o.Exit);
        Assert.Equal("Dark", o.Theme);
        Assert.True(o.IsScreenshot);
    }

    [Theory]
    [InlineData("Dashboard", NavPage.Dashboard)]
    [InlineData("device", NavPage.Device)]
    [InlineData("PAIRING", NavPage.Pairing)]
    [InlineData("Channels", NavPage.Channels)]
    [InlineData("Diagnostics", NavPage.Diagnostics)]
    [InlineData("Console", NavPage.Console)]
    [InlineData("Settings", NavPage.Settings)]
    public void Every_page_name_is_accepted_case_insensitively(string name, NavPage page) =>
        Assert.Equal(page, StartupOptions.Parse(new[] { "--page", name }).Page);

    [Fact]
    public void Defaults_and_extras()
    {
        var o = StartupOptions.Parse(Array.Empty<string>());
        Assert.False(o.Sim);
        Assert.False(o.IsScreenshot);
        Assert.Equal(4, o.Delay);
        Assert.Empty(o.Problems);

        var x = StartupOptions.Parse(new[] { "--sim-timer", "--paired", "10", "--no-demo", "--no-chaos", "--size", "1000x640", "--shot-scale", "1", "--scroll", "300", "--action", "survey", "--theme", "light" });
        Assert.True(x.Sim);
        Assert.True(x.SimTimer);
        Assert.Equal(10, x.Paired);
        Assert.False(x.Demo);
        Assert.True(x.NoChaos);
        Assert.Equal(1000, x.Width);
        Assert.Equal(640, x.Height);
        Assert.Equal(1, x.ShotScale);
        Assert.Equal(300, x.Scroll);
        Assert.Equal("survey", x.Action);
        Assert.Equal("Light", x.Theme);
    }

    [Theory]
    [InlineData("--seed", "abc")]
    [InlineData("--page", "Nowhere")]
    [InlineData("--theme", "pink")]
    [InlineData("--delay", "-1")]
    [InlineData("--paired", "11")]
    [InlineData("--size", "big")]
    public void Bad_values_are_reported_not_thrown(string flag, string value)
    {
        var o = StartupOptions.Parse(new[] { flag, value });
        Assert.Single(o.Problems);
        Assert.Contains(flag, o.Problems[0]);
    }

    [Fact]
    public void Missing_values_and_unknown_flags_are_reported()
    {
        Assert.Contains("needs a value", StartupOptions.Parse(new[] { "--shot" }).Problems[0]);
        Assert.Contains("unknown", StartupOptions.Parse(new[] { "--bogus" }).Problems[0]);
        Assert.True(StartupOptions.Parse(new[] { "--help" }).Help);
    }
}

public class SettingsServiceTests : IDisposable
{
    private readonly string _dir = Path.Combine(Path.GetTempPath(), "FogSettingsTests-" + Guid.NewGuid().ToString("N"));
    private string PathOf => Path.Combine(_dir, "settings.json");

    public void Dispose()
    {
        try { Directory.Delete(_dir, true); } catch { /* ignore */ }
    }

    [Fact]
    public void Settings_survive_a_restart()
    {
        var a = new SettingsService(PathOf);
        a.Current.LastPort = "COM7";
        a.Current.WindowWidth = 1111;
        a.Current.Theme = "Dark";
        a.Current.LogBufferSize = 10_000;
        a.AppearanceOf("aa:bb:cc:dd:ee:01").Nickname = "Left";
        a.AppearanceOf("aa:bb:cc:dd:ee:01").Color = "#FF0000";
        a.Current.CustomPresets.Add(new TimePreset { On = 0.7, Off = 3.5 });
        a.Save();

        var b = new SettingsService(PathOf);
        Assert.Equal("COM7", b.Current.LastPort);
        Assert.Equal(1111, b.Current.WindowWidth);
        Assert.Equal("Dark", b.Current.Theme);
        Assert.Equal(10_000, b.Current.LogBufferSize);
        Assert.Equal("Left", b.Current.Devices["AA:BB:CC:DD:EE:01"].Nickname);     // keys are upper-case MACs
        Assert.Equal("#FF0000", b.AppearanceOf("AA:BB:CC:DD:EE:01").Color);
        var p = Assert.Single(b.Current.CustomPresets);
        Assert.Equal(0.7, p.On);
        Assert.Equal(3.5, p.Off);
    }

    [Fact]
    public void A_missing_or_corrupt_file_gives_defaults_instead_of_an_exception()
    {
        var fresh = new SettingsService(PathOf);
        Assert.Equal("System", fresh.Current.Theme);
        Assert.True(fresh.Current.AutoReconnect);
        Assert.Equal(5000, fresh.Current.LogBufferSize);

        Directory.CreateDirectory(_dir);
        File.WriteAllText(PathOf, "{ this is not json");
        var broken = new SettingsService(PathOf);
        Assert.Equal("System", broken.Current.Theme);
        File.WriteAllText(PathOf, "null");
        Assert.NotNull(new SettingsService(PathOf).Current);
    }

    [Fact]
    public void Absurd_buffer_sizes_are_clamped_and_unknown_keys_are_ignored()
    {
        Directory.CreateDirectory(_dir);
        File.WriteAllText(PathOf, """{"LogBufferSize": 3, "PacketBufferSize": 99999999, "SomethingNew": {"x":1}, "Theme":"Light"}""");
        var s = new SettingsService(PathOf);
        Assert.Equal(500, s.Current.LogBufferSize);
        Assert.Equal(100_000, s.Current.PacketBufferSize);
        Assert.Equal("Light", s.Current.Theme);
    }

    [Fact]
    public void Saving_is_atomic_and_creates_the_folder()
    {
        var s = new SettingsService(PathOf);
        s.Save();
        Assert.True(File.Exists(PathOf));
        Assert.False(File.Exists(PathOf + ".tmp"));
    }

    [Fact]
    public void The_preset_label_is_short_and_readable()
    {
        CultureInfo.CurrentCulture = CultureInfo.InvariantCulture;
        Assert.Equal("1 / 10 s", new TimePreset { On = 1, Off = 10 }.Label);
        Assert.Equal("0.5 / 5 s", new TimePreset { On = 0.5, Off = 5 }.Label);
        Assert.Equal("2.5 / 15.5 s", new TimePreset { On = 2.5, Off = 15.5 }.Label);
    }
}

public class FilteredRingTests
{
    [Fact]
    public void Rows_are_added_to_the_view_and_the_buffer_is_bounded()
    {
        var ring = new FilteredRing<int>(1000);
        ring.Add(Enumerable.Range(0, 900).ToArray());
        Assert.Equal(900, ring.View.Count);
        ring.Add(Enumerable.Range(900, 400).ToArray());      // 1300 > capacity + 10 %: trimmed in one go
        Assert.Equal(1000, ring.TotalCount);
        Assert.Equal(1000, ring.View.Count);
        Assert.Equal(300, ring.View[0]);                     // the oldest rows are gone
        Assert.Equal(1299, ring.View[^1]);
    }

    [Fact]
    public void The_filter_applies_to_new_and_existing_rows()
    {
        var ring = new FilteredRing<int>(1000);
        ring.Add(new[] { 1, 2, 3, 4, 5, 6 });
        ring.SetFilter(i => i % 2 == 0);
        Assert.Equal(new[] { 2, 4, 6 }, ring.View);
        ring.Add(new[] { 7, 8 });
        Assert.Equal(new[] { 2, 4, 6, 8 }, ring.View);
        ring.SetFilter(_ => true);
        Assert.Equal(8, ring.View.Count);
    }

    [Fact]
    public void Pausing_freezes_the_view_but_not_the_buffer()
    {
        var ring = new FilteredRing<string>(1000);
        ring.Add(new[] { "a", "b" });
        ring.Paused = true;
        ring.Add(new[] { "c", "d" });
        Assert.Equal(2, ring.View.Count);
        Assert.Equal(4, ring.TotalCount);
        ring.Paused = false;
        Assert.Equal(new[] { "a", "b", "c", "d" }, ring.View);
    }

    [Fact]
    public void Bulk_changes_raise_a_single_reset()
    {
        var ring = new FilteredRing<int>(1000);
        int events = 0;
        ring.View.CollectionChanged += (_, e) =>
        {
            events++;
            Assert.Equal(System.Collections.Specialized.NotifyCollectionChangedAction.Reset, e.Action);
        };
        ring.Add(Enumerable.Range(0, 500).ToArray());
        Assert.Equal(1, events);
        ring.Clear();
        Assert.Empty(ring.View);
        Assert.Equal(2, events);
    }
}

public class ConsoleRowTests
{
    [Fact]
    public void Packets_export_to_csv_with_quoted_hex()
    {
        var rows = new[]
        {
            new PacketRow { Time = new DateTime(2026, 10, 20, 12, 0, 0, DateTimeKind.Utc), Dir = "rx", Mac = "AA:BB:CC:DD:EE:01", Kind = "STATUS", Len = 46, Rssi = -60, Seq = 17, Hex = "F6 02 01", DeviceMs = 123 },
            new PacketRow { Time = new DateTime(2026, 10, 20, 12, 0, 1, DateTimeKind.Utc), Dir = "tx", Mac = "AA:BB:CC:DD:EE:02", Kind = "PING", Len = 16, Rssi = 0, Seq = 18, Hex = "F6 02 50" },
        };
        var csv = ConsoleViewModel.PacketsToCsv(rows);
        var lines = csv.Split("\r\n", StringSplitOptions.RemoveEmptyEntries);
        Assert.Equal("time,deviceMs,dir,mac,kind,len,rssi,seq,hex", lines[0]);
        Assert.EndsWith(",123,rx,AA:BB:CC:DD:EE:01,STATUS,46,-60,17,\"F6 02 01\"", lines[1]);
        Assert.Equal(3, lines.Length);
    }

    [Fact]
    public void Row_texts_are_formatted_for_copy_and_save()
    {
        var log = new LogRow { Time = new DateTime(2026, 10, 20, 1, 2, 3, 456), Level = LogLevel.Warn, Source = "link", Text = "T2 offline" };
        Assert.Equal("01:02:03.456", log.TimeText);
        Assert.Equal("WRN", log.LevelText);
        Assert.Contains("T2 offline", log.Line);

        var raw = new RawRow { Time = new DateTime(2026, 10, 20, 1, 2, 3, 4), Direction = Hub.RawDirection.Tx, Kind = Hub.RawKind.Json, Text = "{\"t\":\"ping\"}" };
        Assert.Equal("→", raw.Arrow);
        Assert.StartsWith("01:02:03.004 → ", raw.Line);
        Assert.Equal("←", new RawRow { Time = DateTime.Now, Direction = Hub.RawDirection.Rx, Kind = Hub.RawKind.PlainText, Text = "x" }.Arrow);
    }
}
