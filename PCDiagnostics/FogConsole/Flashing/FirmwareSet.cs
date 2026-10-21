using System.IO;
using System.Security.Cryptography;

namespace FogConsole.Flashing;

/// <summary>What gets written to the device and how much of the flash is touched.</summary>
public enum FlashMode
{
    /// <summary>Only the application. Settings, pairing and the boot loader stay as they are.</summary>
    Update,

    /// <summary>Boot loader, partition table and application. Settings and pairing survive.</summary>
    Reinstall,

    /// <summary>Like Reinstall, and the settings partition is erased as well (factory state).</summary>
    CleanInstall,
}

/// <summary>A block of data for one flash address.</summary>
public sealed record FlashSegment(uint Offset, byte[] Data, string Label)
{
    public string Md5 => Convert.ToHexString(MD5.HashData(Data)).ToLowerInvariant();
}

/// <summary>Compares a region of the device's flash with what this package expects, to detect a different partition layout.</summary>
public sealed record LayoutGuard(uint Offset, uint Length, string Md5, IReadOnlyList<FlashSegment> WriteInstead);

public sealed class FlashPlan
{
    public required IReadOnlyList<FlashSegment> Segments { get; init; }
    public LayoutGuard? Guard { get; init; }
    public long TotalBytes => Segments.Sum(s => (long)s.Data.Length);
}

/// <summary>
/// Everything needed to put one firmware on a device: the application image and, when available, the boot loader,
/// the partition table and the boot_app0 (OTA data) blob that go with it.
/// </summary>
public sealed class FirmwareSet
{
    public const uint DefaultAppOffset = 0x10000;
    public const uint DefaultBootApp0Offset = 0xE000;
    public const uint DefaultNvsOffset = 0x9000;
    public const uint DefaultNvsSize = 0x5000;

    public required string Title { get; init; }

    /// <summary>The folder or file it was loaded from.</summary>
    public required string Source { get; init; }

    /// <summary>Where it comes from in words: "Built in this repository", "Installed with the app", "Chosen by you".</summary>
    public string Origin { get; init; } = "";

    public FirmwareKind Kind { get; init; }
    public string? Version { get; init; }
    public required EspImageInfo AppInfo { get; init; }
    public required byte[] App { get; init; }
    public byte[]? Bootloader { get; init; }
    public byte[]? PartitionTableBin { get; init; }
    public byte[]? BootApp0 { get; init; }
    public DateTime FileTimeUtc { get; init; }

    public uint AppOffset { get; init; } = DefaultAppOffset;
    public uint BootApp0Offset { get; init; } = DefaultBootApp0Offset;
    public uint NvsOffset { get; init; } = DefaultNvsOffset;
    public uint NvsSize { get; init; } = DefaultNvsSize;
    public uint MaxAppSize { get; init; } = 0x300000;

    /// <summary>The partition table has an OTA data partition (a layout with two application slots): then every write also selects slot 0.</summary>
    public bool HasOtaData { get; init; }

    /// <summary>The boot loader and partition table are present, so "Reinstall" is possible.</summary>
    public bool CanReinstall => Bootloader is not null && PartitionTableBin is not null;

    public string KindText => Kind switch { FirmwareKind.Timer => "Timer", FirmwareKind.Remote => "Remote", _ => "Unknown" };

    public string DisplayName => Version is null ? KindText + " firmware" : $"{KindText} firmware {Version}";

    /// <summary>Problems that make writing this firmware a bad idea (empty = fine).</summary>
    public IReadOnlyList<string> Problems
    {
        get
        {
            var list = new List<string>();
            if (!AppInfo.IsEspImage) list.Add("The application file is not an ESP32 image.");
            else if (!AppInfo.Intact) list.Add($"The application image is damaged: {AppInfo.Problem}.");
            if (AppInfo.IsEspImage && AppInfo.ChipId != Esp32C3.ChipId)
                list.Add($"The image is built for {Esp32C3.ChipName(AppInfo.ChipId)}, not for the ESP32-C3.");
            if (App.Length > MaxAppSize) list.Add("The application does not fit into the application partition.");
            if (Bootloader is { } b)
            {
                var info = EspImageInfo.Parse(b);
                if (!info.Intact) list.Add($"The boot loader file is damaged: {info.Problem}.");
                else if (b[3] >> 4 != 0x2) list.Add("The boot loader is built for a flash size other than 4 MB (the XIAO ESP32-C3 has 4 MB).");
            }
            return list;
        }
    }

    // ---- plans -----------------------------------------------------------------------------------------------------

