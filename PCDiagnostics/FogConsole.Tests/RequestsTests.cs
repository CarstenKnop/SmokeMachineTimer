using System.Text.Json.Nodes;
using FogConsole.Protocol;
using Xunit;

namespace FogConsole.Tests;

public class RequestsTests
{
    private static JsonObject Roundtrip(JsonObject req, int id = 7) =>
        JsonNode.Parse(Requests.Serialize(req, id))!.AsObject();

    [Fact]
    public void Serialize_puts_type_and_id_first_and_is_a_single_line()
    {
        var line = Requests.Serialize(Requests.GetSettings(), 3);
        Assert.StartsWith("{\"t\":\"get\",\"id\":3,", line);
        Assert.DoesNotContain('\n', line);
        var o = Roundtrip(Requests.GetSettings(), 3);
        Assert.Equal("settings", o["what"]!.GetValue<string>());
        Assert.Equal(3, o["id"]!.GetValue<int>());
    }

    [Fact]
    public void Serialize_does_not_modify_the_template()
    {
        var req = Requests.Ping();
        _ = Requests.Serialize(req, 1);
        _ = Requests.Serialize(req, 2);
        Assert.False(req.ContainsKey("id"));
    }

    [Fact]
    public void Cmd_addresses_by_index_mac_or_all()
    {
        var byIdx = Roundtrip(Requests.Cmd(CommandSpec.Toggle(CommandTarget.Dev(2))));
        Assert.Equal(2, byIdx["dev"]!.GetValue<int>());
        Assert.Equal("toggle", byIdx["op"]!.GetValue<string>());

        var byMac = Roundtrip(Requests.Cmd(CommandSpec.Reset(CommandTarget.ByMac("aa:bb:cc:dd:ee:01"))));
        Assert.Equal("AA:BB:CC:DD:EE:01", byMac["mac"]!.GetValue<string>());
        Assert.False(byMac.ContainsKey("dev"));

        var all = Roundtrip(Requests.Cmd(CommandSpec.Toggle(CommandTarget.All)));
        Assert.Equal("all", all["dev"]!.GetValue<string>());
    }

    [Fact]
    public void SetTimes_rounds_to_a_tenth()
    {
        var o = Roundtrip(Requests.Cmd(CommandSpec.SetTimes(CommandTarget.Dev(0), 1.5000001, 9.96)));
        Assert.Equal(1.5, o["on"]!.GetValue<double>());
        Assert.Equal(10.0, o["off"]!.GetValue<double>());
    }

    [Fact]
    public void Mode_force_on_includes_a_clamped_lease_and_auto_does_not()
    {
        var f = Roundtrip(Requests.Cmd(CommandSpec.Mode(CommandTarget.Dev(1), TimerMode.ForceOn, 1000)));
        Assert.Equal("forceOn", f["mode"]!.GetValue<string>());
        Assert.Equal(1000, f["leaseMs"]!.GetValue<int>());

        Assert.Equal(200, Roundtrip(Requests.Cmd(CommandSpec.Mode(CommandTarget.Dev(1), TimerMode.ForceOn, 5)))["leaseMs"]!.GetValue<int>());
        Assert.Equal(5000, Roundtrip(Requests.Cmd(CommandSpec.Mode(CommandTarget.Dev(1), TimerMode.ForceOn, 99999)))["leaseMs"]!.GetValue<int>());
        Assert.Equal(1000, Roundtrip(Requests.Cmd(CommandSpec.Mode(CommandTarget.Dev(1), TimerMode.ForceOn)))["leaseMs"]!.GetValue<int>());

        var a = Roundtrip(Requests.Cmd(CommandSpec.Mode(CommandTarget.Dev(1), TimerMode.Auto, 1000)));
        Assert.Equal("auto", a["mode"]!.GetValue<string>());
        Assert.False(a.ContainsKey("leaseMs"));
        Assert.Equal("paused", Roundtrip(Requests.Cmd(CommandSpec.Mode(CommandTarget.Dev(1), TimerMode.Paused)))["mode"]!.GetValue<string>());
    }

