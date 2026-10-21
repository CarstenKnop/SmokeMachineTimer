using System.Globalization;
using System.Windows;
using System.Windows.Media;

namespace FogConsole.Controls;

internal static class Draw
{
    public static readonly Typeface UiFont = new(new FontFamily("Segoe UI Variable Text, Segoe UI"), FontStyles.Normal, FontWeights.Normal, FontStretches.Normal);
    public static readonly Typeface UiFontBold = new(new FontFamily("Segoe UI Variable Text, Segoe UI"), FontStyles.Normal, FontWeights.SemiBold, FontStretches.Normal);
    public static readonly Typeface MonoFont = new(new FontFamily("Cascadia Mono, Consolas"), FontStyles.Normal, FontWeights.Normal, FontStretches.Normal);

    public static FormattedText Text(Visual owner, string text, double size, Brush brush, Typeface? face = null)
    {
        return new FormattedText(text, CultureInfo.CurrentUICulture, FlowDirection.LeftToRight, face ?? UiFont, size, brush,
            VisualTreeHelper.GetDpi(owner).PixelsPerDip);
    }

    public static Color ColorOf(Brush? b, Color fallback) => b is SolidColorBrush s ? s.Color : fallback;

    public static Color Blend(Color a, Color b, double t)
    {
        t = Math.Clamp(t, 0, 1);
        return Color.FromArgb(
            (byte)Math.Round(a.A + (b.A - a.A) * t),
            (byte)Math.Round(a.R + (b.R - a.R) * t),
            (byte)Math.Round(a.G + (b.G - a.G) * t),
            (byte)Math.Round(a.B + (b.B - a.B) * t));
    }

    public static Pen RoundPen(Brush? brush, double thickness)
    {
        var p = new Pen(brush, thickness) { StartLineCap = PenLineCap.Round, EndLineCap = PenLineCap.Round, LineJoin = PenLineJoin.Round };
        return p;
    }
}

/// <summary>A circular progress ring (progress through the current phase of a timer).</summary>
public sealed class ArcProgress : FrameworkElement
{
    public static readonly DependencyProperty ValueProperty = DependencyProperty.Register(nameof(Value), typeof(double), typeof(ArcProgress),
        new FrameworkPropertyMetadata(0.0, FrameworkPropertyMetadataOptions.AffectsRender));
    public static readonly DependencyProperty ThicknessProperty = DependencyProperty.Register(nameof(Thickness), typeof(double), typeof(ArcProgress),
        new FrameworkPropertyMetadata(8.0, FrameworkPropertyMetadataOptions.AffectsRender));
    public static readonly DependencyProperty BrushProperty = DependencyProperty.Register(nameof(Brush), typeof(Brush), typeof(ArcProgress),
        new FrameworkPropertyMetadata(Brushes.DodgerBlue, FrameworkPropertyMetadataOptions.AffectsRender));
    public static readonly DependencyProperty TrackBrushProperty = DependencyProperty.Register(nameof(TrackBrush), typeof(Brush), typeof(ArcProgress),
        new FrameworkPropertyMetadata(Brushes.Gray, FrameworkPropertyMetadataOptions.AffectsRender));

    public double Value { get => (double)GetValue(ValueProperty); set => SetValue(ValueProperty, value); }
    public double Thickness { get => (double)GetValue(ThicknessProperty); set => SetValue(ThicknessProperty, value); }
    public Brush Brush { get => (Brush)GetValue(BrushProperty); set => SetValue(BrushProperty, value); }
    public Brush TrackBrush { get => (Brush)GetValue(TrackBrushProperty); set => SetValue(TrackBrushProperty, value); }

    protected override void OnRender(DrawingContext dc)
    {
        double size = Math.Min(ActualWidth, ActualHeight);
        if (size <= 2) return;
        double t = Thickness;
        double r = (size - t) / 2;
        var c = new Point(ActualWidth / 2, ActualHeight / 2);
        dc.DrawEllipse(null, new Pen(TrackBrush, t), c, r, r);

        double v = Math.Clamp(Value, 0, 1);
        if (v <= 0.002) return;
        var pen = Draw.RoundPen(Brush, t);
        if (v >= 0.998)
        {
            dc.DrawEllipse(null, pen, c, r, r);
            return;
        }
        double a = v * 2 * Math.PI;
        var start = new Point(c.X, c.Y - r);
        var end = new Point(c.X + r * Math.Sin(a), c.Y - r * Math.Cos(a));
        var g = new StreamGeometry();
        using (var ctx = g.Open())
        {
            ctx.BeginFigure(start, false, false);
            ctx.ArcTo(end, new Size(r, r), 0, v > 0.5, SweepDirection.Clockwise, true, false);
        }
        g.Freeze();
        dc.DrawGeometry(null, pen, g);
    }
}

