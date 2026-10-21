using System.Text.Json.Nodes;
using FogConsole.Protocol;
using FogConsole.Simulator;

namespace FogConsole.Tests;

/// <summary>Drives a <see cref="SimDevice"/> in virtual time and collects everything it sends.</summary>
public sealed class SimHarness
{
    private int _id;
    private int _pingId = 1_000_000;
    private double _sincePing;

    public SimHarness(SimOptions? options = null)
    {
        Dev = new SimDevice(options ?? Quiet());
    }

    /// <summary>No random power cuts, no scripted demo: fully predictable.</summary>
    public static SimOptions Quiet(int seed = 1, int paired = 7, SimKind kind = SimKind.Remote) =>
        new() { Seed = seed, Chaos = false, Demo = false, PairedTimers = paired, Kind = kind };

    public SimDevice Dev { get; }
    public List<Message> Received { get; } = new();
    public List<string> RawLines { get; } = new();

    public IEnumerable<T> Of<T>() where T : Message => Received.OfType<T>();

    public void Pump()
    {
        foreach (var line in Dev.DrainOutput())
        {
            RawLines.Add(line);
            Received.Add(ProtocolParser.Parse(line));
        }
    }

    /// <summary>Sends a request, lets the device process it and returns its response (null when none arrived yet).</summary>
    public ResponseMessage? Request(JsonObject req, double advanceMs = 0)
    {
        int id = ++_id;
        Dev.HandleLine(Requests.Serialize(req, id));
        if (advanceMs > 0) Advance(advanceMs);
        else Pump();
        return Received.OfType<ResponseMessage>().LastOrDefault(r => r.Id == id);
    }

    public ResponseMessage Ok(JsonObject req, double advanceMs = 0)
    {
        var r = Request(req, advanceMs);
        if (r is null) throw new InvalidOperationException("no response");
        if (!r.Ok) throw new InvalidOperationException("request failed: " + r.Describe());
        return r;
    }

    public int LastRequestId => _id;

    /// <summary><c>get stats</c>: the self counters from the response plus the devStat events that follow it.</summary>
    public StatsSnapshot GetStats()
    {
        int start = Received.Count;
        var res = Ok(Requests.GetStats());
        var devs = Received.Skip(start).OfType<DevStatMessage>().Select(d => d.Entry).ToList();
        return res.Stats!.WithDevs(devs);
    }

    /// <summary>The last streamed <c>stats</c> event completed with the <c>devStat</c> events that followed it.</summary>
    public StatsSnapshot LatestStats()
    {
        int idx = Received.FindLastIndex(m => m is StatsMessage);
        var snap = ((StatsMessage)Received[idx]).Snapshot;
        var devs = new List<DevStatsEntry>();
        for (int i = idx + 1; i < Received.Count && Received[i] is not StatsMessage; i++)
            if (Received[i] is DevStatMessage d) devs.Add(d.Entry);
        return snap.WithDevs(devs);
    }

    /// <summary>Sends a cmd and returns the id (the response is in <see cref="Received"/>).</summary>
    public ResponseMessage Cmd(CommandSpec spec, double advanceMs = 0) => Ok(Requests.Cmd(spec), advanceMs);

    /// <summary>Advances virtual time in 100 ms slices, sending a keep-alive ping every 2 s like the real hub.</summary>
    public void Advance(double ms)
    {
        double left = ms;
        while (left > 0)
        {
            double slice = Math.Min(100, left);
            Dev.Advance(TimeSpan.FromMilliseconds(slice));
            left -= slice;
            _sincePing += slice;
            if (_sincePing >= 2000)
            {
                _sincePing = 0;
                Dev.HandleLine(Requests.Serialize(Requests.Ping(), ++_pingId));
            }
            Pump();
        }
    }

    public FleetDevice Fleet(int index)
    {
        var f = Of<FleetMessage>().Last();
        return f.Devs.Single(d => d.Index == index);
    }

    public FleetMessage LastFleet => Of<FleetMessage>().Last();

    public void Clear()
    {
        Received.Clear();
        RawLines.Clear();
    }

    /// <summary>Starts a PC session (first valid line) and streams for a moment.</summary>
    public void StartSession()
    {
        Ok(Requests.HelloReq());
        Advance(600);
    }
}
