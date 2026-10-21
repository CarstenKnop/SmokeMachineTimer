using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Threading;
using FogConsole.Hub;
using FogConsole.Services;
using FogConsole.ViewModels;

namespace FogConsole;

public partial class App : Application
{
    private MainViewModel? _vm;
    private StartupOptions _options = new();
    private int _errorCount;

    protected override async void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        _options = StartupOptions.Parse(e.Args);

        DispatcherUnhandledException += OnDispatcherUnhandledException;
        AppDomain.CurrentDomain.UnhandledException += (_, args) =>
            ReportFatal("Unhandled exception", args.ExceptionObject as Exception ?? new Exception(args.ExceptionObject?.ToString()));
        TaskScheduler.UnobservedTaskException += (_, args) =>
        {
            CrashLog.Write("Unobserved task exception", args.Exception);
            args.SetObserved();
        };

        if (_options.Help || _options.Problems.Count > 0)
        {
            string text = (_options.Problems.Count > 0 ? string.Join(Environment.NewLine, _options.Problems) + Environment.NewLine + Environment.NewLine : "") + StartupOptions.Usage;
            if (_options.IsScreenshot || _options.Exit) Console.Error.WriteLine(text);
            else MessageBox.Show(text, "Fog Console - command line", MessageBoxButton.OK, _options.Help ? MessageBoxImage.Information : MessageBoxImage.Warning);
            Shutdown(_options.Help ? 0 : 2);
            return;
        }