/// <summary>A tiny line chart without axes.</summary>
public sealed class Sparkline : FrameworkElement
{
    public static readonly DependencyProperty ValuesProperty = DependencyProperty.Register(nameof(Values), typeof(IReadOnlyList<double>), typeof(Sparkline),
        new FrameworkPropertyMetadata(null, FrameworkPropertyMetadataOptions.AffectsRender));
    public static readonly DependencyProperty StrokeProperty = DependencyProperty.Register(nameof(Stroke), typeof(Brush), typeof(Sparkline),
        new FrameworkPropertyMetadata(Brushes.DodgerBlue, FrameworkPropertyMetadataOptions.AffectsRender));
    public static readonly DependencyProperty FillProperty = DependencyProperty.Register(nameof(Fill), typeof(Brush), typeof(Sparkline),
        new FrameworkPropertyMetadata(null, FrameworkPropertyMetadataOptions.AffectsRender));
    public static readonly DependencyProperty MinimumProperty = DependencyProperty.Register(nameof(Minimum), typeof(double), typeof(Sparkline),
        new FrameworkPropertyMetadata(double.NaN, FrameworkPropertyMetadataOptions.AffectsRender));
    public static readonly DependencyProperty MaximumProperty = DependencyProperty.Register(nameof(Maximum), typeof(double), typeof(Sparkline),
        new FrameworkPropertyMetadata(double.NaN, FrameworkPropertyMetadataOptions.AffectsRender));
    public static readonly DependencyProperty StrokeThicknessProperty = DependencyProperty.Register(nameof(StrokeThickness), typeof(double), typeof(Sparkline),
        new FrameworkPropertyMetadata(1.6, FrameworkPropertyMetadataOptions.AffectsRender));

    public IReadOnlyList<double>? Values { get => (IReadOnlyList<double>?)GetValue(ValuesProperty); set => SetValue(ValuesProperty, value); }
    public Brush Stroke { get => (Brush)GetValue(StrokeProperty); set => SetValue(StrokeProperty, value); }
    public Brush? Fill { get => (Brush?)GetValue(FillProperty); set => SetValue(FillProperty, value); }
    public double Minimum { get => (double)GetValue(MinimumProperty); set => SetValue(MinimumProperty, value); }
    public double Maximum { get => (double)GetValue(MaximumProperty); set => SetValue(MaximumProperty, value); }
    public double StrokeThickness { get => (double)GetValue(StrokeThicknessProperty); set => SetValue(StrokeThicknessProperty, value); }

    protected override void OnRender(DrawingContext dc)
    {
        var v = Values;
        double w = ActualWidth, h = ActualHeight;
        if (v is null || v.Count < 2 || w < 4 || h < 4) return;

        double min = Minimum, max = Maximum;
        if (double.IsNaN(min) || double.IsNaN(max))
        {
            double lo = double.MaxValue, hi = double.MinValue;
            foreach (var x in v)
            {
                if (double.IsNaN(x)) continue;
                lo = Math.Min(lo, x);
                hi = Math.Max(hi, x);
            }
            if (lo > hi) return;
            if (hi - lo < 1e-6) { lo -= 1; hi += 1; }
            if (double.IsNaN(min)) min = lo;
            if (double.IsNaN(max)) max = hi;
        }
        double pad = StrokeThickness + 1;
        double ph = h - 2 * pad;
        double step = (w - 2 * pad) / (v.Count - 1);
        Point P(int i) => new(pad + i * step, pad + ph * (1 - Math.Clamp((v[i] - min) / (max - min), 0, 1)));

        var line = new StreamGeometry();
        var area = new StreamGeometry();
        Point? last = null;
        using (var lc = line.Open())
        using (var ac = area.Open())
        {
            bool open = false;
            Point first = default;
            for (int i = 0; i < v.Count; i++)
            {
                if (double.IsNaN(v[i])) { open = false; continue; }
                var p = P(i);
                if (!open)
                {
                    lc.BeginFigure(p, false, false);
                    ac.BeginFigure(new Point(p.X, h - pad), true, true);
                    ac.LineTo(p, false, false);
                    open = true;
                    first = p;
                }
                else
                {
                    lc.LineTo(p, true, true);
                    ac.LineTo(p, false, false);
                }
                last = p;
            }
            if (last is { } l)
            {
                ac.LineTo(new Point(l.X, h - pad), false, false);
                _ = first;
            }
        }
        line.Freeze();
        area.Freeze();
        if (Fill is not null) dc.DrawGeometry(Fill, null, area);
        dc.DrawGeometry(null, new Pen(Stroke, StrokeThickness) { LineJoin = PenLineJoin.Round, StartLineCap = PenLineCap.Round, EndLineCap = PenLineCap.Round }, line);
        if (last is { } d) dc.DrawEllipse(Stroke, null, d, StrokeThickness + 0.6, StrokeThickness + 0.6);
    }
}

