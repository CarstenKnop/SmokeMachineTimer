using System.Text.Json.Nodes;
using FogConsole.Protocol;
using Xunit;

namespace FogConsole.Tests;

public class ProtocolParserTests
{
    [Fact]
    public void Hello_response_is_parsed()
    {
        var m = ProtocolParser.Parse("""
            {"t":"res","id":1,"ok":true,"kind":"remote","fw":"2.0.0","proto":2,"mac":"aa:bb:cc:00:11:22",
             "name":"Remote","ch":6,"net":1,"upMs":123456,"rst":"deepsleep","boots":12,"heap":182000,
             "build":"Oct 20 2026 12:00:00","maxDevs":10}
            """.ReplaceLineEndings(""));
        var res = Assert.IsType<ResponseMessage>(m);
        Assert.True(res.Ok);
        Assert.Equal(1, res.Id);
        var h = Assert.IsType<HelloInfo>(res.Hello);
        Assert.Equal(DeviceKind.Remote, h.Kind);
        Assert.Equal("2.0.0", h.Fw);
        Assert.Equal(2, h.Proto);
        Assert.Equal("AA:BB:CC:00:11:22", h.Mac);
        Assert.Equal(6, h.Channel);
        Assert.Equal("deepsleep", h.ResetReason);
        Assert.Equal(12, h.Boots);
        Assert.Equal(10, h.MaxDevs);
    }

    [Fact]
    public void Error_response_carries_code_and_message()
    {
        var res = Assert.IsType<ResponseMessage>(ProtocolParser.Parse("""{"t":"res","id":9,"ok":false,"err":"noDevice","msg":"no such timer"}"""));
        Assert.False(res.Ok);
        Assert.Equal("noDevice", res.Err);
        Assert.Equal("no such timer", res.Msg);
        Assert.Contains("noDevice", res.Describe());
    }

    [Fact]
    public void Cmd_response_has_queued_count_and_ping_has_ms()
    {
        var q = Assert.IsType<ResponseMessage>(ProtocolParser.Parse("""{"t":"res","id":5,"ok":true,"queued":3}"""));
        Assert.Equal(3, q.Queued);
        var p = Assert.IsType<ResponseMessage>(ProtocolParser.Parse("""{"t":"res","id":2,"ok":true,"ms":123456}"""));
        Assert.Equal(123456, p.DeviceMs);
        Assert.Null(p.Queued);
    }

    [Fact]
    public void Fleet_event_is_parsed_with_all_fields()
    {
        var m = ProtocolParser.Parse("""
            {"t":"fleet","seq":812,"active":0,"ch":6,"devs":[
             {"i":0,"mac":"AA:BB:CC:DD:EE:01","name":"Fog1","online":1,"on":1,"ph":"on","mode":"auto",
              "ton":1.5,"toff":10.0,"el":0.8,"rssi":-61,"rssiT":-58,"age":340,"loss":3,"ch":6,
              "boot":7,"up":3600,"fw":"2.0","pend":0,"id":0}]}
            """.ReplaceLineEndings(""));
        var f = Assert.IsType<FleetMessage>(m);
        Assert.Equal(812, f.Seq);
        Assert.Equal(0, f.Active);
        Assert.Equal(6, f.Channel);
        var d = Assert.Single(f.Devs);
        Assert.Equal("Fog1", d.Name);
        Assert.True(d.Online);
        Assert.True(d.On);
        Assert.Equal(CyclePhase.On, d.Phase);
        Assert.Equal(TimerMode.Auto, d.Mode);
        Assert.Equal(1.5, d.Ton);
        Assert.Equal(10.0, d.Toff);
        Assert.Equal(0.8, d.Elapsed);
        Assert.Equal(-61, d.Rssi);
        Assert.Equal(-58, d.RssiT);
        Assert.Equal(340, d.AgeMs);
        Assert.Equal(3, d.Loss);
        Assert.Equal(7, d.Boot);
        Assert.Equal(3600, d.UpSeconds);
        Assert.Equal("2.0", d.Fw);
        Assert.Equal(0, d.SlotId);
    }

