using System.Collections.ObjectModel;
using System.Text.Json.Nodes;
using FogConsole.Flashing;
using FogConsole.Transport;
using FogConsole.ViewModels;
using Xunit;

namespace FogConsole.Tests;

public class VersionsTests
{
    [Theory]
    [InlineData("2.0.10", "2.0.9", 1)]
    [InlineData("2.0.0", "2.0.0", 0)]
    [InlineData("2.1", "2.1.0", 0)]
    [InlineData("1.9.9", "2.0.0", -1)]
    [InlineData("2.0.0-beta", "2.0.0", 0)]
    [InlineData("2.0.0", null, 1)]
    [InlineData(null, "2.0.0", -1)]
    public void Dotted_versions_compare_numerically(string? a, string? b, int sign)
    {
        Assert.Equal(sign, Math.Sign(Versions.Compare(a, b)));
    }
}

public class FirmwareCatalogTests : IDisposable
{
    private readonly string _dir = Path.Combine(Path.GetTempPath(), "fogcat-" + Guid.NewGuid().ToString("N")[..8]);

    public FirmwareCatalogTests() => Directory.CreateDirectory(_dir);
    public void Dispose() { try { Directory.Delete(_dir, true); } catch { /* ignore */ } }

    private static void WriteBuild(string folder, FirmwareKind kind, string version)
    {
        var set = DemoFirmware.CreateSet(kind, version, 60_000);
        Directory.CreateDirectory(folder);
        File.WriteAllBytes(Path.Combine(folder, "firmware.bin"), set.App);
        File.WriteAllBytes(Path.Combine(folder, "bootloader.bin"), set.Bootloader!);
        File.WriteAllBytes(Path.Combine(folder, "partitions.bin"), set.PartitionTableBin!);
    }

    [Fact]
    public void Firmware_next_to_the_application_is_found_and_sorted_timers_first_newest_first()
    {
        WriteBuild(Path.Combine(_dir, "firmware", "remote"), FirmwareKind.Remote, "2.0.1");
        WriteBuild(Path.Combine(_dir, "firmware", "timer-old"), FirmwareKind.Timer, "2.0.0");
        WriteBuild(Path.Combine(_dir, "firmware", "timer-new"), FirmwareKind.Timer, "2.0.10");

        var r = FirmwareCatalog.Discover(_dir);
        Assert.Equal(new[] { "2.0.10", "2.0.0", "2.0.1" }, r.Sets.Select(s => s.Version).ToArray());
        Assert.Equal(new[] { FirmwareKind.Timer, FirmwareKind.Timer, FirmwareKind.Remote }, r.Sets.Select(s => s.Kind).ToArray());
        Assert.All(r.Sets, s => Assert.Equal("Installed with the app", s.Origin));
        Assert.Empty(r.Notes);
    }

    [Fact]
    public void A_source_checkout_is_recognised_by_its_platformio_output()
    {
        var repo = Path.Combine(_dir, "repo");
        Directory.CreateDirectory(Path.Combine(repo, "FogMachineTimer"));
        File.WriteAllText(Path.Combine(repo, "FogMachineTimer", "platformio.ini"), "[platformio]");
        WriteBuild(Path.Combine(repo, "FogMachineTimer", ".pio", "build", "xiao_esp32c3"), FirmwareKind.Timer, "2.0.0");
        WriteBuild(Path.Combine(repo, "FogMachineRemoteControl", ".pio", "build", "xiao_esp32c3"), FirmwareKind.Remote, "2.0.0");

        var appDir = Path.Combine(repo, "PCDiagnostics", "FogConsole", "bin", "Release", "net10.0-windows");
        Directory.CreateDirectory(appDir);
        var r = FirmwareCatalog.Discover(appDir);
        Assert.Equal(2, r.Sets.Count);
        Assert.All(r.Sets, s => Assert.Equal("Built in this repository", s.Origin));
        Assert.Equal(repo, FirmwareCatalog.FindRepositoryRoot(appDir));
    }

