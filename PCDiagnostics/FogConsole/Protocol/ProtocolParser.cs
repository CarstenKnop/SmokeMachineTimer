using System.Globalization;
using System.Text.Json;

namespace FogConsole.Protocol;

/// <summary>
/// Tolerant JSON helpers: unknown fields are ignored, missing fields yield defaults, numbers may be
/// delivered as strings or booleans (0/1) and still be understood.
/// </summary>
internal static class Jx
{
    public static bool IsObject(this JsonElement e) => e.ValueKind == JsonValueKind.Object;

    public static bool TryGet(this JsonElement e, string name, out JsonElement value)
    {
        if (e.ValueKind == JsonValueKind.Object && e.TryGetProperty(name, out value))
            return true;
        value = default;
        return false;
    }

    public static JsonElement? Obj(this JsonElement e, string name) =>
        e.TryGet(name, out var v) && v.ValueKind == JsonValueKind.Object ? v : null;

    public static JsonElement? Arr(this JsonElement e, string name) =>
        e.TryGet(name, out var v) && v.ValueKind == JsonValueKind.Array ? v : null;

    public static string Str(this JsonElement e, string name, string def = "")
    {
        if (!e.TryGet(name, out var v)) return def;
        return v.ValueKind switch
        {
            JsonValueKind.String => v.GetString() ?? def,
            JsonValueKind.Number => v.GetRawText(),
            JsonValueKind.True => "true",
            JsonValueKind.False => "false",
            _ => def,
        };
    }

    public static double Dbl(this JsonElement e, string name, double def = 0) =>
        e.TryGet(name, out var v) ? AsDouble(v, def) : def;

    public static double AsDouble(JsonElement v, double def = 0)
    {
        switch (v.ValueKind)
        {
            case JsonValueKind.Number:
                return v.TryGetDouble(out var d) && double.IsFinite(d) ? d : def;
            case JsonValueKind.String:
                return double.TryParse(v.GetString(), NumberStyles.Float, CultureInfo.InvariantCulture, out var s) ? s : def;
            case JsonValueKind.True: return 1;
            case JsonValueKind.False: return 0;
            default: return def;
        }
    }

    public static long Long(this JsonElement e, string name, long def = 0)
    {
        if (!e.TryGet(name, out var v)) return def;
        if (v.ValueKind == JsonValueKind.Number && v.TryGetInt64(out var l)) return l;
        var d = AsDouble(v, double.NaN);
        return double.IsNaN(d) || Math.Abs(d) > 9e18 ? def : (long)Math.Round(d);
    }

    public static int Int(this JsonElement e, string name, int def = 0)
    {
        var l = e.Long(name, def);
        return l > int.MaxValue ? int.MaxValue : l < int.MinValue ? int.MinValue : (int)l;
    }

    public static bool Bool(this JsonElement e, string name, bool def = false)
    {
        if (!e.TryGet(name, out var v)) return def;
        return v.ValueKind switch
        {
            JsonValueKind.True => true,
            JsonValueKind.False => false,
            JsonValueKind.Number => AsDouble(v) != 0,
            JsonValueKind.String => v.GetString() is { } s && (s == "1" || s.Equals("true", StringComparison.OrdinalIgnoreCase) || s.Equals("on", StringComparison.OrdinalIgnoreCase)),
            _ => def,
        };
    }

    public static bool? NullableBool(this JsonElement e, string name) => e.TryGet(name, out _) ? e.Bool(name) : null;
    public static int? NullableInt(this JsonElement e, string name) => e.TryGet(name, out var v) && v.ValueKind is JsonValueKind.Number or JsonValueKind.String ? e.Int(name) : null;
    public static double? NullableDouble(this JsonElement e, string name) => e.TryGet(name, out var v) && v.ValueKind is JsonValueKind.Number or JsonValueKind.String ? e.Dbl(name) : null;
}

