using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Text;

namespace FogConsole.Ota;

// The over-the-air update protocol between Fog Console and a timer (docs/OTA_UPDATE.md, section 4).
// Everything here works on the OTA frame payload  [op u8][session u8][body ...], which the remote tunnels unchanged.

/// <summary>Op codes of the OTA frames (PC → timer, and the timer's State report).</summary>
public static class OtaOp
{
    public const byte Begin = 0x01;
    public const byte Data = 0x02;
    public const byte Poll = 0x03;
    public const byte Verify = 0x04;
    public const byte Activate = 0x05;
    public const byte Abort = 0x06;
    public const byte State = 0x81;

    /// <summary>The magic byte that makes an Activate frame valid.</summary>
    public const byte ActivateMagic = 0xA7;
}

public enum OtaState : byte { Idle = 0, Erasing = 1, Receiving = 2, Verifying = 3, Verified = 4, Activating = 5, Failed = 6 }

public enum OtaError : byte
{
    None = 0,
    Busy = 1,
    NoSlot = 2,
    TooBig = 3,
    BadImage = 4,
    HashMismatch = 5,
    FlashError = 6,
    BadRequest = 7,
    AuthFailed = 8,
    Timeout = 9,
    LowResources = 10,
}

/// <summary>What a timer says about the transfer (the <c>0x81</c> State frame).</summary>
/// <param name="Contig">Chunks received without a gap from chunk 0.</param>
/// <param name="Mask">Bit i set: chunk Contig+1+i has been received.</param>
/// <param name="Progress">Percent of the current activity (erase / hash), otherwise 100.</param>
public sealed record OtaStateReport(byte Session, OtaState State, OtaError Error, int Contig, uint Mask, int Progress, int Total, int Slot)
{
    public const int Length = 14;

    public static bool TryParse(ReadOnlySpan<byte> payload, out OtaStateReport report)
    {
        report = null!;
        if (payload.Length < Length || payload[0] != OtaOp.State) return false;
        report = new OtaStateReport(
            payload[1], (OtaState)payload[2], (OtaError)payload[3],
            BinaryPrimitives.ReadUInt16LittleEndian(payload[4..]), BinaryPrimitives.ReadUInt32LittleEndian(payload[6..]),
            payload[10], BinaryPrimitives.ReadUInt16LittleEndian(payload[11..]), payload[13]);
        return true;
    }

    public byte[] ToBytes()
    {
        var b = new byte[Length];
        b[0] = OtaOp.State;
        b[1] = Session;
        b[2] = (byte)State;
        b[3] = (byte)Error;
        BinaryPrimitives.WriteUInt16LittleEndian(b.AsSpan(4), (ushort)Contig);
        BinaryPrimitives.WriteUInt32LittleEndian(b.AsSpan(6), Mask);
        b[10] = (byte)Progress;
        BinaryPrimitives.WriteUInt16LittleEndian(b.AsSpan(11), (ushort)Total);
        b[13] = (byte)Slot;
        return b;
    }

    /// <summary>True when the timer reports chunk <paramref name="chunk"/> as received (below Contig, or marked in the mask).</summary>
    public bool Has(int chunk)
    {
        if (chunk < Contig) return true;
        int bit = chunk - Contig - 1;
        return bit is >= 0 and < 32 && (Mask & (1u << bit)) != 0;
    }

    public int ReceivedCount => Contig + System.Numerics.BitOperations.PopCount(Mask);

    public override string ToString() =>
        $"session {Session:X2} {State}{(Error != OtaError.None ? $" ({Error})" : "")} contig {Contig}/{Total} mask {Mask:X8} {Progress}% slot {Slot}";
}

/// <summary>What <c>Begin</c> carries (besides the authentication).</summary>
public sealed record OtaBegin(byte Session, uint Size, byte[] Sha256, int Chunk, byte FwMajor, byte FwMinor, byte FwPatch, byte Flags = 0);

public static class OtaFrames
{
    public const int DefaultChunk = 224;
    public const int MaxChunk = 232;
    public const int MinChunk = 32;

    public static byte[] Begin(OtaBegin b, byte[] key)
    {
        if (b.Sha256.Length != 32) throw new ArgumentException("SHA-256 must be 32 bytes", nameof(b));
        var p = new byte[60];
        p[0] = OtaOp.Begin;
        p[1] = b.Session;
        BinaryPrimitives.WriteUInt32LittleEndian(p.AsSpan(2), b.Size);
        b.Sha256.CopyTo(p, 6);
        BinaryPrimitives.WriteUInt16LittleEndian(p.AsSpan(38), (ushort)b.Chunk);
        p[40] = b.FwMajor;
        p[41] = b.FwMinor;
        p[42] = b.FwPatch;
        p[43] = b.Flags;
        OtaAuth.Compute(key, b).CopyTo(p, 44);
        return p;
    }

