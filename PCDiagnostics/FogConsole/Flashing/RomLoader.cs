using System.Buffers.Binary;
using System.IO;
using System.Text;

namespace FogConsole.Flashing;

/// <summary>Command codes of the ESP32 ROM bootloader serial protocol (the same one esptool speaks).</summary>
public static class RomOp
{
    public const byte FlashBegin = 0x02;
    public const byte FlashData = 0x03;
    public const byte FlashEnd = 0x04;
    public const byte Sync = 0x08;
    public const byte WriteReg = 0x09;
    public const byte ReadReg = 0x0A;
    public const byte SpiSetParams = 0x0B;
    public const byte SpiAttach = 0x0D;
    public const byte SpiFlashMd5 = 0x13;
    public const byte GetSecurityInfo = 0x14;
}

/// <summary>The bootloader answered with an error status.</summary>
public sealed class RomException : Exception
{
    public RomException(byte op, byte error, string message) : base(message)
    {
        Op = op;
        Error = error;
    }

    public byte Op { get; }
    public byte Error { get; }

    public static string Describe(byte error) => error switch
    {
        0x05 => "the bootloader did not understand the request (invalid parameters or length)",
        0x06 => "the bootloader failed to carry out the request",
        0x07 => "the bootloader saw a corrupted packet (checksum mismatch)",
        0x08 => "flash write error (the data read back from flash did not match)",
        0x09 => "flash read error",
        0x0A => "flash read length error",
        0x0B => "decompression error in the bootloader",
        _ => $"error code 0x{error:X2}",
    };
}

/// <summary>The device did not answer in time.</summary>
public sealed class RomTimeoutException : Exception
{
    public RomTimeoutException(string message) : base(message) { }
}

/// <summary>The chip never answered the sync request: it is not in the ROM bootloader.</summary>
public sealed class BootloaderNotFoundException : Exception
{
    public BootloaderNotFoundException(string message, string? strayText) : base(message) => StrayText = strayText;

    /// <summary>What the device printed instead (its application log, for example).</summary>
    public string? StrayText { get; }
}

/// <summary>How the chip is put into download mode.</summary>
public enum BootEntry
{
    /// <summary>Use the DTR/RTS lines; the sequence matching the connected device is chosen automatically.</summary>
    Auto,

    /// <summary>The chip is already in the bootloader (BOOT held while resetting): do not touch the lines.</summary>
    AlreadyThere,
}

public sealed record RomReply(uint Value, byte[] Data);

/// <summary>
/// A minimal client for the ESP32 ROM bootloader: synchronise, read and write registers, write flash (compressed) and
/// ask for an MD5 of a flash region. Written against the protocol documented by Espressif (esptool).
/// </summary>
public sealed class RomLoader
{
    public const int FlashBlockSize = 0x400;
    public const int StatusBytes = 2;

    private static readonly TimeSpan DefaultTimeout = TimeSpan.FromSeconds(3);

    private readonly IFlashLink _link;
    private readonly SlipDecoder _decoder = new();
    private readonly Queue<byte[]> _frames = new();
    private readonly byte[] _readBuffer = new byte[4096];
    private readonly Action<string>? _log;

    public RomLoader(IFlashLink link, Action<string>? log = null)
    {
        _link = link;
        _log = log;
    }

    public string StrayText => _decoder.StrayText;

    // ---- entering the bootloader ----------------------------------------------------------------------------------

