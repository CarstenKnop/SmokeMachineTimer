using System.Globalization;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace FogConsole.Protocol;

/// <summary>Addresses one timer (by list index or MAC) or all of them.</summary>
public readonly record struct CommandTarget
{
    public int? Index { get; }
    public string? Mac { get; }
    public bool IsAll { get; }

    private CommandTarget(int? index, string? mac, bool all) { Index = index; Mac = mac; IsAll = all; }

    public static CommandTarget Dev(int index) => new(index, null, false);
    public static CommandTarget ByMac(string mac) => new(null, mac.ToUpperInvariant(), false);
    public static CommandTarget All => new(null, null, true);

    public override string ToString() => IsAll ? "all" : Mac ?? $"dev {Index}";

    internal void WriteTo(JsonObject o)
    {
        if (IsAll) o["dev"] = "all";
        else if (Mac is not null) o["mac"] = Mac;
        else o["dev"] = Index ?? 0;
    }
}

/// <summary>A <c>cmd</c> request (everything except the <c>id</c>, which the hub assigns).</summary>
public sealed record CommandSpec(CommandTarget Target, string Op, IReadOnlyDictionary<string, object?>? Args = null, bool Confirm = true)
{
    /// <summary>Fire-and-forget: one short attempt on the radio and no <c>cmdResult</c> event (for hold-to-fog refreshes).</summary>
    public CommandSpec WithoutConfirm() => this with { Confirm = false };

    public static CommandSpec Ping(CommandTarget t) => new(t, "ping");

    public static CommandSpec SetTimes(CommandTarget t, double on, double off) =>
        new(t, "setTimes", new Dictionary<string, object?> { ["on"] = RoundTenth(on), ["off"] = RoundTenth(off) });

    public static CommandSpec Mode(CommandTarget t, TimerMode mode, int? leaseMs = null)
    {
        var args = new Dictionary<string, object?> { ["mode"] = ProtocolParser.ModeToWire(mode) };
        if (mode == TimerMode.ForceOn) args["leaseMs"] = Math.Clamp(leaseMs ?? 1000, 200, 5000);
        return new CommandSpec(t, "mode", args);
    }

    public static CommandSpec Toggle(CommandTarget t) => new(t, "toggle");
    public static CommandSpec Reset(CommandTarget t) => new(t, "reset");

    public static CommandSpec SetPhase(CommandTarget t, CyclePhase phase, double elapsed = 0) =>
        new(t, "setPhase", new Dictionary<string, object?> { ["phase"] = phase == CyclePhase.On ? "on" : "off", ["elapsed"] = RoundTenth(elapsed) });

    public static CommandSpec Rename(CommandTarget t, string name) =>
        new(t, "name", new Dictionary<string, object?> { ["name"] = name });

    public static CommandSpec Channel(CommandTarget t, int ch) =>
        new(t, "channel", new Dictionary<string, object?> { ["ch"] = ch });

    public static CommandSpec StartPhase(CommandTarget t, CyclePhase phase) =>
        new(t, "startPhase", new Dictionary<string, object?> { ["phase"] = phase == CyclePhase.On ? "on" : "off" });

    public static CommandSpec Identify(CommandTarget t, int ms = 5000) =>
        new(t, "identify", new Dictionary<string, object?> { ["ms"] = ms });

    public static CommandSpec Reboot(CommandTarget t) => new(t, "reboot");
    public static CommandSpec FactoryReset(CommandTarget t) => new(t, "factoryReset");

    public static double RoundTenth(double v) => Math.Round(v, 1, MidpointRounding.AwayFromZero);

    public string Describe()
    {
        string extra = Op switch
        {
            "setTimes" => $" ON {Fmt(Args, "on")} s / OFF {Fmt(Args, "off")} s",
            "mode" => $" {Fmt(Args, "mode")}",
            "setPhase" => $" {Fmt(Args, "phase")} @ {Fmt(Args, "elapsed")} s",
            "name" => $" \"{Fmt(Args, "name")}\"",
            "channel" => $" -> ch {Fmt(Args, "ch")}",
            "startPhase" => $" {Fmt(Args, "phase")}",
            _ => "",
        };
        return Op + extra;
    }