    public FlashPlan BuildPlan(FlashMode mode)
    {
        var app = new FlashSegment(AppOffset, App, "application");
        var full = FullSegments();

        switch (mode)
        {
            case FlashMode.Update:
                // With two application slots the OTA data says which one boots. A device that was last updated over the air may run
                // from slot 1: writing slot 0 and resetting the OTA data makes sure the firmware that was just written is the one that starts.
                return new FlashPlan
                {
                    Segments = HasOtaData ? new[] { OtaDataSegment(), app } : new[] { app },
                    Guard = PartitionTableBin is { } pt && CanReinstall
                        ? new LayoutGuard(PartitionTable.DefaultOffset, (uint)pt.Length, Convert.ToHexString(MD5.HashData(pt)).ToLowerInvariant(), full)
                        : null,
                };
            case FlashMode.Reinstall:
                if (!CanReinstall) throw new InvalidOperationException("This firmware has no boot loader and partition table; only an update is possible.");
                return new FlashPlan { Segments = full };
            default:
                if (!CanReinstall) throw new InvalidOperationException("This firmware has no boot loader and partition table; only an update is possible.");
                var clean = new List<FlashSegment>(full) { new(NvsOffset, Enumerable.Repeat((byte)0xFF, (int)NvsSize).ToArray(), "settings (erase)") };
                clean.Sort((a, b) => a.Offset.CompareTo(b.Offset));
                return new FlashPlan { Segments = clean };
        }
    }

    private List<FlashSegment> FullSegments()
    {
        var list = new List<FlashSegment>();
        if (Bootloader is not null) list.Add(new FlashSegment(0, Bootloader, "boot loader"));
        if (PartitionTableBin is not null) list.Add(new FlashSegment(PartitionTable.DefaultOffset, PartitionTableBin, "partition table"));
        if (PartitionTableBin is not null) list.Add(OtaDataSegment());
        list.Add(new FlashSegment(AppOffset, App, "application"));
        return list;
    }

    /// <summary>boot_app0.bin (slot 0 boots). Without that file the OTA data is left erased, which makes the boot loader start the first application as well.</summary>
    private FlashSegment OtaDataSegment() =>
        new(BootApp0Offset, BootApp0 ?? Enumerable.Repeat((byte)0xFF, 0x2000).ToArray(), "OTA data");

    // ---- loading ---------------------------------------------------------------------------------------------------

    /// <summary>
    /// Loads a firmware from a PlatformIO build folder (firmware.bin, bootloader.bin, partitions.bin, boot_app0.bin) or
    /// from a single .bin file (an application image, or a merged image that starts at flash address 0).
    /// </summary>
    public static FirmwareSet Load(string path, string? title = null, string origin = "")
    {
        if (Directory.Exists(path)) return FromFolder(path, title, origin);
        if (File.Exists(path)) return FromFile(path, title, origin);
        throw new FileNotFoundException("Firmware not found: " + path);
    }

    public static FirmwareSet FromFolder(string folder, string? title = null, string origin = "")
    {
        string app = Path.Combine(folder, "firmware.bin");
        if (!File.Exists(app))
        {
            var bins = Directory.GetFiles(folder, "*.bin");
            if (bins.Length == 1) app = bins[0];
            else throw new FileNotFoundException($"No firmware.bin in {folder}.");
        }

        byte[]? Optional(string name, params string[] extraFolders)
        {
            foreach (var dir in new[] { folder }.Concat(extraFolders))
            {
                var p = Path.Combine(dir, name);
                if (File.Exists(p)) return File.ReadAllBytes(p);
            }
            return null;
        }

        var appBytes = File.ReadAllBytes(app);
        var info = EspImageInfo.Parse(appBytes);
        var partitions = Optional("partitions.bin");
        var layout = partitions is null ? Array.Empty<PartitionEntry>() : PartitionTable.Parse(partitions);
        var appPart = layout.FirstOrDefault(p => p.IsApp);
        var nvs = layout.FirstOrDefault(p => p.IsNvs);
        var ota = layout.FirstOrDefault(p => p.IsOtaData);

        return new FirmwareSet
        {
            Title = title ?? Path.GetFileName(folder.TrimEnd('\\', '/')),
            Source = folder,
            Origin = origin,
            Kind = info.Kind,
            Version = info.Version,
            AppInfo = info,
            App = appBytes,
            Bootloader = Optional("bootloader.bin"),
            PartitionTableBin = partitions,
            BootApp0 = Optional("boot_app0.bin", PlatformIoBootApp0Folders().ToArray()),
            HasOtaData = ota is not null,
            FileTimeUtc = File.GetLastWriteTimeUtc(app),
            AppOffset = appPart?.Offset ?? DefaultAppOffset,
            MaxAppSize = appPart?.Size ?? 0x300000,
            NvsOffset = nvs?.Offset ?? DefaultNvsOffset,
            NvsSize = nvs?.Size ?? DefaultNvsSize,
            BootApp0Offset = ota?.Offset ?? DefaultBootApp0Offset,
        };
    }

