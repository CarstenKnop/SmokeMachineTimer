using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Text;
using FogConsole.Flashing;
using Xunit;
using Xunit.Abstractions;

namespace FogConsole.Tests;

/// <summary>Exposes a <see cref="SimulatedRom"/> as a TCP serial port (pyserial's <c>socket://</c>) so that the real esptool can talk to it.</summary>
internal sealed class RomSocketServer : IDisposable
{
    private readonly TcpListener _listener = new(IPAddress.Loopback, 0);
    private readonly SimulatedRom _rom;
    private readonly CancellationTokenSource _cts = new();
    private Task? _pump;

    public RomSocketServer(SimulatedRom rom) => _rom = rom;

    public int Port => ((IPEndPoint)_listener.LocalEndpoint).Port;
    public string Url => $"socket://127.0.0.1:{Port}";

    public void Start()
    {
        _listener.Start();
        _pump = Task.Run(RunAsync);
    }

    private async Task RunAsync()
    {
        try
        {
            while (!_cts.IsCancellationRequested)
            {
                using var client = await _listener.AcceptTcpClientAsync(_cts.Token);
                client.NoDelay = true;
                var stream = client.GetStream();
                var toDevice = Task.Run(async () =>
                {
                    var buf = new byte[4096];
                    while (true)
                    {
                        int n = await stream.ReadAsync(buf, _cts.Token);
                        if (n == 0) return;
                        await _rom.WriteAsync(buf.AsMemory(0, n), _cts.Token);
                    }
                });
                var fromDevice = Task.Run(async () =>
                {
                    var buf = new byte[4096];
                    while (true)
                    {
                        int n = await _rom.ReadAsync(buf, TimeSpan.FromMilliseconds(50), _cts.Token);
                        if (n > 0) await stream.WriteAsync(buf.AsMemory(0, n), _cts.Token);
                        if (toDevice.IsCompleted) return;
                    }
                });
                await Task.WhenAny(toDevice, fromDevice);
            }
        }
        catch (Exception ex) when (ex is OperationCanceledException or IOException or SocketException or ObjectDisposedException)
        {
            // client gone or test over
        }
    }

    public void Dispose()
    {
        _cts.Cancel();
        _listener.Stop();
        try { _pump?.Wait(1000); } catch { /* ignore */ }
    }
}

/// <summary>
/// Checks the simulated chip (and with it our protocol client) against the real esptool, when PlatformIO's copy is installed:
/// esptool must be able to flash the simulated chip, and what it writes must end up byte-exact in the simulated flash.
/// </summary>
public class EsptoolCompatibilityTests : IDisposable
{
    private readonly ITestOutputHelper _out;
    private readonly string _dir = Path.Combine(Path.GetTempPath(), "fogtool-" + Guid.NewGuid().ToString("N")[..8]);

    public EsptoolCompatibilityTests(ITestOutputHelper output)
    {
        _out = output;
        Directory.CreateDirectory(_dir);
    }

    public void Dispose() { try { Directory.Delete(_dir, true); } catch { /* ignore */ } }

    private static string? FindEsptool()
    {
        var home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        var p = Path.Combine(home, ".platformio", "packages", "tool-esptool", "esptool.exe");
        return File.Exists(p) ? p : null;
    }

    private static SimulatedRom RomInBootloader(SimulatedRomOptions? o = null)
    {
        var rom = new SimulatedRom(o);
        rom.SetLines(true, false);
        rom.SetLines(true, true);
        rom.SetLines(false, true);
        rom.SetLines(false, false);
        Assert.True(rom.InBootloader);
        return rom;
    }

    private (int Exit, string Output) RunEsptool(string exe, RomSocketServer server, params string[] args)
    {
        var psi = new ProcessStartInfo(exe)
        {
            RedirectStandardOutput = true, RedirectStandardError = true, UseShellExecute = false, CreateNoWindow = true, WorkingDirectory = _dir,
        };
        foreach (var a in new[] { "--chip", "esp32c3", "--port", server.Url, "--before", "no-reset", "--after", "no-reset", "--no-stub" }.Concat(args))
            psi.ArgumentList.Add(a);
        using var p = Process.Start(psi)!;
        var stdout = p.StandardOutput.ReadToEndAsync();
        var stderr = p.StandardError.ReadToEndAsync();
        if (!p.WaitForExit(60_000))
        {
            try { p.Kill(true); } catch { /* ignore */ }
            return (-1, "timeout\n" + stdout.Result + stderr.Result);
        }
        string text = stdout.Result + stderr.Result;
        _out.WriteLine(text);
        return (p.ExitCode, text);
    }

    [Fact]
    public void Esptool_flashes_the_simulated_chip_and_verifies_it()
    {
        var exe = FindEsptool();
        if (exe is null) return;                          // optional check: PlatformIO is not installed here

        var set = DemoFirmware.CreateSet(FirmwareKind.Timer, "2.0.0", approxSize: 150_000);
        var appFile = Path.Combine(_dir, "app.bin");
        File.WriteAllBytes(appFile, set.App);
        var bootFile = Path.Combine(_dir, "bootloader.bin");
        File.WriteAllBytes(bootFile, set.Bootloader!);

        var rom = RomInBootloader();
        using var server = new RomSocketServer(rom);
        server.Start();

        var (exit, text) = RunEsptool(exe, server, "write-flash", "--flash-mode", "dio", "--flash-freq", "80m", "--flash-size", "4MB",
            "0x0", bootFile, "0x10000", appFile);

        Assert.True(exit == 0, text);
        Assert.Contains("Hash of data verified", text);
        Assert.Equal(set.App, rom.Flash.AsSpan(0x10000, set.App.Length).ToArray());

        Assert.Equal(set.Bootloader, rom.Flash.AsSpan(0, set.Bootloader!.Length).ToArray());
    }

    [Fact]
    public void Esptool_reads_the_chip_identity_the_way_our_loader_does()
    {
        var exe = FindEsptool();
        if (exe is null) return;

        var rom = RomInBootloader(new SimulatedRomOptions { Mac = new byte[] { 0x40, 0x4C, 0xCA, 0x5B, 0x12, 0x34 } });
        using var server = new RomSocketServer(rom);
        server.Start();

        var (exit, text) = RunEsptool(exe, server, "read-mac");
        Assert.True(exit == 0, text);
        // the same MAC that RomLoader.ReadMacAsync decodes from the same registers
        Assert.Contains("40:4c:ca:5b:12:34", text.ToLowerInvariant());
    }

    [Fact]
    public async Task Our_loader_and_esptool_agree_on_the_md5_of_what_esptool_wrote()
    {
        var exe = FindEsptool();
        if (exe is null) return;

        var data = DemoFirmware.CreateImage(FirmwareKind.Remote, "2.0.0", 90_000);
        var file = Path.Combine(_dir, "x.bin");
        File.WriteAllBytes(file, data);

        var rom = RomInBootloader();
        using (var server = new RomSocketServer(rom))
        {
            server.Start();
            var (exit, text) = RunEsptool(exe, server, "write-flash", "0x20000", file);
            Assert.True(exit == 0, text);
        }

        var loader = new RomLoader(rom);
        await loader.ConnectAsync(BootEntry.AlreadyThere, CancellationToken.None);
        var md5 = await loader.FlashMd5Async(0x20000, (uint)data.Length, CancellationToken.None);
        Assert.Equal(Convert.ToHexString(System.Security.Cryptography.MD5.HashData(data)).ToLowerInvariant(), md5);
    }
}