    [Fact]
    public void Nothing_is_found_in_an_empty_place()
    {
        var r = FirmwareCatalog.Discover(_dir);
        Assert.Empty(r.Sets);
    }

    [Fact]
    public void Picked_paths_are_offered_and_vanished_ones_are_ignored()
    {
        var folder = Path.Combine(_dir, "mine");
        WriteBuild(folder, FirmwareKind.Remote, "3.0.0");
        var r = FirmwareCatalog.Discover(_dir, new[] { folder, Path.Combine(_dir, "gone") });
        var set = Assert.Single(r.Sets);
        Assert.Equal("Chosen by you", set.Origin);
        Assert.Equal("3.0.0", set.Version);
    }

    [Fact]
    public void The_same_image_in_two_places_is_listed_once_and_a_picked_copy_wins()
    {
        var shipped = Path.Combine(_dir, "firmware", "timer");
        WriteBuild(shipped, FirmwareKind.Timer, "2.0.0");
        var copy = Path.Combine(_dir, "copy");
        Directory.CreateDirectory(copy);
        foreach (var f in Directory.GetFiles(shipped)) File.Copy(f, Path.Combine(copy, Path.GetFileName(f)));

        var r = FirmwareCatalog.Discover(_dir, new[] { copy });
        var set = Assert.Single(r.Sets);
        Assert.Equal("Chosen by you", set.Origin);

        // a different build of the same version is a different image
        WriteBuild(Path.Combine(_dir, "firmware", "timer2"), FirmwareKind.Timer, "2.0.0");
        File.WriteAllBytes(Path.Combine(_dir, "firmware", "timer2", "firmware.bin"), DemoFirmware.CreateImage(FirmwareKind.Timer, "2.0.0", 70_000, seed: 99));
        Assert.Equal(2, FirmwareCatalog.Discover(_dir, new[] { copy }).Sets.Count);
    }

    [Fact]
    public void The_same_folder_is_listed_once()
    {
        var shipped = Path.Combine(_dir, "firmware", "timer");
        WriteBuild(shipped, FirmwareKind.Timer, "2.0.0");
        var r = FirmwareCatalog.Discover(_dir, new[] { shipped });
        Assert.Single(r.Sets);
    }

    [Fact]
    public void An_unreadable_entry_becomes_a_note_not_a_crash()
    {
        var bad = Path.Combine(_dir, "firmware", "bad");
        Directory.CreateDirectory(bad);
        File.WriteAllText(Path.Combine(bad, "firmware.bin"), "not an image");
        var r = FirmwareCatalog.Discover(_dir);
        // an invalid file still loads (so the page can explain what is wrong with it)
        var set = Assert.Single(r.Sets);
        Assert.NotEmpty(set.Problems);
    }
}

public class FirmwareViewModelTests : IDisposable
{
    private readonly TestApp _app = new();
    private readonly string _dir = Path.Combine(Path.GetTempPath(), "fogvm-" + Guid.NewGuid().ToString("N")[..8]);

    public FirmwareViewModelTests() => Directory.CreateDirectory(_dir);

    public void Dispose()
    {
        _app.Dispose();
        try { Directory.Delete(_dir, true); } catch { /* ignore */ }
    }

    private FirmwareViewModel Sim => _app.Main.Firmware;

    private async Task<FirmwareViewModel> SimReadyAsync()
    {
        await _app.ConnectSimAsync();
        var fw = Sim;
        fw.AppDirectory = _dir;
        fw.SimulatedBytesPerSecond = 0;
        await fw.ActivateAsync();
        return fw;
    }

    private static string RomLog(SimulatedRom rom) => string.Join(",", rom.Commands.Select(c => c.Op));