/// <summary>Parses one received line into a <see cref="Message"/>. Never throws.</summary>
public static class ProtocolParser
{
    public static Message Parse(string line)
    {
        if (string.IsNullOrWhiteSpace(line)) return new PlainTextMessage(line ?? "", false);
        var trimmed = line.TrimStart();
        if (trimmed[0] != '{') return new PlainTextMessage(line, false);

        try
        {
            using var doc = JsonDocument.Parse(trimmed, new JsonDocumentOptions { AllowTrailingCommas = true, CommentHandling = JsonCommentHandling.Skip });
            var root = doc.RootElement;
            if (root.ValueKind != JsonValueKind.Object) return new PlainTextMessage(line, true);
            return ParseObject(root);
        }
        catch (JsonException)
        {
            return new PlainTextMessage(line, true);
        }
        catch (Exception ex) when (ex is InvalidOperationException or FormatException or OverflowException or ArgumentException)
        {
            return new PlainTextMessage(line, true);
        }
    }

    public static Message ParseObject(JsonElement root)
    {
        var t = root.Str("t");
        return t switch
        {
            "res" => ParseResponse(root),
            "fleet" => ParseFleet(root),
            "remote" => ParseRemote(root),
            "cmdResult" => ParseCmdResult(root),
            "found" => ParseFound(root),
            "surveyCh" => new SurveyChMessage(root.Int("ch"), root.Int("aps"), root.Int("apRssi"), root.Int("frames"), root.Int("avgRssi"), root.Int("score")),
            "surveyDone" => new SurveyDoneMessage(ParseIntArray(root, "best") ?? Array.Empty<int>(), root.Int("cur")),
            "migrate" => ParseMigrate(root),
            "linkTest" => new LinkTestMessage(root.Int("id"), root.Int("dev"), root.Int("sent"), root.Int("macAck"), root.Int("echo"),
                root.Int("rssi"), root.Dbl("rttMs"), root.Dbl("rttMax"), root.Bool("done")),
            "log" => ParseLog(root),
            "frame" => new FrameMessage(root.Str("dir"), root.Long("ms"), root.Str("mac"), root.Str("kind"), root.Int("len"), root.Int("rssi"),
                root.Long("seq"), root.Str("hex")),
            "stats" => new StatsMessage(ParseStats(root)),
            "devStat" => new DevStatMessage(ParseDevStat(root)),
            "otaRx" => ParseOtaRx(root),
            _ => new UnknownMessage(t, root.Clone()),
        };
    }

    // ---- response -------------------------------------------------------------------------

    private static ResponseMessage ParseResponse(JsonElement r)
    {
        HelloInfo? hello = r.TryGet("kind", out _) ? ParseHello(r) : null;
        RemoteSettings? settings = r.Obj("settings") is { } s ? ParseSettings(s) : null;

        // get stats -> res.stats. The protocol text says it is "the self counters, same object as in a stats event":
        // accept the self part itself as well as a whole stats event object nested under "stats".
        StatsSnapshot? stats = null;
        if (r.Obj("stats") is { } so)
            stats = so.Obj("self") is not null ? ParseStats(so) : new StatsSnapshot { Ms = so.Long("ms", r.Long("ms")), Self = ParseSelfStats(so), Raw = so.Clone() };
        else if (r.Obj("self") is not null)
            stats = ParseStats(r);

        // get devStats -> res.devStat
        DevStatsEntry? devStat = r.Obj("devStat") is { } ds ? ParseDevStat(ds) : null;

        return new ResponseMessage
        {
            Id = r.Int("id"),
            Ok = r.Bool("ok"),
            Err = r.TryGet("err", out _) ? r.Str("err") : null,
            Msg = r.TryGet("msg", out _) ? r.Str("msg") : null,
            Queued = r.NullableInt("queued"),
            Acked = r.NullableBool("acked"),
            DeviceMs = r.TryGet("ms", out _) ? r.Long("ms") : null,
            Hello = hello,
            Settings = settings,
            Stats = stats,
            DevStat = devStat,
            Body = r.Clone(),
        };
    }

    public static HelloInfo ParseHello(JsonElement r) => new(
        r.Str("kind") switch { "remote" => DeviceKind.Remote, "timer" => DeviceKind.Timer, _ => DeviceKind.Unknown },
        r.Str("fw"), r.Int("proto"), r.Str("mac").ToUpperInvariant(), r.Str("name"), r.Int("ch"), r.Int("net"),
        r.Long("upMs"), r.Str("rst"), r.Int("boots"), r.Long("heap"), r.Str("build"), r.Int("maxDevs", 10), ParseStringArray(r, "caps"));

