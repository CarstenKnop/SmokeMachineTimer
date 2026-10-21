using System.Collections.ObjectModel;
using System.Windows.Threading;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;

namespace FogConsole.Services;

public enum ToastKind { Info, Success, Warning, Error }

public sealed partial class ToastItem : ObservableObject
{
    private readonly ToastService _owner;

    public ToastItem(ToastService owner, ToastKind kind, string message, string? title, DateTime expires)
    {
        _owner = owner;
        Kind = kind;
        Message = message;
        Title = title;
        Expires = expires;
        Time = DateTime.Now;
    }

    public ToastKind Kind { get; }
    public string Message { get; }
    public string? Title { get; }
    public DateTime Time { get; }
    public DateTime Expires { get; }

    [RelayCommand]
    private void Dismiss() => _owner.Dismiss(this);
}

/// <summary>Transient notifications (toasts) plus the "last message" shown in the status bar. UI thread only.</summary>
public sealed partial class ToastService : ObservableObject
{
    private readonly DispatcherTimer? _timer;

    public ToastService(Dispatcher? dispatcher)
    {
        if (dispatcher is not null)
        {
            _timer = new DispatcherTimer(DispatcherPriority.Background, dispatcher) { Interval = TimeSpan.FromMilliseconds(500) };
            _timer.Tick += (_, _) => Expire();
            _timer.Start();
        }
    }

    public ObservableCollection<ToastItem> Toasts { get; } = new();

    [ObservableProperty] private string _statusText = "Ready";
    [ObservableProperty] private ToastKind _statusKind = ToastKind.Info;

    public int MaxToasts { get; set; } = 4;

    /// <summary>Raised for every message (also those that only go to the status bar).</summary>
    public event Action<ToastKind, string>? Posted;

    /// <summary>Shows a toast and updates the status bar.</summary>
    public void Show(ToastKind kind, string message, string? title = null, TimeSpan? duration = null)
    {
        StatusText = message;
        StatusKind = kind;
        Posted?.Invoke(kind, message);
        var life = duration ?? kind switch
        {
            ToastKind.Error => TimeSpan.FromSeconds(9),
            ToastKind.Warning => TimeSpan.FromSeconds(7),
            _ => TimeSpan.FromSeconds(4),
        };
        // identical message already visible: just extend it
        var existing = Toasts.FirstOrDefault(t => t.Message == message && t.Kind == kind);
        if (existing is not null) Toasts.Remove(existing);
        Toasts.Add(new ToastItem(this, kind, message, title, DateTime.Now + life));
        while (Toasts.Count > MaxToasts) Toasts.RemoveAt(0);
    }

    public void Info(string message) => Show(ToastKind.Info, message);
    public void Success(string message) => Show(ToastKind.Success, message);
    public void Warning(string message) => Show(ToastKind.Warning, message);
    public void Error(string message) => Show(ToastKind.Error, message);

    /// <summary>Status bar only, no toast.</summary>
    public void Status(string message, ToastKind kind = ToastKind.Info)
    {
        StatusText = message;
        StatusKind = kind;
        Posted?.Invoke(kind, message);
    }

    public void Dismiss(ToastItem item) => Toasts.Remove(item);

    public void Expire()
    {
        var now = DateTime.Now;
        for (int i = Toasts.Count - 1; i >= 0; i--)
            if (Toasts[i].Expires <= now) Toasts.RemoveAt(i);
    }
}
