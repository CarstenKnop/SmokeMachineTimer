using System.IO;
using System.Collections.Concurrent;
using FogConsole.Hub;
using FogConsole.Services;
using FogConsole.Simulator;
using FogConsole.ViewModels;

namespace FogConsole.Tests;

/// <summary>A dispatcher that queues work until the test pumps it (stands in for the UI thread).</summary>
public sealed class QueueDispatcher : IUiDispatcher
{
    private readonly ConcurrentQueue<Action> _queue = new();
    public void Post(Action action) => _queue.Enqueue(action);
    public bool CheckAccess() => true;

    public int Run()
    {
        int n = 0;
        while (_queue.TryDequeue(out var a))
        {
            a();
            n++;
        }
        return n;
    }
}

/// <summary>Manually advanced time for timers (hold-to-fog refresh cadence, release retries).</summary>
public sealed class ManualTimeProvider : TimeProvider
{
    private readonly List<FakeTimer> _timers = new();
    private DateTimeOffset _now = new(2026, 10, 20, 12, 0, 0, TimeSpan.Zero);

    public override DateTimeOffset GetUtcNow() => _now;

    public override ITimer CreateTimer(TimerCallback callback, object? state, TimeSpan dueTime, TimeSpan period)
    {
        var t = new FakeTimer(this, callback, state);
        t.Change(dueTime, period);
        _timers.Add(t);
        return t;
    }

    public int ActiveTimers => _timers.Count(t => !t.Disposed && t.Next is not null);

    public void Advance(TimeSpan by)
    {
        var target = _now + by;
        while (true)
        {
            var due = _timers.Where(t => !t.Disposed && t.Next is not null && t.Next <= target).OrderBy(t => t.Next).FirstOrDefault();
            if (due is null) break;
            _now = due.Next!.Value;
            due.Fire();
        }
        _now = target;
    }

    private sealed class FakeTimer : ITimer
    {
        private readonly ManualTimeProvider _owner;
        private readonly TimerCallback _callback;
        private readonly object? _state;
        private TimeSpan _period;

        public FakeTimer(ManualTimeProvider owner, TimerCallback callback, object? state)
        {
            _owner = owner;
            _callback = callback;
            _state = state;
        }

        public DateTimeOffset? Next { get; private set; }
        public bool Disposed { get; private set; }

        public bool Change(TimeSpan dueTime, TimeSpan period)
        {
            _period = period;
            Next = dueTime == Timeout.InfiniteTimeSpan ? null : _owner._now + dueTime;
            return true;
        }

        public void Fire()
        {
            Next = _period == Timeout.InfiniteTimeSpan || _period == TimeSpan.Zero ? null : _owner._now + _period;
            _callback(_state);
        }

        public void Dispose() => Disposed = true;
        public ValueTask DisposeAsync()
        {
            Disposed = true;
            return ValueTask.CompletedTask;
        }
    }
}

/// <summary>The whole application object graph without WPF windows, driven by a queue dispatcher and a manual event pump.</summary>
public sealed class TestApp : IDisposable
{
    private readonly string _dir = Path.Combine(Path.GetTempPath(), "FogConsoleTests-" + Guid.NewGuid().ToString("N"));

    public TestApp(HubOptions? hubOptions = null, SimOptions? sim = null)
    {
        Directory.CreateDirectory(_dir);
        Ui = new QueueDispatcher();
        Pump = new EventPump(null);
        Settings = new SettingsService(Path.Combine(_dir, "settings.json"));
        Dialogs = new ScriptedDialogService();
        Toasts = new ToastService(null);
        Hub = new HubClient(hubOptions ?? new HubOptions());
        Sim = new SimulatorController { Seed = 7, TimerCount = 7, DemoScenario = false };
        var services = new AppServices
        {
            Hub = Hub,
            Settings = Settings,
            Toasts = Toasts,
            Dialogs = Dialogs,
            Ui = Ui,
            Clock = new UiClock(),
            Sim = Sim,
            Theme = new ThemeService(),
        };
        Services = services;
        Main = new MainViewModel(services, Pump, null);
        _simOptions = sim;
    }

    private readonly SimOptions? _simOptions;

    public QueueDispatcher Ui { get; }
    public EventPump Pump { get; }
    public SettingsService Settings { get; }
    public ScriptedDialogService Dialogs { get; }
    public ToastService Toasts { get; }
    public HubClient Hub { get; }
    public SimulatorController Sim { get; }
    public AppServices Services { get; }
    public MainViewModel Main { get; }
    public FleetState Fleet => Main.Fleet;

    /// <summary>Starts the simulator (chaos off, no demo script unless asked) and connects the hub to it.</summary>
    public async Task ConnectSimAsync(bool chaos = false)
    {
        Sim.StartNew();
        if (!chaos)
        {
            // deterministic, quiet world: replace the device with one without random power cuts
            var opt = _simOptions ?? new SimOptions { Seed = Sim.Seed, Chaos = false, Demo = false, PairedTimers = Sim.TimerCount, TotalTimers = Math.Min(13, Sim.TimerCount + 3) };
            _quietDevice = new SimDevice(opt);
        }
        await Hub.ConnectAsync(() => new SimTransport(_quietDevice ?? Sim.Device!), waitIfAbsent: true);
        await PumpUntil(() => Fleet.Devices.Count > 0, 5000);
    }

    private SimDevice? _quietDevice;
    public SimDevice Device => _quietDevice ?? Sim.Device!;

    /// <summary>Flushes hub events and queued UI work.</summary>
    public void Flush()
    {
        Pump.FlushAll();
        Ui.Run();
        Pump.FlushAll();
    }

    public async Task PumpUntil(Func<bool> condition, int timeoutMs = 4000)
    {
        var sw = System.Diagnostics.Stopwatch.StartNew();
        while (true)
        {
            Flush();
            if (condition()) return;
            if (sw.ElapsedMilliseconds > timeoutMs) throw new TimeoutException("condition not met in time");
            await Task.Delay(15);
        }
    }

    public async Task Pause(int ms)
    {
        var sw = System.Diagnostics.Stopwatch.StartNew();
        while (sw.ElapsedMilliseconds < ms)
        {
            Flush();
            await Task.Delay(15);
        }
    }

    public void Dispose()
    {
        try { Services.Hold.Dispose(); } catch { /* ignore */ }
        try { Hub.DisconnectAsync().GetAwaiter().GetResult(); } catch { /* ignore */ }
        try { Directory.Delete(_dir, true); } catch { /* ignore */ }
    }
}
