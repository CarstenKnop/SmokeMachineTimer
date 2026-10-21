using FogConsole.Protocol;

namespace FogConsole.Hub;

public enum HubState
{
    Disconnected,
    Connecting,
    Connected,

    /// <summary>The port vanished (device rebooted / deep sleep / unplugged); we keep trying to reopen it.</summary>
    WaitingForDevice,
}

public readonly record struct HubStateChange(HubState Old, HubState New, string Reason);

public enum RawDirection { Rx, Tx, Info }

public enum RawKind { Json, PlainText, MalformedJson, Oversize, Info }

/// <summary>One line as seen on the wire (for the Raw console).</summary>
public readonly record struct RawLine(DateTimeOffset Time, RawDirection Direction, RawKind Kind, string Text);

public class HubException : Exception
{
    public HubException(string message, Exception? inner = null) : base(message, inner) { }
}

public sealed class HubDisconnectedException : HubException
{
    public HubDisconnectedException(string message = "Not connected", Exception? inner = null) : base(message, inner) { }
}

public sealed class HubTimeoutException : HubException
{
    public string RequestType { get; }
    public TimeSpan Timeout { get; }

    public HubTimeoutException(string requestType, TimeSpan timeout)
        : base($"No answer to '{requestType}' within {timeout.TotalSeconds:0.#} s")
    {
        RequestType = requestType;
        Timeout = timeout;
    }
}

public sealed class HubRequestException : HubException
{
    public ResponseMessage Response { get; }

    public HubRequestException(ResponseMessage response)
        : base(response.Describe())
    {
        Response = response;
    }
}

public sealed class HubOptions
{
    public TimeSpan RequestTimeout { get; init; } = TimeSpan.FromSeconds(1.5);

    /// <summary><c>get devStats</c> asks a timer over the radio and may take up to 3.5 s.</summary>
    public TimeSpan DevStatsTimeout { get; init; } = TimeSpan.FromSeconds(5);

    /// <summary>How long <c>SendCommandAsync</c> waits for the cmdResult(s).</summary>
    public TimeSpan CommandTimeout { get; init; } = TimeSpan.FromSeconds(4);

    public TimeSpan PingInterval { get; init; } = TimeSpan.FromSeconds(2);

    public TimeSpan ReconnectInterval { get; init; } = TimeSpan.FromSeconds(1);

    public int HelloAttempts { get; init; } = 3;

    /// <summary>Consecutive unanswered pings before the connection is considered dead and re-opened.</summary>
    public int PingFailuresBeforeReconnect { get; init; } = 4;

    public bool KeepAlive { get; init; } = true;

    public TimeSpan TimeoutFor(string type, string? what = null) =>
        type == "get" && what == "devStats" ? DevStatsTimeout : RequestTimeout;
}

/// <summary>Thread-safe counters for the status bar.</summary>
public sealed class HubCounters
{
    private long _linesRx, _linesTx, _badJson, _plainText, _oversize, _unmatched, _timeouts;

    public long LinesRx => Interlocked.Read(ref _linesRx);
    public long LinesTx => Interlocked.Read(ref _linesTx);
    public long BadJson => Interlocked.Read(ref _badJson);
    public long PlainText => Interlocked.Read(ref _plainText);
    public long Oversize => Interlocked.Read(ref _oversize);
    public long UnmatchedResponses => Interlocked.Read(ref _unmatched);
    public long Timeouts => Interlocked.Read(ref _timeouts);

    internal void Rx() => Interlocked.Increment(ref _linesRx);
    internal void Tx() => Interlocked.Increment(ref _linesTx);
    internal void Bad() => Interlocked.Increment(ref _badJson);
    internal void Plain() => Interlocked.Increment(ref _plainText);
    internal void Over() => Interlocked.Increment(ref _oversize);
    internal void Unmatched() => Interlocked.Increment(ref _unmatched);
    internal void Timeout() => Interlocked.Increment(ref _timeouts);
}

/// <summary>Round-trip latency of the keep-alive ping.</summary>
public sealed class LatencyTracker
{
    private readonly object _lock = new();
    private readonly Queue<double> _recent = new();

    public double LastMs { get; private set; } = double.NaN;
    public double AvgMs { get; private set; } = double.NaN;
    public double MaxMs { get; private set; } = double.NaN;
    public int Samples { get; private set; }
    public int ConsecutiveFailures { get; private set; }

    public void Add(double ms)
    {
        lock (_lock)
        {
            LastMs = ms;
            _recent.Enqueue(ms);
            while (_recent.Count > 20) _recent.Dequeue();
            AvgMs = _recent.Average();
            MaxMs = double.IsNaN(MaxMs) ? ms : Math.Max(MaxMs, ms);
            Samples++;
            ConsecutiveFailures = 0;
        }
    }

    public int Fail()
    {
        lock (_lock) return ++ConsecutiveFailures;
    }

    public void Reset()
    {
        lock (_lock)
        {
            _recent.Clear();
            LastMs = AvgMs = MaxMs = double.NaN;
            Samples = 0;
            ConsecutiveFailures = 0;
        }
    }
}
