using System;
using System.Runtime.InteropServices;

namespace SmokeMachineDiagnostics.Protocol;

public static class DebugProtocol
{
    public const byte PacketMagic = 0xD1;
    public const int MaxDataBytes = 96;

    [Flags]
    public enum PacketFlags : byte
    {
        None = 0,
        Response = 0x01,
        RequiresTimer = 0x02,
        Streaming = 0x04
    }

    public enum Command : byte
    {
        Ping = 1,
        GetRemoteStats = 2,
        GetTimerStats = 3,
        SetChannel = 4,
        ForceChannel = 5,
        GetRssi = 6,
        ReadConfig = 7,
        WriteConfig = 8,
        GetDeviceInfo = 9,
        GetLogSnapshot = 10,
        GetDeviceInventory = 11,
        SelectDevice = 12,
        StartDiscovery = 13,
        StopDiscovery = 14,
        GetDiscoveredDevices = 15,
        PairDiscoveredDevice = 16,
        UnpairDevice = 17,
        RenameDevice = 18
    }

    public enum Status : byte
    {
        Ok = 0,
        Busy = 1,
        InvalidArgument = 2,
        Unsupported = 3,
        TransportError = 4,
        Timeout = 5,
        NotReady = 6
    }

    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public unsafe struct Packet
    {
        public byte Magic;
        public Command Command;
        public Status Status;
        public PacketFlags Flags;
        public ushort RequestId;
        public ushort DataLength;
        public fixed byte Data[MaxDataBytes];
    }

    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct LinkHealth
    {
        public ReliableProtocol.TransportStats Transport;
        public sbyte RssiLocal;
        public sbyte RssiPeer;
        public byte Channel;
        private byte _reserved;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public unsafe struct DeviceInfo
    {
        public uint FirmwareVersion;
        public uint BuildTimestamp;
        public byte DeviceKind;
        private fixed byte Reserved[11];
    }

    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct TimerSnapshot
    {
        public float TonSeconds;
        public float ToffSeconds;
        public float ElapsedSeconds;
        public byte OutputOn;
        public byte OverrideActive;
        public byte Channel;
        private byte _reserved;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct TimerStatsPayload
    {
        public LinkHealth Link;
        public TimerSnapshot Timer;
        public TimerSnapshot Remote;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct SerialLinkSummary
    {
        public uint TxFrames;
        public uint RxFrames;
        public uint Errors;
        public byte LastStatusCode;
        private byte _pad0;
        private ushort _pad1;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct RemoteStatsPayload
    {
        public LinkHealth RemoteLink;
        public TimerSnapshot Remote;
        public SerialLinkSummary SerialLink;
    }

    public const int InventoryEntrySize = 20; // bytes per DeviceInventoryEntry
    public const int InventoryMaxEntries = 4;

    public const int DiscoveryEntrySize = 30; // bytes per DiscoveredDeviceEntry
    public const int DiscoveryMaxEntries = 3;

    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct DeviceInventoryPayload
    {
        public byte TotalCount;
        public byte BatchStart;
        public byte BatchCount;
        public byte ActiveIndex;
        // Entries follow immediately in payload (InventoryEntrySize * BatchCount bytes)
    }

    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct DiscoveredDevicesPayload
    {
        public byte TotalCount;
        public byte BatchStart;
        public byte BatchCount;
        public byte Reserved;
        // Entries follow immediately in payload (DiscoveryEntrySize * BatchCount bytes)
    }

    public static unsafe Packet CreateRequest(Command command, ReadOnlySpan<byte> payload, ushort requestId)
    {
        var packet = new Packet
        {
            Magic = PacketMagic,
            Command = command,
            Status = Status.Ok,
            Flags = PacketFlags.None,
            RequestId = requestId,
            DataLength = (ushort)Math.Min(payload.Length, MaxDataBytes)
        };
        for (int i = 0; i < packet.DataLength; i++)
        {
            packet.Data[i] = payload[i];
        }
        return packet;
    }

    public static Packet CreateEmpty(Command command, ushort requestId)
    {
        return new Packet
        {
            Magic = PacketMagic,
            Command = command,
            Status = Status.Ok,
            Flags = PacketFlags.None,
            RequestId = requestId,
            DataLength = 0
        };
    }

    public static bool IsValid(in Packet packet) => packet.Magic == PacketMagic && packet.DataLength <= MaxDataBytes;

    public static string DescribeCommand(Command command) => command switch
    {
        Command.Ping => "Ping",
        Command.GetRemoteStats => "GetRemoteStats",
        Command.GetTimerStats => "GetTimerStats",
        Command.SetChannel => "SetChannel",
        Command.ForceChannel => "ForceChannel",
        Command.GetRssi => "GetRssi",
        Command.ReadConfig => "ReadConfig",
        Command.WriteConfig => "WriteConfig",
        Command.GetDeviceInfo => "GetDeviceInfo",
        Command.GetLogSnapshot => "GetLogSnapshot",
        Command.GetDeviceInventory => "GetDeviceInventory",
        Command.SelectDevice => "SelectDevice",
        Command.StartDiscovery => "StartDiscovery",
        Command.StopDiscovery => "StopDiscovery",
        Command.GetDiscoveredDevices => "GetDiscoveredDevices",
        Command.PairDiscoveredDevice => "PairDiscoveredDevice",
        Command.UnpairDevice => "UnpairDevice",
        Command.RenameDevice => "RenameDevice",
        _ => command.ToString()
    };

    public static string DescribeStatus(Status status) => status switch
    {
        Status.Ok => "Ok",
        Status.Busy => "Busy",
        Status.InvalidArgument => "InvalidArgument",
        Status.Unsupported => "Unsupported",
        Status.TransportError => "TransportError",
        Status.Timeout => "Timeout",
        Status.NotReady => "NotReady",
        _ => status.ToString()
    };
}