    public static byte[] Data(byte session, int seq, ReadOnlySpan<byte> bytes)
    {
        var p = new byte[4 + bytes.Length];
        p[0] = OtaOp.Data;
        p[1] = session;
        BinaryPrimitives.WriteUInt16LittleEndian(p.AsSpan(2), (ushort)seq);
        bytes.CopyTo(p.AsSpan(4));
        return p;
    }

    public static byte[] Poll(byte session) => new[] { OtaOp.Poll, session };
    public static byte[] Verify(byte session) => new[] { OtaOp.Verify, session };
    public static byte[] Activate(byte session) => new[] { OtaOp.Activate, session, OtaOp.ActivateMagic };
    public static byte[] Abort(byte session) => new[] { OtaOp.Abort, session };

    /// <summary>Parses a Begin payload (used by the simulated timer and by tests). Authentication is not checked here.</summary>
    public static bool TryParseBegin(ReadOnlySpan<byte> p, out OtaBegin begin, out byte[] auth)
    {
        begin = null!;
        auth = Array.Empty<byte>();
        if (p.Length != 60 || p[0] != OtaOp.Begin) return false;
        begin = new OtaBegin(p[1], BinaryPrimitives.ReadUInt32LittleEndian(p[2..]), p.Slice(6, 32).ToArray(),
            BinaryPrimitives.ReadUInt16LittleEndian(p[38..]), p[40], p[41], p[42], p[43]);
        auth = p.Slice(44, 16).ToArray();
        return true;
    }

    public static bool TryParseData(ReadOnlySpan<byte> p, out byte session, out int seq, out ReadOnlySpan<byte> bytes)
    {
        session = 0;
        seq = 0;
        bytes = default;
        if (p.Length < 4 || p[0] != OtaOp.Data) return false;
        session = p[1];
        seq = BinaryPrimitives.ReadUInt16LittleEndian(p[2..]);
        bytes = p[4..];
        return true;
    }
}

/// <summary>The authentication of <c>Begin</c>: the first 16 bytes of HMAC-SHA256(key, "FOGOTA1" ‖ session ‖ size ‖ sha256 ‖ chunk ‖ fw ‖ flags).</summary>
public static class OtaAuth
{
    public const string DefaultKey = "FogLink-OTA-v1";

    public static byte[] KeyBytes(string? key) => Encoding.UTF8.GetBytes(string.IsNullOrEmpty(key) ? DefaultKey : key);

    public static byte[] Input(OtaBegin b)
    {
        var m = new byte[7 + 1 + 4 + 32 + 2 + 3 + 1];
        "FOGOTA1"u8.CopyTo(m);
        m[7] = b.Session;
        BinaryPrimitives.WriteUInt32LittleEndian(m.AsSpan(8), b.Size);
        b.Sha256.CopyTo(m, 12);
        BinaryPrimitives.WriteUInt16LittleEndian(m.AsSpan(44), (ushort)b.Chunk);
        m[46] = b.FwMajor;
        m[47] = b.FwMinor;
        m[48] = b.FwPatch;
        m[49] = b.Flags;
        return m;
    }

    public static byte[] Compute(byte[] key, OtaBegin b) => HMACSHA256.HashData(key, Input(b))[..16];

    public static bool Verify(byte[] key, OtaBegin b, ReadOnlySpan<byte> auth) =>
        auth.Length == 16 && CryptographicOperations.FixedTimeEquals(Compute(key, b), auth);
}

/// <summary>The FogLink radio frame (only needed to check against the published test vectors and for the packet inspector).</summary>
public static class FogFrame
{
    public const byte Magic = 0xF6;
    public const byte Version = 2;
    public const byte TypeOta = 5;

    public static ushort Crc16(ReadOnlySpan<byte> data, ushort seed = 0xFFFF)
    {
        ushort crc = seed;
        foreach (byte b in data)
        {
            crc ^= (ushort)(b << 8);
            for (int i = 0; i < 8; i++)
                crc = (crc & 0x8000) != 0 ? (ushort)((crc << 1) ^ 0x1021) : (ushort)(crc << 1);
        }
        return crc;
    }

