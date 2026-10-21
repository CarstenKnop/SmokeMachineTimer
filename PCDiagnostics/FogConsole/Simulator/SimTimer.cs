using System.Text.Json.Nodes;
using FogConsole.Protocol;

namespace FogConsole.Simulator;

/// <summary>Counters a timer keeps itself (reset at every boot, like the real firmware).</summary>
internal sealed class SimTimerCounters
{
    public long Rx, RxBad, RxForeign, RxDrop, RxProbes, RxCmds;
    public long Tx, TxFail, TxDrop, Beats, Replies;
    public long CmdsRun, CmdsDup, CmdsRejected;
    public long LoopMaxUs;
    public int RssiMin = 0, RssiMax = -200, RssiLast;
    public double RssiSum;
    public long RssiN;

    public void AddRssi(int rssi)
    {
        RssiLast = rssi;
        RssiMin = RssiN == 0 ? rssi : Math.Min(RssiMin, rssi);
        RssiMax = RssiN == 0 ? rssi : Math.Max(RssiMax, rssi);
        RssiSum += rssi;
        RssiN++;
    }

    public int RssiAvg => RssiN == 0 ? 0 : (int)Math.Round(RssiSum / RssiN);
}

/// <summary>What the remote counts about one timer (persists while the remote runs).</summary>
internal sealed class SimLinkCounters
{
    public long Rx, Expected, Dups, MacOk, MacFail;
    public long Cmds, CmdsOk, CmdsTimeout, Retries;
    public double RttSum;
    public long RttN;
    public double RttMax;
    public int RssiMin, RssiMax;
    public double RssiSum;
    public long RssiN;
    public long OfflineEvents, OfflineMs, Reboots;

    public long Lost => Math.Max(0, Expected - Rx);
    public double RttAvg => RttN == 0 ? 0 : RttSum / RttN;
    public int RssiAvg => RssiN == 0 ? 0 : (int)Math.Round(RssiSum / RssiN);

    public void AddRssi(int rssi)
    {
        RssiMin = RssiN == 0 ? rssi : Math.Min(RssiMin, rssi);
        RssiMax = RssiN == 0 ? rssi : Math.Max(RssiMax, rssi);
        RssiSum += rssi;
        RssiN++;
    }

    public void AddRtt(double ms)
    {
        RttSum += ms;
        RttN++;
        RttMax = Math.Max(RttMax, ms);
    }
}

/// <summary>A command waiting in a timer's queue on the remote.</summary>
internal sealed class SimJob
{
    public int RequestId;
    public bool Cancelled;

    /// <summary>False = fire-and-forget (<c>confirm:false</c>): one short attempt, no cmdResult.</summary>
    public bool Confirm = true;
    public string Op = "";
    public JsonObject Args = new();
}

/// <summary>A simulated smoke machine timer.</summary>
internal sealed class SimTimer
{
    public SimTimer(string mac, string name)
    {
        Mac = mac;
        Name = name;
    }

    // identity & persisted settings
    public string Mac { get; }
    public string Name { get; set; }
    public double Ton { get; set; } = 1.5;
    public double Toff { get; set; } = 10.0;
    public CyclePhase StartPhase { get; set; } = CyclePhase.Off;
    public int Channel { get; set; } = 6;
    public string Fw { get; set; } = "2.0.0";
    public bool Led { get; set; } = true;

    /// <summary>Stable slot id (0…9) while paired.</summary>
    public int Slot { get; set; }

    // power & cycle state
    public bool Powered { get; set; } = true;
    public double PowerOffMs { get; set; }
    public bool NextBootBrownout { get; set; }
    public string NextBootReason { get; set; } = "poweron";
    public int Boot { get; set; } = 1;
    public double UptimeMs { get; set; }
    public CyclePhase Phase { get; set; } = CyclePhase.Off;
    public double ElapsedMs { get; set; }
    public TimerMode Mode { get; set; } = TimerMode.Auto;
    public double LeaseMs { get; set; }
    public double IdentifyMs { get; set; }
    public long BootedAtMs { get; set; }

    // radio quality model
    public double BaseRssi { get; set; } = -60;
    public double BaseRssiT { get; set; } = -60;
    public double RssiPhase { get; set; }
    public double ExtraLoss { get; set; }
    public double NextBeatMs { get; set; }

    // lifetime reset counters (persisted over reboots)
    public long RstPowerOn { get; set; } = 1;
    public long RstBrownout, RstWdt, RstPanic, RstSw, RstDeepSleep, RstOther;
    public string RstReason { get; set; } = "poweron";
    public long Heap { get; set; } = 205000;
    public long HeapMin { get; set; } = 205000;
    public SimTimerCounters C { get; set; } = new();