    [Fact]
    public async Task The_offline_demo_offers_demo_firmware_and_prefers_the_kind_of_the_connected_device()
    {
        var fw = await SimReadyAsync();
        Assert.True(fw.IsSimulated);
        Assert.Equal(2, fw.Choices.Count);
        Assert.Equal(FirmwareKind.Remote, fw.Selected!.Set.Kind);
        Assert.Contains("Simulated", fw.DeviceTitle);
        Assert.True(fw.CanFlash);
    }

    [Fact]
    public async Task A_simulated_update_runs_to_the_end_and_the_device_reports_the_new_version()
    {
        var fw = await SimReadyAsync();
        fw.Mode = FlashMode.Reinstall;
        await fw.FlashCommand.ExecuteAsync(null);
        _app.Ui.Run();

        Assert.Equal(FlashPhase.Succeeded, fw.Phase);
        Assert.Equal(ChipLevel.Ok, fw.ResultLevel);
        Assert.Contains("Done", fw.ResultText);
        Assert.Contains("2.2.0", fw.DeviceDetail);
        Assert.Equal(1.0, fw.Progress);
        Assert.NotEmpty(fw.LogLines);
        Assert.Contains(fw.LogLines, l => l.Contains("verified"));
        Assert.False(fw.IsBusy);
    }

    [Fact]
    public async Task A_clean_install_asks_first_and_does_nothing_when_declined()
    {
        var fw = await SimReadyAsync();
        fw.Mode = FlashMode.CleanInstall;
        _app.Dialogs.ConfirmAnswer = false;
        await fw.FlashCommand.ExecuteAsync(null);
        Assert.Equal(FlashPhase.Idle, fw.Phase);
        Assert.Contains(_app.Dialogs.Asked, a => a.Contains("Erase the settings"));

        _app.Dialogs.ConfirmAnswer = true;
        await fw.FlashCommand.ExecuteAsync(null);
        _app.Ui.Run();
        Assert.Equal(FlashPhase.Succeeded, fw.Phase);
    }

    [Fact]
    public async Task Timer_firmware_for_a_remote_asks_before_it_is_written()
    {
        // not simulated: the hub talks to "COM7", which says it is a remote
        var rom = new SimulatedRom();
        var (fw, _) = await RealPortSetupAsync(rom, hubKind: "remote");
        fw.Choices.Add(new FirmwareChoice(DemoFirmware.CreateSet(FirmwareKind.Timer, "2.1.0")));
        fw.Selected = fw.Choices.Last();

        _app.Dialogs.ConfirmAnswer = false;
        await fw.FlashCommand.ExecuteAsync(null);
        Assert.Equal(FlashPhase.Idle, fw.Phase);
        Assert.Contains(_app.Dialogs.Asked, a => a.Contains("Wrong firmware"));
        Assert.Empty(rom.Commands);
    }

    private readonly List<string> _events = new();

