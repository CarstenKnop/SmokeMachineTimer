using System.Text.Json.Nodes;
using FogConsole.Protocol;

namespace FogConsole.Simulator;

public sealed partial class SimDevice
{
    private static readonly HashSet<string> KnownOps = new()
    {
        "ping", "setTimes", "mode", "toggle", "reset", "setPhase", "name", "channel", "startPhase", "identify", "reboot", "factoryReset",
    };

    private void HandleCmd(int id, JsonObject req)
    {
        string? op = TryString(req["op"]);
        if (op is null) { Fail(id, "badRequest", "'op' required"); return; }
        if (!KnownOps.Contains(op)) { Fail(id, "badArg", $"unknown op '{op}'"); return; }
        if (!ValidateOp(op, req, out var args, out var err)) { Fail(id, "badArg", err!); return; }

        var targets = ResolveTargets(req, allowAll: true, out var terr);
        if (targets is null) { Fail(id, "noDevice", terr ?? "no such device"); return; }

        bool confirm = !req.ContainsKey("confirm") || !TryBool(req["confirm"], out var c) || c;
        Reply(id, r => r["queued"] = targets.Count);
        foreach (var t in targets)
            EnqueueJob(t, new SimJob { RequestId = id, Op = op, Confirm = confirm, Args = args.DeepClone().AsObject() });
    }

    private static bool ValidateOp(string op, JsonObject req, out JsonObject args, out string? error)
    {
        args = new JsonObject();
        error = null;
        switch (op)
        {
            case "setTimes":
                if (!TryDouble(req["on"], out var on) || on is < 0.1 or > 9999.9) { error = "'on' must be 0.1 ... 9999.9"; return false; }
                if (!TryDouble(req["off"], out var off) || off is < 0.1 or > 9999.9) { error = "'off' must be 0.1 ... 9999.9"; return false; }
                args["on"] = Math.Round(on, 1);
                args["off"] = Math.Round(off, 1);
                return true;
            case "mode":
                var mode = TryString(req["mode"]);
                if (mode is not ("auto" or "forceOn" or "paused")) { error = "'mode' must be auto|forceOn|paused"; return false; }
                args["mode"] = mode;
                if (mode == "forceOn")
                {
                    int lease = 1000;
                    if (req.ContainsKey("leaseMs") && (!TryInt(req["leaseMs"], out lease) || lease is < 200 or > 5000)) { error = "'leaseMs' must be 200 ... 5000"; return false; }
                    args["leaseMs"] = lease;
                }
                return true;
            case "setPhase":
                var ph = TryString(req["phase"]);
                if (ph is not ("on" or "off")) { error = "'phase' must be on|off"; return false; }
                double el = 0;
                if (req.ContainsKey("elapsed") && (!TryDouble(req["elapsed"], out el) || el < 0 || el > 9999.9)) { error = "'elapsed' must be >= 0"; return false; }
                args["phase"] = ph;
                args["elapsed"] = el;
                return true;
            case "name":
                var name = TryString(req["name"]);
                if (!ValidName(name)) { error = "name must be 1-9 printable ASCII characters"; return false; }
                args["name"] = name;
                return true;
            case "channel":
                if (!TryInt(req["ch"], out int ch) || ch is < 1 or > 13) { error = "'ch' must be 1 ... 13"; return false; }
                args["ch"] = ch;
                return true;
            case "startPhase":
                var sp = TryString(req["phase"]);
                if (sp is not ("on" or "off")) { error = "'phase' must be on|off"; return false; }
                args["phase"] = sp;
                return true;
            case "identify":
                int ms = 5000;
                if (req.ContainsKey("ms") && (!TryInt(req["ms"], out ms) || ms is < 100 or > 60_000)) { error = "'ms' must be 100 ... 60000"; return false; }
                args["ms"] = ms;
                return true;
            default:
                return true;
        }
    }

    private void EnqueueJob(SimTimer t, SimJob job)
    {
        if (job.Confirm) _cmds++;
        t.Jobs.Enqueue(job);
        _fleetDirty = true;
        if (t.Jobs.Count == 1) StartJob(t, job);
    }

    private void StartJob(SimTimer t, SimJob job)
    {
        int attempts = 0;
        double ms = 0;
        bool delivered = false;

        if (!IsRemote)
        {
            attempts = 1;
            ms = 4 + _rng.NextDouble() * 6;
            delivered = true;
        }
        else if (!job.Confirm)
        {
            // fire-and-forget: a single short attempt
            attempts = 1;
            delivered = Unicast(t, "CMD");
            ms = delivered ? Rtt() : 20;
        }
        else
        {
            bool reachable = t.Powered && t.Channel == _homeCh;
            bool ackLost = false;
            while (attempts < 8 && ms < 2500)
            {
                attempts++;
                if (Unicast(t, "CMD"))
                {
                    delivered = true;
                    ms += Rtt();
                    break;
                }
                if (reachable && Chance(0.3)) ackLost = true;
                ms += reachable ? 45 + _rng.Next(0, 25) : 280 + _rng.Next(0, 60);
            }
            if (delivered && ackLost) { t.C.CmdsDup++; t.L.Dups++; }
            if (!delivered) ms = 2500;
            if (attempts > 1)
            {
                _retries += attempts - 1;
                t.L.Retries += attempts - 1;
            }
        }

        int rounded = (int)Math.Round(ms);
        Schedule(rounded, () => FinishJob(t, job, delivered, attempts, rounded));
    }