    /// <summary>Resets the chip into download mode and synchronises with the ROM. Throws <see cref="BootloaderNotFoundException"/>.</summary>
    public async Task ConnectAsync(BootEntry entry, CancellationToken ct, int attempts = 6)
    {
        _decoder.ClearStray();
        if (entry == BootEntry.AlreadyThere)
        {
            // nothing to reset: just try to talk
            if (await TrySyncAsync(ct).ConfigureAwait(false)) return;
            throw new BootloaderNotFoundException("The device does not answer. Put it into download mode by hand: hold the BOOT button, tap RESET, release BOOT.", StrayOrNull());
        }

        for (int attempt = 1; attempt <= attempts; attempt++)
        {
            ct.ThrowIfCancellationRequested();
            // the USB id says which wiring the board has; on a UART bridge alternate between a short and a long boot-strap delay
            try
            {
                if (_link.IsUsbJtagSerial) await ResetViaUsbJtagSerialAsync(ct).ConfigureAwait(false);
                else await ResetViaUartBridgeAsync(attempt % 2 == 1, ct).ConfigureAwait(false);

                if (await TrySyncAsync(ct).ConfigureAwait(false))
                {
                    _log?.Invoke($"Bootloader answered (attempt {attempt}).");
                    return;
                }
            }
            catch (Exception ex) when (ex is IOException or InvalidOperationException or UnauthorizedAccessException)
            {
                // the USB device went away with the reset and comes back as a new port handle: open it again and talk to the chip
                _log?.Invoke("The USB device re-appeared after the reset; reopening the port.");
                if (!await _link.TryReopenAsync(ct).ConfigureAwait(false)) throw;
                _decoder.Reset();
                if (await TrySyncAsync(ct).ConfigureAwait(false))
                {
                    _log?.Invoke($"Bootloader answered after reopening the port (attempt {attempt}).");
                    return;
                }
            }
            _log?.Invoke($"No answer from the bootloader (attempt {attempt} of {attempts}).");
        }

        throw new BootloaderNotFoundException(
            "The device did not enter its bootloader. Put it into download mode by hand: hold the BOOT button, tap RESET, release BOOT, then try again.",
            StrayOrNull());
    }

    private string? StrayOrNull()
    {
        var t = _decoder.StrayText.Trim();
        return t.Length == 0 ? null : t;
    }

    /// <summary>The reset sequence for the ESP32-C3/S3 USB-Serial/JTAG controller (DTR/RTS are decoded by the controller itself).</summary>
    private async Task ResetViaUsbJtagSerialAsync(CancellationToken ct)
    {
        _link.SetLines(false, false);
        await Task.Delay(100, ct).ConfigureAwait(false);
        _link.SetLines(true, false);          // IO0 low
        await Task.Delay(100, ct).ConfigureAwait(false);
        _link.SetLines(true, true);           // transient state the controller needs to see
        _link.SetLines(false, true);          // reset
        await Task.Delay(100, ct).ConfigureAwait(false);
        _link.SetLines(false, false);         // released: the chip starts in download mode
        await Task.Delay(50, ct).ConfigureAwait(false);
    }

    /// <summary>The classic auto-reset circuit of USB-UART bridges: DTR = IO0, RTS = EN.</summary>
    private async Task ResetViaUartBridgeAsync(bool normalDelay, CancellationToken ct)
    {
        _link.SetLines(false, false);
        _link.SetLines(false, true);          // EN low, IO0 high
        await Task.Delay(100, ct).ConfigureAwait(false);
        _link.SetLines(true, false);          // EN high, IO0 low
        await Task.Delay(normalDelay ? 50 : 450, ct).ConfigureAwait(false);
        _link.SetLines(false, false);         // IO0 high
    }

    private async Task<bool> TrySyncAsync(CancellationToken ct)
    {
        var packet = new byte[36];
        packet[0] = 0x07; packet[1] = 0x07; packet[2] = 0x12; packet[3] = 0x20;
        Array.Fill(packet, (byte)0x55, 4, 32);

        for (int i = 0; i < 5; i++)
        {
            ct.ThrowIfCancellationRequested();
            DiscardPending();
            try
            {
                await CommandAsync(RomOp.Sync, packet, 0, TimeSpan.FromMilliseconds(120), ct).ConfigureAwait(false);
            }
            catch (RomTimeoutException)
            {
                continue;
            }
            // the ROM sends several sync answers; swallow them so they are not mistaken for later replies
            await DrainAsync(TimeSpan.FromMilliseconds(120), ct).ConfigureAwait(false);
            return true;
        }
        return false;
    }

    private void DiscardPending()
    {
        _link.DiscardInput();
        _frames.Clear();
        _decoder.Reset();
    }

    private async Task DrainAsync(TimeSpan quiet, CancellationToken ct)
    {
        while (true)
        {
            int n = await _link.ReadAsync(_readBuffer, quiet, ct).ConfigureAwait(false);
            if (n == 0) break;
        }
        _frames.Clear();
        _decoder.Reset();
    }

