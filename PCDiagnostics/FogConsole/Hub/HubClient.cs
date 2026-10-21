using System.IO;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Text.Json.Nodes;
using FogConsole.Protocol;
using FogConsole.Transport;

namespace FogConsole.Hub;

/// <summary>
/// Owns the transport and speaks FogSerial/2: request/response correlation by id, keep-alive ping with latency
/// measurement, event dispatch, awaitable command results and the connection state machine
/// (including automatic re-connection when the port vanishes).
/// Events are raised on background threads; the UI layer marshals them.
/// </summary>
public sealed class HubClient : IAsyncDisposable
{
    private readonly SemaphoreSlim _lifecycle = new(1, 1);
    private readonly ConcurrentDictionary<int, TaskCompletionSource<ResponseMessage>> _pending = new();
    private readonly ConcurrentDictionary<int, CommandHandle> _commands = new();
    private readonly ConcurrentQueue<(int Id, DateTimeOffset Finished)> _finishedCommands = new();

    private Func<ITransport>? _factory;
    private ITransport? _transport;
    private CancellationTokenSource? _loopCts;
    private bool _userDisconnected = true;
    private int _nextId;
    private volatile HubState _state = HubState.Disconnected;

    public HubClient(HubOptions? options = null)
    {
        Options = options ?? new HubOptions();
    }

    public HubOptions Options { get; }
    public HubCounters Counters { get; } = new();
    public LatencyTracker Latency { get; } = new();

    public HubState State => _state;
    public HelloInfo? Hello { get; private set; }
    /// <summary>Why the connection was lost / could not be made.</summary>
    public string? LastError { get; private set; }

    /// <summary>Error of the most recent automatic re-connection attempt (while waiting for the device).</summary>
    public string? LastAttemptError { get; private set; }
    public string TransportName => _transport?.Name ?? _lastName ?? "";
    public bool AutoReconnect { get; set; } = true;

    /// <summary>True while at least one of the recent pings went unanswered.</summary>
    public bool IsResponsive => Latency.ConsecutiveFailures == 0;

    private string? _lastName;

    // ---- events (raised on background threads) ---------------------------------------------

    public event Action<HubStateChange>? StateChanged;

    /// <summary>Every non-response message (events, plain text lines, unknown types).</summary>
    public event Action<Message>? MessageReceived;

    /// <summary>Every line in either direction (also plain text).</summary>
    public event Action<RawLine>? RawLineSeen;

    /// <summary>Raised after a successful handshake (also after an automatic re-connection).</summary>
    public event Action<HelloInfo>? Connected;

    /// <summary>A command handle changed (queued, result arrived, timed out ...).</summary>
    public event Action<CommandHandle>? CommandChanged;

    // ---- connection -----------------------------------------------------------------------

    /// <summary>
    /// Opens a transport and performs the hello handshake. On failure throws; the state returns to
    /// <see cref="HubState.Disconnected"/> unless <paramref name="waitIfAbsent"/> is set, in which case the hub
    /// keeps retrying in the background (<see cref="HubState.WaitingForDevice"/>).
    /// </summary>
    public async Task ConnectAsync(Func<ITransport> factory, bool waitIfAbsent = false, CancellationToken ct = default)
    {
        await _lifecycle.WaitAsync(ct).ConfigureAwait(false);
        try
        {
            await DisconnectCoreAsync("reconnecting", userInitiated: true).ConfigureAwait(false);
            _factory = factory;
            _userDisconnected = false;
            _loopCts = new CancellationTokenSource();
            SetState(HubState.Connecting, "connecting");
            try
            {
                await OpenAndHandshakeAsync(ct).ConfigureAwait(false);
            }
            catch (Exception ex) when (ex is not OperationCanceledException || !ct.IsCancellationRequested)
            {
                LastError = ex.Message;
                if (waitIfAbsent && AutoReconnect)
                {
                    SetState(HubState.WaitingForDevice, ex.Message);
                    StartReconnectLoop();
                    return;
                }
                await DisconnectCoreAsync(ex.Message, userInitiated: true).ConfigureAwait(false);
                throw;
            }
            catch
            {
                await DisconnectCoreAsync("cancelled", userInitiated: true).ConfigureAwait(false);
                throw;
            }
        }
        finally
        {
            _lifecycle.Release();
        }
    }

