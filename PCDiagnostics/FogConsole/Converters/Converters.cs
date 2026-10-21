using System.Globalization;
using System.Windows;
using System.Windows.Data;

namespace FogConsole.Converters;

/// <summary>Maps a bool to two arbitrary values (Visibility, Brush, string ...).</summary>
public sealed class BoolConverter : IValueConverter
{
    public object? TrueValue { get; set; } = true;
    public object? FalseValue { get; set; } = false;

    public object? Convert(object value, Type targetType, object parameter, CultureInfo culture) =>
        value is true ? TrueValue : FalseValue;

    public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture) =>
        Equals(value, TrueValue);
}

/// <summary>null / empty string / 0 map to <see cref="NullValue"/>, everything else to <see cref="NotNullValue"/>.</summary>
public sealed class NullConverter : IValueConverter
{
    public object? NullValue { get; set; } = Visibility.Collapsed;
    public object? NotNullValue { get; set; } = Visibility.Visible;

    public object? Convert(object? value, Type targetType, object parameter, CultureInfo culture)
    {
        bool isNull = value is null || value is string { Length: 0 } || value is int and 0;
        return isNull ? NullValue : NotNullValue;
    }

    public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture) => throw new NotSupportedException();
}

/// <summary>True when the bound value equals the converter parameter (compared as text).</summary>
public sealed class EqualsConverter : IValueConverter
{
    public object Convert(object? value, Type targetType, object? parameter, CultureInfo culture) =>
        string.Equals(value?.ToString(), parameter?.ToString(), StringComparison.OrdinalIgnoreCase);

    public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture) =>
        value is true ? parameter : Binding.DoNothing;
}

/// <summary>Compares a number with the converter parameter: true when value &gt; parameter.</summary>
public sealed class GreaterThanConverter : IValueConverter
{
    public object Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
        double v = System.Convert.ToDouble(value ?? 0, CultureInfo.InvariantCulture);
        double p = System.Convert.ToDouble(parameter ?? 0, CultureInfo.InvariantCulture);
        return v > p;
    }

    public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture) => throw new NotSupportedException();
}

/// <summary>Combines several bools with AND.</summary>
public sealed class AllTrueConverter : IMultiValueConverter
{
    public object Convert(object[] values, Type targetType, object parameter, CultureInfo culture) =>
        values.All(v => v is true);

    public object[] ConvertBack(object value, Type[] targetTypes, object parameter, CultureInfo culture) => throw new NotSupportedException();
}

/// <summary>Icon glyph (Segoe Fluent Icons) for a health level: check, info, warning, error.</summary>
public sealed class HealthGlyphConverter : IValueConverter
{
    public object Convert(object? value, Type targetType, object? parameter, CultureInfo culture) => value switch
    {
        FogConsole.Diagnostics.HealthLevel.Ok => "\uE73E",
        FogConsole.Diagnostics.HealthLevel.Info => "\uE946",
        FogConsole.Diagnostics.HealthLevel.Warn => "\uE7BA",
        FogConsole.Diagnostics.HealthLevel.Bad => "\uEA39",
        _ => "",
    };

    public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture) => throw new NotSupportedException();
}

public static class Conv
{
    public static readonly HealthGlyphConverter HealthGlyph = new();
    public static readonly BoolConverter BoolToVisibility = new() { TrueValue = Visibility.Visible, FalseValue = Visibility.Collapsed };
    public static readonly BoolConverter InverseBoolToVisibility = new() { TrueValue = Visibility.Collapsed, FalseValue = Visibility.Visible };
    public static readonly BoolConverter Not = new() { TrueValue = false, FalseValue = true };
    public static readonly NullConverter NullToCollapsed = new();
    public static readonly NullConverter NullToVisible = new() { NullValue = Visibility.Visible, NotNullValue = Visibility.Collapsed };
    public static readonly EqualsConverter Equal = new();
    public static readonly GreaterThanConverter GreaterThan = new();
    public static readonly AllTrueConverter AllTrue = new();
}
