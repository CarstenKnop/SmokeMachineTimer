using System.IO;
using System.IO.Ports;

namespace FogConsole.Flashing;

/// <summary>The byte pipe to a device in (or about to enter) its ROM bootloader, plus its DTR / RTS lines.</summary>
public interface IFlashLink : IDisposable
{
    string Name { get; }

    /// <summary>True when the device is an ESP32 with the built-in USB-Serial/JTAG controller (USB id 303A:1001).</summary>
    bool IsUsbJtagSerial { get; }

    Task WriteAsync(ReadOnlyMemory<byte> data, CancellationToken ct);

    /// <summary>Reads what is available, waiting at most <paramref name="timeout"/>. Returns 0 when nothing arrived.</summary>
    Task<int> ReadAsync(Memory<byte> buffer, TimeSpan timeout, CancellationToken ct);

    void DiscardInput();

    /// <summary>
    /// After a reset the USB device may disappear and come back (the port is re-enumerated). Closes the dead handle, waits for the port to
    /// re-appear and opens it again. Returns false when it does not come back. Links without a real port return false.
    /// </summary>
    Task<bool> TryReopenAsync(CancellationToken ct);

    /// <summary>Sets the control lines. true = asserted. On the ESP32 boards DTR drives IO0 (boot mode), RTS drives EN (reset).</summary>
    void SetLines(bool dtr, bool rts);
}

/// <summary>A real serial port (Windows COM port, USB CDC or UART bridge).</summary>
public sealed class SerialFlashLink : IFlashLink
{
    private readonly int _baud;
    private SerialPort _port;

    private SerialFlashLink(SerialPort port, string name, bool usbJtag, int baud)
    {
        _port = port;
        Name = name;
        IsUsbJtagSerial = usbJtag;
        _baud = baud;
    }

    public string Name { get; }
    public bool IsUsbJtagSerial { get; }

    /// <summary>Opens the port with both control lines released (opening must not reset the chip by itself).</summary>
    public static SerialFlashLink Open(string portName, bool usbJtag, int baudRate = 115200) =>
        new(OpenPort(portName, baudRate), portName, usbJtag, baudRate);

    private static SerialPort OpenPort(string portName, int baudRate)
    {
        var port = new SerialPort(portName, baudRate, Parity.None, 8, StopBits.One)
        {
            DtrEnable = false,
            RtsEnable = false,
            Handshake = Handshake.None,
            ReadTimeout = 100,
            WriteTimeout = 3000,
            ReadBufferSize = 65536,
            WriteBufferSize = 8192,
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
        return port;
    }

    public async Task<bool> TryReopenAsync(CancellationToken ct)
    {
        CloseQuietly(_port);
        for (int i = 0; i < 40; i++)
        {
            ct.ThrowIfCancellationRequested();
            bool listed;
            try { listed = SerialPort.GetPortNames().Contains(Name, StringComparer.OrdinalIgnoreCase); }
            catch (Exception ex) when (ex is IOException or InvalidOperationException) { listed = false; }
            if (listed)
            {
                try
                {
                    _port = OpenPort(Name, _baud);
                    return true;
                }
                catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or InvalidOperationException)
                {
                    // the driver may still be setting the port up
                }
            }
            await Task.Delay(150, ct).ConfigureAwait(false);
        }
        return false;
    }

    private static void CloseQuietly(SerialPort port)
    {
        // Close() can hang for a while on a USB device that has just reset and vanished
        var closer = Task.Run(() =>
        {
            try { port.Close(); } catch { /* ignore */ }
            try { port.Dispose(); } catch { /* ignore */ }
        });
        closer.Wait(1500);
    }

    public async Task WriteAsync(ReadOnlyMemory<byte> data, CancellationToken ct)
    {
        await _port.BaseStream.WriteAsync(data, ct).ConfigureAwait(false);
        await _port.BaseStream.FlushAsync(ct).ConfigureAwait(false);
    }

    public async Task<int> ReadAsync(Memory<byte> buffer, TimeSpan timeout, CancellationToken ct)
    {
        var deadline = DateTime.UtcNow + timeout;
        var array = new byte[buffer.Length];
        while (true)
        {
            ct.ThrowIfCancellationRequested();
            var left = deadline - DateTime.UtcNow;
            if (left <= TimeSpan.Zero) return 0;
            int slice = (int)Math.Clamp(left.TotalMilliseconds, 1, 40);
            int n = await Task.Run(() =>
            {
                _port.ReadTimeout = slice;
                try { return _port.Read(array, 0, array.Length); }
                catch (TimeoutException) { return 0; }
            }, CancellationToken.None).ConfigureAwait(false);
            if (n > 0)
            {
                array.AsMemory(0, n).CopyTo(buffer);
                return n;
            }
        }
    }

    public void DiscardInput()
    {
        try { _port.DiscardInBuffer(); }
        catch (Exception ex) when (ex is IOException or InvalidOperationException) { /* nothing to discard on a dead port */ }
    }

    public void SetLines(bool dtr, bool rts)
    {
        // Windows' CDC driver only forwards a DTR change together with an RTS write, so both are always written.
        _port.DtrEnable = dtr;
        _port.RtsEnable = rts;
    }

    public void Dispose() => CloseQuietly(_port);
}