    public async Task DisconnectAsync()
    {
        await _lifecycle.WaitAsync().ConfigureAwait(false);
        try
        {
            await DisconnectCoreAsync("disconnected", userInitiated: true).ConfigureAwait(false);
        }
        finally
        {
            _lifecycle.Release();
        }
    }

    private async Task DisconnectCoreAsync(string reason, bool userInitiated)
    {
        if (userInitiated) _userDisconnected = true;
        var cts = _loopCts;
        _loopCts = null;
        try { cts?.Cancel(); } catch { /* ignore */ }

        var t = _transport;
        _transport = null;
        if (t is not null)
        {
            try { await t.CloseAsync().ConfigureAwait(false); } catch { /* ignore */ }
            try { await t.DisposeAsync().ConfigureAwait(false); } catch { /* ignore */ }
        }
        FailAllPending(new HubDisconnectedException(reason));
        cts?.Dispose();
        if (userInitiated)
        {
            _factory = null;
            Hello = null;
        }
        SetState(HubState.Disconnected, reason);
    }

    private async Task OpenAndHandshakeAsync(CancellationToken ct)
    {
        var transport = _factory!();
        transport.LineReceived += OnLine;
        transport.LineOverflow += OnOverflow;
        transport.Closed += info => OnTransportClosed(transport, info);
        _lastName = transport.Name;

        try
        {
            await transport.OpenAsync(ct).ConfigureAwait(false);
            _transport = transport;
            RaiseRaw(RawDirection.Info, RawKind.Info, $"-- opened {transport.Name} --");

            HelloInfo? hello = null;
            string? failure = null;
            for (int attempt = 0; attempt < Math.Max(1, Options.HelloAttempts); attempt++)
            {
                ct.ThrowIfCancellationRequested();
                try
                {
                    var res = await RequestCoreAsync(Requests.HelloReq(), Options.RequestTimeout, ct).ConfigureAwait(false);
                    if (res is { Ok: true, Hello: { } h }) { hello = h; break; }
                    failure = res.Describe();
                    if (!res.Ok) break;
                }
                catch (HubTimeoutException)
                {
                    failure = "the device did not answer 'hello'";
                }
            }
            if (hello is null)
                throw new HubException($"{failure ?? "no answer"} - is this a Fog remote/timer running protocol 2?");

            Hello = hello;
            Latency.Reset();
            LastError = null;
            SetState(HubState.Connected, $"connected to {hello.Kind.ToString().ToLowerInvariant()} \"{hello.Name}\"");
            StartKeepAlive();
            try { Connected?.Invoke(hello); } catch { /* subscriber bug must not kill the hub */ }
        }
        catch
        {
            _transport = null;
            try { await transport.CloseAsync().ConfigureAwait(false); } catch { /* ignore */ }
            try { await transport.DisposeAsync().ConfigureAwait(false); } catch { /* ignore */ }
            throw;
        }
    }

    private void OnTransportClosed(ITransport transport, TransportClosedInfo info)
    {
        if (!info.Unexpected) return;
        // fail in-flight requests right away (a handshake in progress must not wait for its timeouts)
        if (ReferenceEquals(_transport, transport)) FailAllPending(new HubDisconnectedException(info.Reason));
        // never block the reader thread: handle on the thread pool
        _ = Task.Run(async () =>
        {
            await _lifecycle.WaitAsync().ConfigureAwait(false);
            try
            {
                if (!ReferenceEquals(_transport, transport) || _userDisconnected) return;

                RaiseRaw(RawDirection.Info, RawKind.Info, $"-- connection lost: {info.Reason} --");
                LastError = info.Reason;
                var cts = _loopCts;
                _loopCts = null;
                try { cts?.Cancel(); } catch { /* ignore */ }
                _transport = null;
                try { await transport.CloseAsync().ConfigureAwait(false); } catch { /* ignore */ }
                try { await transport.DisposeAsync().ConfigureAwait(false); } catch { /* ignore */ }
                cts?.Dispose();
                FailAllPending(new HubDisconnectedException(info.Reason));

                if (AutoReconnect && _factory is not null)
                {
                    _loopCts = new CancellationTokenSource();
                    SetState(HubState.WaitingForDevice, info.Reason);
                    StartReconnectLoop();
                }
                else
                {
                    SetState(HubState.Disconnected, info.Reason);
                }
            }
            catch
            {
                // swallow: this runs fire-and-forget
            }
            finally
            {
                _lifecycle.Release();
            }
        });
    }