    [Fact]
    public void Fleet_is_tolerant_to_unknown_and_missing_fields_and_odd_types()
    {
        var m = ProtocolParser.Parse("""{"t":"fleet","seq":1,"future":{"x":1},"devs":[{"mac":"aa:bb:cc:dd:ee:02","online":true,"on":"1","ph":"off","mode":"forceOn","ton":"2.5","extra":[1,2]},{"i":5}]}""");
        var f = Assert.IsType<FleetMessage>(m);
        Assert.Equal(2, f.Devs.Count);
        var d = f.Devs[0];
        Assert.Equal(0, d.Index);              // falls back to array position
        Assert.Equal("AA:BB:CC:DD:EE:02", d.Mac);
        Assert.True(d.Online);                 // boolean instead of 0/1
        Assert.True(d.On);                     // "1" string
        Assert.Equal(TimerMode.ForceOn, d.Mode);
        Assert.Equal(2.5, d.Ton);              // number as string
        Assert.Equal(0, d.Rssi);               // missing -> default
        Assert.Equal(-1, d.Loss);              // missing loss means "not enough data"
        Assert.Equal(5, f.Devs[1].Index);
        Assert.Equal(-1, f.Active);
    }

    [Fact]
    public void Fleet_with_no_devs_or_wrong_type_is_empty()
    {
        Assert.Empty(Assert.IsType<FleetMessage>(ProtocolParser.Parse("""{"t":"fleet","seq":1,"devs":[]}""")).Devs);
        Assert.Empty(Assert.IsType<FleetMessage>(ProtocolParser.Parse("""{"t":"fleet","seq":1,"devs":"nope"}""")).Devs);
        Assert.Empty(Assert.IsType<FleetMessage>(ProtocolParser.Parse("""{"t":"fleet"}""")).Devs);
    }

    [Fact]
    public void Remote_event_is_parsed()
    {
        var r = Assert.IsType<RemoteMessage>(ProtocolParser.Parse("""{"t":"remote","bat":87,"mv":3987,"chg":0,"usb":1,"ch":6,"net":1,"up":1234,"heap":181000,"menu":"main","blank":0,"scan":0,"txq":0}"""));
        Assert.Equal(87, r.Bat);
        Assert.Equal(3987, r.Mv);
        Assert.False(r.Charging);
        Assert.True(r.Usb);
        Assert.Equal(6, r.Channel);
        Assert.Equal("main", r.Menu);
        Assert.False(r.Scan);
    }

    [Fact]
    public void Timer_kind_remote_event_has_only_a_few_fields()
    {
        var r = Assert.IsType<RemoteMessage>(ProtocolParser.Parse("""{"t":"remote","up":12,"heap":200000,"ch":6,"net":1}"""));
        Assert.Equal(-1, r.Bat);
        Assert.Equal(12, r.UpSeconds);
    }

    [Theory]
    [InlineData("ok", CmdOutcome.Ok)]
    [InlineData("rejected", CmdOutcome.Rejected)]
    [InlineData("timeout", CmdOutcome.Timeout)]
    [InlineData("cancelled", CmdOutcome.Cancelled)]
    [InlineData("whatever", CmdOutcome.Unknown)]
    public void CmdResult_outcomes(string text, CmdOutcome expected)
    {
        var m = ProtocolParser.Parse($$"""{"t":"cmdResult","id":5,"dev":2,"mac":"aa:bb:cc:dd:ee:03","op":"setTimes","result":"{{text}}","attempts":2,"ms":180}""");
        var c = Assert.IsType<CmdResultMessage>(m);
        Assert.Equal(expected, c.Result);
        Assert.Equal(5, c.Id);
        Assert.Equal(2, c.Dev);
        Assert.Equal("AA:BB:CC:DD:EE:03", c.Mac);
        Assert.Equal("setTimes", c.Op);
        Assert.Equal(2, c.Attempts);
        Assert.Equal(180, c.Ms);
    }