/// <summary>Six ascending bars like a phone's signal indicator.</summary>
public sealed class SignalBars : FrameworkElement
{
    public static readonly DependencyProperty BarsProperty = DependencyProperty.Register(nameof(Bars), typeof(int), typeof(SignalBars),
        new FrameworkPropertyMetadata(0, FrameworkPropertyMetadataOptions.AffectsRender));
    public static readonly DependencyProperty OnBrushProperty = DependencyProperty.Register(nameof(OnBrush), typeof(Brush), typeof(SignalBars),
        new FrameworkPropertyMetadata(Brushes.LimeGreen, FrameworkPropertyMetadataOptions.AffectsRender));
    public static readonly DependencyProperty OffBrushProperty = DependencyProperty.Register(nameof(OffBrush), typeof(Brush), typeof(SignalBars),
        new FrameworkPropertyMetadata(Brushes.Gray, FrameworkPropertyMetadataOptions.AffectsRender));

    public int Bars { get => (int)GetValue(BarsProperty); set => SetValue(BarsProperty, value); }
    public Brush OnBrush { get => (Brush)GetValue(OnBrushProperty); set => SetValue(OnBrushProperty, value); }
    public Brush OffBrush { get => (Brush)GetValue(OffBrushProperty); set => SetValue(OffBrushProperty, value); }

    protected override Size MeasureOverride(Size availableSize) => new(Math.Min(availableSize.Width, 30), Math.Min(availableSize.Height, 18));

    protected override void OnRender(DrawingContext dc)
    {
        const int n = 6;
        double w = ActualWidth, h = ActualHeight;
        if (w < 6 || h < 6) return;
        double gap = 1.5;
        double bw = (w - gap * (n - 1)) / n;
        for (int i = 0; i < n; i++)
        {
            double bh = h * (0.28 + 0.72 * (i + 1) / n);
            var rect = new Rect(i * (bw + gap), h - bh, bw, bh);
            dc.DrawRoundedRectangle(i < Bars ? OnBrush : OffBrush, null, rect, 1, 1);
        }
    }
}

public enum TimelineState : byte { Off = 0, On = 1, Offline = 2, ForceOn = 3, Paused = 4 }

public readonly record struct TimelineSample(double T, TimelineState State);

/// <summary>The last ~60 s of a timer's output as a coloured strip (ON / OFF / force / paused / offline).</summary>
public sealed class PhaseTimeline : FrameworkElement
{
    public static readonly DependencyProperty SamplesProperty = DependencyProperty.Register(nameof(Samples), typeof(IReadOnlyList<TimelineSample>), typeof(PhaseTimeline),
        new FrameworkPropertyMetadata(null, FrameworkPropertyMetadataOptions.AffectsRender));
    public static readonly DependencyProperty NowProperty = DependencyProperty.Register(nameof(Now), typeof(double), typeof(PhaseTimeline),
        new FrameworkPropertyMetadata(0.0, FrameworkPropertyMetadataOptions.AffectsRender));
    public static readonly DependencyProperty WindowSecondsProperty = DependencyProperty.Register(nameof(WindowSeconds), typeof(double), typeof(PhaseTimeline),
        new FrameworkPropertyMetadata(60.0, FrameworkPropertyMetadataOptions.AffectsRender));
    public static readonly DependencyProperty OnBrushProperty = BrushProp(nameof(OnBrush), Brushes.DodgerBlue);
    public static readonly DependencyProperty OffBrushProperty = BrushProp(nameof(OffBrush), Brushes.DimGray);
    public static readonly DependencyProperty ForceBrushProperty = BrushProp(nameof(ForceBrush), Brushes.Red);
    public static readonly DependencyProperty PausedBrushProperty = BrushProp(nameof(PausedBrush), Brushes.Orange);
    public static readonly DependencyProperty OfflineBrushProperty = BrushProp(nameof(OfflineBrush), Brushes.Gray);
    public static readonly DependencyProperty TextBrushProperty = BrushProp(nameof(TextBrush), Brushes.Gray);
    public static readonly DependencyProperty NoDataBrushProperty = BrushProp(nameof(NoDataBrush), Brushes.Gray);

