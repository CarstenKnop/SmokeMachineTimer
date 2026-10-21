using System.Text.Json.Nodes;
using FogConsole.Ota;

namespace FogConsole.Simulator;

// Over-the-air firmware update in the simulator (docs/OTA_UPDATE.md, sections 5 and 6): the remote tunnels frames between the PC and the
// paired timers (otaTx / otaRx / otaHold); every timer has a SimOtaReceiver. A timer on USB receives the frames directly.
public sealed partial class SimDevice
{
    private sealed record OtaQueued(int RequestId, SimTimer Timer, byte[] Payload);

    private const int OtaQueueLimit = 8;
    private const int OtaHoldMs = 15_000;

    /// <summary>The remote forwards at most this many frames per 50 ms step (5 ms per frame: a tunnel of about 45 KB/s).</summary>
    private const int OtaFramesPerStep = 10;
    private int _otaBudget = OtaFramesPerStep;

    private readonly Queue<OtaQueued> _otaTx = new();
    private long _otaHoldUntil = -1;

    /// <summary>True while a PC keeps the remote's radio reserved for a firmware transfer.</summary>
    public bool OtaHoldActive { get { lock (_lock) return _otaHoldUntil >= 0 && _now <= _otaHoldUntil; } }

    private bool OtaHeld => _otaHoldUntil >= 0 && _now <= _otaHoldUntil;

    /// <summary>The over-the-air receiver of a paired timer (tests and demo scripts).</summary>
    internal SimOtaReceiver? OtaOf(int pairedIndex)
    {
        lock (_lock)
        {
            IReadOnlyList<SimTimer> list = IsRemote ? _paired : _timers;           // a timer on USB is a fleet of one
            return pairedIndex >= 0 && pairedIndex < list.Count ? list[pairedIndex].Ota : null;
        }
    }

    private bool RefuseWhileOtaHeld(int id)
    {
        if (!OtaHeld) return false;
        Fail(id, "busy", "firmware update running");
        return true;
    }

    // ---- requests ---------------------------------------------------------------------------------------------------

    private void HandleOtaTx(int id, JsonObject req)
    {
        SimTimer t;
        if (IsRemote)
        {
            var list = ResolveTargets(req, allowAll: false, out var err);
            if (list is null) { Fail(id, "noDevice", err ?? "no such device"); return; }
            if (list.Count != 1) { Fail(id, "badArg", "exactly one device expected"); return; }
            t = list[0];
        }
        else
        {
            t = _timers[0];
        }

        byte[] payload;
        try { payload = Convert.FromBase64String(TryString(req["d"]) ?? ""); }
        catch (FormatException) { Fail(id, "badArg", "'d' must be base64"); return; }
        if (payload.Length is 0 or > FogConsole.Protocol.Requests.OtaMaxPayload) { Fail(id, "badArg", "'d' must be 1 ... 242 bytes"); return; }

        if (!IsRemote)
        {
            // a timer on USB: no radio in between
            Reply(id, r => r["acked"] = true);
            foreach (var reply in t.Ota.OnFrame(payload, _now)) EmitOtaRx(t, reply);
            return;
        }

        _otaHoldUntil = _now + OtaHoldMs;
        if (_otaTx.Count >= OtaQueueLimit) { Fail(id, "busy", "transmit queue full"); return; }
        var q = new OtaQueued(id, t, payload);
        if (_otaTx.Count == 0 && _otaBudget > 0) { _otaBudget--; DeliverOta(q); }       // the radio is free: sent at once
        else _otaTx.Enqueue(q);                                                           // otherwise it waits for the next step
    }

    private void DeliverOta(OtaQueued q)
    {
        bool acked = Unicast(q.Timer, "OTA");
        Reply(q.RequestId, r => r["acked"] = acked);
        if (!acked) return;
        foreach (var reply in q.Timer.Ota.OnFrame(q.Payload, _now)) SendOtaBack(q.Timer, reply);
    }

    private void HandleOtaHold(int id, JsonObject req)
    {
        if (!TryBool(req["on"], out bool on)) { Fail(id, "badArg", "'on' (bool) required"); return; }
        if (IsRemote) _otaHoldUntil = on ? _now + OtaHoldMs : -1;
        Reply(id);
    }

    // ---- stepping -----------------------------------------------------------------------------------------------------

    private void StepOta()
    {
        if (_otaHoldUntil >= 0 && _now > _otaHoldUntil) _otaHoldUntil = -1;

        _otaBudget = OtaFramesPerStep;
        while (_otaBudget > 0 && _otaTx.TryDequeue(out var q))
        {
            _otaBudget--;
            DeliverOta(q);
        }

        foreach (var t in _timers)
        {
            if (!t.Powered) continue;
            foreach (var reply in t.Ota.Tick(_now))
            {
                if (IsRemote) SendOtaBack(t, reply); else EmitOtaRx(t, reply);
            }
            ApplyActivation(t);
        }
    }

    private void ApplyActivation(SimTimer t)
    {
        if (t.Ota.TakePendingActivation() is not { } fw) return;
        t.PendingFw = $"{fw.Major}.{fw.Minor}.{fw.Patch}";
        Log(2, "ota", $"{t.Name}: new firmware {t.PendingFw} activated, rebooting");
        t.PowerCut(2500, brownout: false);                 // a software reset: gone for a moment, then boot + 1 with the new version
        t.NextBootReason = "sw";
    }

    /// <summary>A timer's OTA frame travels back over the radio (it can be lost like any other frame).</summary>
    private void SendOtaBack(SimTimer t, byte[] payload)
    {
        if (!t.Powered || t.Channel != _homeCh) return;
        t.C.Tx++;
        if (_rng.NextDouble() < LossProbability(t) * 0.6) { t.C.TxFail++; return; }
        _rx++;
        Chan(_homeCh).Rx++;
        EmitOtaRx(t, payload, SampleRssi(t, t.BaseRssi));
    }

    private void EmitOtaRx(SimTimer t, byte[] payload, int rssi = 0)
    {
        Emit(new JsonObject
        {
            ["t"] = "otaRx", ["dev"] = IsRemote ? IndexOf(t) : 0, ["mac"] = t.Mac, ["rssi"] = rssi, ["d"] = Convert.ToBase64String(payload),
        });
    }
}