        try
        {
            await StartUiAsync();
        }
        catch (Exception ex)
        {
            ReportFatal("Startup failed", ex);
        }
    }

    private async Task StartUiAsync()
    {
        // screenshots and --no-settings runs must neither read nor overwrite the user's settings
        string? settingsPath = _options.IsScreenshot || _options.NoSettings
            ? Path.Combine(Path.GetTempPath(), $"FogConsole-{Environment.ProcessId}", "settings.json")
            : null;
        var settings = new SettingsService(settingsPath);

        var theme = new ThemeService();
        theme.Install(this);
        theme.Apply(_options.Theme ?? settings.Current.Theme);

        var dispatcher = Dispatcher;
        var ui = new WpfUiDispatcher(dispatcher);
        var toasts = new ToastService(dispatcher);
        IDialogService dialogs = _options.IsScreenshot ? new ScriptedDialogService() : new WpfDialogService();
        var services = new AppServices
        {
            Hub = new HubClient(),
            Settings = settings,
            Toasts = toasts,
            Dialogs = dialogs,
            Ui = ui,
            Clock = new UiClock(),
            Sim = new SimulatorController(),
            Theme = theme,
        };
        var pump = new EventPump(dispatcher);
        var tick = new DispatcherTimer(DispatcherPriority.Render, dispatcher) { Interval = TimeSpan.FromMilliseconds(50) };

        _vm = new MainViewModel(services, pump, tick);
        var window = new MainWindow(_vm, settings, _options);
        MainWindow = window;
        window.Show();

        await _vm.StartAsync(_options);

        if (_options.IsScreenshot) await RunScreenshotAsync(window);
        else if (_options.Exit)
        {
            // smoke test: run for --delay seconds (default 4), optionally prepare a situation, then quit with exit code 0
            await Task.Delay(TimeSpan.FromSeconds(_options.Delay));
            if (!string.IsNullOrEmpty(_options.Action)) await ScreenshotScript.RunAsync(_vm, _options.Action!);
            Shutdown(0);
        }
    }

    // ---- --shot ---------------------------------------------------------------------------

    private async Task RunScreenshotAsync(MainWindow window)
    {
        var vm = _vm!;
        await Task.Delay(TimeSpan.FromSeconds(_options.Delay));
        if (!string.IsNullOrEmpty(_options.Action)) await ScreenshotScript.RunAsync(vm, _options.Action!);
        if (_options.Scroll is { } offset) ScrollPage(window, offset);
        await Task.Delay(300);
        // let layout and the 20 Hz interpolation settle
        await Dispatcher.InvokeAsync(() => { }, DispatcherPriority.ApplicationIdle);
        string path = ScreenshotService.Capture(window, _options.Shot!, _options.ShotScale);
        Console.Out.WriteLine($"screenshot: {path}");
        if (_options.Exit) Shutdown(0);
    }

    /// <summary>Scrolls the vertical scroll viewer of the current page (for screenshots of long pages).</summary>
    private static void ScrollPage(Window window, double offset)
    {
        static ScrollViewer? Find(DependencyObject d)
        {
            int n = System.Windows.Media.VisualTreeHelper.GetChildrenCount(d);
            for (int i = 0; i < n; i++)
            {
                var child = System.Windows.Media.VisualTreeHelper.GetChild(d, i);
                if (child is ScrollViewer sv && sv.ScrollableHeight > 0 && sv.VerticalScrollBarVisibility != ScrollBarVisibility.Disabled) return sv;
                if (Find(child) is { } inner) return inner;
            }
            return null;
        }
        var viewer = Find(window);
        viewer?.ScrollToVerticalOffset(offset);
        viewer?.UpdateLayout();
    }

    // ---- crash handling ------------------------------------------------------------------

    private void OnDispatcherUnhandledException(object sender, DispatcherUnhandledExceptionEventArgs e)
    {
        CrashLog.Write("UI thread exception", e.Exception);
        _errorCount++;
        if (_options.IsScreenshot || _options.Exit)
        {
            Console.Error.WriteLine(e.Exception.ToString());
            e.Handled = true;
            Shutdown(1);
            return;
        }
        e.Handled = true;
        bool again = MessageBox.Show(
            "Fog Console ran into an unexpected problem.\n\n" + e.Exception.Message +
            "\n\nDetails were saved to:\n" + CrashLog.PathOnDisk +
            "\n\nYou can keep working, but if things look wrong please restart the application." +
            (_errorCount >= 3 ? "\n\nThis keeps happening - the application will close." : ""),
            "Fog Console", MessageBoxButton.OK, MessageBoxImage.Warning) == MessageBoxResult.OK;
        if (again && _errorCount >= 3) Shutdown(1);
    }

    private void ReportFatal(string where, Exception ex)
    {
        CrashLog.Write(where, ex);
        try { _vm?.App.Hold.EndAll(); } catch { /* best effort: never leave an output forced on */ }
        Console.Error.WriteLine($"{where}: {ex}");
        if (_options.IsScreenshot || _options.Exit)
        {
            Environment.Exit(1);
            return;
        }
        try
        {
            Dispatcher.Invoke(() => MessageBox.Show(
                "Fog Console hit a fatal error and has to close.\n\n" + ex.Message + "\n\nDetails were saved to:\n" + CrashLog.PathOnDisk,
                "Fog Console", MessageBoxButton.OK, MessageBoxImage.Error));
        }
        catch
        {
            // nothing more to do
        }
    }
}