    [Fact]
    public void Found_survey_and_link_test_events()
    {
        var f = Assert.IsType<FoundMessage>(ProtocolParser.Parse("""{"t":"found","mac":"AA:BB:CC:DD:EE:08","name":"FogTimer","rssi":-60,"ch":6,"paired":-1,"ton":1.5,"toff":10.0,"on":0,"boot":3}"""));
        Assert.Equal(-1, f.Paired);
        Assert.Equal(3, f.Boot);
        Assert.False(f.On);

        var s = Assert.IsType<SurveyChMessage>(ProtocolParser.Parse("""{"t":"surveyCh","ch":6,"aps":4,"apRssi":-48,"frames":212,"avgRssi":-71,"score":37}"""));
        Assert.Equal(6, s.Channel);
        Assert.Equal(37, s.Score);
        Assert.Equal(4, s.Aps);

        var d = Assert.IsType<SurveyDoneMessage>(ProtocolParser.Parse("""{"t":"surveyDone","best":[11,1,6],"cur":6}"""));
        Assert.Equal(new[] { 11, 1, 6 }, d.Best);
        Assert.Equal(6, d.Current);

        var l = Assert.IsType<LinkTestMessage>(ProtocolParser.Parse("""{"t":"linkTest","id":12,"dev":0,"sent":20,"macAck":20,"echo":19,"rssi":-61,"rttMs":7.2,"rttMax":31,"done":false}"""));
        Assert.Equal(20, l.Sent);
        Assert.Equal(19, l.Echo);
        Assert.Equal(7.2, l.RttMs);
        Assert.False(l.Done);
    }

    [Fact]
    public void Migrate_ok_is_a_bool_for_dev_stage_and_a_count_for_done_stage()
    {
        var start = Assert.IsType<MigrateMessage>(ProtocolParser.Parse("""{"t":"migrate","stage":"start","ch":11,"targets":3}"""));
        Assert.Equal("start", start.Stage);
        Assert.Equal(3, start.Targets);

        var dev = Assert.IsType<MigrateMessage>(ProtocolParser.Parse("""{"t":"migrate","stage":"dev","dev":0,"ok":true}"""));
        Assert.Equal(true, dev.DevOk);
        Assert.Equal(0, dev.Dev);

        var bad = Assert.IsType<MigrateMessage>(ProtocolParser.Parse("""{"t":"migrate","stage":"dev","dev":1,"ok":false}"""));
        Assert.Equal(false, bad.DevOk);

        var done = Assert.IsType<MigrateMessage>(ProtocolParser.Parse("""{"t":"migrate","stage":"done","ch":11,"ok":2,"failed":1,"committed":true}"""));
        Assert.Equal(2, done.OkCount);
        Assert.Equal(1, done.Failed);
        Assert.True(done.Committed);
        Assert.Null(done.DevOk);
    }

    [Fact]
    public void Log_and_frame_events()
    {
        var l = Assert.IsType<LogMessage>(ProtocolParser.Parse("""{"t":"log","lvl":"warn","ms":123456,"src":"link","msg":"T2 online ch6"}"""));
        Assert.Equal(LogLevel.Warn, l.Level);
        Assert.Equal("link", l.Src);
        Assert.Equal("T2 online ch6", l.Text);

        var f = Assert.IsType<FrameMessage>(ProtocolParser.Parse("""{"t":"frame","dir":"rx","ms":123456,"mac":"AA:BB:CC:DD:EE:01","kind":"STATUS","len":46,"rssi":-60,"seq":17,"hex":"F6 02 01"}"""));
        Assert.Equal("rx", f.Dir);
        Assert.Equal("STATUS", f.Kind);
        Assert.Equal(46, f.Len);
        Assert.Equal("F6 02 01", f.Hex);
    }

    [Fact]
    public void Settings_response_is_parsed_with_dbm_conversion()
    {
        var res = Assert.IsType<ResponseMessage>(ProtocolParser.Parse("""{"t":"res","id":2,"ok":true,"settings":{"txPower":80,"brightness":200,"autoOff":60,"rssiLow":-90,"rssiHigh":-50,"batCal":[3300,3750,4150],"net":1,"unknown":true}}"""));
        var s = Assert.IsType<RemoteSettings>(res.Settings);
        Assert.Equal(80, s.TxPower);
        Assert.Equal(20.0, s.TxPowerDbm);
        Assert.Equal(200, s.Brightness);
        Assert.Equal(-90, s.RssiLow);
        Assert.Equal(new[] { 3300, 3750, 4150 }, s.BatCal);
        Assert.Null(s.StartPhase);
    }

