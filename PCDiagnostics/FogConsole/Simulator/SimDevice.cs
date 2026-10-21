using System.Diagnostics;
using System.Text.Json.Nodes;
using FogConsole.Protocol;

namespace FogConsole.Simulator;

public enum SimKind { Remote, Timer }

public sealed class SimOptions
{
    public int Seed { get; init; } = 1;
    public SimKind Kind { get; init; } = SimKind.Remote;

    /// <summary>How many of the virtual timers start out paired (the rest can be discovered and paired).</summary>
    public int PairedTimers { get; init; } = 7;

    public int TotalTimers { get; init; } = 10;

    /// <summary>Random power cuts ("the machine is heating"), CPU hiccups and so on.</summary>
    public bool Chaos { get; init; } = true;

    /// <summary>Scripted start situation: one machine is already heating (offline), one loses power shortly after start.</summary>
    public bool Demo { get; init; } = true;

    public double PowerCutsPerTimerPerMinute { get; init; } = 0.10;

    public int HomeChannel { get; init; } = 6;

    /// <summary>Wall clock speed multiplier used by <see cref="SimDevice.SyncToClock"/>.</summary>
    public double Speed { get; init; } = 1.0;
}

/// <summary>
/// A software model of the remote control firmware (or of a timer connected directly): it speaks the real
/// FogSerial/2 JSON protocol and drives ten virtual smoke machine timers, a noisy 2.4 GHz band and the
/// performance counters. Runs in virtual time (<see cref="Advance"/>) so that it is deterministic for a given seed.
/// </summary>
public sealed partial class SimDevice
{
    public const int StepMs = 50;
    private const int MaxDevs = 10;

    private readonly object _lock = new();
    private readonly SimOptions _opt;
    private readonly Random _rng;
    private readonly SimBand _band;
    private readonly List<SimTimer> _timers = new();   // every virtual machine in range
    private readonly List<SimTimer> _paired = new();   // the remote's fleet list
    private readonly List<string> _out = new();
    private readonly PriorityQueue<Action, (long Due, long Seq)> _sched = new();
    private readonly Stopwatch _wall = Stopwatch.StartNew();

    private long _now;                 // device millis()
    private long _schedSeq;
    private double _stepRemainder;
    private double _extraLoss;
    private bool _timePaused;
    private double _wallConsumedMs;
    private int _homeCh;
    private int _active;
    private long _fleetSeq;

    public SimDevice(SimOptions? options = null)
    {
        _opt = options ?? new SimOptions();
        _rng = new Random(_opt.Seed);
        _band = new SimBand(_rng);
        _homeCh = Math.Clamp(_opt.HomeChannel, 1, 13);
        _now = 123_450;
        InitSelf();
        BuildTimers();
        PreloadCounters();
    }

    public SimKind Kind => _opt.Kind;
    public long NowMs { get { lock (_lock) return _now; } }

    /// <summary>Raised (under the device lock) when the device wants to reboot; the argument is the time the USB port stays away.</summary>
    public event Action<TimeSpan>? RebootRequested;

    /// <summary>Wall clock moment until which the virtual USB port does not exist (after a reboot).</summary>
    public DateTime DownUntilUtc { get; set; } = DateTime.MinValue;

    // ---- hooks for tests -----------------------------------------------------------------

    internal void TestPowerCut(int pairedIndex, double ms, bool brownout) => CutPower(pairedIndex, TimeSpan.FromMilliseconds(ms), brownout);

    internal T TestRead<T>(Func<IReadOnlyList<SimTimer>, T> read)
    {
        lock (_lock) return read(_paired);
    }

    // ---- demo controls (used by the Simulator panel) ---------------------------------------

    /// <summary>Number of timers in the remote's fleet list.</summary>
    public int PairedCount { get { lock (_lock) return _paired.Count; } }

    /// <summary>Additional packet loss in percent on every link (0 ... 90).</summary>
    public double ExtraLossPercent
    {
        get { lock (_lock) return _extraLoss * 100.0; }
        set { lock (_lock) _extraLoss = Math.Clamp(value, 0, 90) / 100.0; }
    }

    /// <summary>When true the virtual clock stands still (the devices keep answering requests but nothing evolves).</summary>
    public bool TimePaused
    {
        get { lock (_lock) return _timePaused; }
        set
        {
            lock (_lock)
            {
                if (_timePaused == value) return;
                _timePaused = value;
                // the wall time spent paused must not be caught up on resume
                _wallConsumedMs = _wall.Elapsed.TotalMilliseconds * _opt.Speed;
            }
        }
    }

    /// <summary>Cuts the power of one paired timer ("the machine is heating up"); it returns with boot+1.</summary>
    public bool CutPower(int pairedIndex, TimeSpan duration, bool brownout = false)
    {
        lock (_lock)
        {
            if (pairedIndex < 0 || pairedIndex >= _paired.Count) return false;
            _paired[pairedIndex].PowerCut(duration.TotalMilliseconds, brownout);
            return true;
        }
    }

    /// <summary>A jammer appears on a channel (default: the current home channel) for a while.</summary>
    public int InjectInterference(TimeSpan duration, int? channel = null)
    {
        lock (_lock)
        {
            int ch = channel ?? _homeCh;
            _band.Inject(ch);
            Schedule(duration.TotalMilliseconds, () => _band.Remove(ch));
            Log(2, "sim", $"interference injected on ch{ch} for {duration.TotalSeconds:0} s");
            return ch;
        }
    }

    // ---- public API ----------------------------------------------------------------------

    /// <summary>Feeds one line coming from the PC.</summary>
    public void HandleLine(string line)
    {
        lock (_lock) HandleLineCore(line);
    }

