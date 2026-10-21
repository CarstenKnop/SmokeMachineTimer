using System.Text.Json;

namespace FogConsole.Protocol;

/// <summary>Base type of everything the device can send us.</summary>
public abstract record Message;

public enum TimerMode { Auto, ForceOn, Paused, Unknown }

public enum CyclePhase { Off, On }

public enum DeviceKind { Unknown, Remote, Timer }

/// <summary>Response to a request (<c>{"t":"res",...}</c>). The raw body is kept for forward compatibility.</summary>
public sealed record ResponseMessage : Message
{
    public int Id { get; init; }
    public bool Ok { get; init; }
    public string? Err { get; init; }
    public string? Msg { get; init; }

    /// <summary>Number of jobs queued (for <c>cmd</c> requests).</summary>
    public int? Queued { get; init; }

    /// <summary>For <c>otaTx</c>: the timer's MAC acknowledged the frame (null when the response carries no such field).</summary>
    public bool? Acked { get; init; }

    /// <summary>Device millis() (for <c>ping</c>).</summary>
    public long? DeviceMs { get; init; }

    public HelloInfo? Hello { get; init; }
    public RemoteSettings? Settings { get; init; }
    public StatsSnapshot? Stats { get; init; }

    /// <summary>For <c>get devStats</c>: the <c>devStat</c> object (its <c>Timer</c> is null when the timer did not answer).</summary>
    public DevStatsEntry? DevStat { get; init; }

    public JsonElement Body { get; init; }

    public string Describe() => Ok ? "ok" : $"{Err ?? "error"}{(string.IsNullOrEmpty(Msg) ? "" : ": " + Msg)}";
}

public sealed record HelloInfo(
    DeviceKind Kind,
    string Fw,
    int Proto,
    string Mac,
    string Name,
    int Channel,
    int Net,
    long UpMs,
    string ResetReason,
    int Boots,
    long Heap,
    string Build,
    int MaxDevs,
    IReadOnlyList<string>? Caps = null)
{
    /// <summary>The capabilities the device lists in <c>hello</c> (e.g. <c>ota</c>: firmware update frames can be tunnelled / received).</summary>
    public bool Has(string capability) => Caps is not null && Caps.Contains(capability, StringComparer.OrdinalIgnoreCase);

    public bool SupportsOta => Has("ota");
}

/// <summary>Settings of the remote (or of a directly connected timer). Missing values are null.</summary>
public sealed record RemoteSettings
{
    public int? TxPower { get; init; }
    public int? Brightness { get; init; }
    public int? AutoOff { get; init; }
    public int? RssiLow { get; init; }
    public int? RssiHigh { get; init; }
    /// <summary>Battery millivolts that count as 0 %, 50 % and 100 %.</summary>
    public IReadOnlyList<int>? BatCal { get; init; }
    public int? Net { get; init; }

    // timer settings (direct USB)
    public string? StartPhase { get; init; }
    public string? Name { get; init; }
    public double? Ton { get; init; }
    public double? Toff { get; init; }
    public bool? Led { get; init; }

    public JsonElement Raw { get; init; }

    public double? TxPowerDbm => TxPower is { } p ? p * 0.25 : null;
}

public sealed record FleetDevice
{
    public int Index { get; init; }
    public string Mac { get; init; } = "";
    public string Name { get; init; } = "";
    public bool Online { get; init; }
    public bool On { get; init; }
    public CyclePhase Phase { get; init; }
    public TimerMode Mode { get; init; }
    public double Ton { get; init; }
    public double Toff { get; init; }
    public double Elapsed { get; init; }
    public int Rssi { get; init; }
    public int RssiT { get; init; }
    public int AgeMs { get; init; }
    public int Loss { get; init; } = -1;
    public int Channel { get; init; }
    public int Boot { get; init; }
    public long UpSeconds { get; init; }
    public string Fw { get; init; } = "";
    public int Pending { get; init; }
    public int SlotId { get; init; }
}

public sealed record FleetMessage(long Seq, int Active, int Channel, IReadOnlyList<FleetDevice> Devs) : Message;

