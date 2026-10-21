using System.Windows;
using System.Windows.Media;
using Microsoft.Win32;

namespace FogConsole.Services;

public enum EffectiveTheme { Light, Dark }

/// <summary>
/// Applies the Windows Fluent theme (light / dark / follow system) and keeps the application's own palette
/// (<c>Fog.*</c> brushes) in sync with it. All views consume the palette through DynamicResource.
/// </summary>
public sealed class ThemeService
{
    private readonly ResourceDictionary _palette = new();
    private Color _accent = Color.FromRgb(0x00, 0x78, 0xD4);

    public ThemeService()
    {
        SystemEvents.UserPreferenceChanged += OnUserPreferenceChanged;
    }

    public string Mode { get; private set; } = "System";
    public EffectiveTheme Effective { get; private set; } = EffectiveTheme.Dark;
    public ResourceDictionary Palette => _palette;

    public event Action? Changed;

    /// <summary>Call once at startup: adds the palette to the application resources.</summary>
    public void Install(Application app)
    {
        if (!app.Resources.MergedDictionaries.Contains(_palette))
            app.Resources.MergedDictionaries.Add(_palette);
    }

    public void Apply(string? mode)
    {
        Mode = mode is "Light" or "Dark" ? mode : "System";
        var app = Application.Current;
        if (app is not null)
        {
            app.ThemeMode = Mode switch { "Light" => ThemeMode.Light, "Dark" => ThemeMode.Dark, _ => ThemeMode.System };
            foreach (Window w in app.Windows) w.ThemeMode = app.ThemeMode;
        }

        Effective = Mode switch
        {
            "Light" => EffectiveTheme.Light,
            "Dark" => EffectiveTheme.Dark,
            _ => SystemUsesLightTheme() ? EffectiveTheme.Light : EffectiveTheme.Dark,
        };
        _accent = ReadSystemAccent();
        BuildPalette(Effective);
        Changed?.Invoke();
    }

    private void OnUserPreferenceChanged(object? sender, UserPreferenceChangedEventArgs e)
    {
        if (Mode != "System" || Application.Current is null) return;
        if (e.Category is UserPreferenceCategory.General or UserPreferenceCategory.Color or UserPreferenceCategory.VisualStyle)
            Application.Current.Dispatcher.BeginInvoke(() => Apply("System"));
    }

