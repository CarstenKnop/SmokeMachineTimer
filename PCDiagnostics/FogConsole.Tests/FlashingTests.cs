using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Text;
using FogConsole.Flashing;
using Xunit;

namespace FogConsole.Tests;

internal sealed class SyncProgress<T> : IProgress<T>
{
    private readonly Action<T> _on;
    public SyncProgress(Action<T> on) => _on = on;
    public List<T> Items { get; } = new();

    public void Report(T value)
    {
        Items.Add(value);
        _on(value);
    }
}

public class SlipTests
{
    private static List<byte[]> Decode(params byte[][] chunks)
    {
        var d = new SlipDecoder();
        var frames = new List<byte[]>();
        foreach (var c in chunks) d.Feed(c, frames.Add);
        return frames;
    }

    [Fact]
    public void Special_bytes_are_escaped_and_restored()
    {
        var packet = new byte[] { 0x01, Slip.End, 0x02, Slip.Esc, 0x03, Slip.End, Slip.End };
        var wire = Slip.Encode(packet);
        Assert.Equal(Slip.End, wire[0]);
        Assert.Equal(Slip.End, wire[^1]);
        Assert.DoesNotContain(Slip.End, wire[1..^1]);
        Assert.Equal(packet, Assert.Single(Decode(wire)));
    }

    [Fact]
    public void Every_split_point_gives_the_same_frames()
    {
        var a = new byte[] { 1, 0xC0, 3 };
        var b = new byte[] { 0xDB, 0xDC, 0xDD, 9 };
        var wire = Slip.Encode(a).Concat(Slip.Encode(b)).ToArray();
        for (int split = 0; split <= wire.Length; split++)
        {
            var frames = Decode(wire[..split], wire[split..]);
            Assert.Equal(2, frames.Count);
            Assert.Equal(a, frames[0]);
            Assert.Equal(b, frames[1]);
        }
    }

    [Fact]
    public void Text_outside_frames_is_kept_for_diagnostics_and_does_not_disturb_framing()
    {
        var d = new SlipDecoder();
        var frames = new List<byte[]>();
        d.Feed(Encoding.ASCII.GetBytes("ESP-ROM:esp32c3\r\nwaiting for download\r\n"), frames.Add);
        d.Feed(Slip.Encode(new byte[] { 7, 7 }), frames.Add);
        Assert.Single(frames);
        Assert.Contains("waiting for download", d.StrayText);
    }

    [Fact]
    public void Back_to_back_end_bytes_make_no_empty_frames()
    {
        var frames = Decode(new byte[] { Slip.End, Slip.End, Slip.End, 5, Slip.End, Slip.End });
        Assert.Equal(new byte[] { 5 }, Assert.Single(frames));
    }

    [Fact]
    public void A_broken_escape_drops_only_that_frame()
    {
        var d = new SlipDecoder();
        var frames = new List<byte[]>();
        d.Feed(new byte[] { Slip.End, 1, Slip.Esc, 0x42, 2, Slip.End }, frames.Add);
        d.Feed(Slip.Encode(new byte[] { 9 }), frames.Add);
        Assert.Equal(1, d.BadFrames);
        Assert.Equal(new byte[] { 9 }, Assert.Single(frames));
    }
}

public class EspImageTests
{
    [Fact]
    public void A_generated_image_is_intact_and_identified()
    {
        var img = DemoFirmware.CreateImage(FirmwareKind.Timer, "2.3.4");
        var info = EspImageInfo.Parse(img);
        Assert.True(info.Intact, info.Problem);
        Assert.Equal(Esp32C3.ChipId, info.ChipId);
        Assert.Equal(FirmwareKind.Timer, info.Kind);
        Assert.Equal("2.3.4", info.Version);
        Assert.True(info.HashAppended);
    }

    [Fact]
    public void Remote_images_are_recognised_as_remote()
    {
        var info = EspImageInfo.Parse(DemoFirmware.CreateImage(FirmwareKind.Remote, "2.0.0"));
        Assert.Equal(FirmwareKind.Remote, info.Kind);
    }

    [Theory]
    [InlineData(0)]
    [InlineData(1)]
    [InlineData(100)]
    [InlineData(2000)]
    public void A_truncated_image_is_rejected(int cut)
    {
        var img = DemoFirmware.CreateImage(FirmwareKind.Timer, "2.0.0");
        var info = EspImageInfo.Parse(img.AsSpan(0, img.Length - 33 - cut));
        Assert.False(info.Intact);
        Assert.NotNull(info.Problem);
    }

    [Fact]
    public void A_flipped_bit_is_detected()
    {
        var img = DemoFirmware.CreateImage(FirmwareKind.Timer, "2.0.0");
        img[5000] ^= 0x10;
        var info = EspImageInfo.Parse(img);
        Assert.False(info.Intact);
        Assert.Contains("damaged", info.Problem);
    }

    [Fact]
    public void A_flipped_bit_in_the_digest_is_detected_even_when_the_checksum_is_fine()
    {
        var img = DemoFirmware.CreateImage(FirmwareKind.Timer, "2.0.0");
        img[^3] ^= 0x01;
        Assert.False(EspImageInfo.Parse(img).Intact);
    }

    [Fact]
    public void Other_files_are_not_images()
    {
        var info = EspImageInfo.Parse(Encoding.ASCII.GetBytes("MZ this is an exe or something else"));
        Assert.False(info.IsEspImage);
        Assert.False(info.Intact);
    }

    [Fact]
    public void An_image_for_another_chip_is_reported_with_that_chip_id()
    {
        var info = EspImageInfo.Parse(DemoFirmware.CreateImage(FirmwareKind.Timer, "2.0.0", chipId: 9));
        Assert.True(info.Intact);
        Assert.Equal(9u, info.ChipId);
    }