    private static IReadOnlyList<string>? ParseStringArray(JsonElement r, string name)
    {
        if (r.Arr(name) is not { } a) return null;
        var list = new List<string>();
        foreach (var e in a.EnumerateArray())
            if (e.ValueKind == JsonValueKind.String && e.GetString() is { Length: > 0 } s) list.Add(s);
        return list;
    }

    private static Message ParseOtaRx(JsonElement r)
    {
        try
        {
            var bytes = Convert.FromBase64String(r.Str("d"));
            return new OtaRxMessage(r.Int("dev"), r.Str("mac").ToUpperInvariant(), r.Int("rssi"), bytes);
        }
        catch (FormatException)
        {
            return new UnknownMessage("otaRx", r.Clone());      // damaged base64: keep it visible in the Raw console, never throw
        }
    }

    public static RemoteSettings ParseSettings(JsonElement s) => new()
    {
        TxPower = s.NullableInt("txPower"),
        Brightness = s.NullableInt("brightness"),
        AutoOff = s.NullableInt("autoOff"),
        RssiLow = s.NullableInt("rssiLow"),
        RssiHigh = s.NullableInt("rssiHigh"),
        BatCal = ParseIntArray(s, "batCal"),
        Net = s.NullableInt("net"),
        StartPhase = s.TryGet("startPhase", out _) ? s.Str("startPhase") : null,
        Name = s.TryGet("name", out _) ? s.Str("name") : null,
        Ton = s.NullableDouble("ton"),
        Toff = s.NullableDouble("toff"),
        Led = s.NullableBool("led"),
        Raw = s.Clone(),
    };

    private static IReadOnlyList<int>? ParseIntArray(JsonElement e, string name)
    {
        if (e.Arr(name) is not { } arr) return null;
        var list = new List<int>(arr.GetArrayLength());
        foreach (var item in arr.EnumerateArray())
            list.Add((int)Math.Round(Jx.AsDouble(item)));
        return list;
    }

    // ---- events ---------------------------------------------------------------------------

    private static FleetMessage ParseFleet(JsonElement r)
    {
        var devs = new List<FleetDevice>();
        if (r.Arr("devs") is { } arr)
        {
            int pos = 0;
            foreach (var d in arr.EnumerateArray())
            {
                if (!d.IsObject()) { pos++; continue; }
                devs.Add(ParseFleetDevice(d, pos));
                pos++;
            }
        }
        return new FleetMessage(r.Long("seq"), r.Int("active", -1), r.Int("ch"), devs);
    }

    public static FleetDevice ParseFleetDevice(JsonElement d, int fallbackIndex = 0) => new()
    {
        Index = d.Int("i", fallbackIndex),
        Mac = d.Str("mac").ToUpperInvariant(),
        Name = d.Str("name"),
        Online = d.Bool("online"),
        On = d.Bool("on"),
        Phase = d.Str("ph").Equals("on", StringComparison.OrdinalIgnoreCase) ? CyclePhase.On : CyclePhase.Off,
        Mode = ParseMode(d.Str("mode", "auto")),
        Ton = d.Dbl("ton"),
        Toff = d.Dbl("toff"),
        Elapsed = d.Dbl("el"),
        Rssi = d.Int("rssi"),
        RssiT = d.Int("rssiT"),
        AgeMs = d.Int("age", -1),
        Loss = d.Int("loss", -1),
        Channel = d.Int("ch"),
        Boot = d.Int("boot"),
        UpSeconds = d.Long("up"),
        Fw = d.Str("fw"),
        Pending = d.Int("pend"),
        SlotId = d.Int("id", fallbackIndex),
    };

    public static TimerMode ParseMode(string s) => s switch
    {
        "auto" => TimerMode.Auto,
        "forceOn" => TimerMode.ForceOn,
        "paused" => TimerMode.Paused,
        _ => TimerMode.Unknown,
    };

    public static string ModeToWire(TimerMode m) => m switch
    {
        TimerMode.ForceOn => "forceOn",
        TimerMode.Paused => "paused",
        _ => "auto",
    };

