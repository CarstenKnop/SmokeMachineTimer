using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Text;

namespace FogConsole.Flashing;

public enum FirmwareKind { Unknown, Timer, Remote }

/// <summary>What could be read from an ESP32 application image (the .bin an ESP-IDF / Arduino build produces).</summary>
public sealed class EspImageInfo
{
    public const byte Magic = 0xE9;
    public const int HeaderLength = 24;

    public bool IsEspImage { get; init; }
    public uint ChipId { get; init; }
    public int SegmentCount { get; init; }
    public bool HashAppended { get; init; }

    /// <summary>True when the file ends where the header says it does and the checksum (and SHA-256, when present) match.</summary>
    public bool Intact { get; init; }

    /// <summary>Why the image is not intact (null when it is).</summary>
    public string? Problem { get; init; }

    public FirmwareKind Kind { get; init; }

    /// <summary>The version string compiled into the image by <c>FOG_FW_IDENT</c>, when present.</summary>
    public string? Version { get; init; }

    public static EspImageInfo Parse(ReadOnlySpan<byte> file)
    {
        var (kind, version) = FindIdentity(file);

        EspImageInfo Bad(string problem, bool esp = true, uint chip = 0, int segs = 0, bool hash = false) => new()
        {
            IsEspImage = esp, ChipId = chip, SegmentCount = segs, HashAppended = hash, Intact = false, Problem = problem, Kind = kind, Version = version,
        };

        if (file.Length < HeaderLength + 8 || file[0] != Magic)
            return Bad("not an ESP32 firmware image (wrong first byte)", esp: false);

        int segments = file[1];
        uint chipId = BinaryPrimitives.ReadUInt16LittleEndian(file.Slice(12, 2));
        bool hash = file[23] == 1;
        if (segments == 0 || segments > 16) return Bad("the image header is damaged (segment count)", true, chipId, segments, hash);

        // walk the segments, XOR the data
        int pos = HeaderLength;
        byte checksum = 0xEF;
        for (int i = 0; i < segments; i++)
        {
            if (pos + 8 > file.Length) return Bad("the file is truncated (segment header missing)", true, chipId, segments, hash);
            int len = (int)BinaryPrimitives.ReadUInt32LittleEndian(file.Slice(pos + 4, 4));
            pos += 8;
            if (len < 0 || pos + len > file.Length) return Bad("the file is truncated (segment data missing)", true, chipId, segments, hash);
            foreach (byte b in file.Slice(pos, len)) checksum ^= b;
            pos += len;
        }

        // padding up to a 16 byte boundary, the last byte of which is the checksum
        int checksumPos = (pos + 16) / 16 * 16 - 1;
        if (checksumPos >= file.Length) return Bad("the file is truncated (checksum missing)", true, chipId, segments, hash);
        if (file[checksumPos] != checksum) return Bad("the checksum does not match: the file is damaged", true, chipId, segments, hash);
        int end = checksumPos + 1;

        if (hash)
        {
            if (end + 32 > file.Length) return Bad("the file is truncated (SHA-256 digest missing)", true, chipId, segments, hash);
            var digest = SHA256.HashData(file[..end]);
            if (!digest.AsSpan().SequenceEqual(file.Slice(end, 32))) return Bad("the SHA-256 digest does not match: the file is damaged", true, chipId, segments, hash);
        }

        return new EspImageInfo
        {
            IsEspImage = true, ChipId = chipId, SegmentCount = segments, HashAppended = hash, Intact = true, Kind = kind, Version = version,
        };
    }

    /// <summary>Looks for the marker <c>FOGFW|kind|version|</c> that both firmwares carry.</summary>
    public static (FirmwareKind Kind, string? Version) FindIdentity(ReadOnlySpan<byte> file)
    {
        ReadOnlySpan<byte> marker = "FOGFW|"u8;
        int at = file.IndexOf(marker);
        if (at < 0) return (FirmwareKind.Unknown, null);

        var rest = file[(at + marker.Length)..];
        int n = Math.Min(rest.Length, 48);
        var text = Encoding.ASCII.GetString(rest[..n]);
        var parts = text.Split('|');
        if (parts.Length < 3) return (FirmwareKind.Unknown, null);

        var kind = parts[0] switch { "timer" => FirmwareKind.Timer, "remote" => FirmwareKind.Remote, _ => FirmwareKind.Unknown };
        if (kind == FirmwareKind.Unknown) return (kind, null);
        var version = parts[1].Length is > 0 and <= 16 && parts[1].All(c => char.IsAsciiLetterOrDigit(c) || c is '.' or '-' or '+') ? parts[1] : null;
        return (kind, version);
    }
}

/// <summary>One entry of an ESP-IDF partition table.</summary>
public sealed record PartitionEntry(string Name, byte Type, byte SubType, uint Offset, uint Size)
{
    public bool IsApp => Type == 0x00;
    public bool IsNvs => Type == 0x01 && SubType == 0x02;
    public bool IsOtaData => Type == 0x01 && SubType == 0x00;
}

public static class PartitionTable
{
    public const uint DefaultOffset = 0x8000;
    public const int MaxLength = 0xC00;

    public static IReadOnlyList<PartitionEntry> Parse(ReadOnlySpan<byte> data)
    {
        var list = new List<PartitionEntry>();
        for (int pos = 0; pos + 32 <= data.Length && pos < MaxLength; pos += 32)
        {
            if (data[pos] != 0xAA || data[pos + 1] != 0x50) break;    // MD5 entry (EB EB), erased flash (FF FF) or garbage: end of table
            uint offset = BinaryPrimitives.ReadUInt32LittleEndian(data.Slice(pos + 4, 4));
            uint size = BinaryPrimitives.ReadUInt32LittleEndian(data.Slice(pos + 8, 4));
            var name = Encoding.ASCII.GetString(data.Slice(pos + 12, 16)).TrimEnd('\0');
            list.Add(new PartitionEntry(name, data[pos + 2], data[pos + 3], offset, size));
        }
        return list;
    }
}
