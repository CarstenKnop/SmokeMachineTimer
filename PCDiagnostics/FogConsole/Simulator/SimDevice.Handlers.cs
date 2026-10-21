using System.Text.Json.Nodes;
using FogConsole.Protocol;

namespace FogConsole.Simulator;

public sealed partial class SimDevice
{
    private const string Build = "Oct 20 2026 12:00:00";

    // ---- argument helpers -----------------------------------------------------------------

    private static bool TryInt(JsonNode? n, out int v)
    {
        v = 0;
        if (n is JsonValue jv)
        {
            if (jv.TryGetValue<int>(out v)) return true;
            if (jv.TryGetValue<double>(out var d) && d == Math.Floor(d) && Math.Abs(d) < int.MaxValue) { v = (int)d; return true; }
        }
        return false;
    }

    private static bool TryDouble(JsonNode? n, out double v)
    {
        v = 0;
        return n is JsonValue jv && jv.TryGetValue<double>(out v) && double.IsFinite(v);
    }

    private static bool TryBool(JsonNode? n, out bool v)
    {
        v = false;
        return n is JsonValue jv && jv.TryGetValue<bool>(out v);
    }

    private static string? TryString(JsonNode? n) =>
        n is JsonValue jv && jv.TryGetValue<string>(out var s) ? s : null;

    private static bool ValidName(string? s) =>
        s is { Length: >= 1 and <= 9 } && s.All(c => c is >= ' ' and <= '~');

    // ---- entry point ----------------------------------------------------------------------

    private void HandleLineCore(string line)
    {
        bool wasPresent = PcPresent;
        JsonObject? req;
        try { req = JsonNode.Parse(line) as JsonObject; }
        catch { req = null; }
        if (req is null)
        {
            _pcBadJson++;
            return;
        }

        _pcLinesRx++;
        _lastPcMs = _now;
        if (!wasPresent) OnSessionStart();

        string? type = TryString(req["t"]);
        if (!TryInt(req["id"], out int id) || id < 1)
        {
            Fail(0, "badRequest", "missing or invalid 'id'");
            return;
        }
        if (type is null)
        {
            Fail(id, "badRequest", "missing 't'");
            return;
        }

        try { Dispatch(type, id, req); }
        catch (Exception ex) { Fail(id, "failed", ex.Message); }
    }

    private bool IsRemote => _opt.Kind == SimKind.Remote;

    private bool RequireRemote(int id)
    {
        if (IsRemote) return true;
        Fail(id, "unsupported", "not available on a timer connected directly");
        return false;
    }

    private void Dispatch(string type, int id, JsonObject req)
    {
        switch (type)
        {
            case "hello": Reply(id, HelloFill); break;
            case "ping": Reply(id, r => r["ms"] = _now); break;
            case "get": HandleGet(id, req); break;
            case "statsCfg": HandleStatsCfg(id, req); break;
            case "set": HandleSet(id, req); break;
            case "cmd": HandleCmd(id, req); break;
            case "select": if (RequireRemote(id)) HandleSelect(id, req); break;
            case "pair": if (RequireRemote(id)) HandlePair(id, req); break;
            case "unpair": if (RequireRemote(id)) HandleUnpair(id, req); break;
            case "discover": if (RequireRemote(id)) HandleDiscover(id, req); break;
            case "survey": if (RequireRemote(id)) HandleSurvey(id); break;
            case "channel": if (RequireRemote(id)) HandleChannel(id, req); break;
            case "linkTest": if (RequireRemote(id)) HandleLinkTest(id, req); break;
            case "recover": if (RequireRemote(id)) HandleRecover(id); break;
            case "logCfg": HandleLogCfg(id, req); break;
            case "sniff":
                if (!TryBool(req["on"], out bool on)) Fail(id, "badArg", "'on' (bool) required");
                else { _sniff = on; Reply(id); }
                break;
            case "reboot":
                Reply(id);
                Schedule(250, () => DoReboot(TimeSpan.FromSeconds(3.5)));
                break;
            case "factoryReset": HandleFactoryReset(id, req); break;
            default: Fail(id, "unknownType", $"unknown request type '{type}'"); break;
        }
    }

