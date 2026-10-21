namespace FogConsole.Transport;

/// <summary>Why a transport stopped.</summary>
/// <param name="Unexpected">True when the other side vanished (USB unplugged, device rebooted); false for a normal Close().</param>
/// <param name="Reason">Human readable reason.</param>
public sealed record TransportClosedInfo(bool Unexpected, string Reason);

/// <summary>
/// A bidirectional stream of text lines (JSON Lines on a USB-CDC port, or a simulated device).
/// Events are raised on a background thread.
/// </summary>
public interface ITransport : IAsyncDisposable
{
    /// <summary>Human readable name, e.g. "COM7" or "Simulator".</summary>
    string Name { get; }

    bool IsOpen { get; }

    /// <summary>A complete received line (terminator removed, never empty).</summary>
    event Action<string>? LineReceived;

    /// <summary>A received line exceeded the size limit and was dropped (argument: bytes dropped).</summary>
    event Action<int>? LineOverflow;

    /// <summary>Raised once when the transport stops (explicit close, or the port vanished).</summary>
    event Action<TransportClosedInfo>? Closed;

    Task OpenAsync(CancellationToken ct = default);

    Task CloseAsync();

    /// <summary>Writes one line; the terminator is appended by the transport.</summary>
    Task WriteLineAsync(string line, CancellationToken ct = default);
}