    [Fact]
    public void Identity_marker_needs_a_sane_version()
    {
        Assert.Equal((FirmwareKind.Timer, "2.0.0"), EspImageInfo.FindIdentity("xxFOGFW|timer|2.0.0|yy"u8));
        Assert.Equal((FirmwareKind.Unknown, null), EspImageInfo.FindIdentity("xxFOGFW|toaster|2.0.0|"u8));
        Assert.Equal((FirmwareKind.Remote, null), EspImageInfo.FindIdentity("FOGFW|remote|<script>|"u8));
        Assert.Equal((FirmwareKind.Unknown, null), EspImageInfo.FindIdentity("nothing here"u8));
    }

    [Fact]
    public void The_partition_table_is_parsed()
    {
        var entries = PartitionTable.Parse(DemoFirmware.CreatePartitionTable());
        Assert.Equal(5, entries.Count);
        Assert.Equal(0x9000u, entries.Single(e => e.IsNvs).Offset);
        Assert.Equal(new[] { 0x10000u, 0x1F0000u }, entries.Where(e => e.IsApp).Select(e => e.Offset).ToArray());     // two application slots
        Assert.All(entries.Where(e => e.IsApp), e => Assert.Equal(0x1E0000u, e.Size));
        Assert.Equal(0xE000u, entries.Single(e => e.IsOtaData).Offset);
    }

    [Fact]
    public void An_erased_partition_table_is_empty()
    {
        Assert.Empty(PartitionTable.Parse(Enumerable.Repeat((byte)0xFF, 0xC00).ToArray()));
    }
}

public class FirmwareSetTests : IDisposable
{
    private readonly string _dir = Path.Combine(Path.GetTempPath(), "fogfw-" + Guid.NewGuid().ToString("N")[..8]);

    public FirmwareSetTests() => Directory.CreateDirectory(_dir);
    public void Dispose() { try { Directory.Delete(_dir, true); } catch { /* ignore */ } }

    private string WriteFolder(FirmwareSet set, string name = "build")
    {
        var d = Path.Combine(_dir, name);
        Directory.CreateDirectory(d);
        File.WriteAllBytes(Path.Combine(d, "firmware.bin"), set.App);
        if (set.Bootloader is not null) File.WriteAllBytes(Path.Combine(d, "bootloader.bin"), set.Bootloader);
        if (set.PartitionTableBin is not null) File.WriteAllBytes(Path.Combine(d, "partitions.bin"), set.PartitionTableBin);
        if (set.BootApp0 is not null) File.WriteAllBytes(Path.Combine(d, "boot_app0.bin"), set.BootApp0);
        return d;
    }

    [Fact]
    public void A_build_folder_is_loaded_with_all_parts()
    {
        var src = DemoFirmware.CreateSet(FirmwareKind.Remote, "2.0.0");
        var set = FirmwareSet.Load(WriteFolder(src));
        Assert.Equal(FirmwareKind.Remote, set.Kind);
        Assert.Equal("2.0.0", set.Version);
        Assert.True(set.CanReinstall);
        Assert.Equal(0x10000u, set.AppOffset);
        Assert.Equal(0x9000u, set.NvsOffset);
        Assert.Equal(0x5000u, set.NvsSize);
        Assert.Equal(0xE000u, set.BootApp0Offset);
        Assert.Empty(set.Problems);
        Assert.Equal("Remote firmware 2.0.0", set.DisplayName);
    }

    [Fact]
    public void A_single_application_file_can_only_update()
    {
        var src = DemoFirmware.CreateImage(FirmwareKind.Timer, "2.0.0");
        var file = Path.Combine(_dir, "timer.bin");
        File.WriteAllBytes(file, src);
        var set = FirmwareSet.Load(file);
        Assert.Equal(FirmwareKind.Timer, set.Kind);
        Assert.False(set.CanReinstall);
        Assert.Single(set.BuildPlan(FlashMode.Update).Segments);
        Assert.Throws<InvalidOperationException>(() => set.BuildPlan(FlashMode.Reinstall));
        Assert.Throws<InvalidOperationException>(() => set.BuildPlan(FlashMode.CleanInstall));
    }

    [Fact]
    public void Update_writes_only_the_application_and_guards_the_layout()
    {
        var set = DemoFirmware.CreateSet(FirmwareKind.Timer, "2.0.0");
        var plan = set.BuildPlan(FlashMode.Update);
        // the OTA data goes first (slot 0 must be the one that boots), then the application
        Assert.Equal(new[] { "OTA data", "application" }, plan.Segments.Select(s => s.Label).ToArray());
        Assert.Equal(new[] { 0xE000u, 0x10000u }, plan.Segments.Select(s => s.Offset).ToArray());
        Assert.NotNull(plan.Guard);
        Assert.Equal(0x8000u, plan.Guard!.Offset);
        Assert.Equal(4, plan.Guard.WriteInstead.Count);
    }

    [Fact]
    public void Reinstall_writes_boot_loader_table_ota_data_and_application_in_address_order()
    {
        var plan = DemoFirmware.CreateSet(FirmwareKind.Timer, "2.0.0").BuildPlan(FlashMode.Reinstall);
        Assert.Equal(new uint[] { 0, 0x8000, 0xE000, 0x10000 }, plan.Segments.Select(s => s.Offset).ToArray());
        Assert.Null(plan.Guard);
        Assert.DoesNotContain(plan.Segments, s => s.Offset == 0x9000);        // settings are left alone
    }

    [Fact]
    public void Clean_install_also_erases_the_settings_partition()
    {
        var plan = DemoFirmware.CreateSet(FirmwareKind.Timer, "2.0.0").BuildPlan(FlashMode.CleanInstall);
        Assert.Equal(new uint[] { 0, 0x8000, 0x9000, 0xE000, 0x10000 }, plan.Segments.Select(s => s.Offset).ToArray());
        var nvs = plan.Segments.Single(s => s.Offset == 0x9000);
        Assert.Equal(0x5000, nvs.Data.Length);
        Assert.All(nvs.Data, b => Assert.Equal(0xFF, b));
    }