    private void HelloFill(JsonObject r)
    {
        bool remote = IsRemote;
        r["kind"] = remote ? "remote" : "timer";
        r["fw"] = "2.0.0";
        r["proto"] = 2;
        r["mac"] = remote ? "AA:BB:CC:00:11:22" : _timers[0].Mac;
        r["name"] = remote ? "Remote" : _timers[0].Name;
        r["ch"] = _homeCh;
        r["net"] = _settings.Net;
        r["upMs"] = _now - _bootAtMs;
        r["rst"] = _rstReason;
        r["boots"] = remote ? _boots : _timers[0].Boot;
        r["heap"] = _heap;
        r["build"] = Build;
        r["maxDevs"] = remote ? MaxDevs : 1;
    }

    // ---- get / settings / stats -----------------------------------------------------------

    private void HandleGet(int id, JsonObject req)
    {
        switch (TryString(req["what"]))
        {
            case "fleet":
                Reply(id);
                EmitFleet();
                break;
            case "settings":
                Reply(id, r => r["settings"] = SettingsJson());
                break;
            case "stats":
                // res.stats = the self counters; one devStat event per paired timer follows
                Reply(id, r => r["stats"] = BuildStats()["self"]!.DeepClone());
                if (IsRemote)
                    for (int i = 0; i < _paired.Count; i++) Emit(BuildDevStat(i, _paired[i]));
                break;
            case "devStats":
                if (RequireRemote(id)) HandleDevStats(id, req);
                break;
            default:
                Fail(id, "badArg", "unknown 'what'");
                break;
        }
    }

    private JsonObject SettingsJson()
    {
        if (!IsRemote)
        {
            var t = _timers[0];
            return new JsonObject
            {
                ["startPhase"] = t.StartPhase == CyclePhase.On ? "on" : "off", ["name"] = t.Name,
                ["ton"] = Math.Round(t.Ton, 1), ["toff"] = Math.Round(t.Toff, 1), ["led"] = t.Led,
            };
        }
        return new JsonObject
        {
            ["txPower"] = _settings.TxPower, ["brightness"] = _settings.Brightness, ["autoOff"] = _settings.AutoOff,
            ["rssiLow"] = _settings.RssiLow, ["rssiHigh"] = _settings.RssiHigh,
            ["batCal"] = new JsonArray(_settings.BatCal.Select(v => (JsonNode?)JsonValue.Create(v)).ToArray()),
            ["net"] = _settings.Net,
        };
    }

    private void HandleStatsCfg(int id, JsonObject req)
    {
        if (!TryInt(req["intervalMs"], out int ms) || (ms != 0 && ms < 500) || ms > 3_600_000)
        {
            Fail(id, "badArg", "'intervalMs' must be 0 (off) or >= 500");
            return;
        }
        _statsInterval = ms;
        if (ms > 0)
        {
            _nextStatsMs = _now + 100;
            _nextInfoPollMs = _now;
        }
        Reply(id, r => r["intervalMs"] = ms);
    }