    public static FirmwareSet FromFile(string file, string? title = null, string origin = "")
    {
        var bytes = File.ReadAllBytes(file);
        if (bytes.Length > PartitionTable.DefaultOffset + 2 && bytes[PartitionTable.DefaultOffset] == 0xAA && bytes[PartitionTable.DefaultOffset + 1] == 0x50)
            return FromMerged(file, bytes, title, origin);

        var info = EspImageInfo.Parse(bytes);
        return new FirmwareSet
        {
            Title = title ?? Path.GetFileName(file),
            Source = file,
            Origin = origin,
            Kind = info.Kind,
            Version = info.Version,
            AppInfo = info,
            App = bytes,
            FileTimeUtc = File.GetLastWriteTimeUtc(file),
        };
    }

    /// <summary>A merged flash image (boot loader, partition table, ..., application). It is split so that the settings partition is not overwritten.</summary>
    private static FirmwareSet FromMerged(string file, byte[] bytes, string? title, string origin)
    {
        var layout = PartitionTable.Parse(bytes.AsSpan((int)PartitionTable.DefaultOffset));
        var appPart = layout.FirstOrDefault(p => p.IsApp);
        uint appOffset = appPart?.Offset ?? DefaultAppOffset;
        if (appOffset >= bytes.Length) throw new InvalidDataException("The merged image ends before the application starts.");
        var nvs = layout.FirstOrDefault(p => p.IsNvs);
        var ota = layout.FirstOrDefault(p => p.IsOtaData);

        var app = bytes.AsSpan((int)appOffset).ToArray();
        var info = EspImageInfo.Parse(app);
        if (info.Intact)
        {
            // a merged image is usually padded with 0xFF up to the flash size: keep exactly the image and its digest
            app = app[..Math.Min(app.Length, ImageLength(app))];
            info = EspImageInfo.Parse(app);
        }

        return new FirmwareSet
        {
            Title = title ?? Path.GetFileName(file),
            Source = file,
            Origin = origin,
            Kind = info.Kind,
            Version = info.Version,
            AppInfo = info,
            App = app,
            Bootloader = bytes.AsSpan(0, (int)PartitionTable.DefaultOffset).ToArray(),
            PartitionTableBin = bytes.AsSpan((int)PartitionTable.DefaultOffset, PartitionTable.MaxLength).ToArray(),
            BootApp0 = ota is null ? null : bytes.AsSpan((int)ota.Offset, (int)Math.Min(ota.Size, (uint)(bytes.Length - ota.Offset))).ToArray(),
            HasOtaData = ota is not null,
            FileTimeUtc = File.GetLastWriteTimeUtc(file),
            AppOffset = appOffset,
            MaxAppSize = appPart?.Size ?? 0x300000,
            NvsOffset = nvs?.Offset ?? DefaultNvsOffset,
            NvsSize = nvs?.Size ?? DefaultNvsSize,
            BootApp0Offset = ota?.Offset ?? DefaultBootApp0Offset,
        };
    }

    /// <summary>Length of an intact ESP image including the digest, as declared by its own structure.</summary>
    private static int ImageLength(ReadOnlySpan<byte> image)
    {
        int pos = EspImageInfo.HeaderLength;
        for (int i = 0; i < image[1]; i++)
            pos += 8 + (int)System.Buffers.Binary.BinaryPrimitives.ReadUInt32LittleEndian(image.Slice(pos + 4, 4));
        int end = (pos + 16) / 16 * 16;
        return image[23] == 1 ? end + 32 : end;
    }

    /// <summary>Where PlatformIO keeps boot_app0.bin (it is not part of the build output folder).</summary>
    public static IEnumerable<string> PlatformIoBootApp0Folders()
    {
        var roots = new List<string>();
        if (Environment.GetEnvironmentVariable("PLATFORMIO_CORE_DIR") is { Length: > 0 } core) roots.Add(core);
        roots.Add(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), ".platformio"));
        foreach (var root in roots)
            yield return Path.Combine(root, "packages", "framework-arduinoespressif32", "tools", "partitions");
    }
}
