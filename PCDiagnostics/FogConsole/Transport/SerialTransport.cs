using System.IO;
using System.IO.Ports;
using System.Text;

namespace FogConsole.Transport;

/// <summary>
/// USB-CDC serial port transport. DTR and RTS are kept low so that opening the port does not reset an
/// ESP32-C3 into its bootloader. The port may vanish at any time (USB re-enumeration after a reboot or deep sleep).
/// </summary>
public sealed class SerialTransport : ITransport
{
    private readonly string _portName;
    private readonly int _baud;
    private readonly LineFramer _framer = new();
    private readonly SemaphoreSlim _writeLock = new(1, 1);
    private SerialPort? _port;
    private Thread? _reader;
    private volatile bool _closing;
    private int _closedRaised;

    public SerialTransport(string portName, int baudRate = 115200)
    {
        _portName = portName;
        _baud = baudRate;
        _framer.LineReady += l => LineReceived?.Invoke(l);
        _framer.Overflow += n => LineOverflow?.Invoke(n);
    }

    public string Name => _portName;
    public bool IsOpen => _port is { IsOpen: true } && !_closing;

    public event Action<string>? LineReceived;
    public event Action<int>? LineOverflow;
    public event Action<TransportClosedInfo>? Closed;

    public Task OpenAsync(CancellationToken ct = default) => Task.Run(() =>
    {
        ct.ThrowIfCancellationRequested();
        var port = new SerialPort(_portName, _baud, Parity.None, 8, StopBits.One)
        {
            // Keep both control lines low: DTR/RTS toggling is what resets ESP32 boards into the bootloader.
            DtrEnable = false,
            RtsEnable = false,
            Handshake = Handshake.None,
            ReadTimeout = 250,
            WriteTimeout = 1000,
            ReadBufferSize = 32768,
            WriteBufferSize = 4096,
            Encoding = Encoding.UTF8,
        };
        try
        {
            port.Open();
            port.DiscardInBuffer();
        }
        catch
        {
            try { port.Dispose(); } catch { /* ignore */ }
            throw;
        }

        _port = port;
        _closing = false;
        _closedRaised = 0;
        _framer.Reset();
        _reader = new Thread(ReadLoop) { IsBackground = true, Name = $"Serial reader {_portName}" };
        _reader.Start();
    }, ct);

    private void ReadLoop()
    {
        var port = _port!;
        var buf = new byte[4096];
        string reason = "port closed";
        try
        {
            while (!_closing)
            {
                int n;
                try
                {
                    n = port.Read(buf, 0, buf.Length);
                }
                catch (TimeoutException)
                {
                    if (!port.IsOpen) { reason = "port disappeared"; break; }
                    continue;
                }
                if (n > 0) _framer.Feed(buf.AsSpan(0, n));
            }
        }
        catch (Exception ex) when (ex is IOException or InvalidOperationException or UnauthorizedAccessException or ObjectDisposedException or OperationCanceledException)
        {
            reason = _closing ? "closed" : $"port lost ({ex.GetType().Name}: {ex.Message})";
        }
        RaiseClosed(!_closing, reason);
    }

    private void RaiseClosed(bool unexpected, string reason)
    {
        if (Interlocked.Exchange(ref _closedRaised, 1) == 0)
            Closed?.Invoke(new TransportClosedInfo(unexpected, reason));
    }

    public async Task WriteLineAsync(string line, CancellationToken ct = default)
    {
        var port = _port;
        if (port is null || !port.IsOpen || _closing) throw new IOException("Port is not open");
        var bytes = Encoding.UTF8.GetBytes(line + "\n");
        await _writeLock.WaitAsync(ct).ConfigureAwait(false);
        try
        {
            await port.BaseStream.WriteAsync(bytes, ct).ConfigureAwait(false);
            await port.BaseStream.FlushAsync(ct).ConfigureAwait(false);
        }
        catch (Exception ex) when (ex is IOException or InvalidOperationException or UnauthorizedAccessException or TimeoutException or ObjectDisposedException)
        {
            if (!_closing) RaiseClosed(true, $"write failed ({ex.Message})");
            throw new IOException("Port write failed: " + ex.Message, ex);
        }
        finally
        {
            _writeLock.Release();
        }
    }

    public async Task CloseAsync()
    {
        var port = _port;
        if (port is null) return;
        _closing = true;
        _port = null;
        await Task.Run(() =>
        {
            // SerialPort.Close can block for a long time on a vanished USB device; do not let it hang the caller.
            var closer = Task.Run(() =>
            {
                try { port.Close(); } catch { /* ignore */ }
                try { port.Dispose(); } catch { /* ignore */ }
            });
            closer.Wait(1500);
            _reader?.Join(1000);
        }).ConfigureAwait(false);
        RaiseClosed(false, "closed");
    }

    public async ValueTask DisposeAsync() => await CloseAsync().ConfigureAwait(false);
}