    private static RemoteMessage ParseRemote(JsonElement r) => new()
    {
        Bat = r.Int("bat", -1),
        Mv = r.Int("mv"),
        Charging = r.Bool("chg"),
        Usb = r.Bool("usb"),
        Channel = r.Int("ch"),
        Net = r.Int("net"),
        UpSeconds = r.Long("up"),
        Heap = r.Long("heap"),
        Menu = r.Str("menu"),
        Blank = r.Bool("blank"),
        Scan = r.Bool("scan"),
        Txq = r.Int("txq"),
    };

    private static CmdResultMessage ParseCmdResult(JsonElement r)
    {
        var text = r.Str("result");
        var outcome = text switch
        {
            "ok" => CmdOutcome.Ok,
            "rejected" => CmdOutcome.Rejected,
            "timeout" => CmdOutcome.Timeout,
            "cancelled" => CmdOutcome.Cancelled,
            _ => CmdOutcome.Unknown,
        };
        return new CmdResultMessage(r.Int("id"), r.Int("dev", -1), r.Str("mac").ToUpperInvariant(), r.Str("op"), outcome, text, r.Int("attempts"), r.Int("ms"));
    }

    private static FoundMessage ParseFound(JsonElement r) => new(
        r.Str("mac").ToUpperInvariant(), r.Str("name"), r.Int("rssi"), r.Int("ch"), r.Int("paired", -1),
        r.Dbl("ton"), r.Dbl("toff"), r.Bool("on"), r.Int("boot"));

    private static MigrateMessage ParseMigrate(JsonElement r)
    {
        // "ok" is a boolean for stage "dev" but a count for stage "done".
        bool? devOk = null;
        int okCount = 0;
        if (r.TryGet("ok", out var ok))
        {
            if (ok.ValueKind is JsonValueKind.True or JsonValueKind.False) devOk = ok.GetBoolean();
            else { okCount = r.Int("ok"); devOk = okCount > 0; }
        }
        var stage = r.Str("stage");
        return new MigrateMessage
        {
            Stage = stage,
            Channel = r.Int("ch"),
            Targets = r.Int("targets"),
            Dev = r.Int("dev", -1),
            DevOk = stage == "dev" ? devOk : null,
            OkCount = stage == "dev" ? 0 : okCount,
            Failed = r.Int("failed"),
            Committed = r.Bool("committed"),
        };
    }

    public static LogLevel ParseLevel(string s) => s.ToLowerInvariant() switch
    {
        "error" or "err" or "e" => LogLevel.Error,
        "warn" or "warning" or "w" => LogLevel.Warn,
        "info" or "i" => LogLevel.Info,
        "debug" or "d" or "trace" or "verbose" => LogLevel.Debug,
        "off" => LogLevel.Off,
        _ => LogLevel.Info,
    };

    private static LogMessage ParseLog(JsonElement r)
    {
        var lvl = r.Str("lvl", "info");
        return new LogMessage(ParseLevel(lvl), lvl, r.Long("ms"), r.Str("src"), r.Str("msg"));
    }

    // ---- performance counters -------------------------------------------------------------

    public static StatsSnapshot ParseStats(JsonElement e)
    {
        var self = e.Obj("self") is { } s ? ParseSelfStats(s) : new SelfStats();
        return new StatsSnapshot { Ms = e.Long("ms"), Self = self, Raw = e.Clone() };
    }

    /// <summary>Parses a <c>devStat</c> object (event, or the value of <c>res.devStat</c>).</summary>
    public static DevStatsEntry ParseDevStat(JsonElement d) => new(
        d.Int("i"), d.Str("mac").ToUpperInvariant(),
        d.Obj("link") is { } l ? ParseDevLink(l) : null,
        d.Obj("timer") is { } t ? ParseTimerStats(t) : null,
        d.Long("ms"));

    private static ResetCounts ParseRst(JsonElement? r) => r is not { } e
        ? ResetCounts.Empty
        : new ResetCounts(e.Long("poweron"), e.Long("brownout"), e.Long("wdt"), e.Long("panic"), e.Long("sw"), e.Long("deepsleep"), e.Long("other"));