    [Fact]
    public void Other_ops_carry_their_arguments()
    {
        var sp = Roundtrip(Requests.Cmd(CommandSpec.SetPhase(CommandTarget.Dev(3), CyclePhase.Off, 2.25)));
        Assert.Equal("off", sp["phase"]!.GetValue<string>());
        Assert.Equal(2.3, sp["elapsed"]!.GetValue<double>(), 3);

        Assert.Equal("Fog7", Roundtrip(Requests.Cmd(CommandSpec.Rename(CommandTarget.Dev(0), "Fog7")))["name"]!.GetValue<string>());
        Assert.Equal(11, Roundtrip(Requests.Cmd(CommandSpec.Channel(CommandTarget.Dev(0), 11)))["ch"]!.GetValue<int>());
        Assert.Equal("on", Roundtrip(Requests.Cmd(CommandSpec.StartPhase(CommandTarget.Dev(0), CyclePhase.On)))["phase"]!.GetValue<string>());
        Assert.Equal(5000, Roundtrip(Requests.Cmd(CommandSpec.Identify(CommandTarget.Dev(0))))["ms"]!.GetValue<int>());
    }

    [Fact]
    public void Fleet_management_and_tools()
    {
        Assert.Equal(2, Roundtrip(Requests.Select(2))["dev"]!.GetValue<int>());
        var pair = Roundtrip(Requests.Pair("aa:bb:cc:dd:ee:08", "Fog8"));
        Assert.Equal("AA:BB:CC:DD:EE:08", pair["mac"]!.GetValue<string>());
        Assert.Equal("Fog8", pair["name"]!.GetValue<string>());
        Assert.False(Roundtrip(Requests.Pair("aa:bb:cc:dd:ee:08", " "))!.ContainsKey("name"));
        Assert.Equal(1, Roundtrip(Requests.Unpair(1))["dev"]!.GetValue<int>());
        var disc = Roundtrip(Requests.Discover(true, true));
        Assert.True(disc["on"]!.GetValue<bool>());
        Assert.True(disc["sweep"]!.GetValue<bool>());
        var ch = Roundtrip(Requests.Channel(11, false));
        Assert.Equal(11, ch["ch"]!.GetValue<int>());
        Assert.False(ch["force"]!.GetValue<bool>());
        var lt = Roundtrip(Requests.LinkTest(0, 50, 40));
        Assert.Equal(50, lt["count"]!.GetValue<int>());
        Assert.Equal(40, lt["gapMs"]!.GetValue<int>());
        Assert.Equal("debug", Roundtrip(Requests.LogCfg("debug"))["level"]!.GetValue<string>());
        Assert.True(Roundtrip(Requests.Sniff(true))["on"]!.GetValue<bool>());
        var fr = Roundtrip(Requests.FactoryReset("remote"));
        Assert.True(fr["confirm"]!.GetValue<bool>());
        Assert.Equal("remote", fr["scope"]!.GetValue<string>());
    }

    [Fact]
    public void Stats_requests()
    {
        var cfg = Roundtrip(Requests.StatsCfg(1000));
        Assert.Equal("statsCfg", cfg["t"]!.GetValue<string>());
        Assert.Equal(1000, cfg["intervalMs"]!.GetValue<int>());
        var dev = Roundtrip(Requests.GetDevStats(CommandTarget.Dev(3)));
        Assert.Equal("devStats", dev["what"]!.GetValue<string>());
        Assert.Equal(3, dev["dev"]!.GetValue<int>());
        Assert.Equal("stats", Roundtrip(Requests.GetStats())["what"]!.GetValue<string>());
    }

    [Theory]
    [InlineData(0, 0)]
    [InlineData(-5, 0)]
    [InlineData(100, 500)]       // protocol lower bound
    [InlineData(500, 500)]
    [InlineData(2000, 2000)]
    public void The_stats_interval_respects_the_protocol_bounds(int asked, int sent)
    {
        Assert.Equal(sent, Roundtrip(Requests.StatsCfg(asked))["intervalMs"]!.GetValue<int>());
    }

    [Fact]
    public void Describe_is_human_readable()
    {
        Assert.Contains("ON 1.5 s / OFF 10 s", CommandSpec.SetTimes(CommandTarget.Dev(0), 1.5, 10).Describe());
        Assert.Contains("forceOn", CommandSpec.Mode(CommandTarget.Dev(0), TimerMode.ForceOn).Describe());
    }
}
