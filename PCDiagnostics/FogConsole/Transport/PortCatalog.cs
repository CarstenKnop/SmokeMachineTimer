using System.IO.Ports;
using System.Text.RegularExpressions;

namespace FogConsole.Transport;

/// <param name="IsUsbJtag">An ESP32 with the built-in USB-Serial/JTAG controller (USB id 303A:1001), like the XIAO ESP32-C3.</param>
public sealed record PortInfo(string Name, string Description, bool IsEspressif, bool IsUsbJtag = false)
{
    public string Display => string.IsNullOrWhiteSpace(Description) || Description.Contains(Name, StringComparison.OrdinalIgnoreCase)
        ? (IsEspressif ? $"{Name}  -  ESP32 (USB)" : Name)
        : $"{Name}  -  {Description}";

    public override string ToString() => Display;
}

/// <summary>Lists serial ports with friendly names (via WMI; the Espressif USB vendor id is 303A).</summary>
public static partial class PortCatalog
{
    public static Task<IReadOnlyList<PortInfo>> GetPortsAsync() => Task.Run(GetPorts);

    public static IReadOnlyList<PortInfo> GetPorts()
    {
        string[] names;
        try { names = SerialPort.GetPortNames(); }
        catch { names = []; }

        var friendly = new Dictionary<string, (string Desc, bool Esp, bool Jtag)>(StringComparer.OrdinalIgnoreCase);
        try
        {
            using var searcher = new System.Management.ManagementObjectSearcher(
                "SELECT Name, PNPDeviceID FROM Win32_PnPEntity WHERE Name LIKE '%(COM%)'");
            foreach (System.Management.ManagementBaseObject o in searcher.Get())
            {
                var name = o["Name"] as string ?? "";
                var pnp = o["PNPDeviceID"] as string ?? "";
                var m = ComRegex().Match(name);
                if (!m.Success) continue;
                var com = m.Groups[1].Value;
                var desc = name.Replace($"({com})", "", StringComparison.OrdinalIgnoreCase).Trim();
                friendly[com] = (desc, pnp.Contains("VID_303A", StringComparison.OrdinalIgnoreCase),
                    pnp.Contains("VID_303A&PID_1001", StringComparison.OrdinalIgnoreCase));
            }
        }
        catch
        {
            // WMI is optional; plain port names are fine.
        }

        return names
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .OrderBy(NaturalKey)
            .Select(n => friendly.TryGetValue(n, out var f) ? new PortInfo(n, f.Desc, f.Esp, f.Jtag) : new PortInfo(n, "", false))
            .ToList();
    }

    private static int NaturalKey(string name)
    {
        var m = DigitsRegex().Match(name);
        return m.Success && int.TryParse(m.Value, out var n) ? n : int.MaxValue;
    }

    [GeneratedRegex(@"\((COM\d+)\)", RegexOptions.IgnoreCase)]
    private static partial Regex ComRegex();

    [GeneratedRegex(@"\d+")]
    private static partial Regex DigitsRegex();
}