    private void StartReconnectLoop()
    {
        var cts = _loopCts;
        if (cts is null) return;
        var token = cts.Token;
        _ = Task.Run(async () =>
        {
            while (!token.IsCancellationRequested)
            {
                try { await Task.Delay(Options.ReconnectInterval, token).ConfigureAwait(false); }
                catch (OperationCanceledException) { return; }

                try { await _lifecycle.WaitAsync(token).ConfigureAwait(false); }
                catch (OperationCanceledException) { return; }
                try
                {
                    if (token.IsCancellationRequested || _userDisconnected || _factory is null) return;
                    await OpenAndHandshakeAsync(token).ConfigureAwait(false);
                    return; // connected
                }
                catch (OperationCanceledException)
                {
                    return;
                }
                catch (Exception ex)
                {
                    LastAttemptError = ex.Message; // stay in WaitingForDevice and try again
                }
                finally
                {
                    _lifecycle.Release();
                }
            }
        });
    }

    private void StartKeepAlive()
    {
        if (!Options.KeepAlive) return;
        var token = _loopCts?.Token ?? CancellationToken.None;
        _ = Task.Run(() => KeepAliveLoopAsync(token));
    }

    private async Task KeepAliveLoopAsync(CancellationToken ct)
    {
        using var timer = new PeriodicTimer(Options.PingInterval);
        try
        {
            while (await timer.WaitForNextTickAsync(ct).ConfigureAwait(false))
            {
                CleanupFinishedCommands();
                var sw = Stopwatch.StartNew();
                try
                {
                    await RequestCoreAsync(Requests.Ping(), Options.RequestTimeout, ct).ConfigureAwait(false);
                    Latency.Add(sw.Elapsed.TotalMilliseconds);
                }
                catch (HubTimeoutException)
                {
                    int fails = Latency.Fail();
                    if (fails >= Options.PingFailuresBeforeReconnect && AutoReconnect)
                    {
                        // the handle may be stale (device re-enumerated without us noticing)
                        var t = _transport;
                        if (t is not null) OnTransportClosed(t, new TransportClosedInfo(true, "device stopped answering"));
                        return;
                    }
                }
                catch (HubDisconnectedException)
                {
                    return;
                }
            }
        }
        catch (OperationCanceledException)
        {
            // normal shutdown
        }
    }

    private void CleanupFinishedCommands()
    {
        var limit = DateTimeOffset.UtcNow - TimeSpan.FromSeconds(60);
        while (_finishedCommands.TryPeek(out var f) && f.Finished < limit)
        {
            _finishedCommands.TryDequeue(out _);
            _commands.TryRemove(f.Id, out _);
        }
    }

    private void SetState(HubState state, string reason)
    {
        var old = _state;
        if (old == state) return;
        _state = state;
        RaiseRaw(RawDirection.Info, RawKind.Info, $"-- {state}{(string.IsNullOrEmpty(reason) ? "" : ": " + reason)} --");
        try { StateChanged?.Invoke(new HubStateChange(old, state, reason)); } catch { /* ignore */ }
    }

    // ---- requests -------------------------------------------------------------------------

    private int NextId()
    {
        int id = Interlocked.Increment(ref _nextId);
        if (id is <= 0 or > 1_000_000_000)
        {
            Interlocked.Exchange(ref _nextId, 1);
            id = 1;
        }
        return id;
    }

