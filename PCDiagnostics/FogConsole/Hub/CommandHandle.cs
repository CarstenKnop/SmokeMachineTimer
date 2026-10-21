using FogConsole.Protocol;

namespace FogConsole.Hub;

public enum CommandState
{
    /// <summary>Request written, waiting for the hub's "queued" response.</summary>
    Sending,

    /// <summary>Queued on the remote; waiting for the timer(s) to confirm.</summary>
    Queued,

    /// <summary>All expected cmdResult events arrived (individual results may still be failures).</summary>
    Completed,

    /// <summary>The device refused the request (badArg, noDevice, busy, ...).</summary>
    Rejected,

    /// <summary>Not all results arrived within the client side time budget.</summary>
    TimedOut,

    /// <summary>Connection lost / write failed.</summary>
    Failed,
}

/// <summary>
/// Tracks one <c>cmd</c> request from "sent" to the last <c>cmdResult</c>. Thread-safe; <see cref="Changed"/> is raised on a
/// background thread.
/// </summary>
public sealed class CommandHandle
{
    private readonly object _lock = new();
    private readonly List<CmdResultMessage> _results = new();
    private readonly TaskCompletionSource<CommandHandle> _done = new(TaskCreationOptions.RunContinuationsAsynchronously);

    public CommandHandle(CommandSpec spec)
    {
        Spec = spec;
        Created = DateTimeOffset.Now;
    }

    public int Id { get; internal set; }
    public CommandSpec Spec { get; }
    public DateTimeOffset Created { get; }
    public CommandState State { get; private set; } = CommandState.Sending;

    /// <summary>Number of cmdResult events expected (known after the hub answered), -1 before.</summary>
    public int Expected { get; private set; } = -1;

    /// <summary>Error text when rejected or failed.</summary>
    public string? Error { get; private set; }

    public IReadOnlyList<CmdResultMessage> Results
    {
        get { lock (_lock) return _results.ToArray(); }
    }

    /// <summary>Completes (never faults) when the command reached a final state; see <see cref="State"/>.</summary>
    public Task<CommandHandle> Completion => _done.Task;

    public bool IsFinished => State is CommandState.Completed or CommandState.Rejected or CommandState.TimedOut or CommandState.Failed;

    /// <summary>True when finished and every addressed timer confirmed.</summary>
    public bool AllOk
    {
        get
        {
            lock (_lock)
                return State == CommandState.Completed && _results.Count > 0 && _results.All(r => r.Result == CmdOutcome.Ok);
        }
    }

    public int OkCount
    {
        get { lock (_lock) return _results.Count(r => r.Result == CmdOutcome.Ok); }
    }

    public event Action<CommandHandle>? Changed;

    internal void MarkQueued(int queued)
    {
        bool done;
        lock (_lock)
        {
            Expected = Math.Max(queued, 0);
            State = CommandState.Queued;
            done = _results.Count >= Expected;
            if (done) State = CommandState.Completed;
        }
        Raise();
        if (done) Finish();
    }

    internal void AddResult(CmdResultMessage r)
    {
        bool done = false;
        lock (_lock)
        {
            _results.Add(r);
            if (Expected >= 0 && _results.Count >= Expected)
            {
                done = true;
                // late results can upgrade a timed-out command to completed
                if (State is CommandState.Queued or CommandState.TimedOut) State = CommandState.Completed;
            }
        }
        Raise();
        if (done) Finish();
    }

    internal void MarkRejected(ResponseMessage res)
    {
        lock (_lock)
        {
            State = CommandState.Rejected;
            Error = res.Describe();
        }
        Raise();
        Finish();
    }

    internal void MarkTimedOut()
    {
        lock (_lock)
        {
            if (IsFinished) return;
            State = CommandState.TimedOut;
            Error = Expected < 0 ? "The remote did not answer" : $"Only {_results.Count} of {Expected} result(s) arrived";
        }
        Raise();
        Finish();
    }

    internal void MarkFailed(string error)
    {
        lock (_lock)
        {
            if (IsFinished) return;
            State = CommandState.Failed;
            Error = error;
        }
        Raise();
        Finish();
    }

    private void Raise()
    {
        try { Changed?.Invoke(this); }
        catch { /* a faulty subscriber must not break the reader thread */ }
    }

    private void Finish()
    {
        _done.TrySetResult(this);
    }
}
