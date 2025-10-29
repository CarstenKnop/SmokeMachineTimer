using System;
using System.Collections.Generic;
using System.IO.Ports;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using SmokeMachineDiagnostics.Protocol;
using System.Buffers.Binary;

namespace SmokeMachineDiagnostics.Services;

public sealed class SerialClient : IDisposable
{
    private readonly object _sync = new();
    private readonly List<byte> _buffer = new();
    private readonly Dictionary<ushort, TaskCompletionSource<DebugProtocol.Packet>> _pendingRequests = new();

    private SerialPort? _port;
    private CancellationTokenSource? _readCts;
    private Task? _readerTask;
    private byte _nextSequence = 1;
    private ushort _nextRequestId = 0;
    private ReliableProtocol.TransportStats _stats;

    public ReliableProtocol.TransportStats Stats => _stats;

    public event Action<DebugProtocol.Packet>? PacketReceived;
    public event Action<string>? Log;

    public bool IsConnected => _port?.IsOpen == true;

    public async Task ConnectAsync(string portName, int baudRate, CancellationToken cancellationToken = default)
    {
        Disconnect();
        var port = new SerialPort(portName, baudRate)
        {
            DataBits = 8,
            StopBits = StopBits.One,
            Parity = Parity.None,
            Handshake = Handshake.None,
            ReadTimeout = -1,
            WriteTimeout = -1
        };
        port.Open();
        port.DiscardInBuffer();
        port.DiscardOutBuffer();

        _port = port;
    _stats.Reset();
        _buffer.Clear();
        _readCts = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        _readerTask = Task.Run(() => ReaderLoopAsync(_readCts.Token), cancellationToken);
        Log?.Invoke($"Connected to {portName}");
    }

    public void Disconnect()
    {
        lock (_sync)
        {
            if (_readCts != null)
            {
                _readCts.Cancel();
                _readCts = null;
            }
        }
        try
        {
            _readerTask?.Wait(200);
        }
        catch
        {
            // ignore
        }
        _readerTask = null;

        if (_port != null)
        {
            try
            {
                _port.Close();
            }
            catch
            {
                // ignore
            }
            _port.Dispose();
            _port = null;
        }

        lock (_sync)
        {
            foreach (var pending in _pendingRequests.Values)
            {
                pending.TrySetCanceled();
            }
            _pendingRequests.Clear();
            _buffer.Clear();
        }
    }

    public async Task<DebugProtocol.Packet> SendAsync(DebugProtocol.Command command, ReadOnlyMemory<byte> payload, CancellationToken cancellationToken = default)
    {
    ushort requestId = AllocateRequestId();
        var packet = DebugProtocol.CreateRequest(command, payload.Span, requestId);
        return await SendAsync(packet, cancellationToken).ConfigureAwait(false);
    }

    public async Task<DebugProtocol.Packet> SendAsync(DebugProtocol.Packet packet, CancellationToken cancellationToken = default)
    {
        if (!DebugProtocol.IsValid(packet))
        {
            throw new ArgumentException("Packet invalid", nameof(packet));
        }

        var tcs = new TaskCompletionSource<DebugProtocol.Packet>(TaskCreationOptions.RunContinuationsAsynchronously);
        lock (_sync)
        {
            if (_port == null)
            {
                throw new InvalidOperationException("Serial port is not connected.");
            }
            _pendingRequests[packet.RequestId] = tcs;
            SendPacketLocked(packet, requireAck: true);
        }

        using var linkedCts = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        var timeoutTask = Task.Delay(TimeSpan.FromSeconds(3), linkedCts.Token);
        var completed = await Task.WhenAny(tcs.Task, timeoutTask).ConfigureAwait(false);
        if (completed == timeoutTask)
        {
            lock (_sync)
            {
                _pendingRequests.Remove(packet.RequestId);
                _stats.TxTimeout++;
            }
            throw new TimeoutException("Timed out waiting for device response.");
        }
        linkedCts.Cancel();
        return await tcs.Task.ConfigureAwait(false);
    }

    public void Dispose()
    {
        Disconnect();
    }

    private async Task ReaderLoopAsync(CancellationToken token)
    {
        try
        {
            if (_port == null) return;
            var stream = _port.BaseStream;
            var buffer = new byte[256];
            while (!token.IsCancellationRequested)
            {
                int read;
                try
                {
                    read = await stream.ReadAsync(buffer.AsMemory(0, buffer.Length), token).ConfigureAwait(false);
                }
                catch (OperationCanceledException)
                {
                    break;
                }
                catch (Exception ex)
                {
                    Log?.Invoke($"Serial read error: {ex.Message}");
                    break;
                }
                if (read <= 0) continue;
                AppendData(buffer.AsSpan(0, read));
            }
        }
        finally
        {
            Log?.Invoke("Serial reader stopped");
        }
    }

    private void AppendData(ReadOnlySpan<byte> data)
    {
        lock (_sync)
        {
            for (int i = 0; i < data.Length; ++i)
            {
                _buffer.Add(data[i]);
            }
            ProcessBufferLocked();
        }
    }