/// <summary>Prepares special situations for screenshots (<c>--action</c>).</summary>
internal static class ScreenshotScript
{
    public static async Task RunAsync(MainViewModel vm, string action)
    {
        switch (action.ToLowerInvariant())
        {
            case "survey":
                vm.Navigate(NavPage.Channels);
                vm.Channels.SurveyCommand.Execute(null);
                await Task.Delay(4500);
                break;
            case "linktest":
                vm.Navigate(NavPage.Channels);
                vm.Channels.SurveyCommand.Execute(null);
                await Task.Delay(4000);
                vm.Channels.LabDevice = vm.Fleet.Devices.FirstOrDefault(d => d.Online);
                vm.Channels.RunLinkTestCommand.Execute(null);
                await Task.Delay(3000);
                vm.Channels.LabDevice = vm.Fleet.Devices.Where(d => d.Online).Skip(5).FirstOrDefault() ?? vm.Channels.LabDevice;
                vm.Channels.RunLinkTestCommand.Execute(null);
                await Task.Delay(3000);
                break;
            case "migrate":
                vm.Navigate(NavPage.Channels);
                vm.Channels.SurveyCommand.Execute(null);
                await Task.Delay(4500);
                vm.Channels.MoveCommand.Execute(null);
                await Task.Delay(2200);
                break;
            case "fail":
                // a command to a timer that is powered off
                vm.Fleet.Devices.FirstOrDefault(d => !d.Online)?.ToggleCommand.Execute(null);
                var offline = vm.Fleet.Devices.FirstOrDefault(d => !d.Online);
                if (offline is not null) vm.App.Commands.Run(Protocol.CommandSpec.Toggle(Protocol.CommandTarget.ByMac(offline.Mac)), "Toggle", offline.DisplayName, offline.Mac);
                await Task.Delay(3200);
                break;
            case "baseline":
                vm.Navigate(NavPage.Diagnostics);
                await Task.Delay(2500);
                vm.Diagnostics.SetBaselineCommand.Execute(null);
                vm.Sim.Device?.InjectInterference(TimeSpan.FromSeconds(30));
                await Task.Delay(9000);
                break;
            case "hold":
                vm.Fleet.Devices.FirstOrDefault(d => d.Online)?.HoldStartCommand.Execute(null);
                await Task.Delay(1500);
                break;
            case "device":
                {
                    // a few commands so that "recent commands" and the cycle timeline have something to show
                    var d = vm.Fleet.Devices.FirstOrDefault(x => x.Online);
                    if (d is null) break;
                    vm.Fleet.SelectDevice(d);
                    d.ToggleCommand.Execute(null);
                    await Task.Delay(1500);
                    d.PauseResumeCommand.Execute(null);
                    await Task.Delay(2500);
                    d.PauseResumeCommand.Execute(null);
                    await Task.Delay(1500);
                    d.IdentifyCommand.Execute(null);
                    await Task.Delay(900);
                    vm.Toasts.Toasts.Clear();                  // keep the picture free of pop-ups
                    break;
                }
            case "rename":
                vm.Fleet.Devices.FirstOrDefault()?.StartEditNameCommand.Execute(null);
                break;
            case "console":
                vm.Navigate(NavPage.Console);
                vm.Console.DeviceLogLevel = "debug";
                await Task.Delay(800);
                foreach (var d in vm.Fleet.Devices.Where(d => d.Online).Take(3)) d.ToggleCommand.Execute(null);
                vm.Navigate(NavPage.Channels);
                vm.Channels.SurveyCommand.Execute(null);
                await Task.Delay(4500);
                vm.Navigate(NavPage.Console);
                await Task.Delay(9000);                      // let the debug chatter (radio summaries, missed beats) accumulate
                break;
            case "packets":
                vm.Navigate(NavPage.Console);
                vm.Console.SelectedTab = 1;
                vm.Console.CaptureFrames = true;
                await Task.Delay(3500);
                break;
            case "raw":
                vm.Navigate(NavPage.Console);
                vm.Console.SelectedTab = 2;
                vm.Console.RawInput = "{\"t\":\"get\",\"what\":\"stats\"}";
                vm.Console.SendRawCommand.Execute(null);
                await Task.Delay(1500);
                break;
            case "discover":
                vm.Navigate(NavPage.Pairing);
                vm.Pairing.Sweep = true;
                vm.Pairing.DiscoverOn = true;
                await Task.Delay(9000);
                break;
            case "simpanel":
                vm.SimPanelInline = true;
                break;
            case "showcase":
                {
                    // a mix of states for the documentation screenshot: one paused, one forced on (fogging), plus whatever the demo script produced
                    var live = vm.Fleet.Devices.Where(d => d.Online).ToList();
                    live.ElementAtOrDefault(6)?.PauseResumeCommand.Execute(null);
                    (live.ElementAtOrDefault(5) ?? live.FirstOrDefault())?.HoldStartCommand.Execute(null);
                    await Task.Delay(1800);
                    // one machine in its ON phase right now (Fog3 fires for 2 s)
                    live.ElementAtOrDefault(2)?.ToggleCommand.Execute(null);
                    await Task.Delay(550);
                    break;
                }
            case "forceon":
                vm.Fleet.Devices.FirstOrDefault(d => d.Online)?.HoldStartCommand.Execute(null);
                await Task.Delay(800);
                break;
        }
    }
}