    [Fact]
    public void Without_boot_app0_the_ota_data_is_left_erased()
    {
        var src = DemoFirmware.CreateSet(FirmwareKind.Timer, "2.0.0");
        var d = WriteFolder(src);
        File.Delete(Path.Combine(d, "boot_app0.bin"));
        var set = FirmwareSet.FromFolder(d);
        // the framework folder of PlatformIO may supply one on a developer machine; the plan must work either way
        var ota = set.BuildPlan(FlashMode.Reinstall).Segments.Single(s => s.Offset == 0xE000);
        Assert.Equal(0x2000, ota.Data.Length);
    }

    [Fact]
    public void A_damaged_or_foreign_image_is_a_problem()
    {
        var src = DemoFirmware.CreateSet(FirmwareKind.Timer, "2.0.0");
        src.App[300] ^= 0xFF;
        var d = WriteFolder(src);
        Assert.Contains(FirmwareSet.FromFolder(d).Problems, p => p.Contains("damaged"));

        var s3 = new FirmwareSet
        {
            Title = "x", Source = "x", App = DemoFirmware.CreateImage(FirmwareKind.Timer, "2.0.0", chipId: 9),
            AppInfo = EspImageInfo.Parse(DemoFirmware.CreateImage(FirmwareKind.Timer, "2.0.0", chipId: 9)),
        };
        Assert.Contains(s3.Problems, p => p.Contains("ESP32-S3"));
    }

    [Fact]
    public void A_merged_image_is_split_so_that_the_settings_survive()
    {
        var src = DemoFirmware.CreateSet(FirmwareKind.Remote, "2.0.0");
        var merged = new byte[0x400000];
        Array.Fill(merged, (byte)0xFF);
        src.Bootloader!.CopyTo(merged, 0);
        src.PartitionTableBin!.CopyTo(merged, 0x8000);
        src.BootApp0!.CopyTo(merged, 0xE000);
        src.App.CopyTo(merged, 0x10000);
        var file = Path.Combine(_dir, "merged.bin");
        File.WriteAllBytes(file, merged);

        var set = FirmwareSet.Load(file);
        Assert.Equal(FirmwareKind.Remote, set.Kind);
        Assert.Equal("2.0.0", set.Version);
        Assert.Equal(src.App, set.App);                        // padding after the image is not carried along
        Assert.True(set.CanReinstall);
        var plan = set.BuildPlan(FlashMode.Reinstall);
        Assert.DoesNotContain(plan.Segments, s => s.Offset == 0x9000);
    }

    [Fact]
    public void The_images_built_in_this_repository_are_valid_when_they_exist()
    {
        // Optional: only meaningful on a machine where both firmwares have been built with PlatformIO.
        var root = FindRepositoryRoot();
        if (root is null) return;
        foreach (var (folder, kind) in new[] { ("FogMachineTimer", FirmwareKind.Timer), ("FogMachineRemoteControl", FirmwareKind.Remote) })
        {
            var build = Path.Combine(root, folder, ".pio", "build", "xiao_esp32c3");
            if (!File.Exists(Path.Combine(build, "firmware.bin"))) continue;
            var set = FirmwareSet.FromFolder(build);
            Assert.True(set.AppInfo.Intact, set.AppInfo.Problem);
            Assert.Equal(kind, set.Kind);
            Assert.Equal(Esp32C3.ChipId, set.AppInfo.ChipId);
            Assert.Empty(set.Problems);
            Assert.True(set.CanReinstall);
            Assert.Equal(0x10000u, set.AppOffset);
            Assert.Equal(0x9000u, set.NvsOffset);
        }
    }

    internal static string? FindRepositoryRoot()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir is not null)
        {
            if (File.Exists(Path.Combine(dir.FullName, "FogMachineTimer", "platformio.ini"))) return dir.FullName;
            dir = dir.Parent;
        }
        return null;
    }
}

public class RomLoaderTests
{
    private static async Task<(SimulatedRom Rom, RomLoader Loader)> ConnectedAsync(SimulatedRomOptions? options = null)
    {
        var rom = new SimulatedRom(options);
        var loader = new RomLoader(rom);
        await loader.ConnectAsync(BootEntry.Auto, CancellationToken.None);
        return (rom, loader);
    }

    [Fact]
    public async Task The_usb_jtag_reset_sequence_puts_the_chip_into_the_bootloader()
    {
        var (rom, loader) = await ConnectedAsync();
        Assert.True(rom.InBootloader);
        Assert.Equal(0u + Esp32C3.ChipId, await loader.GetChipIdAsync(CancellationToken.None));
    }

    [Fact]
    public async Task The_uart_bridge_reset_sequence_puts_the_chip_into_the_bootloader()
    {
        var (rom, _) = await ConnectedAsync(new SimulatedRomOptions { IsUsbJtagSerial = false });
        Assert.True(rom.InBootloader);
    }

    [Fact]
    public async Task A_chip_that_ignores_the_reset_is_reported_with_what_it_prints_instead()
    {
        var rom = new SimulatedRom(new SimulatedRomOptions { EntersBootloader = false });
        var loader = new RomLoader(rom);
        var ex = await Assert.ThrowsAsync<BootloaderNotFoundException>(() => loader.ConnectAsync(BootEntry.Auto, CancellationToken.None, attempts: 2));
        Assert.Contains("BOOT", ex.Message);
    }

    [Fact]
    public async Task A_chip_that_is_already_in_the_bootloader_can_be_used_without_touching_the_lines()
    {
        var rom = new SimulatedRom();
        rom.SetLines(true, false);
        rom.SetLines(true, true);
        rom.SetLines(false, true);
        rom.SetLines(false, false);
        Assert.True(rom.InBootloader);
        var loader = new RomLoader(rom);
        await loader.ConnectAsync(BootEntry.AlreadyThere, CancellationToken.None);
        Assert.Equal(0u + Esp32C3.ChipId, await loader.GetChipIdAsync(CancellationToken.None));
    }