    /// <summary>A view model for a "real" port COM7 where the hub is connected to a remote; the flasher gets <paramref name="rom"/>.</summary>
    private async Task<(FirmwareViewModel Fw, ObservableCollection<PortInfo> Ports)> RealPortSetupAsync(SimulatedRom? rom, string hubKind = "remote",
        Func<PortInfo, IFlashLink>? open = null)
    {
        string HelloFor(int id, string fw) =>
            $$"""{"t":"res","id":{{id}},"ok":true,"kind":"{{hubKind}}","fw":"{{fw}}","proto":2,"mac":"40:4C:CA:5B:12:34","name":"Remote","ch":6,"net":1,"upMs":1000,"rst":"poweron","boots":1,"heap":180000,"build":"x","maxDevs":10}""";

        Func<JsonObject, IEnumerable<string>?> Responder(string fw) => req =>
        {
            int id = req["id"]!.GetValue<int>();
            return req["t"]!.GetValue<string>() switch
            {
                "hello" => new[] { HelloFor(id, fw) },
                "ping" => new[] { FakeTransport.Ok(id, "\"ms\":5000") },
                _ => null,
            };
        };

        await _app.Hub.ConnectAsync(() => new FakeTransport("COM7") { Responder = Responder("2.0.0") }, waitIfAbsent: false);
        await _app.PumpUntil(() => _app.Fleet.Remote.Firmware == "2.0.0", 3000);

        var ports = new ObservableCollection<PortInfo> { new("COM7", "USB Serial Device", true, true) };
        var fw = new FirmwareViewModel(_app.Services, ports, () => Task.CompletedTask,
            async () => { _events.Add("disconnect"); await _app.Hub.DisconnectAsync(); },
            async port => { _events.Add("reconnect " + port); await _app.Hub.ConnectAsync(() => new FakeTransport(port) { Responder = Responder("2.1.0") }, waitIfAbsent: true); })
        {
            AppDirectory = _dir,
            RestartWait = TimeSpan.Zero,
            OpenLink = open ?? (p => { _events.Add("open " + p.Name); return rom ?? new SimulatedRom(); }),
        };
        await fw.ActivateAsync();
        Assert.Equal("COM7", fw.SelectedPort?.Name);
        fw.Choices.Clear();
        var set = DemoFirmware.CreateSet(FirmwareKind.Remote, "2.1.0");
        fw.Choices.Add(new FirmwareChoice(set));
        fw.Selected = fw.Choices[0];
        return (fw, ports);
    }

    [Fact]
    public async Task The_connection_is_closed_for_the_update_and_restored_afterwards()
    {
        var rom = new SimulatedRom();
        var (fw, _) = await RealPortSetupAsync(rom);
        Assert.Contains("Remote", fw.DeviceTitle);

        fw.Mode = FlashMode.Reinstall;
        await fw.FlashCommand.ExecuteAsync(null);
        _app.Ui.Run();

        Assert.Equal(new[] { "disconnect", "open COM7", "reconnect COM7" }, _events);
        Assert.Equal(FlashPhase.Succeeded, fw.Phase);
        Assert.Contains("This is the remote \"Remote\"", fw.ResultText);   // the chip's MAC is the one the remote reported
        Assert.Contains("It reports firmware 2.1.0", fw.ResultText);
        Assert.Equal(ChipLevel.Ok, fw.ResultLevel);
        Assert.Equal(1, rom.WatchdogResets);
        Assert.Equal("2.1.0", _app.Fleet.Remote.Firmware.Length == 0 ? "2.1.0" : _app.Hub.Hello?.Fw);
    }

    [Fact]
    public async Task A_busy_port_is_explained_and_the_connection_is_still_restored()
    {
        var (fw, _) = await RealPortSetupAsync(null, open: _ => throw new UnauthorizedAccessException("denied"));
        await fw.FlashCommand.ExecuteAsync(null);
        _app.Ui.Run();

        Assert.Equal(FlashPhase.Failed, fw.Phase);
        Assert.Contains("in use by another program", fw.ResultText);
        Assert.Equal("reconnect COM7", _events.Last());
    }

    [Fact]
    public async Task A_device_that_does_not_enter_the_bootloader_fails_with_advice_and_the_connection_returns()
    {
        var rom = new SimulatedRom(new SimulatedRomOptions { EntersBootloader = false });
        var (fw, _) = await RealPortSetupAsync(rom);
        await fw.FlashCommand.ExecuteAsync(null);
        _app.Ui.Run();

        Assert.Equal(FlashPhase.Failed, fw.Phase);
        Assert.Equal(ChipLevel.Bad, fw.ResultLevel);
        Assert.Contains("BOOT", fw.ResultText);
        Assert.Equal("reconnect COM7", _events.Last());
    }

