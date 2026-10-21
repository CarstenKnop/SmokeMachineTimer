using FogConsole.Flashing;
using FogConsole.Hub;
using FogConsole.Ota;
using FogConsole.Protocol;
using FogConsole.Simulator;
using FogConsole.ViewModels;
using Xunit;

namespace FogConsole.Tests;

/// <summary>Shared set-up of the tests of the over-the-air route.</summary>
public abstract class OtaViewModelFixture : IDisposable
{
    protected readonly string _dir = Path.Combine(Path.GetTempPath(), "fogota-" + Guid.NewGuid().ToString("N")[..8]);
    protected readonly TestApp _app;

    protected OtaViewModelFixture(SimOptions? sim)
    {
        Directory.CreateDirectory(_dir);
        _app = new TestApp(sim: sim ?? OtaHubRig.Options(speed: 4));
    }

    public void Dispose()
    {
        _app.Dispose();
        try { Directory.Delete(_dir, true); } catch { /* ignore */ }
    }

    protected FirmwareViewModel Fw => _app.Main.Firmware;
    protected OtaViewModel Ota => Fw.Ota;

    protected async Task<OtaViewModel> ConnectedAsync(bool selectTimerFirmware = true)
    {
        await _app.ConnectSimAsync();
        Fw.AppDirectory = _dir;
        await Fw.ActivateAsync();
        Fw.Route = UpdateRoute.Ota;
        if (selectTimerFirmware) UseTimerFirmware();
        Ota.Refresh();
        return Ota;
    }

    protected FirmwareChoice UseTimerFirmware(string version = "2.2.0", int size = 20_000)
    {
        var choice = new FirmwareChoice(DemoFirmware.CreateSet(FirmwareKind.Timer, version, size));
        Fw.Choices.Add(choice);
        Fw.Selected = choice;
        return choice;
    }

    protected Task WaitAsync(Func<bool> condition, int ms = 20000) => _app.PumpUntil(condition, ms);

    protected string WriteFile(string name, byte[] data)
    {
        var p = Path.Combine(_dir, name);
        File.WriteAllBytes(p, data);
        return p;
    }

    /// <summary>A copy of a firmware set with another identity (what a file without or with a different identity string looks like).</summary>
    protected static FirmwareSet WithKind(FirmwareSet src, FirmwareKind kind) => new()
    {
        Title = src.Title, Source = src.Source, Origin = src.Origin, Kind = kind, Version = src.Version, AppInfo = src.AppInfo, App = src.App,
    };
}

/// <summary>The over-the-air route of the Firmware page, with a remote and its paired timers.</summary>
public class OtaViewModelTests : OtaViewModelFixture
{
    public OtaViewModelTests() : base(null) { }

    [Fact]
    public void Without_a_connection_the_route_explains_that_the_remote_is_needed()
    {
        Ota.Refresh();
        Assert.True(Ota.IsBlocked);
        Assert.Contains("connect", Ota.BlockedText, StringComparison.OrdinalIgnoreCase);
        Assert.False(Ota.CanUpdate);
        Assert.False(Ota.IsAvailable);
    }

    [Fact]
    public async Task A_remote_without_the_ota_capability_is_named_as_the_reason()
    {
        await _app.Hub.ConnectAsync(() => new FakeTransport("COM3") { Responder = FakeTransport.Healthy() });   // hello without "caps"
        Ota.Refresh();
        Assert.False(Ota.IsAvailable);
        Assert.Contains("cannot forward firmware updates", Ota.BlockedText);
        Assert.Contains("USB", Ota.BlockedText);
    }

    [Fact]
    public async Task The_reasons_appear_in_order_firmware_then_timers()
    {
        await _app.ConnectSimAsync();
        Fw.AppDirectory = _dir;
        await Fw.ActivateAsync();
        Fw.Route = UpdateRoute.Ota;
        Ota.Refresh();

        // the demo offers remote firmware first (the simulated device is a remote)
        Assert.Equal(FirmwareKind.Remote, Fw.Selected!.Set.Kind);
        Assert.Contains("firmware for the remote", Ota.BlockedText);

        Fw.Selected = Fw.Choices.First(c => c.Set.Kind == FirmwareKind.Timer);
        Assert.Contains("Tick the timers", Ota.BlockedText);

        Ota.Rows[0].IsChecked = true;
        Assert.False(Ota.IsBlocked);
        Assert.True(Ota.CanUpdate);

        Ota.Rows[0].IsChecked = false;
        Fw.Selected = null;
        Assert.Contains("Choose the timer firmware", Ota.BlockedText);
    }