    private static DependencyProperty BrushProp(string name, Brush def) => DependencyProperty.Register(name, typeof(Brush), typeof(PhaseTimeline),
        new FrameworkPropertyMetadata(def, FrameworkPropertyMetadataOptions.AffectsRender));

    public IReadOnlyList<TimelineSample>? Samples { get => (IReadOnlyList<TimelineSample>?)GetValue(SamplesProperty); set => SetValue(SamplesProperty, value); }
    public double Now { get => (double)GetValue(NowProperty); set => SetValue(NowProperty, value); }
    public double WindowSeconds { get => (double)GetValue(WindowSecondsProperty); set => SetValue(WindowSecondsProperty, value); }
    public Brush OnBrush { get => (Brush)GetValue(OnBrushProperty); set => SetValue(OnBrushProperty, value); }
    public Brush OffBrush { get => (Brush)GetValue(OffBrushProperty); set => SetValue(OffBrushProperty, value); }
    public Brush ForceBrush { get => (Brush)GetValue(ForceBrushProperty); set => SetValue(ForceBrushProperty, value); }
    public Brush PausedBrush { get => (Brush)GetValue(PausedBrushProperty); set => SetValue(PausedBrushProperty, value); }
    public Brush OfflineBrush { get => (Brush)GetValue(OfflineBrushProperty); set => SetValue(OfflineBrushProperty, value); }
    public Brush TextBrush { get => (Brush)GetValue(TextBrushProperty); set => SetValue(TextBrushProperty, value); }

    /// <summary>Outline colour of the part of the strip for which no history exists yet.</summary>
    public Brush NoDataBrush { get => (Brush)GetValue(NoDataBrushProperty); set => SetValue(NoDataBrushProperty, value); }

    private Brush BrushFor(TimelineState s) => s switch
    {
        TimelineState.On => OnBrush,
        TimelineState.ForceOn => ForceBrush,
        TimelineState.Paused => PausedBrush,
        TimelineState.Offline => OfflineBrush,
        _ => OffBrush,
    };

    protected override void OnRender(DrawingContext dc)
    {
        double w = ActualWidth, h = ActualHeight;
        if (w < 10 || h < 10) return;
        double stripH = Math.Min(22, h - 14);
        var strip = new Rect(0, 0, w, stripH);
        // the strip starts empty: history builds up while the app runs
        var dashed = new Pen(NoDataBrush, 1) { DashStyle = new DashStyle(new double[] { 3, 3 }, 0) };
        dc.DrawRoundedRectangle(null, dashed, new Rect(strip.X + 0.5, strip.Y + 0.5, strip.Width - 1, strip.Height - 1), 5, 5);
        dc.PushClip(new RectangleGeometry(strip, 5, 5));

        var samples = Samples;
        double win = WindowSeconds, now = Now;
        if (samples is { Count: > 0 })
        {
            double X(double t) => Math.Clamp((t - (now - win)) / win, 0, 1) * w;
            for (int i = 0; i < samples.Count; i++)
            {
                double t0 = samples[i].T;
                double t1 = i + 1 < samples.Count ? samples[i + 1].T : now;
                if (t1 < now - win) continue;
                double x0 = X(t0), x1 = X(t1);
                if (x1 - x0 < 0.3) continue;
                var brush = BrushFor(samples[i].State);
                if (samples[i].State == TimelineState.Offline)
                {
                    dc.DrawRectangle(OffBrush, null, new Rect(x0, 0, x1 - x0, stripH));
                    // hatch: thin diagonal lines
                    var pen = new Pen(brush, 1.2);
                    dc.PushClip(new RectangleGeometry(new Rect(x0, 0, x1 - x0, stripH)));
                    for (double x = x0 - stripH; x < x1; x += 6)
                        dc.DrawLine(pen, new Point(x, stripH), new Point(x + stripH, 0));
                    dc.Pop();
                }
                else
                {
                    dc.DrawRectangle(brush, null, new Rect(x0, 0, x1 - x0, stripH));
                }
            }
        }
        dc.Pop();

        // time ticks
        var tick = new Pen(TextBrush, 1);
        for (int s = 0; s <= (int)win; s += 10)
        {
            double x = w - s / win * w;
            x = Math.Clamp(x, 0.5, w - 0.5);
            dc.DrawLine(tick, new Point(x, stripH + 1), new Point(x, stripH + 4));
            var label = Draw.Text(this, s == 0 ? "now" : $"-{s}s", 10, TextBrush);
            double lx = Math.Clamp(x - label.Width / 2, 0, w - label.Width);
            dc.DrawText(label, new Point(lx, stripH + 4));
        }
    }
}

