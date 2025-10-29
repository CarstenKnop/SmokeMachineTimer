using System;
using System.Buffers.Binary;

namespace SmokeMachineDiagnostics.Protocol;

public static class ReliableProtocol
{
    public const byte FrameMagic = 0xA5;
    public const byte FrameVersion = 1;
    public const byte FlagAckRequest = 0x01;
    public const byte FlagIsAck = 0x02;
    public const byte FlagIsNak = 0x04;

    public const int HeaderSize = 9; // magic, version, flags, seq, payloadLen(2), crc(2), status

    public enum AckType : byte
    {
        Ack,
        Nak,
        Timeout
    }

    public enum Status : byte
    {
        Ok = 0,
        CrcMismatch = 1,
        InvalidLength = 2,
        HandlerDeclined = 3,
        Timeout = 4,
        SendError = 5
    }

    public readonly struct FrameHeader
    {
        public FrameHeader(byte magic, byte version, byte flags, byte sequence, ushort payloadLength, ushort crc, byte status)
        {
            Magic = magic;
            Version = version;
            Flags = flags;
            Sequence = sequence;
            PayloadLength = payloadLength;
            Crc = crc;
            Status = status;
        }

        public byte Magic { get; }
        public byte Version { get; }
        public byte Flags { get; }
        public byte Sequence { get; }
        public ushort PayloadLength { get; }
        public ushort Crc { get; }
        public byte Status { get; }
    }

    public struct TransportStats
    {
        public uint TxFrames;
        public uint TxAcked;
        public uint TxNak;
        public uint TxTimeout;
        public uint TxRetries;
        public uint TxSendErrors;
        public uint RxFrames;
        public uint RxAckRequests;
        public uint RxAckSent;
        public uint RxNakSent;
        public uint RxCrcErrors;
        public uint RxInvalidLength;
        public uint HandlerDeclined;
        public uint LastAckOrNakMs;
        public byte LastStatusCode;
        private byte _pad0;
        private ushort _pad1;

        public void Reset()
        {
            TxFrames = TxAcked = TxNak = TxTimeout = TxRetries = TxSendErrors = 0;
            RxFrames = RxAckRequests = RxAckSent = RxNakSent = RxCrcErrors = RxInvalidLength = 0;
            HandlerDeclined = 0;
            LastAckOrNakMs = 0;
            LastStatusCode = 0;
            _pad0 = 0;
            _pad1 = 0;
        }
    }

    public static string DescribeStatus(byte status) => status switch
    {
        (byte)Status.Ok => "OK",
        (byte)Status.CrcMismatch => "CRC_MISMATCH",
        (byte)Status.InvalidLength => "INVALID_LENGTH",
        (byte)Status.HandlerDeclined => "HANDLER_DECLINED",
        (byte)Status.Timeout => "TIMEOUT",
        (byte)Status.SendError => "SEND_ERROR",
        _ => status.ToString("X2")
    };

    public static ushort ComputeCrc16(ReadOnlySpan<byte> data, ushort seed = 0xFFFF)
    {
        ushort crc = seed;
        for (int i = 0; i < data.Length; i++)
        {
            crc ^= (ushort)(data[i] << 8);
            for (int j = 0; j < 8; j++)
            {
                bool bitSet = (crc & 0x8000) != 0;
                crc <<= 1;
                if (bitSet)
                {
                    crc ^= 0x1021;
                }
            }
        }
        return crc;
    }

    public static FrameHeader ReadHeader(ReadOnlySpan<byte> span)
    {
        byte magic = span[0];
        byte version = span[1];
        byte flags = span[2];
        byte seq = span[3];
        ushort payloadLen = BinaryPrimitives.ReadUInt16LittleEndian(span.Slice(4, 2));
        ushort crc = BinaryPrimitives.ReadUInt16LittleEndian(span.Slice(6, 2));
        byte status = span[8];
        return new FrameHeader(magic, version, flags, seq, payloadLen, crc, status);
    }

    public static void WriteHeader(Span<byte> destination, byte flags, byte sequence, ushort payloadLength, byte status)
    {
        destination[0] = FrameMagic;
        destination[1] = FrameVersion;
        destination[2] = flags;
        destination[3] = sequence;
        BinaryPrimitives.WriteUInt16LittleEndian(destination.Slice(4, 2), payloadLength);
        BinaryPrimitives.WriteUInt16LittleEndian(destination.Slice(6, 2), 0);
        destination[8] = status;
    }
}