    [Fact]
    public async Task Entering_the_bootloader_by_hand_is_refused_cleanly_when_nothing_answers()
    {
        var loader = new RomLoader(new SimulatedRom());                 // still running its application
        await Assert.ThrowsAsync<BootloaderNotFoundException>(() => loader.ConnectAsync(BootEntry.AlreadyThere, CancellationToken.None));
    }

    [Fact]
    public async Task The_mac_address_is_decoded_from_the_efuse_registers()
    {
        var (_, loader) = await ConnectedAsync(new SimulatedRomOptions { Mac = new byte[] { 0xA0, 0xB1, 0xC2, 0xD3, 0xE4, 0xF5 } });
        Assert.Equal("A0:B1:C2:D3:E4:F5", await loader.ReadMacAsync(CancellationToken.None));
    }

    [Fact]
    public async Task Cancellation_is_honoured_while_waiting_for_an_answer()
    {
        var (_, loader) = await ConnectedAsync();
        using var cts = new CancellationTokenSource(80);
        await Assert.ThrowsAnyAsync<OperationCanceledException>(async () =>
        {
            // an unknown command is answered at once, so wait for a reply that never comes: another op sent to a dead link
            while (true) await loader.CommandAsync(0x77, ReadOnlyMemory<byte>.Empty, 0, TimeSpan.FromSeconds(5), cts.Token);
        });
    }

    [Fact]
    public async Task An_unknown_command_is_an_error_status_not_a_hang()
    {
        var (_, loader) = await ConnectedAsync();
        var ex = await Assert.ThrowsAsync<RomException>(() => loader.CheckAsync(0x77, ReadOnlyMemory<byte>.Empty, 0, null, CancellationToken.None));
        Assert.Equal(0x05, ex.Error);
    }

    [Fact]
    public void The_checksum_is_the_xor_of_the_data_seeded_with_0xEF()
    {
        Assert.Equal(0xEFu, RomLoader.Checksum(ReadOnlySpan<byte>.Empty));
        Assert.Equal(0xEFu ^ 0x01 ^ 0x02, RomLoader.Checksum(new byte[] { 1, 2 }));
    }
}


public class DeviceCheckTests
{
    private static async Task<FlashResult> CheckAsync(SimulatedRom rom, FlashPlan plan, bool restart = true)
    {
        return await FirmwareFlasher.RunAsync(rom, plan, new FlashOptions { WriteNothing = true, Restart = restart }, null, null, CancellationToken.None);
    }

    private static bool Untouched(SimulatedRom rom) => rom.Flash.All(b => b == 0xFF);

    [Fact]
    public async Task A_check_identifies_the_chip_and_writes_nothing()
    {
        var set = DemoFirmware.CreateSet(FirmwareKind.Timer, "2.0.0");
        var rom = new SimulatedRom();
        var r = await CheckAsync(rom, set.BuildPlan(FlashMode.Reinstall));

        Assert.True(r.Success, r.Message);
        Assert.True(r.CheckedOnly);
        Assert.Equal("ESP32-C3", r.ChipName);
        Assert.Equal("40:4C:CA:5B:12:34", r.Mac);
        Assert.Contains("MAC 40:4C:CA:5B:12:34", r.Message);
        Assert.Contains("Nothing was written", r.Message);
        Assert.Equal(new[] { "boot loader", "partition table", "OTA data", "application" }, r.DifferentParts);
        Assert.Equal(0, rom.BlocksWritten);
        Assert.True(Untouched(rom));
        Assert.DoesNotContain(rom.Commands, c => c.Op is RomOp.FlashBegin or RomOp.FlashData or RomOp.FlashEnd);
    }

    [Fact]
    public async Task A_check_sends_the_device_back_to_its_application()
    {
        var rom = new SimulatedRom();
        var r = await CheckAsync(rom, DemoFirmware.CreateSet(FirmwareKind.Timer, "2.0.0").BuildPlan(FlashMode.Update));
        Assert.True(r.Restarted);
        Assert.False(rom.InBootloader);
        Assert.Equal(1, rom.WatchdogResets);
    }

    [Fact]
    public async Task A_check_can_leave_the_device_in_the_bootloader()
    {
        var rom = new SimulatedRom();
        var r = await CheckAsync(rom, DemoFirmware.CreateSet(FirmwareKind.Timer, "2.0.0").BuildPlan(FlashMode.Update), restart: false);
        Assert.False(r.Restarted);
        Assert.True(rom.InBootloader);
    }

    [Fact]
    public async Task After_flashing_the_same_firmware_a_check_finds_no_difference()
    {
        var set = DemoFirmware.CreateSet(FirmwareKind.Remote, "2.0.0");
        var rom = new SimulatedRom();
        var flash = await FirmwareFlasher.RunAsync(rom, set.BuildPlan(FlashMode.Reinstall), new FlashOptions(), null, null, CancellationToken.None);
        Assert.True(flash.Success, flash.Message);
        int blocks = rom.BlocksWritten;

        var r = await CheckAsync(rom, set.BuildPlan(FlashMode.Reinstall));
        Assert.True(r.Success, r.Message);
        Assert.Empty(r.DifferentParts);
        Assert.Contains("exactly this firmware", r.Message);
        Assert.Equal(blocks, rom.BlocksWritten);
    }

    [Fact]
    public async Task A_newer_firmware_is_reported_as_a_difference_in_the_application_only()
    {
        var v1 = DemoFirmware.CreateSet(FirmwareKind.Remote, "2.0.0");
        var v2 = DemoFirmware.CreateSet(FirmwareKind.Remote, "2.0.1");
        var rom = new SimulatedRom();
        await FirmwareFlasher.RunAsync(rom, v1.BuildPlan(FlashMode.Reinstall), new FlashOptions(), null, null, CancellationToken.None);

        var r = await CheckAsync(rom, v2.BuildPlan(FlashMode.Update));
        Assert.Equal(new[] { "application" }, r.DifferentParts);
        Assert.Contains("differs from this firmware in: application", r.Message);
    }