    private static bool SystemUsesLightTheme()
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(@"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize");
            return key?.GetValue("AppsUseLightTheme") is int v && v != 0;
        }
        catch
        {
            return false;
        }
    }

    private static Color ReadSystemAccent()
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(@"Software\Microsoft\Windows\DWM");
            if (key?.GetValue("AccentColor") is int abgr)
            {
                // stored as 0xAABBGGRR
                return Color.FromRgb((byte)(abgr & 0xFF), (byte)((abgr >> 8) & 0xFF), (byte)((abgr >> 16) & 0xFF));
            }
        }
        catch
        {
            // fall through
        }
        return Color.FromRgb(0x00, 0x78, 0xD4);
    }

    private static Color Blend(Color a, Color b, double t) => Color.FromRgb(
        (byte)Math.Round(a.R + (b.R - a.R) * t),
        (byte)Math.Round(a.G + (b.G - a.G) * t),
        (byte)Math.Round(a.B + (b.B - a.B) * t));

    private static double Luminance(Color c) => (0.2126 * c.R + 0.7152 * c.G + 0.0722 * c.B) / 255.0;

    private static Color Hex(string hex) => (Color)ColorConverter.ConvertFromString(hex);

    private void Set(string key, Color c, double opacity = 1.0)
    {
        var brush = new SolidColorBrush(Color.FromArgb((byte)Math.Round(255 * opacity), c.R, c.G, c.B));
        brush.Freeze();
        _palette[key] = brush;
        _palette[key + ".Color"] = c;
    }

    private void BuildPalette(EffectiveTheme theme)
    {
        bool dark = theme == EffectiveTheme.Dark;

        // one accent colour: the Windows accent, adjusted so that it stays readable on the current background
        var accent = _accent;
        if (dark && Luminance(accent) < 0.55) accent = Blend(accent, Colors.White, 0.38);
        if (!dark && Luminance(accent) > 0.45) accent = Blend(accent, Colors.Black, 0.30);
        var onAccent = Luminance(accent) > 0.6 ? Hex("#111111") : Colors.White;

        if (dark)
        {
            Set("Fog.Background", Hex("#1A1A1C"));
            Set("Fog.Surface", Hex("#262629"));
            Set("Fog.SurfaceAlt", Hex("#2F2F33"));
            Set("Fog.SurfaceRaised", Hex("#2B2B2F"));
            Set("Fog.Border", Hex("#39393E"));
            Set("Fog.BorderStrong", Hex("#55555C"));
            Set("Fog.Text", Hex("#F3F3F5"));
            Set("Fog.TextDim", Hex("#B9B9C0"));
            Set("Fog.TextFaint", Hex("#8C8C94"));
            Set("Fog.Track", Hex("#3A3A40"));
            Set("Fog.Hover", Colors.White, 0.07);
            Set("Fog.Ok", Hex("#6CCB5F"));
            Set("Fog.OkSoft", Hex("#6CCB5F"), 0.16);
            Set("Fog.Warn", Hex("#FFC83D"));
            Set("Fog.WarnSoft", Hex("#FFC83D"), 0.16);
            Set("Fog.Bad", Hex("#FF7B7B"));
            Set("Fog.BadSoft", Hex("#FF7B7B"), 0.18);
            Set("Fog.Info", Hex("#7FB2FF"));
            Set("Fog.InfoSoft", Hex("#7FB2FF"), 0.16);
            Set("Fog.Sim", Hex("#D58BFF"));
            Set("Fog.SimSoft", Hex("#D58BFF"), 0.16);
            Set("Fog.Danger", Hex("#E5484D"));
            Set("Fog.HeatQuiet", Hex("#6CCB5F"));
            Set("Fog.HeatBusy", Hex("#FFC83D"));
            Set("Fog.HeatJam", Hex("#FF6B6B"));
            Set("Fog.Shadow", Colors.Black, 0.5);
        }
        else
        {
            Set("Fog.Background", Hex("#F1F1F3"));
            Set("Fog.Surface", Hex("#FFFFFF"));
            Set("Fog.SurfaceAlt", Hex("#F6F6F8"));
            Set("Fog.SurfaceRaised", Hex("#FFFFFF"));
            Set("Fog.Border", Hex("#DEDEE3"));
            Set("Fog.BorderStrong", Hex("#B4B4BC"));
            Set("Fog.Text", Hex("#1B1B1F"));
            Set("Fog.TextDim", Hex("#505058"));
            Set("Fog.TextFaint", Hex("#76767F"));
            Set("Fog.Track", Hex("#E1E1E6"));
            Set("Fog.Hover", Colors.Black, 0.05);
            Set("Fog.Ok", Hex("#12700F"));
            Set("Fog.OkSoft", Hex("#12700F"), 0.12);
            Set("Fog.Warn", Hex("#8F5400"));
            Set("Fog.WarnSoft", Hex("#E39A00"), 0.18);
            Set("Fog.Bad", Hex("#BC2F2B"));
            Set("Fog.BadSoft", Hex("#BC2F2B"), 0.12);
            Set("Fog.Info", Hex("#1A56B0"));
            Set("Fog.InfoSoft", Hex("#1A56B0"), 0.11);
            Set("Fog.Sim", Hex("#7B2FBF"));
            Set("Fog.SimSoft", Hex("#7B2FBF"), 0.11);
            Set("Fog.Danger", Hex("#C42B1C"));
            // bars and fills (no text on them): brighter than the text colours above
            Set("Fog.HeatQuiet", Hex("#43B24A"));
            Set("Fog.HeatBusy", Hex("#F5A400"));
            Set("Fog.HeatJam", Hex("#E5484D"));
            Set("Fog.Shadow", Colors.Black, 0.18);
        }

        Set("Fog.Accent", accent);
        Set("Fog.AccentSoft", accent, dark ? 0.20 : 0.14);
        Set("Fog.AccentStrong", dark ? Blend(accent, Colors.White, 0.2) : Blend(accent, Colors.Black, 0.15));
        Set("Fog.OnAccent", onAccent);
        Set("Fog.Selected", accent, dark ? 0.22 : 0.13);
        Set("Fog.DangerText", Colors.White);
    }
}
