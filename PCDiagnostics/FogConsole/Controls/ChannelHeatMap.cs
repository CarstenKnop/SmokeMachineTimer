using System.Windows;
using System.Windows.Input;
using System.Windows.Media;

namespace FogConsole.Controls;

/// <summary>One column of the channel chart.</summary>
public sealed class ChannelCell
{
    public int Channel { get; init; }
    public bool HasData { get; init; }

    /// <summary>0…100, lower = quieter.</summary>
    public int Score { get; init; }
    public int Aps { get; init; }
    public int ApRssi { get; init; }
    public bool IsCurrent { get; init; }

    /// <summary>1…3 = recommended rank, 0 = not recommended.</summary>
    public int BestRank { get; init; }
}

/// <summary>
/// Bar chart / heat map of the 13 Wi-Fi channels: height and colour show how busy a channel is (score), the
/// current channel is outlined and the recommended ones are starred. Clicking a column selects it.
/// </summary>
public sealed class ChannelHeatMap : FrameworkElement
{
    public static readonly DependencyProperty CellsProperty = DependencyProperty.Register(nameof(Cells), typeof(IReadOnlyList<ChannelCell>), typeof(ChannelHeatMap),
        new FrameworkPropertyMetadata(null, FrameworkPropertyMetadataOptions.AffectsRender));
    public static readonly DependencyProperty SelectedChannelProperty = DependencyProperty.Register(nameof(SelectedChannel), typeof(int), typeof(ChannelHeatMap),
        new FrameworkPropertyMetadata(0, FrameworkPropertyMetadataOptions.AffectsRender | FrameworkPropertyMetadataOptions.BindsTwoWayByDefault));
    public static readonly DependencyProperty QuietBrushProperty = Prop(nameof(QuietBrush), Brushes.Green);
    public static readonly DependencyProperty BusyBrushProperty = Prop(nameof(BusyBrush), Brushes.Orange);
    public static readonly DependencyProperty JammedBrushProperty = Prop(nameof(JammedBrush), Brushes.Red);
    public static readonly DependencyProperty RankBrushProperty = Prop(nameof(RankBrush), Brushes.Green);
    public static readonly DependencyProperty AccentBrushProperty = Prop(nameof(AccentBrush), Brushes.DodgerBlue);
    public static readonly DependencyProperty TrackBrushProperty = Prop(nameof(TrackBrush), Brushes.DimGray);
    public static readonly DependencyProperty TextBrushProperty = Prop(nameof(TextBrush), Brushes.White);
    public static readonly DependencyProperty DimTextBrushProperty = Prop(nameof(DimTextBrush), Brushes.Gray);

    private static DependencyProperty Prop(string name, Brush def) => DependencyProperty.Register(name, typeof(Brush), typeof(ChannelHeatMap),
        new FrameworkPropertyMetadata(def, FrameworkPropertyMetadataOptions.AffectsRender));

    public IReadOnlyList<ChannelCell>? Cells { get => (IReadOnlyList<ChannelCell>?)GetValue(CellsProperty); set => SetValue(CellsProperty, value); }
    public int SelectedChannel { get => (int)GetValue(SelectedChannelProperty); set => SetValue(SelectedChannelProperty, value); }
    public Brush QuietBrush { get => (Brush)GetValue(QuietBrushProperty); set => SetValue(QuietBrushProperty, value); }
    public Brush BusyBrush { get => (Brush)GetValue(BusyBrushProperty); set => SetValue(BusyBrushProperty, value); }
    public Brush JammedBrush { get => (Brush)GetValue(JammedBrushProperty); set => SetValue(JammedBrushProperty, value); }
    public Brush RankBrush { get => (Brush)GetValue(RankBrushProperty); set => SetValue(RankBrushProperty, value); }
    public Brush AccentBrush { get => (Brush)GetValue(AccentBrushProperty); set => SetValue(AccentBrushProperty, value); }
    public Brush TrackBrush { get => (Brush)GetValue(TrackBrushProperty); set => SetValue(TrackBrushProperty, value); }
    public Brush TextBrush { get => (Brush)GetValue(TextBrushProperty); set => SetValue(TextBrushProperty, value); }
    public Brush DimTextBrush { get => (Brush)GetValue(DimTextBrushProperty); set => SetValue(DimTextBrushProperty, value); }

    private const double TopPad = 46, BottomPad = 58;
    private int _hover;

    public ChannelHeatMap()
    {
        Focusable = true;
        Cursor = Cursors.Hand;
        MinHeight = 240;
    }

    private Brush ScoreBrush(int score)
    {
        var q = Draw.ColorOf(QuietBrush, Colors.Green);
        var b = Draw.ColorOf(BusyBrush, Colors.Orange);
        var j = Draw.ColorOf(JammedBrush, Colors.Red);
        double t = score / 100.0;
        var c = t < 0.5 ? Draw.Blend(q, b, t / 0.5) : Draw.Blend(b, j, (t - 0.5) / 0.5);
        return new SolidColorBrush(c);
    }

    private double ColWidth => ActualWidth / 13.0;

    private int ChannelAt(Point p)
    {
        int c = (int)(p.X / ColWidth) + 1;
        return c is >= 1 and <= 13 ? c : 0;
    }