    public static byte[] Encode(byte net, byte type, byte seq, ReadOnlySpan<byte> payload)
    {
        var f = new byte[6 + payload.Length + 2];
        f[0] = Magic;
        f[1] = Version;
        f[2] = net;
        f[3] = type;
        f[4] = seq;
        f[5] = (byte)payload.Length;
        payload.CopyTo(f.AsSpan(6));
        BinaryPrimitives.WriteUInt16LittleEndian(f.AsSpan(6 + payload.Length), Crc16(f.AsSpan(0, 6 + payload.Length)));
        return f;
    }
}

/// <summary>Plain-language texts for the codes and states.</summary>
public static class OtaText
{
    public static string Error(OtaError e) => e switch
    {
        OtaError.None => "no error",
        OtaError.Busy => "The timer is busy with another firmware transfer. Wait for it to end (or restart the timer) and try again.",
        OtaError.NoSlot => "The timer has no second application slot for over-the-air updates. It needs one update over its USB cable first (that installs the new partition layout; settings and pairing are kept).",
        OtaError.TooBig => "The firmware does not fit into the timer's application slot.",
        OtaError.BadImage => "The timer refused the image: it is not a firmware for the ESP32-C3 timer.",
        OtaError.HashMismatch => "What the timer wrote does not match the firmware (SHA-256 differs). Try again; if it repeats, the radio link is too bad or the timer's flash is faulty.",
        OtaError.FlashError => "The timer could not write its flash.",
        OtaError.BadRequest => "The timer did not understand the request (wrong state or arguments).",
        OtaError.AuthFailed => "The timer rejected the OTA key. The key in Settings must be the same one the timer firmware was built with.",
        OtaError.Timeout => "The timer dropped the transfer after 30 s of silence.",
        OtaError.LowResources => "The timer is short of resources for an update right now.",
        _ => $"The timer reported error {(int)e}.",
    };

    public static string State(OtaState s) => s switch
    {
        OtaState.Idle => "idle",
        OtaState.Erasing => "erasing",
        OtaState.Receiving => "receiving",
        OtaState.Verifying => "verifying",
        OtaState.Verified => "verified",
        OtaState.Activating => "activating",
        OtaState.Failed => "failed",
        _ => $"state {(int)s}",
    };
}

/// <summary>The image to send, its hash and the version it claims to be.</summary>
public sealed class OtaImage
{
    public OtaImage(byte[] data, string? version, int chunk = OtaFrames.DefaultChunk)
    {
        if (data.Length == 0) throw new ArgumentException("The image is empty.", nameof(data));
        if (chunk is < OtaFrames.MinChunk or > OtaFrames.MaxChunk) throw new ArgumentOutOfRangeException(nameof(chunk));
        Data = data;
        Chunk = chunk;
        Sha256 = SHA256.HashData(data);
        Version = version;
        (Major, Minor, Patch) = ParseVersion(version);
    }

    public byte[] Data { get; }
    public byte[] Sha256 { get; }
    public int Chunk { get; }
    public string? Version { get; }
    public byte Major { get; }
    public byte Minor { get; }
    public byte Patch { get; }

    public int Size => Data.Length;
    public int Chunks => (Data.Length + Chunk - 1) / Chunk;

    public ReadOnlySpan<byte> ChunkBytes(int seq)
    {
        int offset = seq * Chunk;
        return Data.AsSpan(offset, Math.Min(Chunk, Data.Length - offset));
    }

    public static (byte, byte, byte) ParseVersion(string? v)
    {
        if (string.IsNullOrWhiteSpace(v)) return (0, 0, 0);
        var parts = new string(v.TakeWhile(c => char.IsDigit(c) || c == '.').ToArray()).Split('.', StringSplitOptions.RemoveEmptyEntries);
        byte P(int i) => i < parts.Length && int.TryParse(parts[i], out var n) ? (byte)Math.Clamp(n, 0, 255) : (byte)0;
        return (P(0), P(1), P(2));
    }

    /// <summary>True when a firmware version reported by the fleet ("2.1" from the real remote, "2.1.0" from the simulator) is the one this image carries.</summary>
    public bool IsVersion(string? reported)
    {
        if (Version is null || string.IsNullOrWhiteSpace(reported)) return true;       // nothing to compare with
        var got = new string(reported.TakeWhile(c => char.IsDigit(c) || c == '.').ToArray()).Split('.', StringSplitOptions.RemoveEmptyEntries);
        if (got.Length < 2) return false;
        if (!(int.TryParse(got[0], out var ma) && int.TryParse(got[1], out var mi))) return false;
        if (ma != Major || mi != Minor) return false;
        return got.Length < 3 || (int.TryParse(got[2], out var pa) && pa == Patch);
    }
}