    private void FinishJob(SimTimer t, SimJob job, bool delivered, int attempts, int ms)
    {
        if (job.Cancelled) return;
        if (t.Jobs.Count == 0 || !ReferenceEquals(t.Jobs.Peek(), job)) return;
        t.Jobs.Dequeue();

        if (delivered && IsRemote && !(t.Powered && t.Channel == _homeCh)) delivered = false; // power was cut meanwhile

        if (!job.Confirm)
        {
            if (delivered && ApplyOp(t, job))
            {
                t.C.CmdsRun++;
                if (IsRemote) RefreshKnown(t);
            }
            _fleetDirty = true;
            if (t.Jobs.Count > 0) StartJob(t, t.Jobs.Peek());
            return;
        }

        string result;
        if (!delivered)
        {
            result = "timeout";
            _cmdsTimeout++;
            t.L.CmdsTimeout++;
        }
        else if (ApplyOp(t, job))
        {
            result = "ok";
            _cmdsOk++;
            t.L.CmdsOk++;
            _rttSum += ms;
            _rttN++;
            _rttMax = Math.Max(_rttMax, ms);
            t.L.AddRtt(ms);
            t.C.CmdsRun++;
            if (IsRemote)
            {
                t.LastHeardMs = _now;
                t.EverHeard = true;
                RefreshKnown(t);
            }
        }
        else
        {
            result = "rejected";
            _cmdsRejected++;
            t.C.CmdsRejected++;
        }
        t.L.Cmds++;
        _fleetDirty = true;

        if (job.RequestId > 0)
        {
            Emit(new JsonObject
            {
                ["t"] = "cmdResult", ["id"] = job.RequestId, ["dev"] = IsRemote ? IndexOf(t) : 0, ["mac"] = t.Mac,
                ["op"] = job.Op, ["result"] = result, ["attempts"] = attempts, ["ms"] = ms,
            });
        }
        Log(result == "ok" ? 3 : 1, "cmd", $"{job.Op} {t.Name} -> {result} ({attempts} att, {ms} ms)");

        if (t.Jobs.Count > 0) StartJob(t, t.Jobs.Peek());
    }

    /// <summary>The timer executes a command. Returns false when it rejects it.</summary>
    private bool ApplyOp(SimTimer t, SimJob job)
    {
        var a = job.Args;
        switch (job.Op)
        {
            case "ping":
                return true;
            case "setTimes":
                t.Ton = a["on"]!.GetValue<double>();
                t.Toff = a["off"]!.GetValue<double>();
                return true;
            case "mode":
                var mode = ProtocolParser.ParseMode(a["mode"]!.GetValue<string>());
                t.Mode = mode;
                t.LeaseMs = mode == TimerMode.ForceOn ? a["leaseMs"]?.GetValue<int>() ?? 1000 : 0;
                return true;
            case "toggle":
                t.Phase = t.Phase == CyclePhase.On ? CyclePhase.Off : CyclePhase.On;
                t.ElapsedMs = 0;
                return true;
            case "reset":
                t.Phase = CyclePhase.Off;
                t.ElapsedMs = 0;
                if (t.Mode == TimerMode.ForceOn) t.Mode = TimerMode.Auto;
                return true;
            case "setPhase":
                t.Phase = a["phase"]!.GetValue<string>() == "on" ? CyclePhase.On : CyclePhase.Off;
                t.ElapsedMs = Math.Min(a["elapsed"]!.GetValue<double>() * 1000.0, t.PhaseDurationMs - 1);
                return true;
            case "name":
                t.Name = a["name"]!.GetValue<string>();
                return true;
            case "channel":
                t.Channel = a["ch"]!.GetValue<int>();
                return true;
            case "startPhase":
                t.StartPhase = a["phase"]!.GetValue<string>() == "on" ? CyclePhase.On : CyclePhase.Off;
                return true;
            case "identify":
                t.IdentifyMs = a["ms"]?.GetValue<int>() ?? 5000;
                return true;
            case "reboot":
                t.PowerCut(3000, brownout: false);
                t.NextBootReason = "sw";
                return true;
            case "factoryReset":
                t.FactoryReset();
                return true;
            default:
                return false;
        }
    }

    // ---- timer connected directly ---------------------------------------------------------

    private void StepDirectTimer()
    {
        var t = _timers[0];
        if (!t.Powered)
        {
            t.PowerOffMs -= StepMs;
            if (t.PowerOffMs <= 0) t.Boot_(_now, t.NextBootReason);
        }
        else
        {
            if (t.StepCycle(StepMs)) _fleetDirty = true;
            if (_now >= t.NextBeatMs)
            {
                t.NextBeatMs += 500;
                t.C.Beats++;
                t.C.Tx++;
            }
        }
        if (_now % 5000 == 0) t.C.LoopMaxUs = Math.Max(t.C.LoopMaxUs, 1800 + _rng.Next(0, 1500));
        StepEmit();
    }
}