public sealed record RemoteMessage : Message
{
    public int Bat { get; init; }
    public int Mv { get; init; }
    public bool Charging { get; init; }
    public bool Usb { get; init; }
    public int Channel { get; init; }
    public int Net { get; init; }
    public long UpSeconds { get; init; }
    public long Heap { get; init; }
    public string Menu { get; init; } = "";
    public bool Blank { get; init; }
    public bool Scan { get; init; }
    public int Txq { get; init; }
}

public enum CmdOutcome { Ok, Rejected, Timeout, Cancelled, Unknown }

public sealed record CmdResultMessage(int Id, int Dev, string Mac, string Op, CmdOutcome Result, string ResultText, int Attempts, int Ms) : Message;

public sealed record FoundMessage(string Mac, string Name, int Rssi, int Channel, int Paired, double Ton, double Toff, bool On, int Boot) : Message;

public sealed record SurveyChMessage(int Channel, int Aps, int ApRssi, int Frames, int AvgRssi, int Score) : Message;

public sealed record SurveyDoneMessage(IReadOnlyList<int> Best, int Current) : Message;

public sealed record MigrateMessage : Message
{
    public string Stage { get; init; } = "";
    public int Channel { get; init; }
    public int Targets { get; init; }
    public int Dev { get; init; } = -1;
    /// <summary>Per-device stage: whether this timer confirmed/hopped.</summary>
    public bool? DevOk { get; init; }
    /// <summary>Done stage: number of timers that moved.</summary>
    public int OkCount { get; init; }
    public int Failed { get; init; }
    public bool Committed { get; init; }
}

public sealed record LinkTestMessage(int Id, int Dev, int Sent, int MacAck, int Echo, int Rssi, double RttMs, double RttMax, bool Done) : Message;

public enum LogLevel { Error, Warn, Info, Debug, Off }

public sealed record LogMessage(LogLevel Level, string LevelText, long Ms, string Src, string Text) : Message;

public sealed record FrameMessage(string Dir, long Ms, string Mac, string Kind, int Len, int Rssi, long Seq, string Hex) : Message;

/// <summary>The <c>stats</c> event: the counters of the device itself (<c>self</c>).</summary>
public sealed record StatsMessage(StatsSnapshot Snapshot) : Message;

/// <summary>The <c>devStat</c> event: link and timer counters of one paired timer (sent right after every <c>stats</c> event).</summary>
public sealed record DevStatMessage(DevStatsEntry Entry) : Message;

/// <summary>
/// The <c>otaRx</c> event: one over-the-air update frame from a paired timer (or from a timer on USB, <c>dev</c> 0). <see cref="Payload"/> is the
/// OTA frame payload <c>[op][session][body]</c> (see docs/OTA_UPDATE.md).
/// </summary>
public sealed record OtaRxMessage(int Dev, string Mac, int Rssi, byte[] Payload) : Message;

/// <summary>Anything with a type we do not know (kept so the Raw console can still show it).</summary>
public sealed record UnknownMessage(string Type, JsonElement Body) : Message;

/// <summary>A line that is not a JSON object (ROM boot log, ESP-IDF log, garbage) or is invalid JSON.</summary>
public sealed record PlainTextMessage(string Text, bool MalformedJson) : Message;

// ---------------------------------------------------------------------------------------------
// Performance counters (stats event / get stats / get devStats)
// ---------------------------------------------------------------------------------------------

public sealed record ResetCounts(long PowerOn, long Brownout, long Wdt, long Panic, long Sw, long DeepSleep, long Other)
{
    public static readonly ResetCounts Empty = new(0, 0, 0, 0, 0, 0, 0);
    public long Total => PowerOn + Brownout + Wdt + Panic + Sw + DeepSleep + Other;
}

