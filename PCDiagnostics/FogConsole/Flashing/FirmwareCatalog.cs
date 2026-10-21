using System.IO;
using System.Security.Cryptography;

namespace FogConsole.Flashing;

/// <summary>Finds the firmware that is available on this PC.</summary>
public static class FirmwareCatalog
{
    public sealed record Result(IReadOnlyList<FirmwareSet> Sets, IReadOnlyList<string> Notes);

    /// <summary>
    /// Looks in (1) the <c>firmware</c> folder next to the application, (2) the PlatformIO build folders of this
    /// repository when the tool is run from a source checkout, and (3) the files and folders the user picked earlier.
    /// </summary>
    public static Result Discover(string appDirectory, IEnumerable<string>? pickedPaths = null)
    {
        var sets = new List<FirmwareSet>();
        var notes = new List<string>();
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var contents = new HashSet<string>();          // the same image found in two places is listed once

        void Add(string path, string origin)
        {
            string key;
            try { key = Path.GetFullPath(path); }
            catch (Exception ex) when (ex is ArgumentException or NotSupportedException) { return; }
            if (!seen.Add(key)) return;
            try
            {
                var set = FirmwareSet.Load(key, origin: origin);
                if (contents.Add(set.Kind + Convert.ToHexString(SHA256.HashData(set.App)))) sets.Add(set);
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or InvalidDataException or ArgumentException)
            {
                notes.Add($"{key}: {ex.Message}");
            }
        }

        // 0. what the user picked before (first, so that a picked file is not hidden by an identical copy found below)
        foreach (var p in pickedPaths ?? Array.Empty<string>())
        {
            if (File.Exists(p) || Directory.Exists(p)) Add(p, "Chosen by you");
        }

        // 1. shipped with the application:  <app>\firmware\<anything>\firmware.bin   (or loose .bin files)
        var shipped = Path.Combine(appDirectory, "firmware");
        if (Directory.Exists(shipped))
        {
            foreach (var dir in SafeDirectories(shipped).Where(d => File.Exists(Path.Combine(d, "firmware.bin"))))
                Add(dir, "Installed with the app");
            foreach (var file in SafeFiles(shipped, "*.bin"))
                Add(file, "Installed with the app");
        }

        // 2. a source checkout with PlatformIO build output
        var root = FindRepositoryRoot(appDirectory);
        if (root is not null)
        {
            foreach (var project in new[] { "FogMachineTimer", "FogMachineRemoteControl" })
            {
                var build = Path.Combine(root, project, ".pio", "build");
                if (!Directory.Exists(build)) continue;
                foreach (var env in SafeDirectories(build).Where(d => File.Exists(Path.Combine(d, "firmware.bin"))))
                    Add(env, "Built in this repository");
            }
        }

        sets.Sort(Compare);
        return new Result(sets, notes);
    }

    /// <summary>Walks up from the application folder looking for the repository (it holds both firmware projects).</summary>
    public static string? FindRepositoryRoot(string start)
    {
        try
        {
            var dir = new DirectoryInfo(start);
            for (int i = 0; i < 10 && dir is not null; i++, dir = dir.Parent)
            {
                if (File.Exists(Path.Combine(dir.FullName, "FogMachineTimer", "platformio.ini"))) return dir.FullName;
            }
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or ArgumentException)
        {
            // not a checkout
        }
        return null;
    }

    /// <summary>Timers first, then remotes; newest version first.</summary>
    private static int Compare(FirmwareSet a, FirmwareSet b)
    {
        int k = KindRank(a.Kind).CompareTo(KindRank(b.Kind));
        if (k != 0) return k;
        int v = -Versions.Compare(a.Version, b.Version);
        return v != 0 ? v : b.FileTimeUtc.CompareTo(a.FileTimeUtc);
    }

    private static int KindRank(FirmwareKind k) => k switch { FirmwareKind.Timer => 0, FirmwareKind.Remote => 1, _ => 2 };

    private static IEnumerable<string> SafeDirectories(string path)
    {
        try { return Directory.GetDirectories(path); }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { return Array.Empty<string>(); }
    }

    private static IEnumerable<string> SafeFiles(string path, string pattern)
    {
        try { return Directory.GetFiles(path, pattern); }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { return Array.Empty<string>(); }
    }
}

/// <summary>Compares dotted version strings ("2.0.10" is newer than "2.0.9").</summary>
public static class Versions
{
    public static int Compare(string? a, string? b)
    {
        bool pa = TryParse(a, out var va), pb = TryParse(b, out var vb);
        if (pa && pb) return va!.CompareTo(vb);
        if (pa) return 1;
        if (pb) return -1;
        return string.Compare(a, b, StringComparison.Ordinal);
    }

    private static bool TryParse(string? s, out Version? v)
    {
        v = null;
        if (string.IsNullOrWhiteSpace(s)) return false;
        // strip suffixes like "-beta"; System.Version wants at least major.minor
        var core = new string(s.TakeWhile(c => char.IsDigit(c) || c == '.').ToArray()).Trim('.');
        if (core.Length == 0) return false;
        while (core.Count(c => c == '.') < 2) core += ".0";          // "2.1" is "2.1.0"
        return Version.TryParse(core, out v);
    }
}