    /// <summary>Advances virtual time.</summary>
    public void Advance(TimeSpan dt)
    {
        lock (_lock) AdvanceCore(dt.TotalMilliseconds);
    }

    /// <summary>Advances virtual time by the wall clock time elapsed since the last call (times Speed).</summary>
    public void SyncToClock()
    {
        lock (_lock)
        {
            double elapsed = _wall.Elapsed.TotalMilliseconds * _opt.Speed;
            double delta = elapsed - _wallConsumedMs;
            _wallConsumedMs = elapsed;
            if (delta > 0 && !_timePaused) AdvanceCore(delta);
        }
    }

    /// <summary>Takes everything the device wants to send to the PC.</summary>
    public List<string> DrainOutput()
    {
        lock (_lock)
        {
            if (_out.Count == 0) return new List<string>(0);
            var copy = new List<string>(_out);
            _out.Clear();
            return copy;
        }
    }

    // ---- time -----------------------------------------------------------------------------

    private void AdvanceCore(double ms)
    {
        _stepRemainder += ms;
        int steps = (int)(_stepRemainder / StepMs);
        // cap catch-up work after long pauses (e.g. a debugger break)
        if (steps > 20_000) steps = 20_000;
        _stepRemainder -= Math.Floor(_stepRemainder / StepMs) * StepMs;
        for (int i = 0; i < steps; i++) Step();
    }

    private void Schedule(double delayMs, Action action) =>
        _sched.Enqueue(action, (_now + (long)Math.Max(0, Math.Round(delayMs)), _schedSeq++));

    private void RunScheduled()
    {
        while (_sched.TryPeek(out var act, out var key) && key.Due <= _now)
        {
            _sched.Dequeue();
            act();
        }
    }

    private void Step()
    {
        _now += StepMs;
        RunScheduled();
        if (_opt.Kind == SimKind.Timer) StepDirectTimer();
        else StepRemote();
    }

    // ---- helpers --------------------------------------------------------------------------

    private double NextGaussian()
    {
        double u1 = 1.0 - _rng.NextDouble();
        double u2 = _rng.NextDouble();
        return Math.Sqrt(-2.0 * Math.Log(u1)) * Math.Cos(2.0 * Math.PI * u2);
    }

    private int IndexOf(SimTimer t) => _paired.IndexOf(t);

    // While the remote's own radio is busy (survey, recover) it cannot hear anybody and suspends offline detection.
    /// <summary>A timer counts as online when a frame was heard within this window (protocol: 4.5 s).</summary>
    public const int OnlineWindowMs = 4500;

    private bool IsOnline(SimTimer t) =>
        t.EverHeard && (_now - t.LastHeardMs < OnlineWindowMs || (t.WasOnline && _now < _radioBusyUntil + 800));

    private static string ToWireMac(int n) => $"AA:BB:CC:DD:EE:{n:X2}";

    // ---- output ---------------------------------------------------------------------------

    private void Out(JsonObject o)
    {
        var s = o.ToJsonString();
        if (_out.Count > 20_000) { _pcTxDrop++; return; }
        _out.Add(s);
        _pcLinesTx++;
    }

    /// <summary>Events are only streamed while a PC session is active.</summary>
    private void Emit(JsonObject o)
    {
        if (!PcPresent) return;
        Out(o);
    }

    private JsonObject Res(int id, bool ok = true) => new() { ["t"] = "res", ["id"] = id, ["ok"] = ok };

    private void Reply(int id, Action<JsonObject>? fill = null)
    {
        var r = Res(id);
        fill?.Invoke(r);
        Out(r);
    }

    private void Fail(int id, string err, string msg)
    {
        var r = Res(id, false);
        r["err"] = err;
        r["msg"] = msg;
        Out(r);
    }

    private static readonly string[] LevelNames = { "error", "warn", "info", "debug" };

    private void Log(int level, string src, string msg)
    {
        if (!PcPresent || _logLevel < level) return;
        Emit(new JsonObject { ["t"] = "log", ["lvl"] = LevelNames[level], ["ms"] = _now, ["src"] = src, ["msg"] = msg });
    }

    // ---- sniffer --------------------------------------------------------------------------

    private double _sniffTokens = 30;
    private long _sniffLastMs;
    private int _frameSeq;

    private void SniffFrame(string dir, string mac, string kind, int len, int rssi)
    {
        if (!_sniff || !PcPresent) return;
        double refill = (_now - _sniffLastMs) / 1000.0 * 30.0;
        _sniffLastMs = _now;
        _sniffTokens = Math.Min(30, _sniffTokens + refill);
        if (_sniffTokens < 1) return;
        _sniffTokens -= 1;

        int seq = ++_frameSeq;
        var bytes = new byte[Math.Min(len, 48)];
        bytes[0] = 0xF6; // magic
        if (bytes.Length > 1) bytes[1] = 0x02; // version
        if (bytes.Length > 2) bytes[2] = (byte)(kind.Length > 0 ? kind[0] : 0);
        if (bytes.Length > 3) bytes[3] = (byte)seq;
        for (int i = 4; i < bytes.Length; i++) bytes[i] = (byte)((seq * 31 + i * 17) & 0xFF);
        Emit(new JsonObject
        {
            ["t"] = "frame", ["dir"] = dir, ["ms"] = _now, ["mac"] = mac, ["kind"] = kind, ["len"] = len,
            ["rssi"] = rssi, ["seq"] = seq & 0xFFFF, ["hex"] = string.Join(' ', bytes.Select(b => b.ToString("X2"))),
        });
    }
}