    [Fact]
    public async Task A_damaged_wrong_chip_or_unrecognised_image_is_refused_before_anything_is_sent()
    {
        await ConnectedAsync(selectTimerFirmware: false);
        Ota.Rows[0].IsChecked = true;

        var bytes = DemoFirmware.CreateImage(FirmwareKind.Timer, "2.2.0", 20_000);
        bytes[400] ^= 0xFF;
        var damaged = new FirmwareChoice(FirmwareSet.FromFile(WriteFile("damaged.bin", bytes)));
        Fw.Choices.Add(damaged);
        Fw.Selected = damaged;
        Assert.True(damaged.HasProblems);
        Assert.Contains("cannot be used", Ota.BlockedText);
        Assert.False(Ota.CanUpdate);

        var otherChip = new FirmwareChoice(FirmwareSet.FromFile(WriteFile("c6.bin", DemoFirmware.CreateImage(FirmwareKind.Timer, "2.2.0", 20_000, chipId: 0x000D))));
        Fw.Choices.Add(otherChip);
        Fw.Selected = otherChip;
        Assert.Contains("cannot be used", Ota.BlockedText);

        var unknown = new FirmwareChoice(WithKind(DemoFirmware.CreateSet(FirmwareKind.Timer, "2.2.0", 20_000), FirmwareKind.Unknown));
        Fw.Choices.Add(unknown);
        Fw.Selected = unknown;
        Assert.Contains("does not say that it is timer firmware", Ota.BlockedText);

        Fw.Selected = Fw.Choices.First(c => c.Set.Kind == FirmwareKind.Timer && !c.HasProblems);
        Assert.False(Ota.IsBlocked, Ota.BlockedText);
    }

    [Fact]
    public async Task The_rows_show_the_paired_timers_with_version_and_online_state()
    {
        await ConnectedAsync();
        Assert.Equal(7, Ota.Rows.Count);
        var fog1 = Ota.Rows[0];
        Assert.Equal("Fog1", fog1.Name);
        Assert.Equal("AA:BB:CC:DD:EE:01", fog1.Mac);
        Assert.Equal("2.1.0", fog1.Version);
        Assert.True(fog1.Online);
        Assert.Equal("online", fog1.OnlineText);
        Assert.Equal("2.0.0", Ota.Rows[6].Version);                 // the legacy one
        Assert.Equal("not checked", fog1.SupportText);
    }

    [Fact]
    public async Task Select_outdated_ticks_only_the_online_timers_older_than_the_chosen_firmware()
    {
        await ConnectedAsync();
        Ota.Rows[2].Version = "2.2.0";                                // already current
        Ota.SelectOutdatedCommand.Execute(null);
        Assert.True(Ota.Rows[0].IsChecked);
        Assert.False(Ota.Rows[2].IsChecked);
        Assert.Equal(6, Ota.CheckedCount);

        Ota.SelectNoneCommand.Execute(null);
        Assert.Equal(0, Ota.CheckedCount);
        Ota.SelectAllCommand.Execute(null);
        Assert.Equal(7, Ota.CheckedCount);
    }

    [Fact]
    public async Task Check_support_fills_in_what_every_timer_can_do()
    {
        await ConnectedAsync();
        await Ota.CheckSupportCommand.ExecuteAsync(null);
        _app.Ui.Run();

        Assert.Equal(OtaSupport.Capable, Ota.Rows[0].Support);
        Assert.Equal("ready for OTA", Ota.Rows[0].SupportText);
        Assert.Equal(OtaSupport.NeedsUsbUpdate, Ota.Rows[4].Support);
        Assert.Equal("needs a USB update first", Ota.Rows[4].SupportText);
        Assert.Contains("USB", Ota.Rows[4].SupportTooltip);
        Assert.Equal(OtaSupport.NoOtaFirmware, Ota.Rows[6].Support);
        Assert.Equal(ChipLevel.Warn, Ota.Rows[6].SupportLevel);
        Assert.False(Ota.IsBusy);
        Assert.Contains("can be updated over the air", string.Join("\n", Ota.LogLines));
    }

    [Fact]
    public async Task Update_runs_the_ticked_timers_and_shows_what_happened_to_each()
    {
        await ConnectedAsync();
        Ota.Rows[0].IsChecked = true;
        Ota.Rows[1].IsChecked = true;

        await Ota.UpdateCommand.ExecuteAsync(null);
        _app.Ui.Run();

        Assert.False(Ota.IsBusy);
        Assert.All(new[] { Ota.Rows[0], Ota.Rows[1] }, r =>
        {
            Assert.Contains("Updated", r.ResultText);
            Assert.Contains("→ 2.2.0", r.ResultText);
            Assert.Equal(ChipLevel.Ok, r.ResultLevel);
            Assert.Equal("Done", r.Stage);
            Assert.Equal(1.0, r.Fraction);
            Assert.False(r.IsRunning);
        });
        Assert.Equal("", Ota.Rows[2].ResultText);                      // not ticked: untouched
        Assert.Equal("2 of 2 timers updated.", Ota.SummaryText);
        Assert.Equal(ChipLevel.Ok, Ota.SummaryLevel);
        Assert.Contains(_app.Toasts.StatusText, "2 of 2 timers updated.");
        Assert.NotEmpty(Ota.LogLines);
        Assert.Equal(Ota.LogLines.Count > 0, Ota.HasLog);

        // the new version shows up in the fleet, so the list follows
        await WaitAsync(() => _app.Fleet.Devices.Take(2).All(d => d.Fw == "2.2.0"));
        Ota.Refresh();
        Assert.Equal("2.2.0", Ota.Rows[0].Version);
        Assert.Equal("2.1.0", Ota.Rows[3].Version);
    }