    [Fact]
    public void Timer_settings_are_parsed()
    {
        var res = Assert.IsType<ResponseMessage>(ProtocolParser.Parse("""{"t":"res","id":2,"ok":true,"settings":{"startPhase":"on","name":"Fog9","ton":1.5,"toff":10,"led":true}}"""));
        var s = res.Settings!;
        Assert.Equal("on", s.StartPhase);
        Assert.Equal("Fog9", s.Name);
        Assert.Equal(1.5, s.Ton);
        Assert.Equal(true, s.Led);
        Assert.Null(s.TxPower);
    }

    // ---- performance counters -------------------------------------------------------------

    private const string StatsJson = """
        {"t":"stats","ms":123456,
         "self":{"kind":"remote","up":1234,"boots":12,"heap":181000,"heapMin":150000,"loopMaxUs":8200,"loopsPerS":820,
                 "rstReason":"deepsleep",
                 "rst":{"poweron":3,"brownout":0,"wdt":0,"panic":0,"sw":5,"deepsleep":40,"other":0},
                 "radio":{"ch":6,"tx":1200,"txOk":1180,"txFail":20,"txWd":0,"txDrop":0,"rx":3400,"rxBad":2,"rxForeign":0,"rxDrop":0},
                 "chan":[{"ch":6,"tx":1100,"fail":15,"rx":3300},{"ch":1,"tx":12,"fail":2,"rx":0}],
                 "link":{"cmds":40,"ok":38,"timeout":2,"rejected":0,"retries":11,"rttAvg":9.4,"rttMax":88,
                         "sweeps":3,"recovers":1,"found":1,"migrations":0},
                 "pc":{"linesRx":120,"linesTx":4400,"badJson":0,"txDrop":0}}}
        """;

    private const string DevStatJson = """
        {"t":"devStat","ms":123456,"i":0,"mac":"AA:BB:CC:DD:EE:01",
         "link":{"rx":3400,"expected":3520,"lost":120,"loss":3,"dups":0,"macOk":900,"macFail":12,
                 "cmds":20,"cmdsOk":19,"cmdsTimeout":1,"retries":5,"rttAvg":8.1,"rttMax":60,
                 "rssiMin":-80,"rssiAvg":-61,"rssiMax":-50,"offlineEvents":3,"offlineMs":95000,"reboots":2},
         "timer":{"age":2300,"fw":"2.0.0","up":3600,"boots":7,"rstReason":"poweron",
                  "heap":200000,"heapMin":180000,"loopMaxUs":3100,"ch":6,"net":1,
                  "rst":{"poweron":6,"brownout":1,"wdt":0,"panic":0,"sw":0,"other":0},
                  "rx":820,"rxBad":0,"rxForeign":2,"rxDrop":0,"rxProbes":10,"rxCmds":14,
                  "tx":3700,"txFail":3,"txDrop":0,"beats":3600,"replies":24,
                  "cmdsRun":14,"cmdsDup":3,"cmdsRejected":0,
                  "rssiMin":-85,"rssiAvg":-60,"rssiMax":-48,"rssiLast":-58}}
        """;

    private static string OneLine(string json) => json.ReplaceLineEndings("");

    [Fact]
    public void Stats_event_carries_only_the_device_itself()
    {
        var m = ProtocolParser.Parse(OneLine(StatsJson));
        var s = Assert.IsType<StatsMessage>(m).Snapshot;
        Assert.Equal(123456, s.Ms);
        var self = s.Self;
        Assert.Equal(DeviceKind.Remote, self.Kind);
        Assert.Equal(1234, self.UpSeconds);
        Assert.Equal(12, self.Boots);
        Assert.Equal(150000, self.HeapMin);
        Assert.Equal(8200, self.LoopMaxUs);
        Assert.Equal(820, self.LoopsPerS);
        Assert.Equal("deepsleep", self.RstReason);
        Assert.Equal(40, self.Rst.DeepSleep);
        Assert.Equal(5, self.Rst.Sw);
        Assert.Equal(1200, self.Radio.Tx);
        Assert.Equal(20, self.Radio.TxFail);
        Assert.Equal(3400, self.Radio.Rx);
        Assert.Equal(2, self.Chan.Count);
        Assert.Equal(new ChannelCounters(1, 12, 2, 0), self.Chan[1]);
        Assert.Equal(40, self.Link.Cmds);
        Assert.Equal(9.4, self.Link.RttAvg);
        Assert.Equal(3, self.Link.Sweeps);
        Assert.Equal(4400, self.Pc.LinesTx);
        Assert.Empty(s.Devs);
        Assert.Null(self.Timer);
    }