    protected override void OnMouseMove(MouseEventArgs e)
    {
        int c = ChannelAt(e.GetPosition(this));
        if (c != _hover)
        {
            _hover = c;
            InvalidateVisual();
        }
        base.OnMouseMove(e);
    }

    protected override void OnMouseLeave(MouseEventArgs e)
    {
        _hover = 0;
        InvalidateVisual();
        base.OnMouseLeave(e);
    }

    protected override void OnMouseLeftButtonUp(MouseButtonEventArgs e)
    {
        int c = ChannelAt(e.GetPosition(this));
        if (c != 0) SelectedChannel = c;
        Focus();
        base.OnMouseLeftButtonUp(e);
    }

    protected override void OnKeyDown(KeyEventArgs e)
    {
        if (e.Key == Key.Left) { SelectedChannel = Math.Max(1, (SelectedChannel == 0 ? 7 : SelectedChannel) - 1); e.Handled = true; }
        else if (e.Key == Key.Right) { SelectedChannel = Math.Min(13, (SelectedChannel == 0 ? 6 : SelectedChannel) + 1); e.Handled = true; }
        base.OnKeyDown(e);
    }

    protected override void OnRender(DrawingContext dc)
    {
        double w = ActualWidth, h = ActualHeight;
        if (w < 100 || h < 120) return;

        // transparent hit-test background
        dc.DrawRectangle(Brushes.Transparent, null, new Rect(0, 0, w, h));

        double colW = ColWidth;
        double plotH = h - TopPad - BottomPad;
        var cells = Cells;
        var grid = new Pen(TrackBrush, 1) { DashStyle = DashStyles.Dot };
        for (int i = 0; i <= 4; i++)
        {
            double y = TopPad + plotH * i / 4;
            dc.DrawLine(grid, new Point(0, y), new Point(w, y));
        }

        for (int c = 1; c <= 13; c++)
        {
            var cell = cells?.FirstOrDefault(x => x.Channel == c);
            double x0 = (c - 1) * colW;
            double barW = Math.Min(colW - 14, 54);
            double bx = x0 + (colW - barW) / 2;
            bool selected = c == SelectedChannel;
            bool hover = c == _hover;

            if (selected || hover)
                dc.DrawRoundedRectangle(selected ? null : TrackBrush, selected ? new Pen(AccentBrush, 2) : null,
                    new Rect(x0 + 3, 3, colW - 6, h - 6), 8, 8);
            if (selected)
            {
                var wash = AccentBrush.Clone();
                wash.Opacity = 0.10;
                dc.DrawRoundedRectangle(wash, null, new Rect(x0 + 3, 3, colW - 6, h - 6), 8, 8);
            }

            // bar
            var track = new Rect(bx, TopPad, barW, plotH);
            dc.DrawRoundedRectangle(TrackBrush, null, track, 5, 5);
            if (cell is { HasData: true })
            {
                double bh = Math.Max(6, plotH * cell.Score / 100.0);
                var bar = new Rect(bx, TopPad + plotH - bh, barW, bh);
                dc.DrawRoundedRectangle(ScoreBrush(cell.Score), null, bar, 5, 5);
                var score = Draw.Text(this, cell.Score.ToString(), 13, TextBrush, Draw.UiFontBold);
                dc.DrawText(score, new Point(bx + (barW - score.Width) / 2, Math.Max(TopPad - score.Height - 2, bar.Top - score.Height - 3)));
            }
            else
            {
                var dash = Draw.Text(this, "-", 13, DimTextBrush);
                dc.DrawText(dash, new Point(bx + (barW - dash.Width) / 2, TopPad + plotH - dash.Height - 4));
            }

            // channel number
            var num = Draw.Text(this, c.ToString(), 15, cell?.IsCurrent == true ? AccentBrush : TextBrush, Draw.UiFontBold);
            dc.DrawText(num, new Point(x0 + (colW - num.Width) / 2, TopPad + plotH + 6));

            // details below
            if (cell is { HasData: true })
            {
                var aps = Draw.Text(this, cell.Aps == 1 ? "1 AP" : $"{cell.Aps} APs", 10.5, DimTextBrush);
                dc.DrawText(aps, new Point(x0 + (colW - aps.Width) / 2, TopPad + plotH + 28));
                var rssi = Draw.Text(this, cell.Aps > 0 && cell.ApRssi != 0 ? $"{cell.ApRssi} dBm" : "-", 10.5, DimTextBrush);
                dc.DrawText(rssi, new Point(x0 + (colW - rssi.Width) / 2, TopPad + plotH + 42));
            }

            // markers
            if (cell?.IsCurrent == true)
            {
                var tag = Draw.Text(this, "CURRENT", 9.5, AccentBrush, Draw.UiFontBold);
                double tw = tag.Width + 10;
                var r = new Rect(x0 + (colW - tw) / 2, 4, tw, 17);
                dc.DrawRoundedRectangle(null, new Pen(AccentBrush, 1.2), r, 8, 8);
                dc.DrawText(tag, new Point(r.X + 5, r.Y + 2.5));
            }
            else if (cell is { BestRank: > 0 })
            {
                var tag = Draw.Text(this, $"★ #{cell.BestRank}", 10.5, RankBrush, Draw.UiFontBold);
                dc.DrawText(tag, new Point(x0 + (colW - tag.Width) / 2, 6));
            }
        }
    }
}