public sealed class GraphSeries
{
    public GraphSeries(string name, string brushKey, IReadOnlyList<double> values, bool rightAxis = false, double thickness = 1.8)
    {
        Name = name;
        BrushKey = brushKey;
        Values = values;
        RightAxis = rightAxis;
        Thickness = thickness;
    }

    public string Name { get; }

    /// <summary>Resource key of the stroke brush (resolved at render time so theme changes apply).</summary>
    public string BrushKey { get; }
    public IReadOnlyList<double> Values { get; }
    public bool RightAxis { get; }
    public double Thickness { get; }
}

/// <summary>A small multi-series line graph with a left (dBm) and right (percent) axis; samples are evenly spaced, newest last.</summary>
public sealed class LineGraph : FrameworkElement
{
    public static readonly DependencyProperty SeriesProperty = DependencyProperty.Register(nameof(Series), typeof(IReadOnlyList<GraphSeries>), typeof(LineGraph),
        new FrameworkPropertyMetadata(null, FrameworkPropertyMetadataOptions.AffectsRender));
    public static readonly DependencyProperty LeftMinProperty = Num(nameof(LeftMin), -100);
    public static readonly DependencyProperty LeftMaxProperty = Num(nameof(LeftMax), -30);
    public static readonly DependencyProperty RightMinProperty = Num(nameof(RightMin), 0);
    public static readonly DependencyProperty RightMaxProperty = Num(nameof(RightMax), 100);
    public static readonly DependencyProperty WindowPointsProperty = Num(nameof(WindowPoints), 300);
    public static readonly DependencyProperty GridBrushProperty = DependencyProperty.Register(nameof(GridBrush), typeof(Brush), typeof(LineGraph),
        new FrameworkPropertyMetadata(Brushes.Gray, FrameworkPropertyMetadataOptions.AffectsRender));
    public static readonly DependencyProperty TextBrushProperty = DependencyProperty.Register(nameof(TextBrush), typeof(Brush), typeof(LineGraph),
        new FrameworkPropertyMetadata(Brushes.Gray, FrameworkPropertyMetadataOptions.AffectsRender));
    public static readonly DependencyProperty LeftUnitProperty = DependencyProperty.Register(nameof(LeftUnit), typeof(string), typeof(LineGraph),
        new FrameworkPropertyMetadata("", FrameworkPropertyMetadataOptions.AffectsRender));
    public static readonly DependencyProperty RightUnitProperty = DependencyProperty.Register(nameof(RightUnit), typeof(string), typeof(LineGraph),
        new FrameworkPropertyMetadata("%", FrameworkPropertyMetadataOptions.AffectsRender));

    private static DependencyProperty Num(string name, double def) => DependencyProperty.Register(name, typeof(double), typeof(LineGraph),
        new FrameworkPropertyMetadata(def, FrameworkPropertyMetadataOptions.AffectsRender));

