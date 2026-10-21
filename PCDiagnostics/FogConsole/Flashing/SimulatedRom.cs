using System.Buffers.Binary;
using System.IO;
using System.Security.Cryptography;
using System.Text;

namespace FogConsole.Flashing;

public sealed class SimulatedRomOptions
{
    /// <summary>When false the DTR/RTS sequence has no effect and the device keeps running its application.</summary>
    public bool EntersBootloader { get; init; } = true;

    public bool IsUsbJtagSerial { get; init; } = true;
    public uint ChipId { get; init; } = Esp32C3.ChipId;
    public byte[] Mac { get; init; } = { 0x40, 0x4C, 0xCA, 0x5B, 0x12, 0x34 };

    /// <summary>Simulated flash write speed in bytes per second (0 = as fast as possible, used by tests).</summary>
    public int BytesPerSecond { get; init; }

    /// <summary>Flips one bit in everything that is written, so that the verification fails.</summary>
    public bool CorruptWrites { get; init; }

    /// <summary>Behaves as if the cable was pulled after n data blocks (-1 = never).</summary>
    public int DropLinkAtBlock { get; init; } = -1;

    /// <summary>Initial flash contents; null = erased (0xFF).</summary>
    public byte[]? InitialFlash { get; init; }

    /// <summary>The USB device vanishes when the chip resets into the bootloader: every call fails until the port is reopened.</summary>
    public bool ReenumeratesOnReset { get; init; }

    /// <summary>A reopen after such a re-enumeration fails (the device never comes back).</summary>
    public bool NeverComesBack { get; init; }
}

/// <summary>
/// An ESP32-C3 in its ROM bootloader, in memory: speaks the same serial protocol as the real chip (SLIP framed commands
/// for registers, compressed flash writes and MD5). Used by the offline demo and by the tests.
/// </summary>
public sealed class SimulatedRom : IFlashLink
{
    public const int FlashSize = 4 * 1024 * 1024;
    private const uint SectorSize = 4096;

    private readonly SimulatedRomOptions _o;
    private readonly SlipDecoder _decoder = new();
    private readonly object _gate = new();
    private readonly Queue<byte> _out = new();
    private readonly SemaphoreSlim _signal = new(0);

    private bool _dtr, _rts;
    private bool _sawIo0Low;
    private bool _sawReset;
    private bool _banner;
    private bool _dead;
    private bool _vanished;

    private WriteJob? _write;
    private int _blocksReceived;

    private sealed class WriteJob
    {
        public uint Offset;
        public int Blocks, Seen;
    }

    public SimulatedRom(SimulatedRomOptions? options = null)
    {
        _o = options ?? new SimulatedRomOptions();
        Flash = new byte[FlashSize];
        Array.Fill(Flash, (byte)0xFF);
        if (_o.InitialFlash is { } init) Array.Copy(init, Flash, Math.Min(init.Length, Flash.Length));
    }

    public string Name => "Simulated ESP32-C3";
    public int Reopens { get; private set; }
    public bool IsUsbJtagSerial => _o.IsUsbJtagSerial;

    public byte[] Flash { get; }

    /// <summary>True while the chip is in its ROM bootloader (after a successful reset sequence).</summary>
    public bool InBootloader { get; private set; }

    /// <summary>The chip was reset into the application (control lines or watchdog).</summary>
    public int ApplicationStarts { get; private set; }

    public int WatchdogResets { get; private set; }
    public int BlocksWritten { get; private set; }
    public int BytesReceivedForFlash { get; private set; }
    public List<(byte Op, int PayloadLength)> Commands { get; } = new();

    /// <summary>The payload of every command received, in order (parallel to <see cref="Commands"/>).</summary>
    public List<byte[]> Payloads { get; } = new();

    // ---- IFlashLink ----------------------------------------------------------------------------------------------

    public async Task WriteAsync(ReadOnlyMemory<byte> data, CancellationToken ct)
    {
        ThrowIfDead();
        var frames = new List<byte[]>();
        lock (_gate) _decoder.Feed(data.Span, frames.Add);
        foreach (var frame in frames)
        {
            ct.ThrowIfCancellationRequested();
            await HandleFrameAsync(frame, ct).ConfigureAwait(false);
        }
    }

    public async Task<int> ReadAsync(Memory<byte> buffer, TimeSpan timeout, CancellationToken ct)
    {
        ThrowIfDead();
        var deadline = DateTime.UtcNow + timeout;
        while (true)
        {
            ct.ThrowIfCancellationRequested();
            lock (_gate)
            {
                if (_out.Count > 0)
                {
                    int n = Math.Min(buffer.Length, _out.Count);
                    for (int i = 0; i < n; i++) buffer.Span[i] = _out.Dequeue();
                    return n;
                }
                if (!InBootloader && !_banner)
                {
                    // the application is still running and chatting
                    _banner = true;
                    foreach (byte b in "{\"t\":\"log\",\"lvl\":\"info\",\"msg\":\"app running\"}\r\n"u8) _out.Enqueue(b);
                    continue;
                }
            }
            var left = deadline - DateTime.UtcNow;
            if (left <= TimeSpan.Zero) return 0;
            await _signal.WaitAsync(left, ct).ConfigureAwait(false);
        }
    }

