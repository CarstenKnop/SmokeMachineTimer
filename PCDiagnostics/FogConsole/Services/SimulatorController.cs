using CommunityToolkit.Mvvm.ComponentModel;
using FogConsole.Simulator;
using FogConsole.Transport;

namespace FogConsole.Services;

/// <summary>
/// Owns the simulated remote ("offline demo") and its demo controls: number of timers, packet loss, power cuts,
/// interference, pausing time and the random seed.
/// </summary>
public sealed partial class SimulatorController : ObservableObject
{
    public const int MaxTimers = 10;

    private SimDevice? _device;

    [ObservableProperty] private bool _isActive;
    [ObservableProperty] private int _seed = 42;
    [ObservableProperty] private int _timerCount = 7;
    [ObservableProperty] private double _lossPercent;
    [ObservableProperty] private bool _timePaused;
    [ObservableProperty] private SimKind _kind = SimKind.Remote;
    [ObservableProperty] private bool _demoScenario = true;

    /// <summary>The key the simulated timers expect for over-the-air updates (null = the default key; a different key in Settings gives "rejected").</summary>
    public string? OtaKey { get; set; }

    /// <summary>Random power cuts and CPU hiccups (off = a calm, reproducible world).</summary>
    [ObservableProperty] private bool _chaos = true;

    public SimDevice? Device => _device;

    public SimOptions BuildOptions() => new()
    {
        Seed = Seed,
        Kind = Kind,
        PairedTimers = Math.Clamp(TimerCount, 1, MaxTimers),
        TotalTimers = Math.Min(13, Math.Clamp(TimerCount, 1, MaxTimers) + 3),
        Chaos = Chaos,
        Demo = DemoScenario && TimerCount >= 4,
        OtaKey = OtaKey,
        OtaNoSlotIndex = 4,          // Fog5: OTA code, but only one application slot ("needs a USB update first")
        OtaLegacyIndex = 6,          // Fog7: old firmware without OTA support
    };

    /// <summary>Creates a fresh simulated device (the old one is discarded) and applies the live demo settings.</summary>
    public void StartNew()
    {
        _device = new SimDevice(BuildOptions());
        _device.ExtraLossPercent = LossPercent;
        _device.TimePaused = false;
        TimePaused = false;
        IsActive = true;
    }

    public void Stop()
    {
        IsActive = false;
        TimePaused = false;
    }

    public ITransport CreateTransport()
    {
        _device ??= new SimDevice(BuildOptions());
        return new SimTransport(_device);
    }

    partial void OnLossPercentChanged(double value)
    {
        if (_device is not null) _device.ExtraLossPercent = value;
    }

    partial void OnTimePausedChanged(bool value)
    {
        if (_device is not null) _device.TimePaused = value;
    }

    public bool CutPower(int pairedIndex, int seconds = 30) =>
        _device?.CutPower(pairedIndex, TimeSpan.FromSeconds(seconds)) ?? false;

    public int Interfere(int seconds = 60) =>
        _device?.InjectInterference(TimeSpan.FromSeconds(seconds)) ?? 0;
}
