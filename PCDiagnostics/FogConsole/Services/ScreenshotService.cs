using System.IO;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace FogConsole.Services;

/// <summary>Renders the window content to a PNG (used by <c>--shot</c>), DPI-correct.</summary>
public static class ScreenshotService
{
    /// <param name="scale">Pixels per device independent pixel; null = the window's own DPI scale (sharpest, largest file).</param>
    public static string Capture(Window window, string path, double? scale = null)
    {
        if (window.Content is not FrameworkElement root) throw new InvalidOperationException("The window has no content.");
        root.UpdateLayout();

        var dpi = VisualTreeHelper.GetDpi(window);
        double sx = scale ?? dpi.DpiScaleX, sy = scale ?? dpi.DpiScaleY;
        int w = Math.Max(1, (int)Math.Ceiling(root.ActualWidth * sx));
        int h = Math.Max(1, (int)Math.Ceiling(root.ActualHeight * sy));
        var bmp = new RenderTargetBitmap(w, h, 96 * sx, 96 * sy, PixelFormats.Pbgra32);

        // the root grid is transparent: paint the window background first
        var background = new DrawingVisual();
        using (var dc = background.RenderOpen())
            dc.DrawRectangle(window.Background ?? Brushes.Black, null, new Rect(0, 0, root.ActualWidth, root.ActualHeight));
        bmp.Render(background);
        bmp.Render(root);

        var encoder = new PngBitmapEncoder();
        encoder.Frames.Add(BitmapFrame.Create(bmp));
        string full = Path.GetFullPath(path);
        Directory.CreateDirectory(Path.GetDirectoryName(full)!);
        using (var fs = File.Create(full)) encoder.Save(fs);
        return full;
    }
}
