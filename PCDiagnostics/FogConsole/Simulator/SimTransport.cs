using System.IO;
using FogConsole.Transport;

namespace FogConsole.Simulator;

/// <summary>
/// A "USB cable" to a <see cref="SimDevice"/>. The device keeps living (virtual timers keep cycling) while the cable is
/// unplugged; after a simulated reboot the port is gone for a few seconds, just like USB re-enumeration.
/// </summary>
public sealed class SimTransport : ITransport
{
    private readonly SimDevice _device;
    private readonly TimeSpan _pumpInterval;
    private CancellationTokenSource? _cts;
    private Task? _pump;
    private int _closedRaised;
    private volatile bool _open;

    public SimTransport(SimDevice device, TimeSpan? pumpInterval = null)
    {
        _device = device;
        _pumpInterval = pumpInterval ?? TimeSpan.FromMilliseconds(20);
    }

    public SimDevice Device => _device;
    public string Name => _device.Kind == SimKind.Timer ? "Simulator (timer)" : "Simulator";
    public bool IsOpen => _open;

    public event Action<string>? LineReceived;
#pragma warning disable CS0067 // part of ITransport; the simulator never sends oversized lines
    public event Action<int>? LineOverflow;
#pragma warning restore CS0067
    public event Action<TransportClosedInfo>? Closed;

    public Task OpenAsync(CancellationToken ct = default)
    {
        ct.ThrowIfCancellationRequested();
        if (DateTime.UtcNow < _device.DownUntilUtc)
            throw new IOException("The port does not exist (simulated device is rebooting)");

        _device.SyncToClock();
        _device.DrainOutput(); // whatever was queued while nobody was listening is gone
        _device.RebootRequested += OnReboot;
        _closedRaised = 0;
        _open = true;
        _cts = new CancellationTokenSource();

        if (_device.ConsumeBootBanner())
        {
            foreach (var l in new[]
                     {
                         "ESP-ROM:esp32c3-api1-20210207",
                         "Build:Feb  7 2021",
                         "rst:0x15 (USB_UART_HPSYS),boot:0xc (SPI_FAST_FLASH_BOOT)",
                         "SPIWP:0xee",
                         "mode:DIO, clock div:1",
                         "load:0x3fcd5810,len:0x1740",
                         "entry 0x403cc710",
                     })
                LineReceived?.Invoke(l);
        }

        var token = _cts.Token;
        _pump = Task.Run(() => PumpAsync(token), CancellationToken.None);
        return Task.CompletedTask;
    }

    private async Task PumpAsync(CancellationToken ct)
    {
        try
        {
            while (!ct.IsCancellationRequested)
            {
                await Task.Delay(_pumpInterval, ct).ConfigureAwait(false);
                _device.SyncToClock();
                Flush();
            }
        }
        catch (OperationCanceledException)
        {
            // closing
        }
    }

    private void Flush()
    {
        if (!_open) return;
        foreach (var line in _device.DrainOutput())
        {
            if (!_open) return;
            LineReceived?.Invoke(line);
        }
    }

    public Task WriteLineAsync(string line, CancellationToken ct = default)
    {
        if (!_open) throw new IOException("Port is not open");
        ct.ThrowIfCancellationRequested();
        _device.SyncToClock();
        _device.HandleLine(line);
        Flush();
        return Task.CompletedTask;
    }

    private void OnReboot(TimeSpan down)
    {
        _device.DownUntilUtc = DateTime.UtcNow + down;
        _ = Task.Run(async () =>
        {
            await Task.Delay(120).ConfigureAwait(false);
            Flush();
            Stop(unexpected: true, "device rebooted - USB port re-enumerating");
        });
    }

    private void Stop(bool unexpected, string reason)
    {
        if (!_open && _closedRaised != 0) return;
        _open = false;
        _device.RebootRequested -= OnReboot;
        try { _cts?.Cancel(); } catch { /* ignore */ }
        if (Interlocked.Exchange(ref _closedRaised, 1) == 0)
            Closed?.Invoke(new TransportClosedInfo(unexpected, reason));
    }

    public Task CloseAsync()
    {
        Stop(unexpected: false, "closed");
        return Task.CompletedTask;
    }

    public async ValueTask DisposeAsync()
    {
        await CloseAsync().ConfigureAwait(false);
        try { if (_pump is not null) await _pump.ConfigureAwait(false); } catch { /* ignore */ }
        _cts?.Dispose();
    }
}