    private void HandleSet(int id, JsonObject req)
    {
        if (req["settings"] is not JsonObject s)
        {
            Fail(id, "badRequest", "'settings' object required");
            return;
        }

        if (!IsRemote)
        {
            var t = _timers[0];
            string? sp = null, name = null;
            double ton = t.Ton, toff = t.Toff;
            bool led = t.Led;
            if (s.ContainsKey("startPhase"))
            {
                sp = TryString(s["startPhase"]);
                if (sp is not ("on" or "off")) { Fail(id, "badArg", "startPhase must be 'on' or 'off'"); return; }
            }
            if (s.ContainsKey("name"))
            {
                name = TryString(s["name"]);
                if (!ValidName(name)) { Fail(id, "badArg", "name must be 1-9 printable ASCII characters"); return; }
            }
            if (s.ContainsKey("ton") && (!TryDouble(s["ton"], out ton) || ton is < 0.1 or > 9999.9)) { Fail(id, "badArg", "ton must be 0.1 ... 9999.9"); return; }
            if (s.ContainsKey("toff") && (!TryDouble(s["toff"], out toff) || toff is < 0.1 or > 9999.9)) { Fail(id, "badArg", "toff must be 0.1 ... 9999.9"); return; }
            if (s.ContainsKey("led") && !TryBool(s["led"], out led)) { Fail(id, "badArg", "led must be a bool"); return; }
            if (sp is not null) t.StartPhase = sp == "on" ? CyclePhase.On : CyclePhase.Off;
            if (name is not null) t.Name = name;
            t.Ton = Math.Round(ton, 1);
            t.Toff = Math.Round(toff, 1);
            t.Led = led;
            _fleetDirty = true;
            Reply(id, r => r["settings"] = SettingsJson());
            return;
        }

        int txPower = _settings.TxPower, brightness = _settings.Brightness, autoOff = _settings.AutoOff;
        int low = _settings.RssiLow, high = _settings.RssiHigh, net = _settings.Net;
        int[] cal = _settings.BatCal;
        if (s.ContainsKey("txPower") && (!TryInt(s["txPower"], out txPower) || txPower is < 8 or > 84)) { Fail(id, "badArg", "txPower must be 8 ... 84"); return; }
        if (s.ContainsKey("brightness") && (!TryInt(s["brightness"], out brightness) || brightness is < 5 or > 255)) { Fail(id, "badArg", "brightness must be 5 ... 255"); return; }
        if (s.ContainsKey("autoOff") && (!TryInt(s["autoOff"], out autoOff) || autoOff is < 0 or > 86_400)) { Fail(id, "badArg", "autoOff must be 0 ... 86400"); return; }
        if (s.ContainsKey("rssiLow") && !TryInt(s["rssiLow"], out low)) { Fail(id, "badArg", "rssiLow must be an integer"); return; }
        if (s.ContainsKey("rssiHigh") && !TryInt(s["rssiHigh"], out high)) { Fail(id, "badArg", "rssiHigh must be an integer"); return; }
        if (high < low + 5) { Fail(id, "badArg", "rssiHigh must be >= rssiLow + 5"); return; }
        if (s.ContainsKey("net") && (!TryInt(s["net"], out net) || net is < 1 or > 254)) { Fail(id, "badArg", "net must be 1 ... 254"); return; }
        if (s.ContainsKey("batCal"))
        {
            if (s["batCal"] is not JsonArray arr || arr.Count != 3 || !arr.All(n => TryInt(n, out _)))
            { Fail(id, "badArg", "batCal must be [a0,a50,a100]"); return; }
            cal = arr.Select(n => { TryInt(n, out var v); return v; }).ToArray();
            if (!(cal[0] < cal[1] && cal[1] < cal[2])) { Fail(id, "badArg", "batCal must be ascending"); return; }
        }

        _settings.TxPower = txPower;
        _settings.Brightness = brightness;
        _settings.AutoOff = autoOff;
        _settings.RssiLow = low;
        _settings.RssiHigh = high;
        _settings.Net = net;
        _settings.BatCal = cal;
        Log(2, "cfg", "settings changed");
        Reply(id, r => r["settings"] = SettingsJson());
    }

    private void HandleLogCfg(int id, JsonObject req)
    {
        switch (TryString(req["level"]))
        {
            case "off": _logLevel = -1; break;
            case "error": _logLevel = 0; break;
            case "warn": _logLevel = 1; break;
            case "info": _logLevel = 2; break;
            case "debug": _logLevel = 3; break;
            default: Fail(id, "badArg", "level must be error|warn|info|debug|off"); return;
        }
        Reply(id);
        Log(3, "log", $"log level set to {TryString(req["level"])}");
    }

    // ---- target resolution ----------------------------------------------------------------