    /// <summary>
    /// Sends a request and awaits its response. Throws <see cref="HubTimeoutException"/> when nothing arrives in time
    /// (after <paramref name="retries"/> additional attempts - only use retries for idempotent requests) and
    /// <see cref="HubDisconnectedException"/> when the connection is lost. An <c>ok:false</c> response is returned, not thrown.
    /// </summary>
    public async Task<ResponseMessage> RequestAsync(JsonObject request, TimeSpan? timeout = null, int retries = 0, CancellationToken ct = default)
    {
        string type = request["t"]?.GetValue<string>() ?? "";
        string? what = request["what"]?.GetValue<string>();
        var to = timeout ?? Options.TimeoutFor(type, what);
        for (int attempt = 0; ; attempt++)
        {
            try
            {
                return await RequestCoreAsync(request, to, ct).ConfigureAwait(false);
            }
            catch (HubTimeoutException) when (attempt < retries)
            {
                // retry with a fresh id
            }
        }
    }

    /// <summary>Like <see cref="RequestAsync"/> but throws <see cref="HubRequestException"/> when the device answers ok:false.</summary>
    public async Task<ResponseMessage> RequestOkAsync(JsonObject request, TimeSpan? timeout = null, int retries = 0, CancellationToken ct = default)
    {
        var res = await RequestAsync(request, timeout, retries, ct).ConfigureAwait(false);
        if (!res.Ok) throw new HubRequestException(res);
        return res;
    }

    private async Task<ResponseMessage> RequestCoreAsync(JsonObject request, TimeSpan timeout, CancellationToken ct)
    {
        var transport = _transport ?? throw new HubDisconnectedException();
        string type = request["t"]?.GetValue<string>() ?? "";
        int id = NextId();
        var tcs = new TaskCompletionSource<ResponseMessage>(TaskCreationOptions.RunContinuationsAsynchronously);
        _pending[id] = tcs;
        try
        {
            await WriteAsync(transport, Requests.Serialize(request, id), ct).ConfigureAwait(false);
            return await tcs.Task.WaitAsync(timeout, ct).ConfigureAwait(false);
        }
        catch (TimeoutException)
        {
            Counters.Timeout();
            throw new HubTimeoutException(type, timeout);
        }
        finally
        {
            _pending.TryRemove(id, out _);
        }
    }

    private async Task WriteAsync(ITransport transport, string line, CancellationToken ct)
    {
        try
        {
            await transport.WriteLineAsync(line, ct).ConfigureAwait(false);
        }
        catch (Exception ex) when (ex is IOException or InvalidOperationException or ObjectDisposedException or UnauthorizedAccessException)
        {
            throw new HubDisconnectedException("Write failed: " + ex.Message, ex);
        }
        Counters.Tx();
        RaiseRaw(RawDirection.Tx, RawKind.Json, line);
    }

    /// <summary>Sends a line exactly as given (Raw console); the answer shows up in the raw log.</summary>
    public async Task SendRawAsync(string line, CancellationToken ct = default)
    {
        var transport = _transport ?? throw new HubDisconnectedException();
        await WriteAsync(transport, line, ct).ConfigureAwait(false);
    }

    // ---- commands -------------------------------------------------------------------------

    /// <summary>Starts a command and returns immediately; await <see cref="CommandHandle.Completion"/> for the outcome.</summary>
    public CommandHandle BeginCommand(CommandSpec spec, TimeSpan? timeout = null)
    {
        var handle = new CommandHandle(spec);
        handle.Changed += h => CommandChanged?.Invoke(h);
        _ = RunCommandAsync(handle, timeout ?? Options.CommandTimeout);
        return handle;
    }

    /// <summary>Sends a <c>cmd</c> and completes when the cmdResult(s) arrived (or the ~4 s time budget ran out).</summary>
    public async Task<CommandHandle> SendCommandAsync(CommandSpec spec, TimeSpan? timeout = null, CancellationToken ct = default)
    {
        var handle = BeginCommand(spec, timeout);
        return await handle.Completion.WaitAsync(ct).ConfigureAwait(false);
    }

