using System.IO;
using System.Text.Json.Nodes;
using FogConsole.Hub;
using FogConsole.Protocol;
using FogConsole.Transport;
using Xunit;

namespace FogConsole.Tests;

public class HubClientTests
{
    private static HubOptions Fast(bool keepAlive = false) => new()
    {
        RequestTimeout = TimeSpan.FromMilliseconds(200),
        DevStatsTimeout = TimeSpan.FromMilliseconds(900),
        CommandTimeout = TimeSpan.FromMilliseconds(500),
        PingInterval = TimeSpan.FromMilliseconds(60),
        ReconnectInterval = TimeSpan.FromMilliseconds(40),
        HelloAttempts = 2,
        PingFailuresBeforeReconnect = 3,
        KeepAlive = keepAlive,
    };

    private static FakeTransport Healthy(Func<JsonObject, IEnumerable<string>?>? next = null) =>
        new() { Responder = FakeTransport.Healthy(next) };

    private static async Task<(HubClient Hub, FakeTransport Transport)> Connected(Func<JsonObject, IEnumerable<string>?>? next = null, bool keepAlive = false)
    {
        var hub = new HubClient(Fast(keepAlive));
        var t = Healthy(next);
        await hub.ConnectAsync(() => t);
        return (hub, t);
    }

    private static async Task WaitFor(Func<bool> condition, int timeoutMs = 3000)
    {
        var sw = System.Diagnostics.Stopwatch.StartNew();
        while (!condition())
        {
            if (sw.ElapsedMilliseconds > timeoutMs) throw new TimeoutException("condition not met in time");
            await Task.Delay(10);
        }
    }

    private static int IdOf(JsonObject o) => o["id"]!.GetValue<int>();

    // ---- connection -----------------------------------------------------------------------

    [Fact]
    public async Task Connect_performs_hello_and_raises_state_changes()
    {
        var hub = new HubClient(Fast());
        var changes = new List<HubStateChange>();
        hub.StateChanged += changes.Add;
        HelloInfo? seen = null;
        hub.Connected += h => seen = h;

        await hub.ConnectAsync(() => Healthy());

        Assert.Equal(HubState.Connected, hub.State);
        Assert.Equal(DeviceKind.Remote, hub.Hello!.Kind);
        Assert.Same(hub.Hello, seen);
        Assert.Contains(changes, c => c.New == HubState.Connecting);
        Assert.Equal(HubState.Connected, changes[^1].New);
        await hub.DisconnectAsync();
        Assert.Equal(HubState.Disconnected, hub.State);
    }

    [Fact]
    public async Task Connect_fails_when_the_device_does_not_answer_hello()
    {
        var hub = new HubClient(Fast());
        var t = new FakeTransport(); // no responder: silent
        var ex = await Assert.ThrowsAsync<HubException>(() => hub.ConnectAsync(() => t));
        Assert.Contains("hello", ex.Message);
        Assert.Equal(HubState.Disconnected, hub.State);
        Assert.False(t.IsOpen);
        Assert.Equal(2, t.WrittenOfType("hello").Count); // HelloAttempts
    }

    [Fact]
    public async Task Connect_fails_when_the_port_cannot_be_opened()
    {
        var hub = new HubClient(Fast());
        await Assert.ThrowsAsync<IOException>(() => hub.ConnectAsync(() => new FakeTransport { FailOpen = true }));
        Assert.Equal(HubState.Disconnected, hub.State);
        Assert.NotNull(hub.LastError);
    }

    [Fact]
    public async Task Connect_to_an_absent_port_can_wait_for_the_device()
    {
        var hub = new HubClient(Fast());
        var good = Healthy();
        int attempts = 0;
        ITransport Factory() => ++attempts < 4 ? new FakeTransport { FailOpen = true } : good;

        await hub.ConnectAsync(Factory, waitIfAbsent: true);
        Assert.Equal(HubState.WaitingForDevice, hub.State);
        await WaitFor(() => hub.State == HubState.Connected);
        Assert.True(attempts >= 4);
        await hub.DisconnectAsync();
    }

