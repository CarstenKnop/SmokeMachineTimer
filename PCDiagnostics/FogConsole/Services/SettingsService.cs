using System.IO;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace FogConsole.Services;

public sealed class DeviceAppearance
{
    public string? Nickname { get; set; }
    public string? Color { get; set; }
}

public sealed class TimePreset
{
    public double On { get; set; }
    public double Off { get; set; }

    [JsonIgnore]
    public string Label => $"{Format(On)} / {Format(Off)} s";

    private static string Format(double v) => v % 1 == 0 ? v.ToString("0") : v.ToString("0.0#");

    public override string ToString() => Label;
}

/// <summary>Persisted application settings (%AppData%\FogConsole\settings.json).</summary>
public sealed class AppSettings
{
    public string? LastPort { get; set; }
    public bool LastWasSimulator { get; set; }
    public double? WindowLeft { get; set; }
    public double? WindowTop { get; set; }
    public double? WindowWidth { get; set; }
    public double? WindowHeight { get; set; }
    // note: the window always starts maximized; only the normal (restored) bounds are remembered

    public Dictionary<string, DeviceAppearance> Devices { get; set; } = new();

    /// <summary>System | Light | Dark</summary>
    public string Theme { get; set; } = "System";

    /// <summary>error | warn | info | debug | off</summary>
    public string LogLevel { get; set; } = "info";

    public int LogBufferSize { get; set; } = 5000;
    public int PacketBufferSize { get; set; } = 5000;
    public bool AutoConnect { get; set; } = true;
    public bool AutoReconnect { get; set; } = true;

    public List<TimePreset> CustomPresets { get; set; } = new();

    /// <summary>Remote | Timer</summary>
    public string SimulatorKind { get; set; } = "Remote";

    /// <summary>The key a timer firmware expects in an over-the-air update (must be the one the timers were built with).</summary>
    public string OtaKey { get; set; } = "FogLink-OTA-v1";

    /// <summary>Firmware files or folders the user picked on the Firmware page (offered again next time).</summary>
    public List<string> FirmwarePaths { get; set; } = new();
}

public sealed class SettingsService
{
    private static readonly JsonSerializerOptions Options = new()
    {
        WriteIndented = true,
        PropertyNameCaseInsensitive = true,
        DefaultIgnoreCondition = JsonIgnoreCondition.Never,
    };

    private readonly string _path;
    private readonly object _lock = new();
    private Timer? _saveTimer;

    public SettingsService(string? path = null)
    {
        _path = path ?? DefaultPath;
        Current = Load(_path);
    }

    public static string AppDataDirectory => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "FogConsole");
    public static string DefaultPath => Path.Combine(AppDataDirectory, "settings.json");

    public AppSettings Current { get; }
    public string Path_ => _path;

    public static AppSettings Load(string path)
    {
        try
        {
            if (File.Exists(path))
            {
                var s = JsonSerializer.Deserialize<AppSettings>(File.ReadAllText(path), Options);
                if (s is not null)
                {
                    s.Devices ??= new();
                    s.CustomPresets ??= new();
                    s.FirmwarePaths ??= new();
                    if (string.IsNullOrWhiteSpace(s.OtaKey)) s.OtaKey = "FogLink-OTA-v1";
                    s.LogBufferSize = Math.Clamp(s.LogBufferSize, 500, 100_000);
                    s.PacketBufferSize = Math.Clamp(s.PacketBufferSize, 500, 100_000);
                    return s;
                }
            }
        }
        catch
        {
            // corrupt settings must never prevent the app from starting
        }
        return new AppSettings();
    }

    /// <summary>Writes the settings now.</summary>
    public void Save()
    {
        lock (_lock)
        {
            try
            {
                Directory.CreateDirectory(System.IO.Path.GetDirectoryName(_path)!);
                var tmp = _path + ".tmp";
                File.WriteAllText(tmp, JsonSerializer.Serialize(Current, Options));
                File.Move(tmp, _path, overwrite: true);
            }
            catch
            {
                // read-only profile etc.: settings are a convenience
            }
        }
    }

    /// <summary>Coalesces many changes (typing a nickname) into one write.</summary>
    public void SaveSoon()
    {
        lock (_lock)
        {
            _saveTimer ??= new Timer(_ => Save(), null, Timeout.Infinite, Timeout.Infinite);
            _saveTimer.Change(600, Timeout.Infinite);
        }
    }

    public DeviceAppearance AppearanceOf(string mac)
    {
        mac = mac.ToUpperInvariant();
        if (!Current.Devices.TryGetValue(mac, out var a))
            Current.Devices[mac] = a = new DeviceAppearance();
        return a;
    }
}