    [Fact]
    public async Task Without_a_firmware_the_check_only_identifies()
    {
        var rom = new SimulatedRom();
        var r = await CheckAsync(rom, new FlashPlan { Segments = Array.Empty<FlashSegment>() });
        Assert.True(r.Success);
        Assert.Contains("identified", r.Message);
        Assert.Empty(r.DifferentParts);
    }

    [Fact]
    public async Task A_check_of_another_chip_says_so_and_writes_nothing()
    {
        var rom = new SimulatedRom(new SimulatedRomOptions { ChipId = 9 });
        var r = await CheckAsync(rom, DemoFirmware.CreateSet(FirmwareKind.Timer, "2.0.0").BuildPlan(FlashMode.Update));
        Assert.False(r.Success);
        Assert.Contains("ESP32-S3", r.Message);
        Assert.True(Untouched(rom));
    }

    [Fact]
    public async Task A_check_of_a_device_that_does_not_enter_the_bootloader_fails_with_advice()
    {
        var rom = new SimulatedRom(new SimulatedRomOptions { EntersBootloader = false });
        var r = await CheckAsync(rom, DemoFirmware.CreateSet(FirmwareKind.Timer, "2.0.0").BuildPlan(FlashMode.Update));
        Assert.False(r.Success);
        Assert.Contains("BOOT", r.Message);
    }
}

public class FirmwareFlasherTests
{
    private static readonly FlashOptions NoRestartNeeded = new();

    private static async Task<(FlashResult Result, SimulatedRom Rom, List<string> Log, List<FlashProgress> Progress)> RunAsync(
        FirmwareSet set, FlashMode mode, SimulatedRom? rom = null, FlashOptions? options = null, CancellationToken ct = default,
        Action<FlashProgress>? onProgress = null)
    {
        rom ??= new SimulatedRom();
        var log = new List<string>();
        var progress = new SyncProgress<FlashProgress>(p => onProgress?.Invoke(p));
        var result = await FirmwareFlasher.RunAsync(rom, set.BuildPlan(mode), options ?? NoRestartNeeded, progress, log.Add, ct);
        return (result, rom, log, progress.Items);
    }

    private static byte[] Slice(SimulatedRom rom, uint offset, int length) => rom.Flash.AsSpan((int)offset, length).ToArray();

    /// <summary>A device that already carries this project's boot loader and partition table (but no application).</summary>
    private static SimulatedRom RomWithLayout(FirmwareSet set)
    {
        var init = new byte[SimulatedRom.FlashSize];
        Array.Fill(init, (byte)0xFF);
        set.Bootloader!.CopyTo(init, 0);
        set.PartitionTableBin!.CopyTo(init, 0x8000);
        set.BootApp0!.CopyTo(init, 0xE000);                         // slot 0 selected, as after a USB install
        return new SimulatedRom(new SimulatedRomOptions { InitialFlash = init });
    }

    [Fact]
    public async Task An_update_writes_the_application_verifies_it_and_restarts_the_device()
    {
        var set = DemoFirmware.CreateSet(FirmwareKind.Timer, "2.1.0");
        var (r, rom, log, _) = await RunAsync(set, FlashMode.Update, RomWithLayout(set));

        Assert.True(r.Success, r.Message);
        Assert.Equal(Esp32C3.Name, r.ChipName);
        Assert.Equal("40:4C:CA:5B:12:34", r.Mac);
        Assert.Equal(set.App, Slice(rom, 0x10000, set.App.Length));
        Assert.Equal(1, r.SegmentsWritten);
        Assert.True(r.Restarted);
        Assert.False(rom.InBootloader);
        Assert.True(rom.ApplicationStarts >= 1);
        Assert.Contains(log, l => l.Contains("verified"));
        Assert.Equal(set.App.Length, r.BytesSent);
    }

    [Fact]
    public async Task A_blank_device_with_an_update_gets_the_whole_layout_because_the_partition_table_differs()
    {
        var set = DemoFirmware.CreateSet(FirmwareKind.Remote, "2.0.0");
        var (r, rom, log, _) = await RunAsync(set, FlashMode.Update);

        Assert.True(r.Success, r.Message);
        Assert.Equal(4, r.SegmentsWritten);
        Assert.Equal(set.Bootloader, Slice(rom, 0, set.Bootloader!.Length));
        Assert.Equal(set.PartitionTableBin, Slice(rom, 0x8000, set.PartitionTableBin!.Length));
        Assert.Equal(set.App, Slice(rom, 0x10000, set.App.Length));
        Assert.Contains(log, l => l.Contains("different partition table"));
    }

    [Fact]
    public async Task A_second_update_finds_nothing_to_write()
    {
        var set = DemoFirmware.CreateSet(FirmwareKind.Timer, "2.1.0");
        var (_, rom, _, _) = await RunAsync(set, FlashMode.Reinstall);
        int blocks = rom.BlocksWritten;

        var (r, _, log, _) = await RunAsync(set, FlashMode.Update, rom);
        Assert.True(r.Success);
        Assert.True(r.AlreadyUpToDate);
        Assert.Equal(2, r.SegmentsSkipped);                          // OTA data and application
        Assert.Equal(blocks, rom.BlocksWritten);
        Assert.Contains("already has exactly this firmware", r.Message);
        Assert.Contains(log, l => l.Contains("already up to date"));
        Assert.True(r.Restarted, "even when nothing is written the chip must leave the bootloader");
    }

    [Fact]
    public async Task An_update_selects_slot_0_again_when_the_device_last_ran_from_slot_1()
    {
        // after over-the-air updates the OTA data may point at slot 1; a USB update writes slot 0 and must make it boot
        var set = DemoFirmware.CreateSet(FirmwareKind.Timer, "2.1.0");
        var rom = RomWithLayout(set);
        rom.Flash[0xE000] = 0x02;                                    // OTA data: sequence 2 = slot 1
        rom.Flash[0xE001] = 0x00;

        var (r, _, _, _) = await RunAsync(set, FlashMode.Update, rom);
        Assert.True(r.Success, r.Message);
        Assert.Equal(2, r.SegmentsWritten);
        Assert.Equal(set.BootApp0, Slice(rom, 0xE000, set.BootApp0!.Length));
        Assert.Equal(set.App, Slice(rom, 0x10000, set.App.Length));
    }

