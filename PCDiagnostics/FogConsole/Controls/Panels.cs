using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;

namespace FogConsole.Controls;

/// <summary>
/// Lays children out in as many equally wide columns as fit (each at least <see cref="MinItemWidth"/> wide) and
/// stretches them to fill the row. Items of one row share the height of the tallest one.
/// </summary>
public sealed class ReflowPanel : Panel
{
    public static readonly DependencyProperty MinItemWidthProperty = DependencyProperty.Register(nameof(MinItemWidth), typeof(double), typeof(ReflowPanel),
        new FrameworkPropertyMetadata(300.0, FrameworkPropertyMetadataOptions.AffectsMeasure));
    public static readonly DependencyProperty MaxItemWidthProperty = DependencyProperty.Register(nameof(MaxItemWidth), typeof(double), typeof(ReflowPanel),
        new FrameworkPropertyMetadata(480.0, FrameworkPropertyMetadataOptions.AffectsMeasure));
    public static readonly DependencyProperty SpacingProperty = DependencyProperty.Register(nameof(Spacing), typeof(double), typeof(ReflowPanel),
        new FrameworkPropertyMetadata(12.0, FrameworkPropertyMetadataOptions.AffectsMeasure));

    public double MinItemWidth { get => (double)GetValue(MinItemWidthProperty); set => SetValue(MinItemWidthProperty, value); }
    public double MaxItemWidth { get => (double)GetValue(MaxItemWidthProperty); set => SetValue(MaxItemWidthProperty, value); }
    public double Spacing { get => (double)GetValue(SpacingProperty); set => SetValue(SpacingProperty, value); }

    private (int Columns, double ItemWidth) Plan(double available)
    {
        if (double.IsInfinity(available) || available <= 0) return (1, MinItemWidth);
        int n = Math.Max(1, InternalChildren.Count);
        int maxCols = Math.Max(1, (int)Math.Floor((available + Spacing) / (MinItemWidth + Spacing)));
        int cols = Math.Min(maxCols, n);
        // balance the rows: 7 cards on a 5-column screen become 4 + 3 instead of 5 + 2
        int rows = (int)Math.Ceiling(n / (double)cols);
        cols = (int)Math.Ceiling(n / (double)rows);
        double itemW = (available - Spacing * (cols - 1)) / cols;
        if (itemW > MaxItemWidth) itemW = MaxItemWidth;   // a few cards on a very wide window keep a sensible width
        return (cols, Math.Max(itemW, 100));
    }

    protected override Size MeasureOverride(Size availableSize)
    {
        var (cols, itemW) = Plan(availableSize.Width);
        double totalH = 0;
        int n = InternalChildren.Count;
        for (int row = 0; row * cols < n; row++)
        {
            double rowH = 0;
            for (int c = 0; c < cols && row * cols + c < n; c++)
            {
                var child = InternalChildren[row * cols + c];
                child.Measure(new Size(itemW, double.PositiveInfinity));
                rowH = Math.Max(rowH, child.DesiredSize.Height);
            }
            totalH += rowH + (row > 0 ? Spacing : 0);
        }
        double width = double.IsInfinity(availableSize.Width) ? cols * itemW + (cols - 1) * Spacing : availableSize.Width;
        return new Size(width, totalH);
    }

    protected override Size ArrangeOverride(Size finalSize)
    {
        var (cols, itemW) = Plan(finalSize.Width);
        double y = 0;
        int n = InternalChildren.Count;
        for (int row = 0; row * cols < n; row++)
        {
            double rowH = 0;
            for (int c = 0; c < cols && row * cols + c < n; c++)
                rowH = Math.Max(rowH, InternalChildren[row * cols + c].DesiredSize.Height);
            for (int c = 0; c < cols && row * cols + c < n; c++)
                InternalChildren[row * cols + c].Arrange(new Rect(c * (itemW + Spacing), y, itemW, rowH));
            y += rowH + Spacing;
        }
        return finalSize;
    }
}

