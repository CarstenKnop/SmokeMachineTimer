using System.Diagnostics;

namespace FogConsole.Services;

/// <summary>
/// The time base for client-side interpolation (countdowns, rings, timelines). Can be frozen, e.g. while the simulator's
/// time is paused, so the interpolated display does not run away from the (stalled) device data.
/// </summary>
public sealed class UiClock
{
    private readonly Stopwatch _sw = Stopwatch.StartNew();
    private double _frozenAt;
    private double _pausedTotal;
    private bool _paused;

    /// <summary>Seconds since start, excluding paused periods.</summary>
    public double Seconds => (_paused ? _frozenAt : _sw.Elapsed.TotalSeconds) - _pausedTotal;

    public bool Paused
    {
        get => _paused;
        set
        {
            if (value == _paused) return;
            if (value)
            {
                _frozenAt = _sw.Elapsed.TotalSeconds;
            }
            else
            {
                _pausedTotal += _sw.Elapsed.TotalSeconds - _frozenAt;
            }
            _paused = value;
        }
    }
}
