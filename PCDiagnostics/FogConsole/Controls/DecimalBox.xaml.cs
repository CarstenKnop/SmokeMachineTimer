using System.Globalization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;

namespace FogConsole.Controls;

/// <summary>A number box with +/- buttons (pure view logic: parsing, clamping, rounding to one step).</summary>
public partial class DecimalBox : UserControl
{
    public static readonly DependencyProperty ValueProperty = DependencyProperty.Register(nameof(Value), typeof(double), typeof(DecimalBox),
        new FrameworkPropertyMetadata(0.0, FrameworkPropertyMetadataOptions.BindsTwoWayByDefault, (d, _) => ((DecimalBox)d).ShowValue()));
    public static readonly DependencyProperty MinimumProperty = DependencyProperty.Register(nameof(Minimum), typeof(double), typeof(DecimalBox), new PropertyMetadata(0.1));
    public static readonly DependencyProperty MaximumProperty = DependencyProperty.Register(nameof(Maximum), typeof(double), typeof(DecimalBox), new PropertyMetadata(9999.9));
    public static readonly DependencyProperty StepProperty = DependencyProperty.Register(nameof(Step), typeof(double), typeof(DecimalBox), new PropertyMetadata(0.1));
    public static readonly DependencyProperty DecimalsProperty = DependencyProperty.Register(nameof(Decimals), typeof(int), typeof(DecimalBox),
        new PropertyMetadata(1, (d, _) => ((DecimalBox)d).ShowValue()));
    public static readonly DependencyProperty SuffixProperty = DependencyProperty.Register(nameof(Suffix), typeof(string), typeof(DecimalBox), new PropertyMetadata("s"));

    public double Value { get => (double)GetValue(ValueProperty); set => SetValue(ValueProperty, value); }
    public double Minimum { get => (double)GetValue(MinimumProperty); set => SetValue(MinimumProperty, value); }
    public double Maximum { get => (double)GetValue(MaximumProperty); set => SetValue(MaximumProperty, value); }
    public double Step { get => (double)GetValue(StepProperty); set => SetValue(StepProperty, value); }
    public int Decimals { get => (int)GetValue(DecimalsProperty); set => SetValue(DecimalsProperty, value); }
    public string Suffix { get => (string)GetValue(SuffixProperty); set => SetValue(SuffixProperty, value); }

    public DecimalBox()
    {
        InitializeComponent();
        Loaded += (_, _) => ShowValue();
        IsEnabledChanged += (_, _) => SuffixText.Opacity = IsEnabled ? 1 : 0.5;
    }

    private bool _editing;

    private void ShowValue()
    {
        if (Box is null || _editing) return;
        Box.Text = Value.ToString("F" + Decimals, CultureInfo.CurrentCulture);
    }

    private double Normalize(double v)
    {
        if (double.IsNaN(v) || double.IsInfinity(v)) v = Value;
        v = Math.Round(v / Step, MidpointRounding.AwayFromZero) * Step;
        v = Math.Round(v, Math.Max(Decimals, 0));
        return Math.Clamp(v, Minimum, Maximum);
    }

    private void Commit()
    {
        _editing = false;
        if (double.TryParse(Box.Text.Replace(',', '.'), NumberStyles.Float, CultureInfo.InvariantCulture, out var parsed)
            || double.TryParse(Box.Text, NumberStyles.Float, CultureInfo.CurrentCulture, out parsed))
            Value = Normalize(parsed);
        ShowValue();
    }

    private void Nudge(int direction, bool big)
    {
        _editing = false;
        double step = Step * (big ? 10 : 1);
        Value = Normalize(Value + direction * step);
        ShowValue();
    }

    private void Box_GotKeyboardFocus(object sender, KeyboardFocusChangedEventArgs e)
    {
        _editing = true;
        Box.SelectAll();
    }

    private void Box_LostKeyboardFocus(object sender, KeyboardFocusChangedEventArgs e) => Commit();

    private void Box_PreviewKeyDown(object sender, KeyEventArgs e)
    {
        bool big = (Keyboard.Modifiers & ModifierKeys.Shift) != 0;
        switch (e.Key)
        {
            case Key.Up: Nudge(+1, big); e.Handled = true; break;
            case Key.Down: Nudge(-1, big); e.Handled = true; break;
            case Key.Enter: Commit(); Box.SelectAll(); e.Handled = true; break;
            case Key.Escape: _editing = false; ShowValue(); e.Handled = true; break;
        }
    }

    private void Minus_Click(object sender, RoutedEventArgs e) => Nudge(-1, (Keyboard.Modifiers & ModifierKeys.Shift) != 0);
    private void Plus_Click(object sender, RoutedEventArgs e) => Nudge(+1, (Keyboard.Modifiers & ModifierKeys.Shift) != 0);
}