    [Fact]
    public async Task A_device_with_the_old_single_slot_layout_gets_the_new_layout_and_keeps_its_settings()
    {
        // the first over-the-air capable firmware is installed over USB on a device that carries the previous layout (one 3 MB slot)
        var set = DemoFirmware.CreateSet(FirmwareKind.Timer, "2.1.0");
        var init = new byte[SimulatedRom.FlashSize];
        Array.Fill(init, (byte)0xFF);
        var oldTable = DemoFirmware.CreatePartitionTable();
        // pretend the old table differs: single big app partition
        oldTable[2 * 32 + 8] = 0x00; oldTable[2 * 32 + 9] = 0x00; oldTable[2 * 32 + 10] = 0x30;      // app0 size 0x300000
        oldTable.CopyTo(init, 0x8000);
        for (int i = 0; i < 0x5000; i++) init[0x9000 + i] = (byte)(i % 199);
        var settings = init.AsSpan(0x9000, 0x5000).ToArray();

        var rom = new SimulatedRom(new SimulatedRomOptions { InitialFlash = init });
        var (r, _, log, _) = await RunAsync(set, FlashMode.Update, rom);

        Assert.True(r.Success, r.Message);
        Assert.Contains(log, l => l.Contains("different partition table"));
        Assert.Equal(set.PartitionTableBin, Slice(rom, 0x8000, set.PartitionTableBin!.Length));
        Assert.Equal(settings, Slice(rom, 0x9000, 0x5000));
    }

    [Fact]
    public async Task A_new_version_replaces_only_the_application_when_the_layout_is_the_same()
    {
        var v1 = DemoFirmware.CreateSet(FirmwareKind.Timer, "2.0.0");
        var v2 = DemoFirmware.CreateSet(FirmwareKind.Timer, "2.0.1");
        var (_, rom, _, _) = await RunAsync(v1, FlashMode.Reinstall);

        var (r, _, _, _) = await RunAsync(v2, FlashMode.Update, rom);
        Assert.True(r.Success, r.Message);
        Assert.Equal(1, r.SegmentsWritten);
        Assert.Equal(v2.App, Slice(rom, 0x10000, v2.App.Length));
        Assert.Equal(v1.Bootloader, Slice(rom, 0, v1.Bootloader!.Length));
    }

    [Fact]
    public async Task A_shorter_application_does_not_leave_the_tail_of_the_old_one_behind_in_a_way_that_breaks_verification()
    {
        var big = DemoFirmware.CreateSet(FirmwareKind.Timer, "2.0.0", approxSize: 300_000);
        var small = DemoFirmware.CreateSet(FirmwareKind.Timer, "2.0.1", approxSize: 120_000);
        var (_, rom, _, _) = await RunAsync(big, FlashMode.Reinstall);
        var (r, _, _, _) = await RunAsync(small, FlashMode.Update, rom);
        Assert.True(r.Success, r.Message);
        Assert.Equal(small.App, Slice(rom, 0x10000, small.App.Length));
    }

    [Fact]
    public async Task Reinstall_keeps_the_settings_and_clean_install_wipes_them()
    {
        var set = DemoFirmware.CreateSet(FirmwareKind.Timer, "2.0.0");
        var init = new byte[SimulatedRom.FlashSize];
        Array.Fill(init, (byte)0xFF);
        for (int i = 0; i < 0x5000; i++) init[0x9000 + i] = (byte)(i % 251);      // pairing data and so on
        var settings = init.AsSpan(0x9000, 0x5000).ToArray();

        var keep = new SimulatedRom(new SimulatedRomOptions { InitialFlash = init });
        var (r1, _, _, _) = await RunAsync(set, FlashMode.Reinstall, keep);
        Assert.True(r1.Success, r1.Message);
        Assert.Equal(settings, Slice(keep, 0x9000, 0x5000));

        var wipe = new SimulatedRom(new SimulatedRomOptions { InitialFlash = init });
        var (r2, _, _, _) = await RunAsync(set, FlashMode.CleanInstall, wipe);
        Assert.True(r2.Success, r2.Message);
        Assert.All(Slice(wipe, 0x9000, 0x5000), b => Assert.Equal(0xFF, b));
    }

    [Fact]
    public async Task An_update_leaves_the_settings_alone()
    {
        var set = DemoFirmware.CreateSet(FirmwareKind.Timer, "2.0.0");
        var (_, rom, _, _) = await RunAsync(set, FlashMode.Reinstall);
        for (int i = 0; i < 100; i++) rom.Flash[0x9000 + i] = (byte)i;
        var newer = DemoFirmware.CreateSet(FirmwareKind.Timer, "2.0.1");
        await RunAsync(newer, FlashMode.Update, rom);
        for (int i = 0; i < 100; i++) Assert.Equal((byte)i, rom.Flash[0x9000 + i]);
    }

    [Fact]
    public async Task Progress_is_monotonic_and_ends_complete()
    {
        var set = DemoFirmware.CreateSet(FirmwareKind.Timer, "2.0.0", approxSize: 250_000);
        var (r, _, _, progress) = await RunAsync(set, FlashMode.Reinstall);
        Assert.True(r.Success, r.Message);

        var writing = progress.Where(p => p.Stage == FlashStage.Writing && p.Total > 0).ToList();
        Assert.NotEmpty(writing);
        for (int i = 1; i < writing.Count; i++) Assert.True(writing[i].Done >= writing[i - 1].Done);
        Assert.Equal(writing[0].Total, writing[^1].Total);
        Assert.Equal(writing[^1].Total, writing[^1].Done);
        Assert.Equal(FlashStage.Done, progress[^1].Stage);
        Assert.Equal(1.0, progress[^1].Fraction);
        Assert.Contains(progress, p => p.Stage == FlashStage.EnteringBootloader);
        Assert.Contains(progress, p => p.Stage == FlashStage.Verifying);
        Assert.Equal(progress.OrderBy(p => p.Stage).Select(p => p.Stage), progress.Select(p => p.Stage));
    }