    public IReadOnlyList<GraphSeries>? Series { get => (IReadOnlyList<GraphSeries>?)GetValue(SeriesProperty); set => SetValue(SeriesProperty, value); }
    public double LeftMin { get => (double)GetValue(LeftMinProperty); set => SetValue(LeftMinProperty, value); }
    public double LeftMax { get => (double)GetValue(LeftMaxProperty); set => SetValue(LeftMaxProperty, value); }
    public double RightMin { get => (double)GetValue(RightMinProperty); set => SetValue(RightMinProperty, value); }
    public double RightMax { get => (double)GetValue(RightMaxProperty); set => SetValue(RightMaxProperty, value); }
    public double WindowPoints { get => (double)GetValue(WindowPointsProperty); set => SetValue(WindowPointsProperty, value); }
    public Brush GridBrush { get => (Brush)GetValue(GridBrushProperty); set => SetValue(GridBrushProperty, value); }
    public Brush TextBrush { get => (Brush)GetValue(TextBrushProperty); set => SetValue(TextBrushProperty, value); }
    public string LeftUnit { get => (string)GetValue(LeftUnitProperty); set => SetValue(LeftUnitProperty, value); }
    public string RightUnit { get => (string)GetValue(RightUnitProperty); set => SetValue(RightUnitProperty, value); }

    protected override void OnRender(DrawingContext dc)
    {
        double w = ActualWidth, h = ActualHeight;
        if (w < 80 || h < 50) return;
        const double leftPad = 40, rightPad = 36, topPad = 20, bottomPad = 20;
        var plot = new Rect(leftPad, topPad, w - leftPad - rightPad, h - topPad - bottomPad);
        var grid = new Pen(GridBrush, 1) { DashStyle = DashStyles.Dot };

        for (int i = 0; i <= 4; i++)
        {
            double y = plot.Top + plot.Height * i / 4;
            dc.DrawLine(grid, new Point(plot.Left, y), new Point(plot.Right, y));
            double lv = LeftMax - (LeftMax - LeftMin) * i / 4;
            double rv = RightMax - (RightMax - RightMin) * i / 4;
            var lt = Draw.Text(this, $"{lv:0}", 10, TextBrush);
            dc.DrawText(lt, new Point(plot.Left - 6 - lt.Width, y - lt.Height / 2));
            var rt = Draw.Text(this, $"{rv:0}{RightUnit}", 10, TextBrush);
            dc.DrawText(rt, new Point(plot.Right + 6, y - rt.Height / 2));
        }
        if (LeftUnit.Length > 0)
        {
            var u = Draw.Text(this, LeftUnit, 10, TextBrush);
            dc.DrawText(u, new Point(leftPad - 6 - u.Width, 0));
        }
        var minutes = WindowPoints / 60.0;
        var t0 = Draw.Text(this, minutes >= 1 ? $"-{minutes:0.#} min" : $"-{WindowPoints:0} s", 10, TextBrush);
        dc.DrawText(t0, new Point(plot.Left, plot.Bottom + 4));
        var t1 = Draw.Text(this, "now", 10, TextBrush);
        dc.DrawText(t1, new Point(plot.Right - t1.Width, plot.Bottom + 4));

        var series = Series;
        if (series is null) return;
        dc.PushClip(new RectangleGeometry(plot));
        foreach (var s in series)
        {
            var values = s.Values;
            if (values.Count < 2) continue;
            double min = s.RightAxis ? RightMin : LeftMin;
            double max = s.RightAxis ? RightMax : LeftMax;
            double stepX = plot.Width / Math.Max(1, WindowPoints - 1);
            double offsetX = plot.Right - (values.Count - 1) * stepX;
            var g = new StreamGeometry();
            using (var ctx = g.Open())
            {
                bool open = false;
                for (int i = 0; i < values.Count; i++)
                {
                    double v = values[i];
                    if (double.IsNaN(v)) { open = false; continue; }
                    var p = new Point(offsetX + i * stepX, plot.Bottom - plot.Height * Math.Clamp((v - min) / (max - min), 0, 1));
                    if (!open) { ctx.BeginFigure(p, false, false); open = true; }
                    else ctx.LineTo(p, true, true);
                }
            }
            g.Freeze();
            var brush = TryFindResource(s.BrushKey) as Brush ?? Brushes.Gray;
            dc.DrawGeometry(null, new Pen(brush, s.Thickness) { LineJoin = PenLineJoin.Round }, g);
        }
        dc.Pop();
    }
}