    private void ProcessBufferLocked()
    {
        while (_buffer.Count >= ReliableProtocol.HeaderSize)
        {
            var span = CollectionsMarshal.AsSpan(_buffer);
            if (span[0] != ReliableProtocol.FrameMagic || span[1] != ReliableProtocol.FrameVersion)
            {
                _buffer.RemoveAt(0);
                continue;
            }
            var headerSpan = span.Slice(0, ReliableProtocol.HeaderSize);
            var header = ReliableProtocol.ReadHeader(headerSpan);
            int totalLength = ReliableProtocol.HeaderSize + header.PayloadLength;
            if (_buffer.Count < totalLength)
            {
                break;
            }

            var frameSpan = span.Slice(0, totalLength);
            var frameCopy = new byte[totalLength];
            frameSpan.CopyTo(frameCopy);
            frameCopy[6] = 0;
            frameCopy[7] = 0;
            ushort computedCrc = ReliableProtocol.ComputeCrc16(frameCopy);
            if (computedCrc != header.Crc)
            {
                _stats.RxCrcErrors++;
                if ((header.Flags & ReliableProtocol.FlagAckRequest) != 0)
                {
                    SendAckLocked(header.Sequence, ack: false, (byte)ReliableProtocol.Status.CrcMismatch);
                }
                _buffer.RemoveRange(0, totalLength);
                continue;
            }

            bool isAck = (header.Flags & ReliableProtocol.FlagIsAck) != 0;
            bool isNak = (header.Flags & ReliableProtocol.FlagIsNak) != 0;
            bool requiresAck = (header.Flags & ReliableProtocol.FlagAckRequest) != 0;

            if (isAck || isNak)
            {
                if (isAck) _stats.TxAcked++; else _stats.TxNak++;
                _stats.LastAckOrNakMs = (uint)Environment.TickCount;
                _stats.LastStatusCode = header.Status;
                _buffer.RemoveRange(0, totalLength);
                continue;
            }

            _stats.RxFrames++;
            if (requiresAck)
            {
                _stats.RxAckRequests++;
            }

            bool handled = HandlePayload(frameSpan.Slice(ReliableProtocol.HeaderSize), header.PayloadLength);
            if (requiresAck)
            {
                SendAckLocked(header.Sequence, handled, handled ? (byte)ReliableProtocol.Status.Ok : (byte)ReliableProtocol.Status.HandlerDeclined);
            }

            _buffer.RemoveRange(0, totalLength);
        }
    }

    private bool HandlePayload(ReadOnlySpan<byte> payload, int length)
    {
        unsafe
        {
            if (length != sizeof(DebugProtocol.Packet))
            {
                _stats.RxInvalidLength++;
                return false;
            }
            DebugProtocol.Packet packet;
            fixed (byte* ptr = payload)
            {
                packet = Marshal.PtrToStructure<DebugProtocol.Packet>(new IntPtr(ptr));
            }
            if (!DebugProtocol.IsValid(packet))
            {
                _stats.HandlerDeclined++;
                return false;
            }

            if (packet.Flags.HasFlag(DebugProtocol.PacketFlags.Response) && packet.RequestId != 0)
            {
                if (_pendingRequests.TryGetValue(packet.RequestId, out var tcs))
                {
                    _pendingRequests.Remove(packet.RequestId);
                    tcs.TrySetResult(packet);
                    return true;
                }
            }

            PacketReceived?.Invoke(packet);
            return true;
        }
    }

    private void SendPacketLocked(DebugProtocol.Packet packet, bool requireAck)
    {
        if (_port == null)
        {
            throw new InvalidOperationException("Serial port not open");
        }
        unsafe
        {
            int payloadSize = sizeof(DebugProtocol.Packet);
            byte sequence = ReserveSequenceLocked();
            Span<byte> frame = stackalloc byte[ReliableProtocol.HeaderSize + payloadSize];
            ReliableProtocol.WriteHeader(frame, requireAck ? ReliableProtocol.FlagAckRequest : (byte)0, sequence, (ushort)payloadSize, (byte)ReliableProtocol.Status.Ok);
            Span<DebugProtocol.Packet> packetSpan = stackalloc DebugProtocol.Packet[1];
            packetSpan[0] = packet;
            var payloadBytes = MemoryMarshal.AsBytes(packetSpan);
            payloadBytes.CopyTo(frame.Slice(ReliableProtocol.HeaderSize));
            ushort crc = ReliableProtocol.ComputeCrc16(frame);
            BinaryPrimitives.WriteUInt16LittleEndian(frame.Slice(6, 2), crc);
            WriteFrameLocked(frame);
            _stats.TxFrames++;
        }
    }

    private void SendAckLocked(byte sequence, bool ack, byte status)
    {
        if (_port == null) return;
        Span<byte> frame = stackalloc byte[ReliableProtocol.HeaderSize];
        ReliableProtocol.WriteHeader(frame, ack ? ReliableProtocol.FlagIsAck : ReliableProtocol.FlagIsNak, sequence, 0, status);
        ushort crc = ReliableProtocol.ComputeCrc16(frame);
        BinaryPrimitives.WriteUInt16LittleEndian(frame.Slice(6, 2), crc);
        WriteFrameLocked(frame);
    if (ack) _stats.RxAckSent++; else _stats.RxNakSent++;
    }

    private void WriteFrameLocked(ReadOnlySpan<byte> frame)
    {
        if (_port == null) return;
        try
        {
            _port.BaseStream.Write(frame);
            _port.BaseStream.Flush();
        }
        catch (Exception ex)
        {
            Log?.Invoke($"Serial write error: {ex.Message}");
            _stats.TxSendErrors++;
        }
    }

    private ushort AllocateRequestId()
    {
        lock (_sync)
        {
            ushort id = _nextRequestId;
            do
            {
                id = id == ushort.MaxValue ? (ushort)1 : (ushort)(id + 1);
            } while (id == 0 || _pendingRequests.ContainsKey(id));
            _nextRequestId = id;
            return id;
        }
    }

    private byte ReserveSequenceLocked()
    {
        byte seq = _nextSequence;
        _nextSequence = (byte)(_nextSequence == 255 ? 1 : _nextSequence + 1);
        if (seq == 0) seq = ReserveSequenceLocked();
        return seq;
    }
}