    // remote's view
    public SimLinkCounters L { get; set; } = new();
    public long LastHeardMs { get; set; } = -1_000_000;
    public bool EverHeard { get; set; }
    public bool WasOnline { get; set; }
    public long OfflineSinceMs { get; set; }
    public readonly bool[] LossRing = new bool[32];
    public int RingPos { get; set; }
    public int RingCount { get; set; }
    public int LastRssi { get; set; }
    public int LastRssiT { get; set; }
    public Queue<SimJob> Jobs { get; } = new();

    /// <summary>Last status frame the remote received.</summary>
    public SimKnown Known { get; set; } = new();

    // cached GetInfo answer
    public JsonObject? InfoJson { get; set; }
    public long InfoAtMs { get; set; }

    public bool OutputOn => Powered && (Mode == TimerMode.ForceOn || (Mode == TimerMode.Auto && Phase == CyclePhase.On));

    public double PhaseDurationMs => Math.Max(100, (Phase == CyclePhase.On ? Ton : Toff) * 1000.0);

    /// <summary>Advances the cycle engine. Returns true when the phase or mode changed.</summary>
    public bool StepCycle(double dtMs)
    {
        if (!Powered) return false;
        UptimeMs += dtMs;
        if (IdentifyMs > 0) IdentifyMs = Math.Max(0, IdentifyMs - dtMs);
        bool changed = false;
        if (Mode == TimerMode.Paused) return false;
        if (Mode == TimerMode.ForceOn)
        {
            LeaseMs -= dtMs;
            if (LeaseMs <= 0)
            {
                // dead-man lease expired: fall back to the automatic cycle
                Mode = TimerMode.Auto;
                LeaseMs = 0;
                changed = true;
            }
        }
        ElapsedMs += dtMs;
        for (int guard = 0; guard < 4 && ElapsedMs >= PhaseDurationMs; guard++)
        {
            ElapsedMs -= PhaseDurationMs;
            Phase = Phase == CyclePhase.On ? CyclePhase.Off : CyclePhase.On;
            changed = true;
        }
        if (ElapsedMs > PhaseDurationMs) ElapsedMs = 0;
        return changed;
    }

    public void PowerCut(double offMs, bool brownout)
    {
        Powered = false;
        PowerOffMs = offMs;
        NextBootBrownout = brownout;
        NextBootReason = brownout ? "brownout" : "poweron";
        Mode = TimerMode.Auto;
        LeaseMs = 0;
    }

    /// <summary>Boot sequence after power returns (or after a software reboot).</summary>
    public void Boot_(long nowMs, string reason)
    {
        Powered = true;
        PowerOffMs = 0;
        Boot++;
        UptimeMs = 0;
        BootedAtMs = nowMs;
        switch (reason)
        {
            case "brownout": RstBrownout++; break;
            case "sw": RstSw++; break;
            case "wdt": RstWdt++; break;
            default: RstPowerOn++; reason = "poweron"; break;
        }
        RstReason = reason;
        Mode = TimerMode.Auto;
        LeaseMs = 0;
        Phase = StartPhase;
        ElapsedMs = 0;
        C = new SimTimerCounters();
        Heap = 205000;
        HeapMin = 205000;
    }

    public void FactoryReset()
    {
        Name = "FogTimer";
        Ton = 1.5;
        Toff = 10.0;
        StartPhase = CyclePhase.Off;
        Led = true;
        Mode = TimerMode.Auto;
        Phase = CyclePhase.Off;
        ElapsedMs = 0;
    }

    public double LossPercentWindow
    {
        get
        {
            if (RingCount < 8) return -1;
            int lost = 0;
            int n = Math.Min(RingCount, LossRing.Length);
            for (int i = 0; i < n; i++)
                if (LossRing[i]) lost++;
            return 100.0 * lost / n;
        }
    }

    public void RecordFrame(bool lost)
    {
        LossRing[RingPos] = lost;
        RingPos = (RingPos + 1) % LossRing.Length;
        RingCount = Math.Min(RingCount + 1, LossRing.Length);
    }

    public void ClearLossWindow()
    {
        Array.Clear(LossRing);
        RingPos = 0;
        RingCount = 0;
    }
}

/// <summary>Snapshot of the status carried by the last frame that was heard from a timer.</summary>
internal sealed class SimKnown
{
    public CyclePhase Phase { get; set; }
    public double ElapsedMs { get; set; }
    public bool On { get; set; }
    public TimerMode Mode { get; set; }
    public double Ton { get; set; }
    public double Toff { get; set; }
    public int Boot { get; set; }
    public long UpS { get; set; }
    public string Fw { get; set; } = "";
    public int Channel { get; set; }
    public long AtMs { get; set; }
}