    [Fact]
    public async Task A_failed_verification_is_reported_and_the_device_is_still_sent_back_to_its_application()
    {
        var set = DemoFirmware.CreateSet(FirmwareKind.Timer, "2.0.0");
        var rom = new SimulatedRom(new SimulatedRomOptions { CorruptWrites = true });
        var (r, _, _, _) = await RunAsync(set, FlashMode.Reinstall, rom);
        Assert.False(r.Success);
        Assert.Contains("Verification failed", r.Message);
        Assert.True(r.Restarted);
        Assert.False(rom.InBootloader);
    }

    [Fact]
    public async Task Verification_can_be_switched_off()
    {
        var set = DemoFirmware.CreateSet(FirmwareKind.Timer, "2.0.0");
        var rom = new SimulatedRom(new SimulatedRomOptions { CorruptWrites = true });
        var (r, _, _, _) = await RunAsync(set, FlashMode.Update, rom, new FlashOptions { Verify = false });
        Assert.True(r.Success);
    }

    [Fact]
    public async Task Cancelling_stops_the_transfer_and_restarts_the_device()
    {
        var set = DemoFirmware.CreateSet(FirmwareKind.Timer, "2.0.0", approxSize: 400_000);
        using var cts = new CancellationTokenSource();
        var (r, rom, _, _) = await RunAsync(set, FlashMode.Reinstall, ct: cts.Token,
            onProgress: p => { if (p.Stage == FlashStage.Writing && p.Done > 20_000) cts.Cancel(); });
        Assert.False(r.Success);
        Assert.True(r.Cancelled);
        Assert.Contains("Cancelled", r.Message);
        Assert.True(r.Restarted);
        Assert.False(rom.InBootloader);
        Assert.True(r.BytesSent < 400_000 / 2);
    }

    [Fact]
    public async Task A_cable_pulled_during_the_transfer_is_reported_and_nothing_more_is_attempted()
    {
        var set = DemoFirmware.CreateSet(FirmwareKind.Timer, "2.0.0");
        var rom = new SimulatedRom(new SimulatedRomOptions { DropLinkAtBlock = 10 });
        var (r, _, _, _) = await RunAsync(set, FlashMode.Reinstall, rom);
        Assert.False(r.Success);
        Assert.Contains("connection to the device was lost", r.Message);
        Assert.False(r.Restarted);
    }

    [Fact]
    public async Task A_device_that_never_enters_the_bootloader_gets_a_helpful_message()
    {
        var set = DemoFirmware.CreateSet(FirmwareKind.Timer, "2.0.0");
        var rom = new SimulatedRom(new SimulatedRomOptions { EntersBootloader = false });
        var (r, _, _, _) = await RunAsync(set, FlashMode.Update, rom);
        Assert.False(r.Success);
        Assert.Contains("BOOT", r.Message);
        Assert.Equal(0, r.SegmentsWritten);
    }

    [Fact]
    public async Task Another_chip_is_never_written()
    {
        var set = DemoFirmware.CreateSet(FirmwareKind.Timer, "2.0.0");
        var rom = new SimulatedRom(new SimulatedRomOptions { ChipId = 9 });
        var (r, _, _, _) = await RunAsync(set, FlashMode.Update, rom);
        Assert.False(r.Success);
        Assert.Contains("ESP32-S3", r.Message);
        Assert.Equal(0, rom.BlocksWritten);
        Assert.True(r.Restarted);
    }

    [Fact]
    public async Task Skipping_unchanged_parts_can_be_switched_off()
    {
        var set = DemoFirmware.CreateSet(FirmwareKind.Timer, "2.0.0");
        var (_, rom, _, _) = await RunAsync(set, FlashMode.Reinstall);
        int before = rom.BlocksWritten;
        var (r, _, _, _) = await RunAsync(set, FlashMode.Reinstall, rom, new FlashOptions { SkipUnchanged = false });
        Assert.Equal(4, r.SegmentsWritten);
        Assert.True(rom.BlocksWritten > before);
    }

    [Fact]
    public async Task The_device_can_be_left_in_the_bootloader_for_a_following_step()
    {
        var set = DemoFirmware.CreateSet(FirmwareKind.Timer, "2.0.0");
        var (r, rom, _, _) = await RunAsync(set, FlashMode.Update, options: new FlashOptions { Restart = false });
        Assert.True(r.Success);
        Assert.False(r.Restarted);
        Assert.True(rom.InBootloader);
    }

    [Fact]
    public async Task The_application_is_restarted_through_the_watchdog_as_well()
    {
        var set = DemoFirmware.CreateSet(FirmwareKind.Timer, "2.0.0");
        var (_, rom, _, _) = await RunAsync(set, FlashMode.Update);
        Assert.Equal(1, rom.WatchdogResets);
    }

    [Fact]
    public async Task A_port_that_re_enumerates_during_the_reset_is_reopened_and_the_update_goes_on()
    {
        var set = DemoFirmware.CreateSet(FirmwareKind.Timer, "2.0.0");
        var rom = new SimulatedRom(new SimulatedRomOptions { ReenumeratesOnReset = true });
        var (r, _, log, _) = await RunAsync(set, FlashMode.Reinstall, rom);

        Assert.True(r.Success, r.Message);
        Assert.Equal(1, rom.Reopens);
        Assert.Contains(log, l => l.Contains("reopening the port"));
        Assert.Equal(set.App, Slice(rom, 0x10000, set.App.Length));
    }