    [Fact]
    public async Task An_error_answer_to_hello_fails_the_connection_immediately()
    {
        var hub = new HubClient(Fast());
        var t = new FakeTransport
        {
            Responder = req => new[] { $$"""{"t":"res","id":{{IdOf(req)}},"ok":false,"err":"notReady","msg":"booting"}""" },
        };
        var ex = await Assert.ThrowsAsync<HubException>(() => hub.ConnectAsync(() => t));
        Assert.Contains("notReady", ex.Message);
        Assert.Single(t.WrittenOfType("hello"));
    }

    // ---- request / response ---------------------------------------------------------------

    [Fact]
    public async Task Responses_are_matched_by_id_even_when_they_arrive_out_of_order()
    {
        var (hub, t) = await Connected();
        var a = hub.RequestAsync(Requests.GetSettings());
        var b = hub.RequestAsync(Requests.GetFleet());
        await WaitFor(() => t.WrittenOfType("get").Count == 2);
        var gets = t.WrittenOfType("get");
        int idA = IdOf(gets.First(g => g["what"]!.GetValue<string>() == "settings"));
        int idB = IdOf(gets.First(g => g["what"]!.GetValue<string>() == "fleet"));
        Assert.NotEqual(idA, idB);

        t.Feed(FakeTransport.Ok(idB, "\"tag\":\"B\""));
        t.Feed(FakeTransport.Ok(idA, "\"settings\":{\"brightness\":123}"));

        var ra = await a;
        var rb = await b;
        Assert.Equal(123, ra.Settings!.Brightness);
        Assert.Equal("B", rb.Body.GetProperty("tag").GetString());
        await hub.DisconnectAsync();
    }

    [Fact]
    public async Task A_response_with_an_unknown_id_is_ignored_and_counted()
    {
        var (hub, t) = await Connected();
        var events = new List<Message>();
        hub.MessageReceived += events.Add;
        t.Feed(FakeTransport.Ok(424242));
        Assert.Equal(1, hub.Counters.UnmatchedResponses);
        Assert.Empty(events); // responses are not events
        Assert.Equal(HubState.Connected, hub.State);
        await hub.DisconnectAsync();
    }

    [Fact]
    public async Task A_request_times_out_with_HubTimeoutException()
    {
        var (hub, _) = await Connected();
        var sw = System.Diagnostics.Stopwatch.StartNew();
        var ex = await Assert.ThrowsAsync<HubTimeoutException>(() => hub.RequestAsync(Requests.GetSettings()));
        Assert.Equal("get", ex.RequestType);
        Assert.InRange(sw.ElapsedMilliseconds, 150, 1500);
        Assert.Equal(1, hub.Counters.Timeouts);
        await hub.DisconnectAsync();
    }

    [Fact]
    public async Task A_late_response_after_a_timeout_is_just_unmatched()
    {
        var (hub, t) = await Connected();
        await Assert.ThrowsAsync<HubTimeoutException>(() => hub.RequestAsync(Requests.GetSettings()));
        int id = IdOf(t.WrittenOfType("get").Last());
        t.Feed(FakeTransport.Ok(id));
        Assert.Equal(1, hub.Counters.UnmatchedResponses);
        await hub.DisconnectAsync();
    }

    [Fact]
    public async Task Idempotent_requests_can_be_retried_with_a_fresh_id()
    {
        int gets = 0;
        var (hub, t) = await Connected(req =>
        {
            if (req["t"]!.GetValue<string>() != "get") return null;
            return ++gets == 1 ? null : new[] { FakeTransport.Ok(IdOf(req), "\"settings\":{\"autoOff\":5}") };
        });
        var res = await hub.RequestAsync(Requests.GetSettings(), retries: 1);
        Assert.Equal(5, res.Settings!.AutoOff);
        var sent = t.WrittenOfType("get");
        Assert.Equal(2, sent.Count);
        Assert.NotEqual(IdOf(sent[0]), IdOf(sent[1]));
        await hub.DisconnectAsync();
    }

