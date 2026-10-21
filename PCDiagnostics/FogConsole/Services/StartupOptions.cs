using System.Globalization;
using FogConsole.ViewModels;

namespace FogConsole.Services;

/// <summary>Command line: <c>FogConsole.exe --sim [--seed N] [--page Name] [--select N] [--shot out.png] [--delay secs] [--exit] [--theme light|dark]</c></summary>
public sealed record StartupOptions
{
    public bool Sim { get; init; }
    public bool SimTimer { get; init; }
    public int? Seed { get; init; }
    public int? Paired { get; init; }
    public NavPage? Page { get; init; }
    public int? Select { get; init; }
    public string? Shot { get; init; }
    public double Delay { get; init; } = 4;
    public bool Exit { get; init; }
    public string? Theme { get; init; }
    public int? Width { get; init; }
    public int? Height { get; init; }
    public bool Help { get; init; }
    public bool Demo { get; init; } = true;
    public bool NoSettings { get; init; }
    public string? Action { get; init; }
    public double? Scroll { get; init; }
    public double? ShotScale { get; init; }
    public bool NoChaos { get; init; }
    public IReadOnlyList<string> Problems { get; init; } = Array.Empty<string>();

    public bool IsScreenshot => !string.IsNullOrEmpty(Shot);

    public const string Usage =
        "FogConsole.exe [--sim] [--sim-timer] [--seed N] [--paired N] [--page Dashboard|Device|Pairing|Channels|Diagnostics|Console|Settings]\n" +
        "               [--select N] [--shot out.png [--shot-scale 1]] [--delay secs] [--exit] [--theme light|dark|system] [--size WxH]\n" +
        "               [--no-chaos] [--no-demo] [--no-settings] [--action name] [--scroll px]";

    public static StartupOptions Parse(IEnumerable<string> args)
    {
        var o = new StartupOptions();
        var problems = new List<string>();
        var list = args.ToList();

        string? Next(ref int i, string name)
        {
            if (i + 1 >= list.Count)
            {
                problems.Add($"{name} needs a value");
                return null;
            }
            return list[++i];
        }

        for (int i = 0; i < list.Count; i++)
        {
            string a = list[i];
            string key = a.ToLowerInvariant();
            switch (key)
            {
                case "--sim": o = o with { Sim = true }; break;
                case "--sim-timer": o = o with { Sim = true, SimTimer = true }; break;
                case "--exit": o = o with { Exit = true }; break;
                case "--no-demo": o = o with { Demo = false }; break;
                case "--no-chaos": o = o with { NoChaos = true }; break;
                case "--no-settings": o = o with { NoSettings = true }; break;
                case "--help" or "-h" or "/?": o = o with { Help = true }; break;
                case "--seed":
                    if (Next(ref i, a) is { } sv)
                    {
                        if (int.TryParse(sv, NumberStyles.Integer, CultureInfo.InvariantCulture, out var seed)) o = o with { Seed = seed };
                        else problems.Add($"--seed: '{sv}' is not a number");
                    }
                    break;
                case "--paired":
                    if (Next(ref i, a) is { } pv)
                    {
                        if (int.TryParse(pv, NumberStyles.Integer, CultureInfo.InvariantCulture, out var n) && n is >= 0 and <= 10) o = o with { Paired = n };
                        else problems.Add($"--paired: '{pv}' must be 0 ... 10");
                    }
                    break;
                case "--select":
                    if (Next(ref i, a) is { } sel)
                    {
                        if (int.TryParse(sel, NumberStyles.Integer, CultureInfo.InvariantCulture, out var n) && n >= 0) o = o with { Select = n };
                        else problems.Add($"--select: '{sel}' is not a device index");
                    }
                    break;
                case "--page":
                    if (Next(ref i, a) is { } pg)
                    {
                        if (Enum.TryParse<NavPage>(pg, ignoreCase: true, out var page)) o = o with { Page = page };
                        else problems.Add($"--page: '{pg}' is not a page (Dashboard, Device, Pairing, Channels, Diagnostics, Console, Settings)");
                    }
                    break;
                case "--shot":
                    if (Next(ref i, a) is { } shot) o = o with { Shot = shot };
                    break;
                case "--delay":
                    if (Next(ref i, a) is { } dv)
                    {
                        if (double.TryParse(dv, NumberStyles.Float, CultureInfo.InvariantCulture, out var d) && d >= 0) o = o with { Delay = d };
                        else problems.Add($"--delay: '{dv}' is not a number of seconds");
                    }
                    break;
                case "--theme":
                    if (Next(ref i, a) is { } th)
                    {
                        string t = th.ToLowerInvariant();
                        if (t is "light" or "dark" or "system") o = o with { Theme = char.ToUpperInvariant(t[0]) + t[1..] };
                        else problems.Add($"--theme: '{th}' must be light, dark or system");
                    }
                    break;
                case "--size":
                    if (Next(ref i, a) is { } sz)
                    {
                        var parts = sz.ToLowerInvariant().Split('x');
                        if (parts.Length == 2 && int.TryParse(parts[0], out var w) && int.TryParse(parts[1], out var h) && w >= 400 && h >= 300)
                            o = o with { Width = w, Height = h };
                        else problems.Add($"--size: '{sz}' must look like 1280x800");
                    }
                    break;
                case "--shot-scale":
                    if (Next(ref i, a) is { } ssv)
                    {
                        if (double.TryParse(ssv, NumberStyles.Float, CultureInfo.InvariantCulture, out var ss) && ss is > 0 and <= 4) o = o with { ShotScale = ss };
                        else problems.Add($"--shot-scale: '{ssv}' must be a factor between 0 and 4 (1 = one pixel per device independent pixel)");
                    }
                    break;
                case "--scroll":
                    if (Next(ref i, a) is { } scv)
                    {
                        if (double.TryParse(scv, NumberStyles.Float, CultureInfo.InvariantCulture, out var sc) && sc >= 0) o = o with { Scroll = sc };
                        else problems.Add($"--scroll: '{scv}' is not a number of pixels");
                    }
                    break;
                case "--action":
                    if (Next(ref i, a) is { } act) o = o with { Action = act };
                    break;
                default:
                    problems.Add($"unknown argument '{a}'");
                    break;
            }
        }
        return o with { Problems = problems };
    }
}
