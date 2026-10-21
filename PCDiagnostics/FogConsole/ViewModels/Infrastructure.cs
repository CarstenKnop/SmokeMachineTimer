using CommunityToolkit.Mvvm.ComponentModel;
using FogConsole.Hub;
using FogConsole.Protocol;
using FogConsole.Services;

namespace FogConsole.ViewModels;

public enum NavPage { Dashboard, Device, Pairing, Channels, Diagnostics, Console, Settings }

/// <summary>Receives every hub message on the UI thread, in order.</summary>
public interface IMessageSink
{
    void OnMessage(Message message);
}

/// <summary>Pages that start / stop background work when they become visible.</summary>
public interface IPageViewModel
{
    NavPage Page { get; }
    void Activate();
    void Deactivate();
}

/// <summary>Everything a view model may need; created once by the main view model.</summary>
public sealed class AppServices
{
    public required HubClient Hub { get; init; }
    public required SettingsService Settings { get; init; }
    public required ToastService Toasts { get; init; }
    public required IDialogService Dialogs { get; init; }
    public required IUiDispatcher Ui { get; init; }
    public required UiClock Clock { get; init; }
    public required SimulatorController Sim { get; init; }
    public required ThemeService Theme { get; init; }
    public CommandCenter Commands { get; set; } = null!;
    public FleetState Fleet { get; set; } = null!;
    public HoldController Hold { get; set; } = null!;

    /// <summary>Switches the visible page.</summary>
    public Action<NavPage> Navigate { get; set; } = _ => { };

    public bool IsConnected => Hub.State == HubState.Connected;
    public bool IsSimulated => Sim.IsActive;
}

/// <summary>Small helpers shared by view models.</summary>
public static class Fmt
{
    public static string Seconds(double s) => s >= 100 ? $"{s:0}" : s >= 10 ? $"{s:0.0}" : $"{s:0.0}";

    public static string Duration(long seconds)
    {
        if (seconds < 0) return "-";
        if (seconds < 60) return $"{seconds} s";
        if (seconds < 3600) return $"{seconds / 60} min {seconds % 60:00} s";
        if (seconds < 86400) return $"{seconds / 3600} h {seconds % 3600 / 60:00} min";
        return $"{seconds / 86400} d {seconds % 86400 / 3600} h";
    }

    public static string Ago(double seconds)
    {
        if (seconds < 1) return "just now";
        if (seconds < 60) return $"{seconds:0} s ago";
        if (seconds < 3600) return $"{seconds / 60:0} min ago";
        return $"{seconds / 3600:0.#} h ago";
    }

    public static string Count(long n) => n.ToString("N0", System.Globalization.CultureInfo.CurrentCulture);

    public static string Rate(double perSecond) =>
        perSecond <= 0 ? "0/s" : perSecond < 10 ? $"{perSecond:0.0}/s" : $"{perSecond:0}/s";
}