    [Fact]
    public async Task Timers_that_cannot_take_part_are_explained_and_do_not_stop_the_others()
    {
        await ConnectedAsync();
        Ota.Rows[4].IsChecked = true;
        Ota.Rows[6].IsChecked = true;
        Ota.Rows[3].IsChecked = true;

        await Ota.UpdateCommand.ExecuteAsync(null);
        _app.Ui.Run();

        Assert.Contains("USB", Ota.Rows[4].ResultText);
        Assert.Equal(OtaSupport.NeedsUsbUpdate, Ota.Rows[4].Support);
        Assert.Equal(ChipLevel.Bad, Ota.Rows[4].ResultLevel);
        Assert.Contains("no OTA support", Ota.Rows[6].ResultText);
        Assert.Equal(OtaSupport.NoOtaFirmware, Ota.Rows[6].Support);
        Assert.Contains("Updated", Ota.Rows[3].ResultText);
        Assert.Equal("1 of 3 timers updated; 2 did not get the new firmware - see below.", Ota.SummaryText);
        Assert.Equal(ChipLevel.Warn, Ota.SummaryLevel);
    }

    [Fact]
    public async Task A_wrong_ota_key_in_the_settings_is_explained()
    {
        await ConnectedAsync();
        _app.Main.Settings.OtaKey = "not-the-key";
        Assert.Equal("not-the-key", _app.Settings.Current.OtaKey);
        Ota.Rows[0].IsChecked = true;

        await Ota.UpdateCommand.ExecuteAsync(null);
        _app.Ui.Run();

        Assert.Contains("key", Ota.Rows[0].ResultText);
        Assert.Equal(ChipLevel.Bad, Ota.Rows[0].ResultLevel);
        Assert.Equal("0 of 1 timer updated; 1 did not get the new firmware - see below.", Ota.SummaryText);
        Assert.Equal(ChipLevel.Bad, Ota.SummaryLevel);

        _app.Main.Settings.OtaKey = "  ";                                // an empty key means the default one
        Assert.Equal("FogLink-OTA-v1", _app.Settings.Current.OtaKey);
    }

    [Fact]
    public async Task Cancel_stops_the_update_and_says_so()
    {
        await ConnectedAsync();
        UseTimerFirmware(size: 80_000);
        Ota.Rows[0].IsChecked = true;
        Ota.Rows[1].IsChecked = true;

        var run = Ota.UpdateCommand.ExecuteAsync(null);
        await WaitAsync(() => Ota.IsBusy && Ota.Rows[0].Fraction > 0.1);
        Assert.True(Ota.CancelCommand.CanExecute(null));
        Ota.CancelCommand.Execute(null);
        await run;
        _app.Ui.Run();

        Assert.False(Ota.IsBusy);
        Assert.Contains("Cancelled", Ota.Rows[0].ResultText);
        Assert.Contains("(cancelled)", Ota.SummaryText);
        Assert.Equal(ChipLevel.Warn, Ota.SummaryLevel);
        Assert.Equal("2.1.0", _app.Fleet.Devices[0].Fw);
    }

    [Fact]
    public async Task Closing_the_window_during_an_update_asks_first()
    {
        await ConnectedAsync();
        UseTimerFirmware(size: 80_000);
        Ota.Rows[0].IsChecked = true;
        var run = Ota.UpdateCommand.ExecuteAsync(null);
        await WaitAsync(() => Ota.IsBusy);

        _app.Dialogs.ConfirmAnswer = false;
        Assert.False(_app.Main.CanCloseNow());
        Assert.Contains(_app.Dialogs.Asked, a => a.Contains("Firmware update in progress") && a.Contains("keep their old firmware"));
        _app.Dialogs.ConfirmAnswer = true;
        Assert.True(_app.Main.CanCloseNow());

        _app.Main.Firmware.CancelForShutdown();
        await run;
        Assert.False(Ota.IsBusy);
        Assert.True(_app.Main.CanCloseNow());
    }