    /// <summary>Resolves dev/mac to a single timer. Returns false (and answers with an error) otherwise.</summary>
    private bool ResolveOne(int id, JsonObject req, out SimTimer timer, bool allowAll = false)
    {
        timer = null!;
        var list = ResolveTargets(req, allowAll, out var error);
        if (list is null)
        {
            Fail(id, "noDevice", error ?? "no such device");
            return false;
        }
        if (list.Count != 1)
        {
            Fail(id, "badArg", "exactly one device expected");
            return false;
        }
        timer = list[0];
        return true;
    }

    private List<SimTimer>? ResolveTargets(JsonObject req, bool allowAll, out string? error)
    {
        error = null;
        IReadOnlyList<SimTimer> fleet = IsRemote ? _paired : _timers;
        if (req["mac"] is not null)
        {
            string? mac = TryString(req["mac"])?.ToUpperInvariant();
            var found = fleet.FirstOrDefault(t => t.Mac == mac);
            if (found is null) { error = $"no paired device with MAC {mac}"; return null; }
            return new List<SimTimer> { found };
        }
        var dev = req["dev"];
        if (dev is null) { error = "'dev' or 'mac' required"; return null; }
        if (TryString(dev) == "all")
        {
            if (!allowAll) { error = "'all' not allowed here"; return null; }
            return fleet.ToList();
        }
        if (TryInt(dev, out int idx) && idx >= 0 && idx < fleet.Count)
            return new List<SimTimer> { fleet[idx] };
        error = $"no device at index {dev?.ToJsonString()}";
        return null;
    }

    // ---- select / pair / unpair / discover ------------------------------------------------

    private void HandleSelect(int id, JsonObject req)
    {
        if (!ResolveOne(id, req, out var t)) return;
        _active = IndexOf(t);
        _fleetDirty = true;
        Reply(id);
    }

    private void HandlePair(int id, JsonObject req)
    {
        string? mac = TryString(req["mac"])?.ToUpperInvariant();
        if (mac is null || mac.Length != 17) { Fail(id, "badArg", "'mac' (AA:BB:CC:DD:EE:FF) required"); return; }
        string? name = req.ContainsKey("name") ? TryString(req["name"]) : null;
        if (name is not null && !ValidName(name)) { Fail(id, "badArg", "name must be 1-9 printable ASCII characters"); return; }
        var t = _timers.FirstOrDefault(x => x.Mac == mac);
        if (t is null) { Fail(id, "noDevice", "that timer has not been heard"); return; }
        if (_paired.Contains(t)) { Fail(id, "badArg", "already paired"); return; }
        if (_paired.Count >= MaxDevs) { Fail(id, "full", "the list holds at most 10 timers"); return; }

        int slot = Enumerable.Range(0, MaxDevs).First(s => _paired.All(p => p.Slot != s));
        t.Slot = slot;
        t.Name = name ?? (t.Name == "FogTimer" ? $"Fog{_paired.Count + 1}" : t.Name);
        _paired.Add(t);
        t.L = new SimLinkCounters();
        t.ClearLossWindow();
        t.Jobs.Clear();
        int idx = _paired.Count - 1;
        if (t.Powered && t.Channel == _homeCh)
        {
            t.EverHeard = true;
            t.LastHeardMs = _now;
            t.WasOnline = false;
            RefreshKnown(t);
        }
        else if (t.Channel != _homeCh)
        {
            int from = t.Channel;
            Schedule(1200, () =>
            {
                t.Channel = _homeCh;
                Log(2, "link", $"moved {t.Name} from ch{from} to ch{_homeCh}");
            });
        }
        _fleetDirty = true;
        Log(2, "link", $"paired {t.Name} {t.Mac} as T{idx}");
        Reply(id, r => r["i"] = idx);
    }

    private void HandleUnpair(int id, JsonObject req)
    {
        if (!ResolveOne(id, req, out var t)) return;
        int idx = IndexOf(t);
        foreach (var job in t.Jobs)
        {
            if (job.Cancelled) continue;
            job.Cancelled = true;
            if (job.RequestId > 0)
                Emit(new JsonObject { ["t"] = "cmdResult", ["id"] = job.RequestId, ["dev"] = idx, ["mac"] = t.Mac, ["op"] = job.Op, ["result"] = "cancelled", ["attempts"] = 0, ["ms"] = 0 });
        }
        t.Jobs.Clear();
        _paired.RemoveAt(idx);
        if (_active > idx) _active--;
        _active = Math.Max(0, Math.Min(_active, _paired.Count - 1));
        _fleetDirty = true;
        Log(2, "link", $"unpaired {t.Name}");
        Reply(id);
    }

