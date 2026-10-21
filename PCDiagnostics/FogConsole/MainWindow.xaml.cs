using System.ComponentModel;
using System.Windows;
using FogConsole.Services;
using FogConsole.ViewModels;

namespace FogConsole;

public partial class MainWindow : Window
{
    private readonly MainViewModel _vm;
    private readonly SettingsService _settings;

    public MainWindow(MainViewModel vm, SettingsService settings, StartupOptions options)
    {
        InitializeComponent();
        _vm = vm;
        _settings = settings;
        DataContext = vm;

        if (options.Width is { } w && options.Height is { } h)
        {
            Width = w;
            Height = h;
            WindowStartupLocation = WindowStartupLocation.Manual;
            Left = 40;
            Top = 40;
        }
        else
        {
            // The restore size (what you get after un-maximizing) is the saved normal bounds or 1280x800 ...
            ApplySavedBounds(settings.Current);
            // ... but the application always opens maximized.
            WindowState = WindowState.Maximized;
        }

        // never leave an output forced on when the user switches to another window
        Deactivated += (_, _) => _vm.App.Hold.EndAll();
        Closing += OnClosing;
        SizeChanged += (_, e) => _vm.IsCompact = e.NewSize.Width < 1220;
    }

    private void ApplySavedBounds(AppSettings s)
    {
        if (s.WindowWidth is not { } w || s.WindowHeight is not { } h) return;
        Width = Math.Clamp(w, MinWidth, SystemParameters.VirtualScreenWidth);
        Height = Math.Clamp(h, MinHeight, SystemParameters.VirtualScreenHeight);
        if (s.WindowLeft is { } l && s.WindowTop is { } t)
        {
            // keep the window on a screen that still exists
            bool visible = l + 80 < SystemParameters.VirtualScreenLeft + SystemParameters.VirtualScreenWidth &&
                           l + Width - 80 > SystemParameters.VirtualScreenLeft &&
                           t >= SystemParameters.VirtualScreenTop - 10 &&
                           t + 80 < SystemParameters.VirtualScreenTop + SystemParameters.VirtualScreenHeight;
            if (visible)
            {
                WindowStartupLocation = WindowStartupLocation.Manual;
                Left = l;
                Top = t;
            }
        }
    }

    private void OnClosing(object? sender, CancelEventArgs e)
    {
        if (!_vm.CanCloseNow())
        {
            e.Cancel = true;
            return;
        }
        var s = _settings.Current;
        var b = WindowState == WindowState.Normal ? new Rect(Left, Top, Width, Height) : RestoreBounds;
        s.WindowLeft = b.Left;
        s.WindowTop = b.Top;
        s.WindowWidth = b.Width;
        s.WindowHeight = b.Height;
        _vm.ShutDown();
        try
        {
            Task.Run(() => _vm.Hub.DisconnectAsync()).Wait(TimeSpan.FromSeconds(2));
        }
        catch
        {
            // closing anyway
        }
    }

}