/// <summary>
/// Turns any button into a press-and-hold button: <see cref="StartCommand"/> runs when the mouse / Space key goes down,
/// <see cref="EndCommand"/> when it is released, loses capture, loses focus, or the window is deactivated.
/// </summary>
public static class HoldBehavior
{
    public static readonly DependencyProperty StartCommandProperty = DependencyProperty.RegisterAttached("StartCommand", typeof(ICommand), typeof(HoldBehavior),
        new PropertyMetadata(null, OnCommandChanged));
    public static readonly DependencyProperty EndCommandProperty = DependencyProperty.RegisterAttached("EndCommand", typeof(ICommand), typeof(HoldBehavior),
        new PropertyMetadata(null, OnCommandChanged));
    public static readonly DependencyProperty ParameterProperty = DependencyProperty.RegisterAttached("Parameter", typeof(object), typeof(HoldBehavior), new PropertyMetadata(null));

    private static readonly DependencyPropertyKey IsHoldingKey = DependencyProperty.RegisterAttachedReadOnly("IsHolding", typeof(bool), typeof(HoldBehavior), new PropertyMetadata(false));
    public static readonly DependencyProperty IsHoldingProperty = IsHoldingKey.DependencyProperty;

    public static ICommand? GetStartCommand(DependencyObject d) => (ICommand?)d.GetValue(StartCommandProperty);
    public static void SetStartCommand(DependencyObject d, ICommand? v) => d.SetValue(StartCommandProperty, v);
    public static ICommand? GetEndCommand(DependencyObject d) => (ICommand?)d.GetValue(EndCommandProperty);
    public static void SetEndCommand(DependencyObject d, ICommand? v) => d.SetValue(EndCommandProperty, v);
    public static object? GetParameter(DependencyObject d) => d.GetValue(ParameterProperty);
    public static void SetParameter(DependencyObject d, object? v) => d.SetValue(ParameterProperty, v);
    public static bool GetIsHolding(DependencyObject d) => (bool)d.GetValue(IsHoldingProperty);

    private static void OnCommandChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is not UIElement el) return;
        el.PreviewMouseLeftButtonDown -= OnMouseDown;
        el.PreviewMouseLeftButtonUp -= OnMouseUp;
        el.LostMouseCapture -= OnLostCapture;
        el.PreviewKeyDown -= OnKeyDown;
        el.PreviewKeyUp -= OnKeyUp;
        el.LostKeyboardFocus -= OnLostFocus;
        el.IsEnabledChanged -= OnEnabledChanged;
        if (GetStartCommand(d) is null && GetEndCommand(d) is null) return;
        el.PreviewMouseLeftButtonDown += OnMouseDown;
        el.PreviewMouseLeftButtonUp += OnMouseUp;
        el.LostMouseCapture += OnLostCapture;
        el.PreviewKeyDown += OnKeyDown;
        el.PreviewKeyUp += OnKeyUp;
        el.LostKeyboardFocus += OnLostFocus;
        el.IsEnabledChanged += OnEnabledChanged;
    }

    private static void Begin(UIElement el)
    {
        if (GetIsHolding(el) || !el.IsEnabled) return;
        el.SetValue(IsHoldingKey, true);
        var p = GetParameter(el);
        var cmd = GetStartCommand(el);
        if (cmd?.CanExecute(p) == true) cmd.Execute(p);
    }

    private static void End(UIElement el)
    {
        if (!GetIsHolding(el)) return;
        el.SetValue(IsHoldingKey, false);
        var p = GetParameter(el);
        var cmd = GetEndCommand(el);
        if (cmd?.CanExecute(p) == true) cmd.Execute(p);
    }

    private static void OnMouseDown(object sender, MouseButtonEventArgs e)
    {
        var el = (UIElement)sender;
        el.CaptureMouse();
        Begin(el);
        e.Handled = true;
    }

    private static void OnMouseUp(object sender, MouseButtonEventArgs e)
    {
        var el = (UIElement)sender;
        End(el);
        if (el.IsMouseCaptured) el.ReleaseMouseCapture();
        e.Handled = true;
    }

    private static void OnLostCapture(object sender, MouseEventArgs e) => End((UIElement)sender);

    private static void OnKeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key != Key.Space) return;
        if (!e.IsRepeat) Begin((UIElement)sender);
        e.Handled = true;
    }

    private static void OnKeyUp(object sender, KeyEventArgs e)
    {
        if (e.Key != Key.Space) return;
        End((UIElement)sender);
        e.Handled = true;
    }

    private static void OnLostFocus(object sender, KeyboardFocusChangedEventArgs e) => End((UIElement)sender);

    private static void OnEnabledChanged(object sender, DependencyPropertyChangedEventArgs e)
    {
        if (e.NewValue is false) End((UIElement)sender);
    }
}
