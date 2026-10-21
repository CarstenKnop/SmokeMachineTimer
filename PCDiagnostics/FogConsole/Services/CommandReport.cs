using FogConsole.Hub;
using FogConsole.Protocol;

namespace FogConsole.Services;

public enum EntryStatus { Pending, Ok, Warning, Failed }

/// <summary>What happened to a command, in words a stage technician understands.</summary>
public sealed record CommandReportResult(EntryStatus Status, string Detail, string? Problem, int Attempts, int Ms);

public static class CommandReport
{
    /// <summary>Text for one failed per-timer result.</summary>
    public static string FailureText(CmdResultMessage r, string name) => r.Result switch
    {
        CmdOutcome.Timeout => $"{name} did not answer — it may be powered off while heating.",
        CmdOutcome.Rejected => $"{name} rejected \"{r.Op}\".",
        CmdOutcome.Cancelled => $"{name}: \"{r.Op}\" was cancelled (timer unpaired).",
        _ => $"{name}: unexpected result \"{r.ResultText}\" for \"{r.Op}\".",
    };

    private static string ShortFailure(CmdResultMessage r, string name) => r.Result switch
    {
        CmdOutcome.Timeout => $"{name} timed out",
        CmdOutcome.Rejected => $"{name} rejected it",
        CmdOutcome.Cancelled => $"{name} cancelled",
        _ => $"{name} {r.ResultText}",
    };

    public static CommandReportResult Describe(CommandHandle h, Func<CmdResultMessage, string> nameOf)
    {
        var results = h.Results;
        switch (h.State)
        {
            case CommandState.Sending:
            case CommandState.Queued:
                return new CommandReportResult(EntryStatus.Pending, h.State == CommandState.Sending ? "sending..." : "waiting for confirmation...", null, 0, 0);

            case CommandState.Rejected:
                return new CommandReportResult(EntryStatus.Failed, h.Error ?? "refused", $"\"{h.Spec.Op}\" was refused: {h.Error}", 0, 0);

            case CommandState.Failed:
                return new CommandReportResult(EntryStatus.Failed, h.Error ?? "not sent", $"\"{h.Spec.Op}\" was not sent: {h.Error ?? "not connected"}.", 0, 0);

            case CommandState.TimedOut when results.Count == 0:
                return new CommandReportResult(EntryStatus.Failed, "no confirmation", $"No confirmation for \"{h.Spec.Op}\": the remote did not report a result in time.", 0, 0);
        }

        if (results.Count == 0)
        {
            if (!h.Spec.Confirm)
                return new CommandReportResult(EntryStatus.Ok, "sent (no confirmation requested)", null, 1, 0);
            return new CommandReportResult(EntryStatus.Warning, "no timer was addressed", h.Spec.Target.IsAll ? "No timers are paired." : null, 0, 0);
        }

        int ok = results.Count(r => r.Result == CmdOutcome.Ok);
        int total = h.Expected > results.Count ? h.Expected : results.Count;
        var failures = results.Where(r => r.Result != CmdOutcome.Ok).ToList();
        int attempts = results.Max(r => r.Attempts);
        int ms = results.Max(r => r.Ms);

        if (failures.Count == 0 && ok == total)
        {
            string detail = total == 1
                ? $"confirmed in {ms} ms ({attempts} {(attempts == 1 ? "attempt" : "attempts")})"
                : $"{ok}/{total} confirmed";
            return new CommandReportResult(EntryStatus.Ok, detail, null, attempts, ms);
        }

        var parts = new List<string>();
        foreach (var f in failures.Take(3)) parts.Add(ShortFailure(f, nameOf(f)));
        if (failures.Count > 3) parts.Add($"{failures.Count - 3} more failed");
        if (results.Count < total) parts.Add($"{total - results.Count} result(s) missing");
        string summary = $"{ok}/{total} confirmed, {string.Join(", ", parts)}";

        string problem = failures.Count == 1 && total == 1
            ? FailureText(failures[0], nameOf(failures[0]))
            : summary;
        return new CommandReportResult(ok == 0 ? EntryStatus.Failed : EntryStatus.Warning, summary, problem, attempts, ms);
    }

    /// <summary>Aggregates several handles (a bulk action) into one sentence, e.g. "7/8 confirmed, Fog3 timed out".</summary>
    public static (EntryStatus Status, string Summary) Aggregate(IEnumerable<CommandHandle> handles, Func<CmdResultMessage, string> nameOf)
    {
        int ok = 0, total = 0;
        var problems = new List<string>();
        foreach (var h in handles)
        {
            switch (h.State)
            {
                case CommandState.Rejected:
                case CommandState.Failed:
                    total++;
                    problems.Add(h.Error ?? "failed");
                    continue;
                case CommandState.TimedOut when h.Results.Count == 0:
                    total += Math.Max(h.Expected, 1);
                    problems.Add("no answer from the remote");
                    continue;
            }
            var results = h.Results;
            total += Math.Max(h.Expected, results.Count);
            ok += results.Count(r => r.Result == CmdOutcome.Ok);
            foreach (var f in results.Where(r => r.Result != CmdOutcome.Ok))
                problems.Add(ShortFailure(f, nameOf(f)));
            if (h.Expected > results.Count) problems.Add($"{h.Expected - results.Count} no result");
        }

        if (total == 0) return (EntryStatus.Warning, "No timer was addressed.");
        string text = $"{ok}/{total} confirmed";
        if (problems.Count > 0) text += ", " + string.Join(", ", problems.Take(4)) + (problems.Count > 4 ? $" (+{problems.Count - 4} more)" : "");
        var status = ok == total ? EntryStatus.Ok : ok == 0 ? EntryStatus.Failed : EntryStatus.Warning;
        return (status, text);
    }
}