    private void HandleDiscover(int id, JsonObject req)
    {
        if (!TryBool(req["on"], out bool on)) { Fail(id, "badArg", "'on' (bool) required"); return; }
        bool sweep = req.ContainsKey("sweep") && TryBool(req["sweep"], out var sw) && sw;
        _discoverOn = on;
        _discoverSweep = on && sweep;
        if (on)
        {
            _nextDiscoverMs = _now + 200;
            _nextSweepHopMs = _now + 300;
        }
        Log(2, "link", on ? (sweep ? "discovery on (sweep)" : "discovery on") : "discovery off");
        Reply(id);
    }

    // ---- survey / channel / recover / linkTest --------------------------------------------

    private bool RadioBusy => _scanFlag || _now < _radioBusyUntil;

    private void HandleSurvey(int id)
    {
        if (RadioBusy) { Fail(id, "busy", "radio busy"); return; }
        _scanFlag = true;
        _radioBusyUntil = _now + 3300;
        Reply(id);
        Log(2, "scan", "survey started");
        var scores = new int[14];
        for (int ch = 1; ch <= 13; ch++)
        {
            int c = ch;
            Schedule(200 + 230 * (c - 1), () =>
            {
                int score = (int)Math.Clamp(Math.Round(_band.BaseScore(c) + NextGaussian() * 2.5), 0, 100);
                scores[c] = score;
                int aps = _band.ApCount(c);
                _tx++;
                Emit(new JsonObject
                {
                    ["t"] = "surveyCh", ["ch"] = c, ["aps"] = aps, ["apRssi"] = _band.StrongestRssi(c),
                    ["frames"] = (int)(score * 4.2) + _rng.Next(0, 30), ["avgRssi"] = _band.AverageRssi(c) + _rng.Next(-2, 3), ["score"] = score,
                });
            });
        }
        Schedule(3300, () =>
        {
            var best = Enumerable.Range(1, 13).OrderBy(c => scores[c]).ThenBy(c => c).Take(3).ToArray();
            _scanFlag = false;
            Emit(new JsonObject { ["t"] = "surveyDone", ["best"] = new JsonArray(best.Select(b => (JsonNode?)JsonValue.Create(b)).ToArray()), ["cur"] = _homeCh });
            Log(2, "scan", $"survey done, best {string.Join(',', best)}");
        });
    }

    private void HandleChannel(int id, JsonObject req)
    {
        if (!TryInt(req["ch"], out int ch) || ch is < 1 or > 13) { Fail(id, "badArg", "ch must be 1 ... 13"); return; }
        bool force = req.ContainsKey("force") && TryBool(req["force"], out var f) && f;
        if (ch == _homeCh) { Fail(id, "badArg", "already on that channel"); return; }
        if (RadioBusy) { Fail(id, "busy", "radio busy"); return; }

        Reply(id);
        var targets = _paired.Where(IsOnline).ToList();
        Emit(new JsonObject { ["t"] = "migrate", ["stage"] = "start", ["ch"] = ch, ["targets"] = targets.Count });
        Log(2, "migrate", $"channel {_homeCh} -> {ch}, {targets.Count} target(s)");
        _scanFlag = true;
        var confirmed = new List<SimTimer>();
        int stepMs = 450;

        for (int k = 0; k < targets.Count; k++)
        {
            var t = targets[k];
            Schedule(400 + stepMs * k, () =>
            {
                bool ok = false;
                for (int attempt = 0; attempt < 3 && !ok; attempt++) ok = Unicast(t, "MIGRATE");
                if (ok) confirmed.Add(t);
                Emit(new JsonObject { ["t"] = "migrate", ["stage"] = "dev", ["dev"] = IndexOf(t), ["ok"] = ok });
            });
        }

        Schedule(400 + stepMs * targets.Count + 400, () =>
        {
            bool commit = confirmed.Count > 0 || force;
            Emit(new JsonObject
            {
                ["t"] = "migrate", ["stage"] = "done", ["ch"] = ch, ["ok"] = confirmed.Count,
                ["failed"] = _paired.Count - confirmed.Count, ["committed"] = commit,
            });
            if (commit)
            {
                _homeCh = ch;
                _migrations++;
                foreach (var t in confirmed)
                    if (t.Powered) t.Channel = ch;
                // timers that did not confirm stay behind on the old channel until "recover" finds them
            }
            _scanFlag = false;
            _fleetDirty = true;
            Log(commit ? 2 : 1, "migrate", commit ? $"done, now on ch{_homeCh}" : "abandoned: no timer confirmed");
        });
    }