    public void DiscardInput()
    {
        lock (_gate) _out.Clear();
    }

    public void SetLines(bool dtr, bool rts)
    {
        ThrowIfDead();
        lock (_gate)
        {
            bool wasRts = _rts;
            _dtr = dtr;
            _rts = rts;

            if (_o.IsUsbJtagSerial)
            {
                // the USB-Serial/JTAG controller latches "IO0 low" from the (DTR=1, RTS=0) state and resets on RTS
                if (dtr && !rts) _sawIo0Low = true;
                if (!dtr && rts) _sawReset = true;
                if (!dtr && !rts && _sawReset)
                {
                    Reset(intoBootloader: _sawIo0Low);
                    _sawIo0Low = _sawReset = false;
                }
                else if (!dtr && !rts && !_sawReset)
                {
                    _sawIo0Low = false;
                }
            }
            else
            {
                // classic wiring: the chip samples IO0 (DTR) the moment EN (RTS) is released
                if (rts) _sawReset = true;
                if (wasRts && !rts)
                {
                    Reset(intoBootloader: dtr);
                    _sawReset = false;
                }
            }
        }
    }

    public void Dispose()
    {
    }

    // ---- chip behaviour -------------------------------------------------------------------------------------------

    private void Reset(bool intoBootloader)
    {
        _decoder.Reset();
        _out.Clear();
        _write = null;
        if (intoBootloader && _o.EntersBootloader)
        {
            InBootloader = true;
            if (_o.ReenumeratesOnReset && Reopens == 0) _vanished = true;
            foreach (byte b in "ESP-ROM:esp32c3-api1-20210207\r\nBuild:Feb  7 2021\r\nrst:0x15 (USB_UART_CHIP_RESET),boot:0x5 (DOWNLOAD(USB/UART0/1))\r\nwaiting for download\r\n"u8)
                _out.Enqueue(b);
            _signal.Release();
        }
        else
        {
            InBootloader = false;
            _banner = false;
            ApplicationStarts++;
        }
    }

    private void ThrowIfDead()
    {
        if (_dead) throw new IOException("The simulated USB device was unplugged.");
        if (_vanished) throw new IOException("The simulated USB device re-enumerated; reopen the port.");
    }

    public Task<bool> TryReopenAsync(CancellationToken ct)
    {
        if (_dead || _o.NeverComesBack) return Task.FromResult(false);
        if (_vanished) Reopens++;
        _vanished = false;
        lock (_gate) _out.Clear();
        return Task.FromResult(true);
    }

    private async Task HandleFrameAsync(byte[] frame, CancellationToken ct)
    {
        if (!InBootloader || frame.Length < 8 || frame[0] != 0x00) return;
        byte op = frame[1];
        int size = BinaryPrimitives.ReadUInt16LittleEndian(frame.AsSpan(2));
        uint chk = BinaryPrimitives.ReadUInt32LittleEndian(frame.AsSpan(4));
        if (frame.Length - 8 < size) return;
        var payload = frame.AsSpan(8, size).ToArray();
        lock (_gate)
        {
            Commands.Add((op, payload.Length));
            Payloads.Add(payload);
        }

        switch (op)
        {
            case RomOp.Sync:
                // the real ROM answers a sync with a burst of eight identical replies (esptool reads seven more after the first)
                for (int i = 0; i < 8; i++) Reply(op, 0x20120707u);
                break;
            case RomOp.ReadReg:
                Reply(op, ReadRegister(BinaryPrimitives.ReadUInt32LittleEndian(payload)));
                break;
            case RomOp.WriteReg:
                WriteRegister(BinaryPrimitives.ReadUInt32LittleEndian(payload), BinaryPrimitives.ReadUInt32LittleEndian(payload.AsSpan(4)));
                Reply(op, 0);
                break;
            case RomOp.GetSecurityInfo:
                {
                    var d = new byte[20];
                    BinaryPrimitives.WriteUInt32LittleEndian(d.AsSpan(12), _o.ChipId);
                    Reply(op, 0, d);
                    break;
                }
            case RomOp.SpiAttach:
            case RomOp.SpiSetParams:
                Reply(op, 0);
                break;
            case RomOp.FlashBegin:
                BeginWrite(op, payload);
                break;
            case RomOp.FlashData:
                await WriteBlockAsync(op, payload, chk, ct).ConfigureAwait(false);
                break;
            case RomOp.FlashEnd:
                {
                    bool stay = payload.Length >= 4 && BinaryPrimitives.ReadUInt32LittleEndian(payload) != 0;
                    Reply(op, 0);
                    if (!stay)
                    {
                        lock (_gate) Reset(intoBootloader: false);
                    }
                    break;
                }
            case RomOp.SpiFlashMd5:
                {
                    uint addr = BinaryPrimitives.ReadUInt32LittleEndian(payload);
                    uint len = BinaryPrimitives.ReadUInt32LittleEndian(payload.AsSpan(4));
                    if ((long)addr + len > Flash.Length) { Error(op, 0x05); break; }
                    var md5 = Convert.ToHexString(MD5.HashData(Flash.AsSpan((int)addr, (int)len))).ToLowerInvariant();
                    Reply(op, 0, Encoding.ASCII.GetBytes(md5));
                    break;
                }
            default:
                Error(op, 0x05);
                break;
        }
    }