    [Fact]
    public async Task Requests_without_retries_are_sent_once()
    {
        var (hub, t) = await Connected();
        await Assert.ThrowsAsync<HubTimeoutException>(() => hub.RequestAsync(Requests.Unpair(0)));
        Assert.Single(t.WrittenOfType("unpair"));
        await hub.DisconnectAsync();
    }

    [Fact]
    public async Task An_error_response_is_returned_and_RequestOk_throws()
    {
        var (hub, _) = await Connected(req => new[] { $$"""{"t":"res","id":{{IdOf(req)}},"ok":false,"err":"badArg","msg":"nope"}""" });
        var res = await hub.RequestAsync(Requests.Select(9));
        Assert.False(res.Ok);
        Assert.Equal("badArg", res.Err);
        var ex = await Assert.ThrowsAsync<HubRequestException>(() => hub.RequestOkAsync(Requests.Select(9)));
        Assert.Equal("badArg", ex.Response.Err);
        await hub.DisconnectAsync();
    }

    [Fact]
    public async Task A_request_can_be_cancelled_by_the_caller()
    {
        var (hub, _) = await Connected();
        using var cts = new CancellationTokenSource(50);
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => hub.RequestAsync(Requests.GetSettings(), timeout: TimeSpan.FromSeconds(5), ct: cts.Token));
        await hub.DisconnectAsync();
    }

    [Fact]
    public async Task Pending_requests_fail_when_the_port_vanishes()
    {
        var hub = new HubClient(Fast()) { AutoReconnect = false };
        var t = Healthy();
        await hub.ConnectAsync(() => t);
        var pending = hub.RequestAsync(Requests.GetSettings(), timeout: TimeSpan.FromSeconds(10));
        await WaitFor(() => t.WrittenOfType("get").Count == 1);

        t.Vanish("USB unplugged");

        await Assert.ThrowsAsync<HubDisconnectedException>(() => pending);
        await WaitFor(() => hub.State == HubState.Disconnected);
        Assert.Contains("USB unplugged", hub.LastError);
    }

    [Fact]
    public async Task Pending_requests_are_cancelled_by_Disconnect()
    {
        var (hub, t) = await Connected();
        var pending = hub.RequestAsync(Requests.GetSettings(), timeout: TimeSpan.FromSeconds(10));
        await WaitFor(() => t.WrittenOfType("get").Count == 1);
        await hub.DisconnectAsync();
        await Assert.ThrowsAsync<HubDisconnectedException>(() => pending);
        await Assert.ThrowsAsync<HubDisconnectedException>(() => hub.RequestAsync(Requests.Ping()));
    }

    [Fact]
    public async Task Requests_while_disconnected_fail_fast()
    {
        var hub = new HubClient(Fast());
        await Assert.ThrowsAsync<HubDisconnectedException>(() => hub.RequestAsync(Requests.Ping()));
        Assert.Throws<HubDisconnectedException>(() => hub.SendRawAsync("{}").GetAwaiter().GetResult());
    }

    [Fact]
    public async Task Ids_are_unique_among_in_flight_requests()
    {
        var (hub, t) = await Connected();
        var tasks = Enumerable.Range(0, 30).Select(_ => hub.RequestAsync(Requests.GetFleet(), timeout: TimeSpan.FromMilliseconds(150))).ToList();
        foreach (var task in tasks) await Assert.ThrowsAsync<HubTimeoutException>(() => task);
        var ids = t.WrittenOfType("get").Select(IdOf).ToList();
        Assert.Equal(30, ids.Distinct().Count());
        Assert.All(ids, id => Assert.True(id >= 1));
        await hub.DisconnectAsync();
    }

    [Fact]
    public async Task DevStats_uses_the_longer_timeout()
    {
        Assert.Equal(TimeSpan.FromSeconds(5), new HubOptions().DevStatsTimeout);
        Assert.Equal(TimeSpan.FromSeconds(1.5), new HubOptions().RequestTimeout);
        Assert.Equal(TimeSpan.FromSeconds(4), new HubOptions().CommandTimeout);
        Assert.Equal(TimeSpan.FromSeconds(2), new HubOptions().PingInterval);

        FakeTransport? tr = null;
        var (hub, t) = await Connected(req =>
        {
            string type = req["t"]!.GetValue<string>();
            if (type is "get")
            {
                int id = IdOf(req);
                string what = req["what"]!.GetValue<string>();
                _ = Task.Run(async () =>
                {
                    await Task.Delay(450); // longer than RequestTimeout (200 ms), shorter than DevStatsTimeout (900 ms)
                    tr!.Feed(FakeTransport.Ok(id, what == "devStats" ? "\"devStat\":{\"t\":\"devStat\",\"ms\":1,\"i\":0,\"mac\":\"AA:BB:CC:DD:EE:01\",\"link\":{\"rx\":1},\"timer\":null}" : ""));
                });
            }
            return null;
        });
        tr = t;
        var dev = await hub.RequestAsync(Requests.GetDevStats(CommandTarget.Dev(0)));
        Assert.True(dev.Ok);
        Assert.NotNull(dev.DevStat);
        Assert.Null(dev.DevStat!.Timer);
        await Assert.ThrowsAsync<HubTimeoutException>(() => hub.RequestAsync(Requests.GetSettings()));
        await hub.DisconnectAsync();
    }

    // ---- commands -------------------------------------------------------------------------

    private static string CmdResult(int id, int dev, string result = "ok", string op = "setTimes") =>
        $$"""{"t":"cmdResult","id":{{id}},"dev":{{dev}},"mac":"AA:BB:CC:DD:EE:0{{dev + 1}}","op":"{{op}}","result":"{{result}}","attempts":2,"ms":180}""";

    [Fact]
    public async Task SendCommand_completes_on_the_cmdResult_with_the_same_id()
    {
        FakeTransport? tr = null;
        var (hub, t) = await Connected(req =>
        {
            if (req["t"]!.GetValue<string>() != "cmd") return null;
            int id = IdOf(req);
            _ = Task.Run(async () => { await Task.Delay(30); tr!.Feed(CmdResult(id, 2)); });
            return new[] { FakeTransport.Ok(id, "\"queued\":1") };
        });
        tr = t;

        var changes = new List<CommandState>();
        hub.CommandChanged += h => changes.Add(h.State);
        var handle = await hub.SendCommandAsync(CommandSpec.SetTimes(CommandTarget.Dev(2), 1.5, 10));

        Assert.Equal(CommandState.Completed, handle.State);
        Assert.True(handle.AllOk);
        var r = Assert.Single(handle.Results);
        Assert.Equal(CmdOutcome.Ok, r.Result);
        Assert.Equal(2, r.Attempts);
        Assert.Equal(180, r.Ms);
        Assert.Equal(handle.Id, r.Id);
        Assert.Contains(CommandState.Queued, changes);
        Assert.Equal(CommandState.Completed, changes[^1]);
        Assert.Equal(handle.Id, IdOf(t.WrittenOfType("cmd").Single()));
        await hub.DisconnectAsync();
    }

    [Fact]
    public async Task A_command_to_all_waits_for_one_result_per_timer()
    {
        FakeTransport? tr = null;
        var (hub, t) = await Connected(req =>
        {
            if (req["t"]!.GetValue<string>() != "cmd") return null;
            int id = IdOf(req);
            _ = Task.Run(async () =>
            {
                for (int d = 0; d < 3; d++)
                {
                    await Task.Delay(25);
                    tr!.Feed(CmdResult(id, d, d == 1 ? "timeout" : "ok", "toggle"));
                }
            });
            return new[] { FakeTransport.Ok(id, "\"queued\":3") };
        });
        tr = t;

        var handle = await hub.SendCommandAsync(CommandSpec.Toggle(CommandTarget.All));

        Assert.Equal(CommandState.Completed, handle.State);
        Assert.Equal(3, handle.Results.Count);
        Assert.Equal(2, handle.OkCount);
        Assert.False(handle.AllOk);
        Assert.Equal(CmdOutcome.Timeout, handle.Results.Single(r => r.Dev == 1).Result);
        await hub.DisconnectAsync();
    }

    [Fact]
    public async Task A_command_queued_for_nobody_completes_immediately()
    {
        var (hub, _) = await Connected(req => req["t"]!.GetValue<string>() == "cmd" ? new[] { FakeTransport.Ok(IdOf(req), "\"queued\":0") } : null);
        var handle = await hub.SendCommandAsync(CommandSpec.Toggle(CommandTarget.All));
        Assert.Equal(CommandState.Completed, handle.State);
        Assert.Empty(handle.Results);
        await hub.DisconnectAsync();
    }

    [Fact]
    public async Task A_rejected_command_request_completes_with_the_error()
    {
        var (hub, _) = await Connected(req => req["t"]!.GetValue<string>() == "cmd"
            ? new[] { $$"""{"t":"res","id":{{IdOf(req)}},"ok":false,"err":"noDevice","msg":"no such timer"}""" }
            : null);
        var handle = await hub.SendCommandAsync(CommandSpec.Toggle(CommandTarget.Dev(9)));
        Assert.Equal(CommandState.Rejected, handle.State);
        Assert.Contains("noDevice", handle.Error);
        Assert.Contains("no such timer", handle.Error);
        await hub.DisconnectAsync();
    }

    [Fact]
    public async Task A_command_times_out_when_no_result_arrives_and_a_late_result_still_updates_it()
    {
        var (hub, t) = await Connected(req => req["t"]!.GetValue<string>() == "cmd" ? new[] { FakeTransport.Ok(IdOf(req), "\"queued\":1") } : null);
        var sw = System.Diagnostics.Stopwatch.StartNew();
        var handle = await hub.SendCommandAsync(CommandSpec.Toggle(CommandTarget.Dev(0)));

        Assert.Equal(CommandState.TimedOut, handle.State);
        Assert.InRange(sw.ElapsedMilliseconds, 400, 2000);
        Assert.Empty(handle.Results);
        Assert.Contains("0 of 1", handle.Error);

        t.Feed(CmdResult(handle.Id, 0, "ok", "toggle"));
        Assert.Single(handle.Results);
        Assert.Equal(CommandState.Completed, handle.State);
        await hub.DisconnectAsync();
    }

    [Fact]
    public async Task A_cmdResult_arriving_before_the_response_is_not_lost()
    {
        var (hub, _) = await Connected(req =>
        {
            if (req["t"]!.GetValue<string>() != "cmd") return null;
            int id = IdOf(req);
            return new[] { CmdResult(id, 0, "ok", "toggle"), FakeTransport.Ok(id, "\"queued\":1") };
        });
        var handle = await hub.SendCommandAsync(CommandSpec.Toggle(CommandTarget.Dev(0)));
        Assert.Equal(CommandState.Completed, handle.State);
        Assert.True(handle.AllOk);
        await hub.DisconnectAsync();
    }

    [Fact]
    public async Task A_cmdResult_for_an_unknown_command_is_still_published_as_an_event()
    {
        var (hub, t) = await Connected();
        var events = new List<Message>();
        hub.MessageReceived += events.Add;
        t.Feed(CmdResult(777, 1, "ok", "toggle"));
        var m = Assert.IsType<CmdResultMessage>(Assert.Single(events));
        Assert.Equal(777, m.Id);
        await hub.DisconnectAsync();
    }

    [Fact]
    public async Task Pending_commands_fail_when_the_connection_drops()
    {
        var hub = new HubClient(Fast()) { AutoReconnect = false };
        var t = Healthy(req => req["t"]!.GetValue<string>() == "cmd" ? new[] { FakeTransport.Ok(IdOf(req), "\"queued\":1") } : null);
        await hub.ConnectAsync(() => t);
        var handle = hub.BeginCommand(CommandSpec.Toggle(CommandTarget.Dev(0)), TimeSpan.FromSeconds(10));
        await WaitFor(() => handle.State == CommandState.Queued);
        t.Vanish();
        await handle.Completion.WaitAsync(TimeSpan.FromSeconds(2));
        Assert.Equal(CommandState.Failed, handle.State);
    }

    [Fact]
    public async Task A_command_while_disconnected_fails_without_throwing()
    {
        var hub = new HubClient(Fast());
        var handle = await hub.SendCommandAsync(CommandSpec.Toggle(CommandTarget.Dev(0)));
        Assert.Equal(CommandState.Failed, handle.State);
        Assert.NotNull(handle.Error);
    }

    // ---- keep-alive, events, framing noise ------------------------------------------------

    [Fact]
    public async Task Keep_alive_pings_are_sent_and_latency_is_measured()
    {
        var (hub, t) = await Connected(keepAlive: true);
        await WaitFor(() => hub.Latency.Samples >= 3);
        Assert.True(t.WrittenOfType("ping").Count >= 3);
        Assert.False(double.IsNaN(hub.Latency.LastMs));
        Assert.InRange(hub.Latency.AvgMs, 0, 100);
        Assert.True(hub.IsResponsive);
        await hub.DisconnectAsync();
    }

    [Fact]
    public async Task Events_are_dispatched_in_order_and_typed()
    {
        var (hub, t) = await Connected();
        var events = new List<Message>();
        hub.MessageReceived += events.Add;
        t.Feed("""{"t":"fleet","seq":1,"devs":[]}""");
        t.Feed("""{"t":"remote","bat":50}""");
        t.Feed("""{"t":"log","lvl":"info","ms":1,"src":"x","msg":"hi"}""");
        t.Feed("""{"t":"brandNewEvent","a":1}""");
        Assert.IsType<FleetMessage>(events[0]);
        Assert.IsType<RemoteMessage>(events[1]);
        Assert.IsType<LogMessage>(events[2]);
        Assert.IsType<UnknownMessage>(events[3]);
        await hub.DisconnectAsync();
    }

    [Fact]
    public async Task Plain_text_and_malformed_json_are_shown_but_do_not_disturb_the_protocol()
    {
        var (hub, t) = await Connected(req => req["t"]!.GetValue<string>() == "get" ? new[] { FakeTransport.Ok(IdOf(req), "\"settings\":{\"autoOff\":7}") } : null);
        var raw = new List<RawLine>();
        var msgs = new List<Message>();
        hub.RawLineSeen += raw.Add;
        hub.MessageReceived += msgs.Add;

        t.Feed("ESP-ROM:esp32c3-api1-20210207");
        t.Feed("{\"t\":\"fleet\",");
        var res = await hub.RequestAsync(Requests.GetSettings());
        t.Feed("I (123) boot: something");
        t.Feed("""{"t":"fleet","seq":3,"devs":[]}""");

        Assert.Equal(7, res.Settings!.AutoOff);
        Assert.Equal(1, hub.Counters.BadJson);
        Assert.Equal(2, hub.Counters.PlainText);
        Assert.Contains(raw, r => r.Kind == RawKind.PlainText && r.Text.StartsWith("ESP-ROM"));
        Assert.Contains(raw, r => r.Kind == RawKind.MalformedJson);
        Assert.Contains(raw, r => r.Direction == RawDirection.Tx && r.Text.Contains("\"get\""));
        Assert.Equal(2, msgs.OfType<PlainTextMessage>().Count(p => !p.MalformedJson));
        Assert.Single(msgs.OfType<FleetMessage>());
        await hub.DisconnectAsync();
    }

    [Fact]
    public async Task An_oversized_line_is_counted_and_shown()
    {
        var (hub, t) = await Connected();
        var raw = new List<RawLine>();
        hub.RawLineSeen += raw.Add;
        t.FeedOversize(5000);
        Assert.Equal(1, hub.Counters.Oversize);
        Assert.Contains(raw, r => r.Kind == RawKind.Oversize && r.Text.Contains("5000"));
        await hub.DisconnectAsync();
    }

    [Fact]
    public async Task Sending_a_raw_line_writes_it_verbatim()
    {
        var (hub, t) = await Connected();
        await hub.SendRawAsync("{\"t\":\"ping\",\"id\":99}");
        Assert.Contains("{\"t\":\"ping\",\"id\":99}", t.Written);
        Assert.True(hub.Counters.LinesTx >= 2);
        await hub.DisconnectAsync();
    }

    // ---- reconnection ---------------------------------------------------------------------

    [Fact]
    public async Task The_hub_reconnects_when_the_port_reappears()
    {
        var hub = new HubClient(Fast());
        var transports = new List<FakeTransport>();
        int connectedEvents = 0;
        hub.Connected += _ => connectedEvents++;
        bool deviceAsleep = false;
        ITransport Factory()
        {
            var t = new FakeTransport { Responder = FakeTransport.Healthy(), FailOpen = deviceAsleep };
            transports.Add(t);
            return t;
        }

        await hub.ConnectAsync(Factory);
        Assert.Equal(1, connectedEvents);

        deviceAsleep = true;
        transports[0].Vanish("device went to sleep");
        await WaitFor(() => hub.State == HubState.WaitingForDevice);
        Assert.Contains("sleep", hub.LastError);

        await Task.Delay(150); // several failed attempts
        Assert.Equal(HubState.WaitingForDevice, hub.State);
        Assert.True(transports.Count >= 3);

        deviceAsleep = false;
        await WaitFor(() => hub.State == HubState.Connected);
        Assert.Equal(2, connectedEvents);
        Assert.NotNull(hub.Hello);
        await hub.DisconnectAsync();
    }

    [Fact]
    public async Task The_hub_does_not_reconnect_after_a_user_disconnect()
    {
        var hub = new HubClient(Fast());
        int created = 0;
        await hub.ConnectAsync(() => { created++; return Healthy(); });
        await hub.DisconnectAsync();
        await Task.Delay(200);
        Assert.Equal(1, created);
        Assert.Equal(HubState.Disconnected, hub.State);
    }

    [Fact]
    public async Task A_dead_connection_is_detected_by_failing_pings_and_reopened()
    {
        var hub = new HubClient(Fast(keepAlive: true));
        int created = 0;
        bool firstIsMute = false;
        ITransport Factory()
        {
            bool mute = created++ == 0;
            return new FakeTransport
            {
                Responder = req =>
                {
                    var type = req["t"]!.GetValue<string>();
                    if (type == "hello") return new[] { FakeTransport.HelloReply(IdOf(req)) };
                    if (type == "ping" && mute && Volatile.Read(ref firstIsMute)) return null; // stale handle: no more answers
                    return type == "ping" ? new[] { FakeTransport.Ok(IdOf(req)) } : null;
                },
            };
        }
        await hub.ConnectAsync(Factory);
        await WaitFor(() => hub.Latency.Samples >= 1);

        Volatile.Write(ref firstIsMute, true);
        await WaitFor(() => created >= 2, 5000);
        await WaitFor(() => hub.State == HubState.Connected);
        await hub.DisconnectAsync();
    }

    [Fact]
    public async Task Without_auto_reconnect_a_lost_port_ends_in_Disconnected()
    {
        var hub = new HubClient(Fast()) { AutoReconnect = false };
        var t = Healthy();
        await hub.ConnectAsync(() => t);
        t.Vanish("gone");
        await WaitFor(() => hub.State == HubState.Disconnected);
        Assert.Equal("gone", hub.LastError);
    }
}
