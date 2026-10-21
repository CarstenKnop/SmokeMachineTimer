using System.Collections.Concurrent;
using System.Windows;
using System.Windows.Threading;

namespace FogConsole.Services;

/// <summary>Marshals work onto the UI thread.</summary>
public interface IUiDispatcher
{
    void Post(Action action);
    bool CheckAccess();
}

public sealed class WpfUiDispatcher : IUiDispatcher
{
    private readonly Dispatcher _dispatcher;

    public WpfUiDispatcher(Dispatcher dispatcher) => _dispatcher = dispatcher;

    public void Post(Action action) => _dispatcher.BeginInvoke(action, DispatcherPriority.Normal);

    public bool CheckAccess() => _dispatcher.CheckAccess();
}

/// <summary>Runs everything inline (unit tests).</summary>
public sealed class ImmediateDispatcher : IUiDispatcher
{
    public void Post(Action action) => action();
    public bool CheckAccess() => true;
}

/// <summary>
/// Collects items from background threads (serial reader) and hands them to the UI thread in small batches,
/// in order. Prevents hundreds of dispatcher invocations per second when frames and raw lines stream in.
/// </summary>
public sealed class EventPump
{
    private readonly ConcurrentQueue<object> _queue = new();
    private readonly DispatcherTimer? _timer;
    private readonly int _maxPerTick;

    /// <param name="dispatcher">When null no timer is created; call <see cref="Flush"/> manually (tests).</param>
    public EventPump(Dispatcher? dispatcher, int maxPerTick = 400)
    {
        _maxPerTick = maxPerTick;
        if (dispatcher is not null)
        {
            _timer = new DispatcherTimer(DispatcherPriority.Background, dispatcher) { Interval = TimeSpan.FromMilliseconds(40) };
            _timer.Tick += (_, _) => Flush();
            _timer.Start();
        }
    }

    public event Action<object>? ItemReady;

    public int Pending => _queue.Count;

    public void Post(object item) => _queue.Enqueue(item);

    /// <summary>Delivers queued items on the calling (UI) thread.</summary>
    public void Flush() => Flush(_maxPerTick);

    public void FlushAll() => Flush(int.MaxValue);

    private void Flush(int max)
    {
        int n = 0;
        while (n < max && _queue.TryDequeue(out var item))
        {
            n++;
            try { ItemReady?.Invoke(item); }
            catch (Exception ex)
            {
                CrashLog.Write("EventPump item handler", ex);
            }
        }
    }
}

/// <summary>Writes unexpected exceptions to %AppData%\FogConsole\crash.log.</summary>
public static class CrashLog
{
    private static readonly object Lock = new();

    public static string PathOnDisk => System.IO.Path.Combine(SettingsService.AppDataDirectory, "crash.log");

    public static void Write(string where, Exception ex)
    {
        try
        {
            lock (Lock)
            {
                System.IO.Directory.CreateDirectory(SettingsService.AppDataDirectory);
                System.IO.File.AppendAllText(PathOnDisk,
                    $"[{DateTimeOffset.Now:yyyy-MM-dd HH:mm:ss}] {where}{Environment.NewLine}{ex}{Environment.NewLine}{Environment.NewLine}");
            }
        }
        catch
        {
            // nothing left to do
        }
    }
}