    private uint ReadRegister(uint address)
    {
        if (address == RomLoader.ChipMagicRegister) return Esp32C3.MagicValues[0];
        if (address == Esp32C3.MacRegister)
            return ((uint)_o.Mac[2] << 24) | ((uint)_o.Mac[3] << 16) | ((uint)_o.Mac[4] << 8) | _o.Mac[5];
        if (address == Esp32C3.MacRegister + 4)
            return ((uint)_o.Mac[0] << 8) | _o.Mac[1];
        return 0;
    }

    private void WriteRegister(uint address, uint value)
    {
        // enabling the RTC watchdog (bit 31 of CONFIG0) resets the chip into the application a moment later
        if (address == Esp32C3.WdtConfig0 && (value & 0x8000_0000u) != 0)
        {
            WatchdogResets++;
            lock (_gate) Reset(intoBootloader: false);
        }
    }

    private void BeginWrite(byte op, byte[] p)
    {
        if (p.Length < 16) { Error(op, 0x05); return; }
        uint eraseSize = BinaryPrimitives.ReadUInt32LittleEndian(p.AsSpan(0));
        int blocks = (int)BinaryPrimitives.ReadUInt32LittleEndian(p.AsSpan(4));
        uint blockSize = BinaryPrimitives.ReadUInt32LittleEndian(p.AsSpan(8));
        uint offset = BinaryPrimitives.ReadUInt32LittleEndian(p.AsSpan(12));
        if (blockSize != RomLoader.FlashBlockSize || offset % SectorSize != 0 || (long)offset + eraseSize > Flash.Length || blocks <= 0)
        {
            Error(op, 0x05);
            return;
        }

        // erase every sector the new data touches
        uint eraseEnd = (uint)Math.Min((long)Flash.Length, ((long)offset + eraseSize + SectorSize - 1) / SectorSize * SectorSize);
        Array.Fill(Flash, (byte)0xFF, (int)offset, (int)(eraseEnd - offset));

        _write = new WriteJob { Offset = offset, Blocks = blocks };
        Reply(op, 0);
    }

    private async Task WriteBlockAsync(byte op, byte[] p, uint chk, CancellationToken ct)
    {
        if (p.Length < 16) { Error(op, 0x05); return; }
        int len = (int)BinaryPrimitives.ReadUInt32LittleEndian(p.AsSpan(0));
        int seq = (int)BinaryPrimitives.ReadUInt32LittleEndian(p.AsSpan(4));
        if (len != p.Length - 16 || len > RomLoader.FlashBlockSize) { Error(op, 0x05); return; }
        var data = p[16..];
        if (RomLoader.Checksum(data) != chk) { Error(op, 0x07); return; }

        if (_o.DropLinkAtBlock >= 0 && _blocksReceived >= _o.DropLinkAtBlock)
        {
            _dead = true;
            ThrowIfDead();
        }
        _blocksReceived++;
        BytesReceivedForFlash += len;

        if (_o.BytesPerSecond > 0) await Task.Delay(TimeSpan.FromSeconds((double)len / _o.BytesPerSecond), ct).ConfigureAwait(false);

        var w = _write;
        if (w is null || seq != w.Seen || w.Seen >= w.Blocks) { Error(op, 0x05); return; }
        long at = w.Offset + (long)seq * RomLoader.FlashBlockSize;
        if (at + len > Flash.Length) { Error(op, 0x05); return; }
        Array.Copy(data, 0, Flash, at, len);
        w.Seen++;
        BlocksWritten++;

        if (_o.CorruptWrites && w.Seen == w.Blocks) Flash[w.Offset + 7] ^= 0x01;
        Reply(op, 0);
    }

    // ---- replies -------------------------------------------------------------------------------------------------

    private void Reply(byte op, uint value, byte[]? data = null)
    {
        data ??= Array.Empty<byte>();
        var packet = new byte[8 + data.Length + RomLoader.StatusBytes];
        packet[0] = 0x01;
        packet[1] = op;
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(2), (ushort)(data.Length + RomLoader.StatusBytes));
        BinaryPrimitives.WriteUInt32LittleEndian(packet.AsSpan(4), value);
        data.CopyTo(packet, 8);
        Enqueue(Slip.Encode(packet));
    }

    private void Error(byte op, byte code)
    {
        var packet = new byte[8 + RomLoader.StatusBytes];
        packet[0] = 0x01;
        packet[1] = op;
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(2), RomLoader.StatusBytes);
        packet[8] = 1;
        packet[9] = code;
        Enqueue(Slip.Encode(packet));
    }

    private void Enqueue(byte[] bytes)
    {
        lock (_gate)
        {
            foreach (byte b in bytes) _out.Enqueue(b);
        }
        _signal.Release();
    }
}