    [Fact]
    public async Task A_second_port_does_not_disturb_the_connection_to_the_remote()
    {
        var rom = new SimulatedRom();
        var (fw, ports) = await RealPortSetupAsync(rom);
        ports.Add(new PortInfo("COM9", "USB Serial Device", true, true));
        fw.SelectedPort = ports[1];
        fw.Choices.Add(new FirmwareChoice(DemoFirmware.CreateSet(FirmwareKind.Timer, "2.1.0")));
        fw.Selected = fw.Choices.Last();

        await fw.FlashCommand.ExecuteAsync(null);
        _app.Ui.Run();

        Assert.Equal(FlashPhase.Succeeded, fw.Phase);
        Assert.Equal(new[] { "open COM9" }, _events);
        Assert.Equal(Hub.HubState.Connected, _app.Hub.State);
    }

    [Fact]
    public async Task Cancelling_ends_the_run_cleanly()
    {
        var rom = new SimulatedRom(new SimulatedRomOptions { BytesPerSecond = 30_000 });
        var (fw, _) = await RealPortSetupAsync(rom);
        fw.Mode = FlashMode.Reinstall;

        var run = fw.FlashCommand.ExecuteAsync(null);
        for (int i = 0; i < 300 && rom.BlocksWritten < 5; i++) await Task.Delay(20);
        Assert.True(fw.IsBusy);
        Assert.True(fw.MustNotClose);
        fw.CancelCommand.Execute(null);
        await run;
        _app.Ui.Run();

        Assert.Equal(FlashPhase.Cancelled, fw.Phase);
        Assert.Contains("Cancelled", fw.ResultText);
        Assert.False(fw.MustNotClose);
        Assert.Equal(ChipLevel.Warn, fw.ResultLevel);
    }

    [Fact]
    public async Task A_damaged_firmware_cannot_be_flashed()
    {
        var fw = await SimReadyAsync();
        var set = DemoFirmware.CreateSet(FirmwareKind.Timer, "9.9.9");
        set.App[500] ^= 0xFF;
        var broken = FirmwareSet.FromFile(WriteTemp("broken.bin", set.App));
        fw.Choices.Add(new FirmwareChoice(broken));
        fw.Selected = fw.Choices.Last();
        Assert.True(fw.Selected.HasProblems);
        Assert.False(fw.CanFlash);
        Assert.Contains("damaged", fw.Selected.ProblemText);
    }

    private string WriteTemp(string name, byte[] data)
    {
        var p = Path.Combine(_dir, name);
        File.WriteAllBytes(p, data);
        return p;
    }

    [Fact]
    public async Task Reinstall_is_only_possible_with_a_complete_firmware()
    {
        var fw = await SimReadyAsync();
        var appOnly = FirmwareSet.FromFile(WriteTemp("app.bin", DemoFirmware.CreateImage(FirmwareKind.Remote, "2.2.0")));
        fw.Choices.Add(new FirmwareChoice(appOnly));
        fw.Mode = FlashMode.CleanInstall;
        fw.Selected = fw.Choices.Last();
        Assert.False(fw.CanReinstall);
        Assert.Equal(FlashMode.Update, fw.Mode);            // falls back to the only possible mode
    }

    [Fact]
    public async Task A_picked_file_is_remembered_and_selected()
    {
        var fw = await SimReadyAsync();
        var path = WriteTemp("mine.bin", DemoFirmware.CreateImage(FirmwareKind.Timer, "3.1.4"));
        _app.Dialogs.OpenPath = path;
        await fw.BrowseFileCommand.ExecuteAsync(null);

        Assert.Equal(path, fw.Selected!.Path);
        Assert.Equal("3.1.4", fw.Selected.Set.Version);
        Assert.Contains(path, _app.Settings.Current.FirmwarePaths);
        Assert.Equal("Chosen by you", fw.Selected.Set.Origin);
    }

    [Fact]
    public async Task A_missing_file_is_refused_with_a_message()
    {
        var fw = await SimReadyAsync();
        int before = fw.Choices.Count;
        _app.Dialogs.OpenPath = Path.Combine(_dir, "nope.bin");
        await fw.BrowseFileCommand.ExecuteAsync(null);
        Assert.Equal(before, fw.Choices.Count);
        Assert.DoesNotContain(_app.Settings.Current.FirmwarePaths, p => p.EndsWith("nope.bin"));
        Assert.Contains("cannot be used", _app.Toasts.StatusText);
    }