    [Fact]
    public async Task The_firmware_list_is_locked_while_an_update_runs_and_the_page_remembers_nothing_stale()
    {
        await ConnectedAsync();
        UseTimerFirmware(size: 80_000);
        Ota.Rows[0].IsChecked = true;
        var run = Ota.UpdateCommand.ExecuteAsync(null);
        await WaitAsync(() => Ota.IsBusy);
        Assert.False(Fw.ChoicesEnabled);
        Assert.False(Ota.CanUpdate);
        Assert.Equal("Updating...", Ota.UpdateButtonText);
        Ota.CancelCommand.Execute(null);
        await run;
        Assert.True(Fw.ChoicesEnabled);
        Assert.Equal("Update selected timers", Ota.UpdateButtonText);
    }

    [Fact]
    public async Task The_radio_hold_is_active_during_the_update_and_gone_afterwards()
    {
        await ConnectedAsync();
        UseTimerFirmware(size: 60_000);
        Ota.Rows[0].IsChecked = true;
        var run = Ota.UpdateCommand.ExecuteAsync(null);
        await WaitAsync(() => _app.Device.OtaHoldActive);
        await run;
        Assert.False(_app.Device.OtaHoldActive);
    }

    [Fact]
    public async Task An_offline_timer_among_the_ticked_ones_is_skipped_with_a_reason()
    {
        await ConnectedAsync();
        _app.Device.CutPower(1, TimeSpan.FromSeconds(60));
        await WaitAsync(() => !_app.Fleet.Devices[1].Online);
        Ota.Refresh();
        Assert.False(Ota.Rows[1].Online);
        Assert.Equal("offline", Ota.Rows[1].OnlineText);
        Ota.Rows[0].IsChecked = true;
        Ota.Rows[1].IsChecked = true;

        await Ota.UpdateCommand.ExecuteAsync(null);
        _app.Ui.Run();

        Assert.Contains("offline", Ota.Rows[1].ResultText);
        Assert.Equal("Skipped", Ota.Rows[1].Stage);
        Assert.Contains("Updated", Ota.Rows[0].ResultText);
    }

    [Fact]
    public async Task Only_offline_timers_ticked_means_nothing_can_start()
    {
        await ConnectedAsync();
        _app.Device.CutPower(1, TimeSpan.FromSeconds(60));
        await WaitAsync(() => !_app.Fleet.Devices[1].Online);
        Ota.Refresh();
        Ota.Rows[1].IsChecked = true;
        Assert.True(Ota.IsBlocked);
        Assert.Contains("is offline", Ota.BlockedText);
    }

    [Fact]
    public async Task The_route_switch_and_the_page_flags_follow_each_other()
    {
        await ConnectedAsync();
        Assert.True(Fw.IsOtaRoute);
        Assert.False(Fw.IsUsbRoute);
        Fw.IsUsbRoute = true;
        Assert.Equal(UpdateRoute.Usb, Fw.Route);
        Fw.IsOtaRoute = true;
        Assert.Equal(UpdateRoute.Ota, Fw.Route);
    }

    [Fact]
    public async Task The_demo_images_are_newer_than_the_simulated_timers()
    {
        await _app.ConnectSimAsync();
        Fw.AppDirectory = _dir;
        await Fw.ActivateAsync();
        Assert.All(Fw.Choices.Where(c => c.Set.Origin.StartsWith("Demo")), c => Assert.Equal(FirmwareViewModel.DemoVersion, c.Set.Version));
        Assert.Equal("2.2.0", FirmwareViewModel.DemoVersion);
    }

    [Fact]
    public async Task The_firmware_page_is_still_a_page_and_the_ota_list_is_refreshed_with_it()
    {
        await _app.ConnectSimAsync();
        _app.Main.Navigate(NavPage.Firmware);
        await _app.PumpUntil(() => Ota.Rows.Count == 7, 5000);
        Assert.Equal(7, Ota.Rows.Count);
    }
}

/// <summary>The same route when the PC is connected to a timer directly (no remote in between).</summary>
public class OtaViewModelDirectTimerTests : OtaViewModelFixture
{
    public OtaViewModelDirectTimerTests() : base(OtaHubRig.Options(kind: SimKind.Timer, speed: 4)) { }

    [Fact]
    public async Task A_timer_on_usb_shows_as_a_list_of_one_and_can_be_updated_over_its_cable()
    {
        await ConnectedAsync();

        var row = Assert.Single(Ota.Rows);
        Assert.NotEmpty(row.Name);
        Assert.True(Ota.IsAvailable);
        row.IsChecked = true;
        Assert.False(Ota.IsBlocked, Ota.BlockedText);

        await Ota.UpdateCommand.ExecuteAsync(null);
        _app.Ui.Run();
        Assert.Contains("Updated", row.ResultText);
        Assert.Equal("1 of 1 timer updated.", Ota.SummaryText);
    }
}