    [Fact]
    public void DevStat_event_carries_link_and_timer_counters_of_one_timer()
    {
        var d = Assert.IsType<DevStatMessage>(ProtocolParser.Parse(OneLine(DevStatJson))).Entry;
        Assert.Equal(0, d.Index);
        Assert.Equal("AA:BB:CC:DD:EE:01", d.Mac);
        Assert.Equal(123456, d.Ms);
        Assert.Equal(120, d.Link!.Lost);
        Assert.Equal(3, d.Link.Loss);
        Assert.Equal(12, d.Link.MacFail);
        Assert.InRange(d.Link.MacFailPercent!.Value, 1.31, 1.34);
        Assert.Equal(95000, d.Link.OfflineMs);
        Assert.Equal(2, d.Link.Reboots);
        var t = d.Timer!;
        Assert.Equal(2300, t.AgeMs);
        Assert.Equal(1, t.Rst.Brownout);
        Assert.Equal(3, t.CmdsDup);
        Assert.Equal(-58, t.RssiLast);
        Assert.Equal(3100, t.LoopMaxUs);
        Assert.Equal(820, t.Rx);
        Assert.Equal(3600, t.Beats);
    }

    [Fact]
    public void DevStat_with_a_null_timer_is_a_timer_that_has_not_answered_yet()
    {
        var d = Assert.IsType<DevStatMessage>(ProtocolParser.Parse("""{"t":"devStat","ms":5,"i":3,"mac":"aa:bb:cc:dd:ee:04","link":{"rx":10,"expected":10},"timer":null}""")).Entry;
        Assert.Equal(3, d.Index);
        Assert.Equal("AA:BB:CC:DD:EE:04", d.Mac);
        Assert.NotNull(d.Link);
        Assert.Null(d.Timer);
    }

    [Fact]
    public void Snapshots_can_be_completed_with_the_devStat_events()
    {
        var s = Assert.IsType<StatsMessage>(ProtocolParser.Parse(OneLine(StatsJson))).Snapshot;
        var d = Assert.IsType<DevStatMessage>(ProtocolParser.Parse(OneLine(DevStatJson))).Entry;
        var full = s.WithDevs(new[] { d });
        Assert.Single(full.Devs);
        Assert.Empty(s.Devs);                 // immutable
        Assert.Equal(s.Ms, full.Ms);
    }

    [Fact]
    public void Get_stats_response_carries_the_self_counters_under_stats()
    {
        var self = JsonNode.Parse(OneLine(StatsJson))!["self"]!.ToJsonString();
        var res = Assert.IsType<ResponseMessage>(ProtocolParser.Parse("{\"t\":\"res\",\"id\":7,\"ok\":true,\"stats\":" + self + "}"));
        Assert.NotNull(res.Stats);
        Assert.Equal(1200, res.Stats!.Self.Radio.Tx);
        Assert.Equal(12, res.Stats.Self.Boots);
        Assert.Equal(2, res.Stats.Self.Chan.Count);
        Assert.Empty(res.Stats.Devs);
    }

    [Fact]
    public void Get_stats_response_with_a_whole_stats_event_object_is_also_accepted()
    {
        var res = Assert.IsType<ResponseMessage>(ProtocolParser.Parse("{\"t\":\"res\",\"id\":7,\"ok\":true,\"stats\":" + OneLine(StatsJson) + "}"));
        Assert.Equal(12, res.Stats!.Self.Boots);
        Assert.Equal(123456, res.Stats.Ms);
    }