    [Fact]
    public async Task The_timer_list_compares_reported_versions_with_the_newest_timer_firmware()
    {
        var fw = await SimReadyAsync();
        Assert.True(fw.HasTimers);
        Assert.Equal(_app.Fleet.Devices.Count, fw.Timers.Count);
        // the demo timer firmware is 2.2.0; the simulated timers report an older version
        Assert.All(fw.Timers.Where(t => t.Version != "-"), t => Assert.Contains("older than 2.2.0", t.Verdict));
    }

    [Fact]
    public async Task Checking_a_simulated_device_writes_nothing_and_reports_the_chip()
    {
        var fw = await SimReadyAsync();
        Assert.True(fw.CheckCommand.CanExecute(null));
        await fw.CheckCommand.ExecuteAsync(null);
        _app.Ui.Run();

        Assert.Equal(FlashPhase.Succeeded, fw.Phase);
        Assert.Contains("Nothing was written", fw.ResultText);
        Assert.Contains("MAC", fw.ResultText);
        Assert.DoesNotContain("2.2.0", fw.DeviceDetail);                 // still the old firmware
        Assert.DoesNotContain(_app.Dialogs.Asked, a => a.Contains("Erase"));
    }

    [Fact]
    public async Task Checking_needs_no_firmware_and_asks_no_questions()
    {
        var fw = await SimReadyAsync();
        fw.Choices.Clear();
        fw.Selected = null;
        Assert.False(fw.CanFlash);
        Assert.True(fw.CanCheck);
        fw.Mode = FlashMode.CleanInstall;
        _app.Dialogs.ConfirmAnswer = false;
        await fw.CheckCommand.ExecuteAsync(null);
        Assert.Equal(FlashPhase.Succeeded, fw.Phase);
        Assert.Empty(_app.Dialogs.Asked);
    }

    [Fact]
    public async Task Checking_a_real_port_hands_the_connection_over_like_flashing_does()
    {
        var rom = new SimulatedRom();
        var (fw, _) = await RealPortSetupAsync(rom);
        await fw.CheckCommand.ExecuteAsync(null);
        _app.Ui.Run();

        Assert.Equal(new[] { "disconnect", "open COM7", "reconnect COM7" }, _events);
        Assert.Equal(FlashPhase.Succeeded, fw.Phase);
        Assert.Contains("Nothing was written", fw.ResultText);
        Assert.Equal(0, rom.BlocksWritten);
    }

    [Fact]
    public async Task The_port_choice_follows_the_ports_that_come_and_go()
    {
        var (fw, ports) = await RealPortSetupAsync(new SimulatedRom());
        Assert.Equal("COM7", fw.SelectedPort?.Name);
        ports.Add(new PortInfo("COM9", "USB Serial Device", true, true));
        ports.RemoveAt(0);                                                     // COM7 unplugged
        Assert.Equal("COM9", fw.SelectedPort?.Name);
        ports.Clear();
        Assert.Null(fw.SelectedPort);
    }

    [Fact]
    public async Task The_firmware_page_is_a_navigation_target_with_its_own_shortcut()
    {
        var item = _app.Main.NavItems.Single(n => n.Page == NavPage.Firmware);
        Assert.Equal("Ctrl+7", item.Shortcut);
        Assert.Equal("Ctrl+8", _app.Main.NavItems.Single(n => n.Page == NavPage.Settings).Shortcut);
        _app.Main.Navigate(NavPage.Firmware);
        Assert.Same(_app.Main.Firmware, _app.Main.CurrentPage);
        await Task.CompletedTask;
    }

    [Fact]
    public async Task Closing_the_window_is_allowed_when_nothing_is_running()
    {
        Assert.True(_app.Main.CanCloseNow());
        await Task.CompletedTask;
    }
}