    // ---- request / response ---------------------------------------------------------------------------------------

    /// <summary>Sends a command and returns the matching response (data still contains the status bytes).</summary>
    public async Task<RomReply> CommandAsync(byte op, ReadOnlyMemory<byte> payload, uint checksum, TimeSpan timeout, CancellationToken ct)
    {
        var packet = new byte[8 + payload.Length];
        packet[0] = 0x00;
        packet[1] = op;
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(2), (ushort)payload.Length);
        BinaryPrimitives.WriteUInt32LittleEndian(packet.AsSpan(4), checksum);
        payload.Span.CopyTo(packet.AsSpan(8));
        await _link.WriteAsync(Slip.Encode(packet), ct).ConfigureAwait(false);

        var deadline = DateTime.UtcNow + timeout;
        while (true)
        {
            while (_frames.Count > 0)
            {
                var f = _frames.Dequeue();
                if (f.Length < 8 || f[0] != 0x01 || f[1] != op) continue;   // stale or foreign frame
                return new RomReply(BinaryPrimitives.ReadUInt32LittleEndian(f.AsSpan(4)), f.AsSpan(8).ToArray());
            }

            var left = deadline - DateTime.UtcNow;
            if (left <= TimeSpan.Zero) throw new RomTimeoutException($"No answer to command 0x{op:X2} within {timeout.TotalSeconds:0.#} s.");
            int n = await _link.ReadAsync(_readBuffer, left, ct).ConfigureAwait(false);
            if (n > 0) _decoder.Feed(_readBuffer.AsSpan(0, n), _frames.Enqueue);
        }
    }

    /// <summary>Like <see cref="CommandAsync"/>, but checks the status bytes. The returned data has them removed.</summary>
    public async Task<RomReply> CheckAsync(byte op, ReadOnlyMemory<byte> payload, uint checksum, TimeSpan? timeout, CancellationToken ct)
    {
        var reply = await CommandAsync(op, payload, checksum, timeout ?? DefaultTimeout, ct).ConfigureAwait(false);
        if (reply.Data.Length < StatusBytes) throw new RomException(op, 0xFF, $"Short answer to command 0x{op:X2}.");
        var status = reply.Data.AsSpan(reply.Data.Length - StatusBytes);
        if (status[0] != 0)
            throw new RomException(op, status[1], $"Command 0x{op:X2} failed: {RomException.Describe(status[1])}.");
        return new RomReply(reply.Value, reply.Data.AsSpan(0, reply.Data.Length - StatusBytes).ToArray());
    }

    // ---- registers and identity -----------------------------------------------------------------------------------

    public async Task<uint> ReadRegAsync(uint address, CancellationToken ct)
    {
        var payload = new byte[4];
        BinaryPrimitives.WriteUInt32LittleEndian(payload, address);
        return (await CheckAsync(RomOp.ReadReg, payload, 0, null, ct).ConfigureAwait(false)).Value;
    }

    public async Task WriteRegAsync(uint address, uint value, CancellationToken ct, uint mask = 0xFFFFFFFF, uint delayUs = 0, TimeSpan? timeout = null)
    {
        var payload = new byte[16];
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(0), address);
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(4), value);
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(8), mask);
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(12), delayUs);
        await CheckAsync(RomOp.WriteReg, payload, 0, timeout, ct).ConfigureAwait(false);
    }

    /// <summary>The chip id the ROM reports (5 = ESP32-C3), or null when the ROM does not know the request.</summary>
    public async Task<uint?> GetChipIdAsync(CancellationToken ct)
    {
        try
        {
            var r = await CheckAsync(RomOp.GetSecurityInfo, ReadOnlyMemory<byte>.Empty, 0, null, ct).ConfigureAwait(false);
            // flags(4) flash_crypt_cnt(1) key_purposes(7) chip_id(4) eco_version(4)
            if (r.Data.Length >= 16) return BinaryPrimitives.ReadUInt32LittleEndian(r.Data.AsSpan(12));
        }
        catch (RomException)
        {
            // older ROM: fall back to the chip magic register
        }
        uint magic = await ReadRegAsync(ChipMagicRegister, ct).ConfigureAwait(false);
        return Array.IndexOf(Esp32C3.MagicValues, magic) >= 0 ? Esp32C3.ChipId : null;
    }

    public const uint ChipMagicRegister = 0x4000_1000;

    /// <summary>The factory MAC address burnt into the chip (the Wi-Fi station MAC of the application).</summary>
    public async Task<string> ReadMacAsync(CancellationToken ct)
    {
        uint mac0 = await ReadRegAsync(Esp32C3.MacRegister, ct).ConfigureAwait(false);
        uint mac1 = await ReadRegAsync(Esp32C3.MacRegister + 4, ct).ConfigureAwait(false);
        return string.Join(':', new[]
        {
            (byte)(mac1 >> 8), (byte)mac1, (byte)(mac0 >> 24), (byte)(mac0 >> 16), (byte)(mac0 >> 8), (byte)mac0,
        }.Select(b => b.ToString("X2")));
    }

    // ---- flash ----------------------------------------------------------------------------------------------------

    public async Task SpiAttachAsync(CancellationToken ct)
    {
        await CheckAsync(RomOp.SpiAttach, new byte[8], 0, null, ct).ConfigureAwait(false);
    }

    public async Task SetFlashParametersAsync(uint flashSize, CancellationToken ct)
    {
        var p = new byte[24];
        BinaryPrimitives.WriteUInt32LittleEndian(p.AsSpan(0), 0);                // flash id
        BinaryPrimitives.WriteUInt32LittleEndian(p.AsSpan(4), flashSize);
        BinaryPrimitives.WriteUInt32LittleEndian(p.AsSpan(8), 64 * 1024);         // block size
        BinaryPrimitives.WriteUInt32LittleEndian(p.AsSpan(12), 4 * 1024);         // sector size
        BinaryPrimitives.WriteUInt32LittleEndian(p.AsSpan(16), 256);              // page size
        BinaryPrimitives.WriteUInt32LittleEndian(p.AsSpan(20), 0xFFFF);           // status mask
        await CheckAsync(RomOp.SpiSetParams, p, 0, null, ct).ConfigureAwait(false);
    }

    /// <summary>
    /// Starts a write of <paramref name="size"/> bytes at <paramref name="offset"/>; the ROM erases that region first.
    /// The packet is the one esptool sends to the ROM loader of this chip (checked against its --trace output).
    /// </summary>
    public async Task FlashBeginAsync(uint size, uint offset, CancellationToken ct)
    {
        uint blocks = (size + FlashBlockSize - 1) / FlashBlockSize;
        var p = new byte[20];
        BinaryPrimitives.WriteUInt32LittleEndian(p.AsSpan(0), size);              // size to erase
        BinaryPrimitives.WriteUInt32LittleEndian(p.AsSpan(4), blocks);
        BinaryPrimitives.WriteUInt32LittleEndian(p.AsSpan(8), FlashBlockSize);
        BinaryPrimitives.WriteUInt32LittleEndian(p.AsSpan(12), offset);
        BinaryPrimitives.WriteUInt32LittleEndian(p.AsSpan(16), 0);                // not encrypted
        // erasing takes a while (esptool allows 40 s per MB, at least 3 s)
        var timeout = TimeSpan.FromSeconds(Math.Max(3, 40.0 * size / 1e6));
        await CheckAsync(RomOp.FlashBegin, p, 0, timeout, ct).ConfigureAwait(false);
    }

    /// <summary>Sends one block (padded with 0xFF to <see cref="FlashBlockSize"/> bytes).</summary>
    public async Task FlashDataAsync(ReadOnlyMemory<byte> block, uint sequence, CancellationToken ct)
    {
        var padded = new byte[FlashBlockSize];
        Array.Fill(padded, (byte)0xFF);
        block.Span.CopyTo(padded);

        var p = new byte[16 + FlashBlockSize];
        BinaryPrimitives.WriteUInt32LittleEndian(p.AsSpan(0), FlashBlockSize);
        BinaryPrimitives.WriteUInt32LittleEndian(p.AsSpan(4), sequence);
        padded.CopyTo(p, 16);
        await CheckAsync(RomOp.FlashData, p, Checksum(padded), TimeSpan.FromSeconds(5), ct).ConfigureAwait(false);
    }

    /// <summary>MD5 (lower-case hex) of a flash region, calculated by the chip.</summary>
    public async Task<string> FlashMd5Async(uint offset, uint length, CancellationToken ct)
    {
        var p = new byte[16];
        BinaryPrimitives.WriteUInt32LittleEndian(p.AsSpan(0), offset);
        BinaryPrimitives.WriteUInt32LittleEndian(p.AsSpan(4), length);
        var timeout = TimeSpan.FromSeconds(Math.Max(3, 8.0 * length / 1e6));
        var r = await CheckAsync(RomOp.SpiFlashMd5, p, 0, timeout, ct).ConfigureAwait(false);
        return r.Data.Length switch
        {
            32 => Encoding.ASCII.GetString(r.Data).ToLowerInvariant(),
            16 => Convert.ToHexString(r.Data).ToLowerInvariant(),
            _ => throw new RomException(RomOp.SpiFlashMd5, 0xFF, "The bootloader returned an MD5 of unexpected size."),
        };
    }

    /// <summary>The checksum the ROM wants in the header of a data packet: XOR of all bytes, seeded with 0xEF.</summary>
    public static uint Checksum(ReadOnlySpan<byte> data)
    {
        byte x = 0xEF;
        foreach (byte b in data) x ^= b;
        return x;
    }

    // ---- leaving --------------------------------------------------------------------------------------------------

    /// <summary>Resets the chip so that it starts the application. Control lines first (like esptool), plus the watchdog as a second way.</summary>
    public async Task RestartApplicationAsync(CancellationToken ct)
    {
        try
        {
            // RTC watchdog reset through the bootloader: works even when the control lines do not reach the chip
            var quick = TimeSpan.FromMilliseconds(400);          // the chip resets a few ms after the third write
            await WriteRegAsync(Esp32C3.WdtProtect, Esp32C3.WdtKey, ct, timeout: quick).ConfigureAwait(false);
            await WriteRegAsync(Esp32C3.WdtConfig1, 5000, ct, timeout: quick).ConfigureAwait(false);
            await WriteRegAsync(Esp32C3.WdtConfig0, (1u << 31) | (5u << 28) | (1u << 8) | 2u, ct, timeout: quick).ConfigureAwait(false);
            await WriteRegAsync(Esp32C3.WdtProtect, 0, ct, timeout: quick).ConfigureAwait(false);
        }
        catch (Exception ex) when (ex is RomTimeoutException or RomException or IOException)
        {
            // the chip may already be gone; the control lines below are the main way anyway
        }

        try
        {
            _link.SetLines(false, true);      // EN low
            await Task.Delay(200, ct).ConfigureAwait(false);
            _link.SetLines(false, false);     // EN high, IO0 high: boots the application
            await Task.Delay(100, ct).ConfigureAwait(false);
        }
        catch (Exception ex) when (ex is IOException or InvalidOperationException or UnauthorizedAccessException)
        {
            // the port may have vanished with the reset
        }
    }
}

/// <summary>The constants of the ESP32-C3 that the flasher needs.</summary>
public static class Esp32C3
{
    public const uint ChipId = 5;
    public const string Name = "ESP32-C3";

    /// <summary>Values of the chip-magic register of the C3 revisions.</summary>
    public static readonly uint[] MagicValues = { 0x6921506F, 0x1B31506F, 0x4881606F, 0x4361606F };

    public const uint MacRegister = 0x6000_8844;
    public const uint WdtConfig0 = 0x6000_8090;
    public const uint WdtConfig1 = 0x6000_8094;
    public const uint WdtProtect = 0x6000_80A8;
    public const uint WdtKey = 0x50D8_3AA1;

    /// <summary>Flash size assumed for the parameter request (the XIAO ESP32-C3 has 4 MB).</summary>
    public const uint FlashBytes = 4 * 1024 * 1024;

    public static string ChipName(uint chipId) => chipId switch
    {
        0 => "ESP32",
        2 => "ESP32-S2",
        5 => "ESP32-C3",
        9 => "ESP32-S3",
        12 => "ESP32-C2",
        13 => "ESP32-C6",
        16 => "ESP32-H2",
        18 => "ESP32-P4",
        _ => $"chip id {chipId}",
    };
}
