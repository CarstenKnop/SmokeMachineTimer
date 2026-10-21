using System.IO;
using System.Text.Json.Nodes;
using FogConsole.Transport;

namespace FogConsole.Tests;

/// <summary>A scriptable transport: records what the hub writes, lets the test inject lines and drop the connection.</summary>
public sealed class FakeTransport : ITransport
{
    private readonly object _lock = new();
    private int _closed;

    public FakeTransport(string name = "FAKE")
    {
        Name = name;
    }

    public string Name { get; }
    public bool IsOpen { get; private set; }
    public bool FailOpen { get; set; }
    public List<string> Written { get; } = new();

    /// <summary>Called for every written line (already parsed). Return reply lines (or null).</summary>
    public Func<JsonObject, IEnumerable<string>?>? Responder { get; set; }

    public event Action<string>? LineReceived;
    public event Action<int>? LineOverflow;
    public event Action<TransportClosedInfo>? Closed;

    public Task OpenAsync(CancellationToken ct = default)
    {
        if (FailOpen) throw new IOException("The port does not exist");
        IsOpen = true;
        return Task.CompletedTask;
    }

    public Task CloseAsync()
    {
        IsOpen = false;
        RaiseClosed(false, "closed");
        return Task.CompletedTask;
    }

    public Task WriteLineAsync(string line, CancellationToken ct = default)
    {
        if (!IsOpen) throw new IOException("Port is not open");
        lock (_lock) Written.Add(line);
        var responder = Responder;
        if (responder is not null && JsonNode.Parse(line) is JsonObject req)
        {
            var replies = responder(req);
            if (replies is not null)
                foreach (var r in replies) Feed(r);
        }
        return Task.CompletedTask;
    }

    /// <summary>Injects a line as if the device had sent it.</summary>
    public void Feed(string line) => LineReceived?.Invoke(line);

    public void FeedOversize(int bytes) => LineOverflow?.Invoke(bytes);

    /// <summary>The device vanished (USB unplugged / re-enumeration).</summary>
    public void Vanish(string reason = "port lost")
    {
        IsOpen = false;
        RaiseClosed(true, reason);
    }

    private void RaiseClosed(bool unexpected, string reason)
    {
        if (Interlocked.Exchange(ref _closed, 1) == 0)
            Closed?.Invoke(new TransportClosedInfo(unexpected, reason));
    }

    public List<JsonObject> WrittenObjects()
    {
        lock (_lock) return Written.Select(l => JsonNode.Parse(l)!.AsObject()).ToList();
    }

    public List<JsonObject> WrittenOfType(string type) =>
        WrittenObjects().Where(o => o["t"]?.GetValue<string>() == type).ToList();

    public ValueTask DisposeAsync() => ValueTask.CompletedTask;

    public static string HelloReply(int id, string kind = "remote") =>
        $$"""{"t":"res","id":{{id}},"ok":true,"kind":"{{kind}}","fw":"2.0.0","proto":2,"mac":"AA:BB:CC:00:11:22","name":"Remote","ch":6,"net":1,"upMs":1000,"rst":"poweron","boots":1,"heap":180000,"build":"x","maxDevs":10}""";

    public static string Ok(int id, string extra = "") =>
        "{\"t\":\"res\",\"id\":" + id + ",\"ok\":true" + (extra.Length > 0 ? "," + extra : "") + "}";

    /// <summary>A responder that answers hello and ping like a healthy device and ignores everything else.</summary>
    public static Func<JsonObject, IEnumerable<string>?> Healthy(Func<JsonObject, IEnumerable<string>?>? next = null) => req =>
    {
        int id = req["id"]!.GetValue<int>();
        switch (req["t"]!.GetValue<string>())
        {
            case "hello": return new[] { HelloReply(id) };
            case "ping": return new[] { Ok(id, "\"ms\":5000") };
            default: return next?.Invoke(req);
        }
    };
}