    private static string Fmt(IReadOnlyDictionary<string, object?>? a, string k) =>
        a is not null && a.TryGetValue(k, out var v) ? Convert.ToString(v, CultureInfo.InvariantCulture) ?? "" : "";
}

/// <summary>
/// Builds request objects. The hub adds the <c>id</c>. Every builder returns a fresh <see cref="JsonObject"/>.
/// </summary>
public static class Requests
{
    public const string Hello = "hello";

    public static JsonObject Simple(string type) => new() { ["t"] = type };

    public static JsonObject HelloReq() => Simple("hello");
    public static JsonObject Ping() => Simple("ping");

    public static JsonObject Get(string what, CommandTarget? target = null)
    {
        var o = new JsonObject { ["t"] = "get", ["what"] = what };
        target?.WriteTo(o);
        return o;
    }

    public static JsonObject GetFleet() => Get("fleet");
    public static JsonObject GetSettings() => Get("settings");
    public static JsonObject GetStats() => Get("stats");
    public static JsonObject GetDevStats(CommandTarget target) => Get("devStats", target);

    /// <summary>Counter streaming: 0 = off, otherwise at least 500 ms (the protocol's lower bound).</summary>
    public static JsonObject StatsCfg(int intervalMs) => new() { ["t"] = "statsCfg", ["intervalMs"] = intervalMs <= 0 ? 0 : Math.Max(500, intervalMs) };

    public static JsonObject Set(JsonObject settings) => new() { ["t"] = "set", ["settings"] = settings };

    public static JsonObject Cmd(CommandSpec spec)
    {
        var o = new JsonObject { ["t"] = "cmd" };
        spec.Target.WriteTo(o);
        o["op"] = spec.Op;
        if (!spec.Confirm) o["confirm"] = false;
        if (spec.Args is not null)
            foreach (var (k, v) in spec.Args)
                o[k] = ToNode(v);
        return o;
    }

    public static JsonObject Select(int dev) => new() { ["t"] = "select", ["dev"] = dev };

    public static JsonObject Pair(string mac, string? name)
    {
        var o = new JsonObject { ["t"] = "pair", ["mac"] = mac.ToUpperInvariant() };
        if (!string.IsNullOrWhiteSpace(name)) o["name"] = name;
        return o;
    }

    public static JsonObject Unpair(int dev) => new() { ["t"] = "unpair", ["dev"] = dev };

    public static JsonObject Discover(bool on, bool sweep) => new() { ["t"] = "discover", ["on"] = on, ["sweep"] = sweep };

    public static JsonObject Survey() => Simple("survey");

    public static JsonObject Channel(int ch, bool force) => new() { ["t"] = "channel", ["ch"] = ch, ["force"] = force };

    public static JsonObject LinkTest(int dev, int count, int gapMs) =>
        new() { ["t"] = "linkTest", ["dev"] = dev, ["count"] = count, ["gapMs"] = gapMs };

    public static JsonObject Recover() => Simple("recover");

    public static JsonObject LogCfg(string level) => new() { ["t"] = "logCfg", ["level"] = level };

    public static JsonObject Sniff(bool on) => new() { ["t"] = "sniff", ["on"] = on };

    public static JsonObject Reboot() => Simple("reboot");

    public static JsonObject FactoryReset(string scope) => new() { ["t"] = "factoryReset", ["scope"] = scope, ["confirm"] = true };

    /// <summary>Serialises a request, assigning its id.</summary>
    public static string Serialize(JsonObject request, int id)
    {
        // Put "t" and "id" first so the line is easy to read in the Raw console.
        var ordered = new JsonObject();
        if (request["t"] is { } t) ordered["t"] = t.DeepClone();
        ordered["id"] = id;
        foreach (var (k, v) in request)
        {
            if (k is "t" or "id") continue;
            ordered[k] = v?.DeepClone();
        }
        return ordered.ToJsonString();
    }

    private static JsonNode? ToNode(object? v) => v switch
    {
        null => null,
        string s => JsonValue.Create(s),
        bool b => JsonValue.Create(b),
        int i => JsonValue.Create(i),
        long l => JsonValue.Create(l),
        double d => JsonValue.Create(d),
        float f => JsonValue.Create(f),
        JsonNode n => n.DeepClone(),
        _ => JsonValue.Create(Convert.ToString(v, CultureInfo.InvariantCulture)),
    };
}