    private async Task RunCommandAsync(CommandHandle handle, TimeSpan timeout)
    {
        var deadline = Stopwatch.StartNew();
        int id = NextId();
        handle.Id = id;
        _commands[id] = handle;
        var tcs = new TaskCompletionSource<ResponseMessage>(TaskCreationOptions.RunContinuationsAsynchronously);
        _pending[id] = tcs;
        try
        {
            var transport = _transport ?? throw new HubDisconnectedException();
            await WriteAsync(transport, Requests.Serialize(Requests.Cmd(handle.Spec), id), CancellationToken.None).ConfigureAwait(false);

            var res = await tcs.Task.WaitAsync(timeout).ConfigureAwait(false);
            if (!res.Ok)
            {
                handle.MarkRejected(res);
                return;
            }
            // fire-and-forget commands (confirm:false) never produce a cmdResult
            handle.MarkQueued(handle.Spec.Confirm ? res.Queued ?? 1 : 0);
            if (handle.IsFinished) return;

            var remaining = timeout - deadline.Elapsed;
            if (remaining < TimeSpan.FromMilliseconds(50)) remaining = TimeSpan.FromMilliseconds(50);
            try
            {
                await handle.Completion.WaitAsync(remaining).ConfigureAwait(false);
            }
            catch (TimeoutException)
            {
                handle.MarkTimedOut();
            }
        }
        catch (TimeoutException)
        {
            Counters.Timeout();
            handle.MarkTimedOut();
        }
        catch (HubDisconnectedException ex)
        {
            handle.MarkFailed(ex.Message);
        }
        catch (Exception ex)
        {
            handle.MarkFailed(ex.Message);
        }
        finally
        {
            _pending.TryRemove(id, out _);
            _finishedCommands.Enqueue((id, DateTimeOffset.UtcNow));
            if (!handle.IsFinished) handle.MarkFailed("aborted");
        }
    }

    // ---- receiving ------------------------------------------------------------------------

    private void OnOverflow(int bytes)
    {
        Counters.Over();
        RaiseRaw(RawDirection.Rx, RawKind.Oversize, $"(line of {bytes} bytes dropped - longer than the allowed maximum)");
    }

    private void OnLine(string line)
    {
        Counters.Rx();
        var msg = ProtocolParser.Parse(line);
        switch (msg)
        {
            case PlainTextMessage p:
                if (p.MalformedJson) Counters.Bad(); else Counters.Plain();
                RaiseRaw(RawDirection.Rx, p.MalformedJson ? RawKind.MalformedJson : RawKind.PlainText, line);
                break;
            default:
                RaiseRaw(RawDirection.Rx, RawKind.Json, line);
                break;
        }

        switch (msg)
        {
            case ResponseMessage res:
                if (_pending.TryRemove(res.Id, out var tcs)) tcs.TrySetResult(res);
                else Counters.Unmatched();
                return; // responses are not "events"
            case CmdResultMessage cr:
                if (_commands.TryGetValue(cr.Id, out var handle)) handle.AddResult(cr);
                break;
        }

        try { MessageReceived?.Invoke(msg); }
        catch { /* subscriber bug must not kill the reader */ }
    }

    private void RaiseRaw(RawDirection dir, RawKind kind, string text)
    {
        try { RawLineSeen?.Invoke(new RawLine(DateTimeOffset.Now, dir, kind, text)); }
        catch { /* ignore */ }
    }

    private void FailAllPending(Exception ex)
    {
        foreach (var key in _pending.Keys)
            if (_pending.TryRemove(key, out var tcs))
                tcs.TrySetException(ex);
        foreach (var h in _commands.Values)
            if (!h.IsFinished) h.MarkFailed(ex.Message);
    }

    public async ValueTask DisposeAsync()
    {
        try { await DisconnectAsync().ConfigureAwait(false); } catch { /* ignore */ }
    }
}
