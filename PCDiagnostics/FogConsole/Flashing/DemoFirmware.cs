using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Text;

namespace FogConsole.Flashing;

/// <summary>
/// Builds structurally valid ESP32-C3 images that contain nothing but filler and the Fog identity string. They let the
/// offline demo (and the tests) run a complete flash without any real firmware on disk; they are of course not runnable.
/// </summary>
public static class DemoFirmware
{
    /// <summary>An application image of about <paramref name="approxSize"/> bytes with the identity marker for the given kind and version.</summary>
    public static byte[] CreateImage(FirmwareKind kind, string version, int approxSize = 180_000, uint chipId = Esp32C3.ChipId, int seed = 1)
    {
        string marker = $"FOGFW|{(kind == FirmwareKind.Timer ? "timer" : "remote")}|{version}|";
        var rng = new Random(seed ^ (int)kind ^ version.GetHashCode());

        var seg0 = new byte[Math.Max(256, approxSize / 3)];
        FillCompressible(seg0, rng);
        Encoding.ASCII.GetBytes(marker).CopyTo(seg0, 64);
        var seg1 = new byte[Math.Max(256, approxSize - seg0.Length)];
        FillCompressible(seg1, rng);

        var file = new List<byte>(approxSize + 128);
        var header = new byte[EspImageInfo.HeaderLength];
        header[0] = EspImageInfo.Magic;
        header[1] = 2;                                                  // segments
        header[2] = 2;                                                  // dio
        header[3] = 0x2F;                                               // 4 MB, 80 MHz
        BinaryPrimitives.WriteUInt32LittleEndian(header.AsSpan(4), 0x4200_0000);
        BinaryPrimitives.WriteUInt16LittleEndian(header.AsSpan(12), (ushort)chipId);
        header[23] = 1;                                                 // SHA-256 appended
        file.AddRange(header);

        byte checksum = 0xEF;
        uint addr = 0x3C00_0000;
        foreach (var seg in new[] { seg0, seg1 })
        {
            var h = new byte[8];
            BinaryPrimitives.WriteUInt32LittleEndian(h, addr);
            BinaryPrimitives.WriteUInt32LittleEndian(h.AsSpan(4), (uint)seg.Length);
            file.AddRange(h);
            file.AddRange(seg);
            foreach (byte b in seg) checksum ^= b;
            addr += 0x0010_0000;
        }

        int pad = 15 - (file.Count % 16);
        file.AddRange(new byte[pad]);
        file.Add(checksum);
        file.AddRange(SHA256.HashData(file.ToArray()));
        return file.ToArray();
    }

    /// <summary>A complete set: application, boot loader, partition table (the project's huge_app layout) and OTA data.</summary>
    public static FirmwareSet CreateSet(FirmwareKind kind, string version, int approxSize = 180_000, bool complete = true)
    {
        var app = CreateImage(kind, version, approxSize);
        var info = EspImageInfo.Parse(app);
        return new FirmwareSet
        {
            Title = $"{(kind == FirmwareKind.Timer ? "Timer" : "Remote")} {version}",
            Source = "built into the simulator",
            Origin = "Demo image for the offline mode",
            Kind = info.Kind,
            Version = info.Version,
            AppInfo = info,
            App = app,
            Bootloader = complete ? CreateImage(FirmwareKind.Unknown, "boot", 19_000, seed: 7) : null,
            PartitionTableBin = complete ? CreatePartitionTable() : null,
            BootApp0 = complete ? CreateBootApp0() : null,
            HasOtaData = complete,
        };
    }

    /// <summary>The partition table of the project (partitions/fog_ota.csv): settings at 0x9000, OTA data, two application slots.</summary>
    public static byte[] CreatePartitionTable()
    {
        var table = new byte[PartitionTable.MaxLength];
        Array.Fill(table, (byte)0xFF);
        int pos = 0;

        void Add(string name, byte type, byte sub, uint offset, uint size)
        {
            table[pos] = 0xAA;
            table[pos + 1] = 0x50;
            table[pos + 2] = type;
            table[pos + 3] = sub;
            BinaryPrimitives.WriteUInt32LittleEndian(table.AsSpan(pos + 4), offset);
            BinaryPrimitives.WriteUInt32LittleEndian(table.AsSpan(pos + 8), size);
            Array.Clear(table, pos + 12, 16);
            Encoding.ASCII.GetBytes(name).CopyTo(table, pos + 12);
            Array.Clear(table, pos + 28, 4);
            pos += 32;
        }

        Add("nvs", 0x01, 0x02, 0x9000, 0x5000);
        Add("otadata", 0x01, 0x00, 0xE000, 0x2000);
        Add("app0", 0x00, 0x10, 0x10000, 0x1E0000);
        Add("app1", 0x00, 0x11, 0x1F0000, 0x1E0000);
        Add("coredump", 0x01, 0x03, 0x3D0000, 0x10000);

        // MD5 entry that ends the table
        table[pos] = 0xEB;
        table[pos + 1] = 0xEB;
        MD5.HashData(table.AsSpan(0, pos)).CopyTo(table, pos + 16);
        return table;
    }

    /// <summary>OTA data that selects the first application slot (what boot_app0.bin contains).</summary>
    public static byte[] CreateBootApp0()
    {
        var b = new byte[0x2000];
        Array.Fill(b, (byte)0xFF);
        b[0] = 1; b[1] = 0; b[2] = 0; b[3] = 0;
        return b;
    }

    private static void FillCompressible(byte[] data, Random rng)
    {
        // runs of repeated words: compresses to roughly a third, like real code
        int i = 0;
        while (i < data.Length)
        {
            int run = rng.Next(4, 40);
            byte v = (byte)rng.Next(0, 256);
            byte w = (byte)rng.Next(0, 256);
            for (int k = 0; k < run && i < data.Length; k++, i++) data[i] = (k & 1) == 0 ? v : w;
        }
    }
}
