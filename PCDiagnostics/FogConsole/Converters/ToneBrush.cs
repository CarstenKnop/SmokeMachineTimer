using System.Globalization;
using System.Windows;
using System.Windows.Data;
using System.Windows.Markup;
using System.Windows.Media;
using FogConsole.Diagnostics;
using FogConsole.Services;
using FogConsole.ViewModels;

namespace FogConsole.Converters;

public enum Tone { Neutral, Ok, Info, Warn, Bad, Sim, Accent }

public static class Tones
{
    /// <summary>Maps any of the status enums used by the view models to a colour tone.</summary>
    public static Tone From(object? value) => value switch
    {
        Tone t => t,
        ChipLevel c => c switch { ChipLevel.Ok => Tone.Ok, ChipLevel.Warn => Tone.Warn, ChipLevel.Bad => Tone.Bad, ChipLevel.Sim => Tone.Sim, _ => Tone.Neutral },
        HealthLevel h => h switch { HealthLevel.Ok => Tone.Ok, HealthLevel.Info => Tone.Info, HealthLevel.Warn => Tone.Warn, _ => Tone.Bad },
        LinkQuality q => q switch { LinkQuality.Good => Tone.Ok, LinkQuality.Fair => Tone.Warn, LinkQuality.Poor => Tone.Bad, _ => Tone.Neutral },
        PillState p => p switch { PillState.On => Tone.Accent, PillState.ForceOn => Tone.Bad, PillState.Paused => Tone.Warn, _ => Tone.Neutral },
        EntryStatus e => e switch { EntryStatus.Ok => Tone.Ok, EntryStatus.Warning => Tone.Warn, EntryStatus.Failed => Tone.Bad, _ => Tone.Neutral },
        ToastKind k => k switch { ToastKind.Success => Tone.Ok, ToastKind.Warning => Tone.Warn, ToastKind.Error => Tone.Bad, _ => Tone.Info },
        MigrationStatus m => m switch { MigrationStatus.Confirmed => Tone.Ok, MigrationStatus.Failed => Tone.Bad, _ => Tone.Neutral },
        Protocol.LogLevel l => l switch { Protocol.LogLevel.Error => Tone.Bad, Protocol.LogLevel.Warn => Tone.Warn, Protocol.LogLevel.Info => Tone.Neutral, _ => Tone.Neutral },
        bool b => b ? Tone.Ok : Tone.Neutral,
        _ => Tone.Neutral,
    };

    public static string Key(Tone tone, string part) => (tone, part) switch
    {
        (Tone.Neutral, "Fg") => "Fog.TextDim",
        (Tone.Neutral, "Bg") => "Fog.SurfaceAlt",
        (Tone.Neutral, "Border") => "Fog.Border",
        (Tone.Neutral, "Solid") => "Fog.BorderStrong",
        (Tone.Accent, "Fg") => "Fog.Accent",
        (Tone.Accent, "Bg") => "Fog.AccentSoft",
        (Tone.Accent, "Border") => "Fog.Accent",
        (Tone.Accent, "Solid") => "Fog.Accent",
        (Tone.Bad, "Solid") => "Fog.Danger",
        (_, "Fg") => $"Fog.{tone}",
        (_, "Bg") => $"Fog.{tone}Soft",
        (_, "Border") => $"Fog.{tone}",
        (_, "Solid") => $"Fog.{tone}",
        _ => "Fog.TextDim",
    };
}

/// <summary>values[0] = status value; values[1] = theme token (only there to re-evaluate when the theme changes).</summary>
public sealed class ToneBrushConverter : IMultiValueConverter
{
    public static readonly ToneBrushConverter Instance = new();

    public object Convert(object[] values, Type targetType, object parameter, CultureInfo culture)
    {
        var tone = Tones.From(values.Length > 0 ? values[0] : null);
        string part = parameter as string ?? "Fg";
        var key = Tones.Key(tone, part);
        return Application.Current?.TryFindResource(key) as Brush ?? Brushes.Gray;
    }

    public object[] ConvertBack(object value, Type[] targetTypes, object parameter, CultureInfo culture) => throw new NotSupportedException();
}

/// <summary>
/// <c>{c:ToneBrush Path=Level, Part=Bg}</c>: a brush that follows a status property of the view model (and the theme).
/// Parts: Fg, Bg, Border, Solid.
/// </summary>
[MarkupExtensionReturnType(typeof(Brush))]
public sealed class ToneBrushExtension : MarkupExtension
{
    public ToneBrushExtension() { }
    public ToneBrushExtension(string path) => Path = path;

    public string Path { get; set; } = "";
    public string Part { get; set; } = "Fg";

    public override object ProvideValue(IServiceProvider serviceProvider)
    {
        var mb = new MultiBinding { Converter = ToneBrushConverter.Instance, ConverterParameter = Part };
        mb.Bindings.Add(new Binding(Path));
        mb.Bindings.Add(new Binding("DataContext.IsDarkTheme")
        {
            RelativeSource = new RelativeSource(RelativeSourceMode.FindAncestor, typeof(Window), 1),
        });
        return mb.ProvideValue(serviceProvider);
    }
}