    [Fact]
    public void DevStats_response_carries_a_devStat_object_with_or_without_timer_data()
    {
        var ok = Assert.IsType<ResponseMessage>(ProtocolParser.Parse("{\"t\":\"res\",\"id\":8,\"ok\":true,\"devStat\":" + OneLine(DevStatJson) + "}"));
        Assert.NotNull(ok.DevStat);
        Assert.Equal(3, ok.DevStat!.Timer!.CmdsDup);
        Assert.Equal("AA:BB:CC:DD:EE:01", ok.DevStat.Mac);

        var none = Assert.IsType<ResponseMessage>(ProtocolParser.Parse("""{"t":"res","id":8,"ok":true,"devStat":{"t":"devStat","ms":9,"i":0,"mac":"AA:BB:CC:DD:EE:01","link":{"rx":1},"timer":null}}"""));
        Assert.NotNull(none.DevStat);
        Assert.Null(none.DevStat!.Timer);
        Assert.NotNull(none.DevStat.Link);

        Assert.Null(Assert.IsType<ResponseMessage>(ProtocolParser.Parse("""{"t":"res","id":8,"ok":false,"err":"noDevice"}""")).DevStat);
    }

    [Fact]
    public void Direct_timer_stats_carry_the_timer_counters_in_self_timer()
    {
        var m = ProtocolParser.Parse("""{"t":"stats","ms":5,"self":{"kind":"timer","up":3,"boots":2,"heap":1,"heapMin":1,"loopMaxUs":10,"loopsPerS":900,"rstReason":"poweron","rst":{"poweron":2},"radio":{"ch":6,"tx":1},"timer":{"fw":"2.0.0","up":3,"boots":2,"rx":10,"tx":20,"beats":5,"cmdsDup":1}}}""");
        var s = Assert.IsType<StatsMessage>(m).Snapshot;
        Assert.Equal(DeviceKind.Timer, s.Self.Kind);
        Assert.Empty(s.Devs);
        Assert.Equal(2, s.Self.Rst.PowerOn);
        var t = Assert.IsType<TimerStats>(s.Self.Timer);
        Assert.Equal(10, t.Rx);
        Assert.Equal(20, t.Tx);
        Assert.Equal(1, t.CmdsDup);
        Assert.Equal(0, t.AgeMs);
    }

    // ---- everything that is not a normal message ------------------------------------------

    [Fact]
    public void Unknown_message_types_are_kept()
    {
        var u = Assert.IsType<UnknownMessage>(ProtocolParser.Parse("""{"t":"somethingNew","x":1}"""));
        Assert.Equal("somethingNew", u.Type);
        Assert.Equal(1, u.Body.GetProperty("x").GetInt32());
    }

    [Fact]
    public void Object_without_type_is_unknown()
    {
        var u = Assert.IsType<UnknownMessage>(ProtocolParser.Parse("""{"x":1}"""));
        Assert.Equal("", u.Type);
    }

    [Theory]
    [InlineData("ESP-ROM:esp32c3-api1-20210207")]
    [InlineData("I (123) wifi: init")]
    [InlineData("   plain text with leading spaces")]
    [InlineData("[1,2,3]")]
    [InlineData("42")]
    public void Plain_text_lines_are_not_json(string line)
    {
        var p = Assert.IsType<PlainTextMessage>(ProtocolParser.Parse(line));
        Assert.False(p.MalformedJson);
        Assert.Equal(line, p.Text);
    }

    [Theory]
    [InlineData("{")]
    [InlineData("{\"t\":")]
    [InlineData("{\"t\":\"fleet\",,}")]
    [InlineData("{\"t\":\"fleet\",\"devs\":[{\"i\":0}")]
    [InlineData("{not json}")]
    public void Malformed_json_never_throws(string line)
    {
        var p = Assert.IsType<PlainTextMessage>(ProtocolParser.Parse(line));
        Assert.True(p.MalformedJson);
    }

    [Fact]
    public void Empty_and_whitespace_lines_are_plain_text()
    {
        Assert.IsType<PlainTextMessage>(ProtocolParser.Parse(""));
        Assert.IsType<PlainTextMessage>(ProtocolParser.Parse("   "));
    }

    [Fact]
    public void Huge_numbers_and_wrong_types_do_not_throw()
    {
        var m = ProtocolParser.Parse("""{"t":"fleet","devs":[{"rssi":1e99,"age":"abc","ton":null,"el":[1],"up":99999999999999,"i":"x"}]}""");
        var f = Assert.IsType<FleetMessage>(m);
        Assert.Single(f.Devs);
        Assert.Equal(99999999999999, f.Devs[0].UpSeconds);
    }
}