public sealed record RadioCounters(int Channel, long Tx, long TxOk, long TxFail, long TxWd, long TxDrop, long Rx, long RxBad, long RxForeign, long RxDrop)
{
    public static readonly RadioCounters Empty = new(0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}

public sealed record ChannelCounters(int Channel, long Tx, long Fail, long Rx);

public sealed record LinkSummary(long Cmds, long Ok, long Timeout, long Rejected, long Retries, double RttAvg, double RttMax, long Sweeps, long Recovers, long Found, long Migrations)
{
    public static readonly LinkSummary Empty = new(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}

public sealed record PcCounters(long LinesRx, long LinesTx, long BadJson, long TxDrop)
{
    public static readonly PcCounters Empty = new(0, 0, 0, 0);
}

public sealed record SelfStats
{
    public DeviceKind Kind { get; init; }
    public long UpSeconds { get; init; }
    public int Boots { get; init; }
    public long Heap { get; init; }
    public long HeapMin { get; init; }
    public long LoopMaxUs { get; init; }
    public long LoopsPerS { get; init; }
    public string RstReason { get; init; } = "";
    public ResetCounts Rst { get; init; } = ResetCounts.Empty;
    public RadioCounters Radio { get; init; } = RadioCounters.Empty;
    public IReadOnlyList<ChannelCounters> Chan { get; init; } = Array.Empty<ChannelCounters>();
    public LinkSummary Link { get; init; } = LinkSummary.Empty;
    public PcCounters Pc { get; init; } = PcCounters.Empty;

    /// <summary>For a timer connected directly by USB: the timer's own counters (<c>self.timer</c>, without <c>age</c>).</summary>
    public TimerStats? Timer { get; init; }
}

public sealed record DevLinkStats
{
    public long Rx { get; init; }
    public long Expected { get; init; }
    public long Lost { get; init; }
    public int Loss { get; init; } = -1;
    public long Dups { get; init; }
    public long MacOk { get; init; }
    public long MacFail { get; init; }
    public long Cmds { get; init; }
    public long CmdsOk { get; init; }
    public long CmdsTimeout { get; init; }
    public long Retries { get; init; }
    public double RttAvg { get; init; }
    public double RttMax { get; init; }
    public int RssiMin { get; init; }
    public int RssiAvg { get; init; }
    public int RssiMax { get; init; }
    public long OfflineEvents { get; init; }
    public long OfflineMs { get; init; }
    public long Reboots { get; init; }

    /// <summary>macFail / (macOk + macFail) in percent, or null when no unicast frames were sent.</summary>
    public double? MacFailPercent => MacOk + MacFail > 0 ? 100.0 * MacFail / (MacOk + MacFail) : null;
}

public sealed record TimerStats
{
    public long AgeMs { get; init; }
    public string Fw { get; init; } = "";
    public long UpSeconds { get; init; }
    public int Boots { get; init; }
    public string RstReason { get; init; } = "";
    public long Heap { get; init; }
    public long HeapMin { get; init; }
    public long LoopMaxUs { get; init; }
    public int Channel { get; init; }
    public int Net { get; init; }
    public ResetCounts Rst { get; init; } = ResetCounts.Empty;
    public long Rx { get; init; }
    public long RxBad { get; init; }
    public long RxForeign { get; init; }
    public long RxDrop { get; init; }
    public long RxProbes { get; init; }
    public long RxCmds { get; init; }
    public long Tx { get; init; }
    public long TxFail { get; init; }
    public long TxDrop { get; init; }
    public long Beats { get; init; }
    public long Replies { get; init; }
    public long CmdsRun { get; init; }
    public long CmdsDup { get; init; }
    public long CmdsRejected { get; init; }
    public int RssiMin { get; init; }
    public int RssiAvg { get; init; }
    public int RssiMax { get; init; }
    public int RssiLast { get; init; }
}

public sealed record DevStatsEntry(int Index, string Mac, DevLinkStats? Link, TimerStats? Timer, long Ms = 0);

public sealed record StatsSnapshot
{
    public long Ms { get; init; }
    public SelfStats Self { get; init; } = new();
    /// <summary>
    /// Per-timer counters. Not part of the <c>stats</c> event itself: the application assembles them from the
    /// <c>devStat</c> events that follow it (see <see cref="WithDevs"/>).
    /// </summary>
    public IReadOnlyList<DevStatsEntry> Devs { get; init; } = Array.Empty<DevStatsEntry>();

    /// <summary>The object exactly as received (for "Copy as JSON").</summary>
    public JsonElement Raw { get; init; }

    public StatsSnapshot WithDevs(IReadOnlyList<DevStatsEntry> devs) => this with { Devs = devs };
}