    private bool _linkTestActive;
    private bool _bootBanner;

    /// <summary>True once after a reboot: the next USB session starts with the ROM boot text (plain, non-JSON lines).</summary>
    public bool ConsumeBootBanner()
    {
        lock (_lock)
        {
            var b = _bootBanner;
            _bootBanner = false;
            return b;
        }
    }

    private void HandleLinkTest(int id, JsonObject req)
    {
        if (!ResolveOne(id, req, out var t)) return;
        int count = 50, gap = 40;
        if (req.ContainsKey("count") && (!TryInt(req["count"], out count) || count is < 1 or > 200)) { Fail(id, "badArg", "count must be 1 ... 200"); return; }
        if (req.ContainsKey("gapMs") && (!TryInt(req["gapMs"], out gap) || gap is < 5 or > 2000)) { Fail(id, "badArg", "gapMs must be 5 ... 2000"); return; }
        if (_linkTestActive) { Fail(id, "busy", "a link test is already running"); return; }
        if (_scanFlag) { Fail(id, "busy", "radio busy"); return; }

        _linkTestActive = true;
        Reply(id);
        int dev = IndexOf(t);
        int sent = 0, macAck = 0, echo = 0;
        double rttSum = 0, rttMax = 0;

        void Report(bool done)
        {
            Emit(new JsonObject
            {
                ["t"] = "linkTest", ["id"] = id, ["dev"] = dev, ["sent"] = sent, ["macAck"] = macAck, ["echo"] = echo,
                ["rssi"] = t.EverHeard ? t.LastRssiT : 0, ["rttMs"] = echo == 0 ? 0 : Math.Round(rttSum / echo, 1),
                ["rttMax"] = Math.Round(rttMax, 1), ["done"] = done,
            });
        }

        for (int i = 0; i < count; i++)
        {
            int n = i;
            Schedule(gap * (n + 1), () =>
            {
                sent++;
                bool ack = Unicast(t, "PING");
                if (ack) macAck++;
                if (ack && !Chance(0.03 + t.ExtraLoss))
                {
                    echo++;
                    double rtt = Rtt() + (t.BaseRssi < -75 ? 3 : 0);
                    rttSum += rtt;
                    rttMax = Math.Max(rttMax, rtt);
                    t.L.AddRtt(rtt);
                    t.C.Replies++;
                    t.C.Tx++;
                    _rx++;
    Chan(_homeCh).Rx++;
                    t.LastRssi = SampleRssi(t, t.BaseRssi);
                }
                bool last = sent >= count;
                if (last) _linkTestActive = false;
                if (last || sent % 10 == 0) Report(last);
            });
        }
    }