    [Fact]
    public async Task A_device_that_does_not_come_back_after_the_reset_is_reported_as_a_lost_connection()
    {
        var set = DemoFirmware.CreateSet(FirmwareKind.Timer, "2.0.0");
        var rom = new SimulatedRom(new SimulatedRomOptions { ReenumeratesOnReset = true, NeverComesBack = true });
        var (r, _, _, _) = await RunAsync(set, FlashMode.Update, rom);

        Assert.False(r.Success);
        Assert.Contains("connection to the device was lost", r.Message);
        Assert.Equal(0, r.SegmentsWritten);
    }

    [Fact]
    public async Task The_wire_protocol_uses_the_parameters_esptool_uses_for_this_chip()
    {
        // verified against esptool 5.4 --trace: 24 byte SPI params, 20 byte begin packet, 16 + 1024 byte data packets
        var set = DemoFirmware.CreateSet(FirmwareKind.Timer, "2.0.0");
        var (_, rom, _, _) = await RunAsync(set, FlashMode.Update, RomWithLayout(set));
        Assert.Contains(rom.Commands, c => c.Op == RomOp.SpiAttach && c.PayloadLength == 8);
        Assert.Contains(rom.Commands, c => c.Op == RomOp.SpiSetParams && c.PayloadLength == 24);
        Assert.Contains(rom.Commands, c => c.Op == RomOp.FlashBegin && c.PayloadLength == 20);
        Assert.All(rom.Commands.Where(c => c.Op == RomOp.FlashData), c => Assert.Equal(16 + RomLoader.FlashBlockSize, c.PayloadLength));
        Assert.Contains(rom.Commands, c => c.Op == RomOp.SpiFlashMd5 && c.PayloadLength == 16);
        Assert.DoesNotContain(rom.Commands, c => c.Op == RomOp.FlashEnd);        // the ROM would run the application right away
    }

    [Fact]
    public async Task The_packets_are_byte_for_byte_those_of_esptool()
    {
        // Golden values from `esptool 5.4.0 --no-stub --trace write-flash` for this chip (see tools notes in docs/FIRMWARE_UPDATE.md):
        //   SPI_SET_PARAMS  00000000 00004000 00000100 00100000 00010000 ffff0000
        //   FLASH_BEGIN     <size> <blocks> 00040000 <offset> 00000000     (size = exact length of the data, not rounded)
        //   FLASH_DATA      00040000 <seq> 00000000 00000000 <1024 bytes, last block padded with ff>
        //   SPI_FLASH_MD5   <offset> <size> 00000000 00000000
        var set = DemoFirmware.CreateSet(FirmwareKind.Timer, "2.0.0");
        var (_, rom, _, _) = await RunAsync(set, FlashMode.Update, RomWithLayout(set));

        int Find(byte op, int skip = 0) => rom.Commands.Select((c, i) => (c, i)).Where(x => x.c.Op == op).Skip(skip).First().i;

        Assert.Equal(Convert.FromHexString("00000000" + "00004000" + "00000100" + "00100000" + "00010000" + "ffff0000"), rom.Payloads[Find(RomOp.SpiSetParams)]);
        Assert.Equal(new byte[8], rom.Payloads[Find(RomOp.SpiAttach)]);

        uint size = (uint)set.App.Length;
        var begin = rom.Payloads[Find(RomOp.FlashBegin)];
        Assert.Equal(20, begin.Length);
        Assert.Equal(size, BinaryPrimitives.ReadUInt32LittleEndian(begin.AsSpan(0)));
        Assert.Equal((size + 1023) / 1024, BinaryPrimitives.ReadUInt32LittleEndian(begin.AsSpan(4)));
        Assert.Equal(0x400u, BinaryPrimitives.ReadUInt32LittleEndian(begin.AsSpan(8)));
        Assert.Equal(0x10000u, BinaryPrimitives.ReadUInt32LittleEndian(begin.AsSpan(12)));
        Assert.Equal(0u, BinaryPrimitives.ReadUInt32LittleEndian(begin.AsSpan(16)));

        var first = rom.Payloads[Find(RomOp.FlashData)];
        Assert.Equal(Convert.FromHexString("00040000" + "00000000" + "00000000" + "00000000"), first.AsSpan(0, 16).ToArray());
        Assert.Equal(set.App.AsSpan(0, 1024).ToArray(), first.AsSpan(16).ToArray());
        var seqs = rom.Commands.Select((c, i) => (c, i)).Where(x => x.c.Op == RomOp.FlashData)
            .Select(x => BinaryPrimitives.ReadUInt32LittleEndian(rom.Payloads[x.i].AsSpan(4))).ToArray();
        Assert.Equal(Enumerable.Range(0, (int)((size + 1023) / 1024)).Select(i => (uint)i).ToArray(), seqs);

        // the MD5 request covers the exact data (no padding): the last one verifies the application
        var md5 = rom.Payloads[rom.Commands.FindLastIndex(c => c.Op == RomOp.SpiFlashMd5)];
        Assert.Equal(0x10000u, BinaryPrimitives.ReadUInt32LittleEndian(md5.AsSpan(0)));
        Assert.Equal(size, BinaryPrimitives.ReadUInt32LittleEndian(md5.AsSpan(4)));
        Assert.Equal(0u, BinaryPrimitives.ReadUInt32LittleEndian(md5.AsSpan(8)));
        Assert.Equal(0u, BinaryPrimitives.ReadUInt32LittleEndian(md5.AsSpan(12)));
    }

    [Fact]
    public async Task The_last_block_is_padded_with_erased_flash_bytes()
    {
        var set = DemoFirmware.CreateSet(FirmwareKind.Timer, "2.0.0");
        var (_, rom, _, _) = await RunAsync(set, FlashMode.Update, RomWithLayout(set));
        int end = 0x10000 + set.App.Length;
        Assert.NotEqual(0, set.App.Length % RomLoader.FlashBlockSize);                 // the test is only meaningful with a partial last block
        Assert.Equal(0xFF, rom.Flash[end]);
        Assert.Equal(0xFF, rom.Flash[end + 5]);
    }
}