    public static SelfStats ParseSelfStats(JsonElement s)
    {
        var radio = s.Obj("radio") is { } r
            ? new RadioCounters(r.Int("ch", s.Int("ch")), r.Long("tx"), r.Long("txOk"), r.Long("txFail"), r.Long("txWd"), r.Long("txDrop"),
                r.Long("rx"), r.Long("rxBad"), r.Long("rxForeign"), r.Long("rxDrop"))
            : RadioCounters.Empty;

        var chan = new List<ChannelCounters>();
        if (s.Arr("chan") is { } ca)
            foreach (var c in ca.EnumerateArray())
                if (c.IsObject()) chan.Add(new ChannelCounters(c.Int("ch"), c.Long("tx"), c.Long("fail"), c.Long("rx")));

        var link = s.Obj("link") is { } l
            ? new LinkSummary(l.Long("cmds"), l.Long("ok"), l.Long("timeout"), l.Long("rejected"), l.Long("retries"),
                l.Dbl("rttAvg"), l.Dbl("rttMax"), l.Long("sweeps"), l.Long("recovers"), l.Long("found"), l.Long("migrations"))
            : LinkSummary.Empty;

        var pc = s.Obj("pc") is { } p
            ? new PcCounters(p.Long("linesRx"), p.Long("linesTx"), p.Long("badJson"), p.Long("txDrop"))
            : PcCounters.Empty;

        return new SelfStats
        {
            Kind = s.Str("kind") switch { "remote" => DeviceKind.Remote, "timer" => DeviceKind.Timer, _ => DeviceKind.Unknown },
            UpSeconds = s.Long("up"),
            Boots = s.Int("boots"),
            Heap = s.Long("heap"),
            HeapMin = s.Long("heapMin"),
            LoopMaxUs = s.Long("loopMaxUs"),
            LoopsPerS = s.Long("loopsPerS"),
            RstReason = s.Str("rstReason"),
            Rst = ParseRst(s.Obj("rst")),
            Radio = radio,
            Chan = chan,
            Link = link,
            Pc = pc,
            Timer = s.Obj("timer") is { } selfTimer ? ParseTimerStats(selfTimer) : null,
        };
    }

    private static DevLinkStats ParseDevLink(JsonElement l) => new()
    {
        Rx = l.Long("rx"),
        Expected = l.Long("expected"),
        Lost = l.Long("lost"),
        Loss = l.Int("loss", -1),
        Dups = l.Long("dups"),
        MacOk = l.Long("macOk"),
        MacFail = l.Long("macFail"),
        Cmds = l.Long("cmds"),
        CmdsOk = l.Long("cmdsOk"),
        CmdsTimeout = l.Long("cmdsTimeout"),
        Retries = l.Long("retries"),
        RttAvg = l.Dbl("rttAvg"),
        RttMax = l.Dbl("rttMax"),
        RssiMin = l.Int("rssiMin"),
        RssiAvg = l.Int("rssiAvg"),
        RssiMax = l.Int("rssiMax"),
        OfflineEvents = l.Long("offlineEvents"),
        OfflineMs = l.Long("offlineMs"),
        Reboots = l.Long("reboots"),
    };

    public static TimerStats ParseTimerStats(JsonElement t) => new()
    {
        AgeMs = t.Long("age"),
        Fw = t.Str("fw"),
        UpSeconds = t.Long("up"),
        Boots = t.Int("boots"),
        RstReason = t.Str("rstReason"),
        Heap = t.Long("heap"),
        HeapMin = t.Long("heapMin"),
        LoopMaxUs = t.Long("loopMaxUs"),
        Channel = t.Int("ch"),
        Net = t.Int("net"),
        Rst = ParseRst(t.Obj("rst")),
        Rx = t.Long("rx"),
        RxBad = t.Long("rxBad"),
        RxForeign = t.Long("rxForeign"),
        RxDrop = t.Long("rxDrop"),
        RxProbes = t.Long("rxProbes"),
        RxCmds = t.Long("rxCmds"),
        Tx = t.Long("tx"),
        TxFail = t.Long("txFail"),
        TxDrop = t.Long("txDrop"),
        Beats = t.Long("beats"),
        Replies = t.Long("replies"),
        CmdsRun = t.Long("cmdsRun"),
        CmdsDup = t.Long("cmdsDup"),
        CmdsRejected = t.Long("cmdsRejected"),
        RssiMin = t.Int("rssiMin"),
        RssiAvg = t.Int("rssiAvg"),
        RssiMax = t.Int("rssiMax"),
        RssiLast = t.Int("rssiLast"),
    };
}