    private void HandleRecover(int id)
    {
        if (RadioBusy) { Fail(id, "busy", "radio busy"); return; }
        _scanFlag = true;
        _radioBusyUntil = _now + 4300;
        _recovers++;
        _sweeps++;
        Reply(id);
        Log(2, "scan", "recover: sweeping channels for lost timers");
        var lost = _paired.Where(t => !IsOnline(t) && t.Channel != _homeCh).ToList();
        var recovered = new List<SimTimer>();
        for (int c = 1; c <= 13; c++)
        {
            int ch = c;
            Schedule(300 * c, () =>
            {
                if (ch == _homeCh) return;
                foreach (var t in lost)
                {
                    if (recovered.Contains(t)) continue;
                    bool ok = Unicast(t, "PING", ch) || (t.Channel == ch && Unicast(t, "PING", ch));
                    if (ok)
                    {
                        recovered.Add(t);
                        _foundCount++;
                        Log(2, "scan", $"found {t.Name} on ch{ch}");
                        Emit(new JsonObject
                        {
                            ["t"] = "found", ["mac"] = t.Mac, ["name"] = t.Name, ["rssi"] = t.LastRssi, ["ch"] = ch,
                            ["paired"] = IndexOf(t), ["ton"] = t.Ton, ["toff"] = t.Toff, ["on"] = t.OutputOn ? 1 : 0, ["boot"] = t.Boot,
                        });
                    }
                }
            });
        }
        Schedule(4300, () =>
        {
            foreach (var t in recovered) t.Channel = _homeCh;
            _scanFlag = false;
            _fleetDirty = true;
            Log(2, "scan", recovered.Count == 0 ? "recover: nothing found" : $"recover: moved {recovered.Count} timer(s) back to ch{_homeCh}");
        });
    }

    // ---- reboot / factory reset -----------------------------------------------------------

    private void HandleFactoryReset(int id, JsonObject req)
    {
        if (!(req.ContainsKey("confirm") && TryBool(req["confirm"], out var c) && c))
        {
            Fail(id, "badArg", "'confirm': true required");
            return;
        }
        string? scope = TryString(req["scope"]);
        if (scope is not ("remote" or "all")) { Fail(id, "badArg", "scope must be 'remote' or 'all'"); return; }

        if (!IsRemote)
        {
            _timers[0].FactoryReset();
            Reply(id);
            Schedule(250, () => DoReboot(TimeSpan.FromSeconds(3)));
            return;
        }

        if (scope == "all")
            foreach (var t in _paired.Where(t => t.Powered && t.Channel == _homeCh))
            {
                Unicast(t, "CMD");
                t.FactoryReset();
            }
        Reply(id);
        Schedule(250, () =>
        {
            _paired.Clear();
            _active = 0;
            var s = new SimSettings();
            _settings.TxPower = s.TxPower;
            _settings.Brightness = s.Brightness;
            _settings.AutoOff = s.AutoOff;
            _settings.RssiLow = s.RssiLow;
            _settings.RssiHigh = s.RssiHigh;
            _settings.BatCal = s.BatCal;
            DoReboot(TimeSpan.FromSeconds(3.5));
        });
    }

    private void DoReboot(TimeSpan down)
    {
        _boots++;
        _rstSw++;
        _rstReason = "sw";
        _bootAtMs = _now;
        _lastPcMs = -1_000_000;       // the USB session is gone
        _bootBanner = true;
        _discoverOn = _discoverSweep = false;
        _scanFlag = false;
        _linkTestActive = false;
        _radioBusyUntil = 0;
        _statsInterval = 0;
        _logLevel = -1;
        _sniff = false;

        // counters count since boot
        _tx = _txOk = _txFail = _txWd = _txDrop = _rx = _rxBad = _rxForeign = _rxDrop = 0;
        _chan.Clear();
        _cmds = _cmdsOk = _cmdsTimeout = _cmdsRejected = _retries = 0;
        _rttSum = _rttMax = 0;
        _rttN = 0;
        _sweeps = _recovers = _foundCount = _migrations = 0;
        _pcLinesRx = _pcLinesTx = _pcBadJson = _pcTxDrop = 0;
        _loopMaxUs = 3_100;
        _heapMin = _heap - 5_000;
        foreach (var t in _paired) { t.L = new SimLinkCounters(); t.Jobs.Clear(); t.InfoJson = null; }

        if (!IsRemote)
        {
            var t = _timers[0];
            t.Boot_(_now, "sw");
        }
        RebootRequested?.Invoke(down);
    }
}
