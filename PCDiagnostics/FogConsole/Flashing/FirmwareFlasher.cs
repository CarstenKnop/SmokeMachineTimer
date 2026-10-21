using System.Diagnostics;
using System.IO;

namespace FogConsole.Flashing;

public enum FlashStage
{
    Preparing,
    EnteringBootloader,
    Identifying,
    Checking,
    Writing,
    Verifying,
    Restarting,
    Done,
}

/// <summary>Progress of a flash run. <see cref="Done"/> / <see cref="Total"/> count the bytes written to the flash.</summary>
public sealed record FlashProgress(FlashStage Stage, string Message, long Done, long Total)
{
    public double Fraction => Total <= 0 ? 0 : Math.Clamp((double)Done / Total, 0, 1);
}

public sealed class FlashOptions
{
    public BootEntry Entry { get; init; } = BootEntry.Auto;

    /// <summary>Compare the flash with the data afterwards (MD5 calculated by the chip).</summary>
    public bool Verify { get; init; } = true;

    /// <summary>Leave out segments that the flash already contains.</summary>
    public bool SkipUnchanged { get; init; } = true;

    /// <summary>Start the application when done.</summary>
    public bool Restart { get; init; } = true;

    /// <summary>Only look: identify the chip and compare the flash with the plan, but write nothing.</summary>
    public bool WriteNothing { get; init; }
}

public sealed record FlashResult
{
    public bool Success { get; init; }
    public bool Cancelled { get; init; }
    public string Message { get; init; } = "";
    public string? ChipName { get; init; }
    public string? Mac { get; init; }
    public int SegmentsWritten { get; init; }
    public int SegmentsSkipped { get; init; }
    public long BytesSent { get; init; }
    public TimeSpan Duration { get; init; }

    /// <summary>Nothing had to be written: the device already had exactly this firmware.</summary>
    public bool AlreadyUpToDate => Success && SegmentsWritten == 0;

    /// <summary>The chip was told to start the application (it may take a moment until its USB port is back).</summary>
    public bool Restarted { get; init; }

    /// <summary>The run only looked (<see cref="FlashOptions.WriteNothing"/>).</summary>
    public bool CheckedOnly { get; init; }

    /// <summary>For a check: the parts of the plan whose flash content differs from the file.</summary>
    public IReadOnlyList<string> DifferentParts { get; init; } = Array.Empty<string>();
}

/// <summary>Writes a <see cref="FlashPlan"/> to a device in its ROM bootloader and checks the result.</summary>
public static class FirmwareFlasher
{
    private sealed class RunState
    {
        public bool InBootloader;
        public bool LinkLost;
        public required RomLoader Loader;
    }

    /// <summary>
    /// Runs the whole procedure. Whatever happens, the device is sent back to its application afterwards (unless the
    /// cable is gone), so a failed attempt does not leave it sitting in the bootloader looking dead.
    /// </summary>
    public static async Task<FlashResult> RunAsync(IFlashLink link, FlashPlan plan, FlashOptions options,
        IProgress<FlashProgress>? progress, Action<string>? log, CancellationToken ct)
    {
        var state = new RunState { Loader = new RomLoader(link, log) };
        var result = await RunCoreAsync(link, plan, options, progress, log, ct, state).ConfigureAwait(false);
        if (!result.Success && state.InBootloader && options.Restart && !state.LinkLost)
        {
            try
            {
                await state.Loader.RestartApplicationAsync(CancellationToken.None).ConfigureAwait(false);
                result = result with { Restarted = true };
            }
            catch (Exception ex) when (ex is IOException or InvalidOperationException or UnauthorizedAccessException)
            {
                // best effort
            }
        }
        return result;
    }

    private static async Task<FlashResult> RunCoreAsync(IFlashLink link, FlashPlan plan, FlashOptions options,
        IProgress<FlashProgress>? progress, Action<string>? log, CancellationToken ct, RunState state)
    {
        var clock = Stopwatch.StartNew();
        string? chipName = null, mac = null;
        int written = 0, skipped = 0;
        long sent = 0;
        var loader = state.Loader;

        void Report(FlashStage stage, string message, long done = 0, long total = 0) =>
            progress?.Report(new FlashProgress(stage, message, done, total));

        FlashResult Fail(string message, bool cancelled = false) => new()
        {
            Success = false, Cancelled = cancelled, Message = message, ChipName = chipName, Mac = mac,
            SegmentsWritten = written, SegmentsSkipped = skipped, BytesSent = sent, Duration = clock.Elapsed,
        };

        try
        {
            // ---- 1. into the bootloader ------------------------------------------------------------------------
            Report(FlashStage.EnteringBootloader, options.Entry == BootEntry.Auto ? "Resetting the device into its bootloader..." : "Talking to the bootloader...");
            await loader.ConnectAsync(options.Entry, ct).ConfigureAwait(false);
            state.InBootloader = true;

            // ---- 2. who is this --------------------------------------------------------------------------------
            Report(FlashStage.Identifying, "Identifying the chip...");
            uint? chip = await loader.GetChipIdAsync(ct).ConfigureAwait(false);
            if (chip is null) return Fail("The chip did not identify itself. This tool only programs ESP32-C3 devices.");
            chipName = Esp32C3.ChipName(chip.Value);
            if (chip.Value != Esp32C3.ChipId)
                return Fail($"This device is an {chipName}. The firmware is for the ESP32-C3, so nothing was written.");
            try { mac = await loader.ReadMacAsync(ct).ConfigureAwait(false); }
            catch (RomException) { /* the MAC is only shown for convenience */ }
            log?.Invoke($"Chip: {chipName}{(mac is null ? "" : $", MAC {mac}")}");

            await loader.SpiAttachAsync(ct).ConfigureAwait(false);
            await loader.SetFlashParametersAsync(Esp32C3.FlashBytes, ct).ConfigureAwait(false);

            // ---- 3. what has to be written ---------------------------------------------------------------------
            var segments = plan.Segments.ToList();
            if (plan.Guard is { } guard)
            {
                Report(FlashStage.Checking, "Checking the partition layout of the device...");
                string have = await loader.FlashMd5Async(guard.Offset, guard.Length, ct).ConfigureAwait(false);
                if (have != guard.Md5)
                {
                    log?.Invoke("The device has a different partition table than this firmware: boot loader and partition table are written too.");
                    segments = guard.WriteInstead.ToList();
                }
            }

            var todo = new List<FlashSegment>();
            int checkedCount = 0;
            foreach (var seg in segments)
            {
                ct.ThrowIfCancellationRequested();
                if (options.SkipUnchanged || options.WriteNothing)
                {
                    Report(FlashStage.Checking, $"Comparing {seg.Label} with the flash ({++checkedCount}/{segments.Count})...");
                    string have = await loader.FlashMd5Async(seg.Offset, (uint)seg.Data.Length, ct).ConfigureAwait(false);
                    if (have == seg.Md5)
                    {
                        skipped++;
                        log?.Invoke($"{Capitalize(seg.Label)}: already up to date (0x{seg.Offset:X}, {seg.Data.Length:N0} bytes).");
                        continue;
                    }
                }
                todo.Add(seg);
            }

            if (options.WriteNothing)
            {
                // a look only: report what the flash holds, then send the device back to its application
                var different = todo.Select(t => t.Label).ToList();
                bool back = false;
                if (options.Restart)
                {
                    Report(FlashStage.Restarting, "Starting the application again...");
                    await loader.RestartApplicationAsync(ct).ConfigureAwait(false);
                    back = true;
                    state.InBootloader = false;
                }
                string who = $"{chipName}{(mac is null ? "" : $", MAC {mac}")}";
                string what = plan.Segments.Count == 0 ? "identified (no firmware chosen to compare with)"
                    : different.Count == 0 ? "the flash already holds exactly this firmware"
                    : $"the flash differs from this firmware in: {string.Join(", ", different)}";
                string text = $"{who}: {what}. Nothing was written.";
                Report(FlashStage.Done, text);
                return new FlashResult
                {
                    Success = true, Message = text, ChipName = chipName, Mac = mac, SegmentsSkipped = skipped, Duration = clock.Elapsed,
                    Restarted = back, CheckedOnly = true, DifferentParts = different,
                };
            }

            long total = todo.Sum(t => (long)t.Data.Length);
            long done = 0;

            // ---- 4. write --------------------------------------------------------------------------------------
            foreach (var seg in todo)
            {
                ct.ThrowIfCancellationRequested();
                string label = Capitalize(seg.Label);
                Report(FlashStage.Writing, $"Erasing for {seg.Label}...", done, total);
                log?.Invoke($"{label}: writing {seg.Data.Length:N0} bytes to 0x{seg.Offset:X}.");
                await loader.FlashBeginAsync((uint)seg.Data.Length, seg.Offset, ct).ConfigureAwait(false);

                uint seq = 0;
                for (int pos = 0; pos < seg.Data.Length; pos += RomLoader.FlashBlockSize)
                {
                    ct.ThrowIfCancellationRequested();
                    int n = Math.Min(RomLoader.FlashBlockSize, seg.Data.Length - pos);
                    await loader.FlashDataAsync(seg.Data.AsMemory(pos, n), seq++, ct).ConfigureAwait(false);
                    done += n;
                    sent += n;
                    Report(FlashStage.Writing, $"Writing {seg.Label}...", done, total);
                }
                written++;
            }

            // ---- 5. verify -------------------------------------------------------------------------------------
            if (options.Verify)
            {
                int i = 0;
                foreach (var seg in todo)
                {
                    ct.ThrowIfCancellationRequested();
                    Report(FlashStage.Verifying, $"Verifying {seg.Label} ({++i}/{todo.Count})...", total, total);
                    string have = await loader.FlashMd5Async(seg.Offset, (uint)seg.Data.Length, ct).ConfigureAwait(false);
                    if (have != seg.Md5)
                        return Fail($"Verification failed for the {seg.Label}: the flash holds MD5 {have}, expected {seg.Md5}. Try again; if it keeps failing, try another USB cable or port.");
                    log?.Invoke($"{Capitalize(seg.Label)}: verified (MD5 {seg.Md5}).");
                }
            }

            // ---- 6. start the application ----------------------------------------------------------------------
            bool restarted = false;
            if (options.Restart)
            {
                Report(FlashStage.Restarting, "Starting the new firmware...", total, total);
                await loader.RestartApplicationAsync(ct).ConfigureAwait(false);
                restarted = true;
                state.InBootloader = false;
            }

            string summary = written == 0
                ? "Nothing had to be written: the device already has exactly this firmware."
                : $"Done: {written} part{(written == 1 ? "" : "s")} written{(skipped > 0 ? $", {skipped} unchanged" : "")}{(options.Verify ? " and verified" : "")}.";
            Report(FlashStage.Done, summary, total, total);
            return new FlashResult
            {
                Success = true, Message = summary, ChipName = chipName, Mac = mac, SegmentsWritten = written, SegmentsSkipped = skipped,
                BytesSent = sent, Duration = clock.Elapsed, Restarted = restarted,
            };
        }
        catch (OperationCanceledException)
        {
            return Fail(state.InBootloader
                ? "Cancelled. The device may be left without working firmware until it is flashed again; its bootloader is untouched, so that is always possible."
                : "Cancelled.", cancelled: true);
        }
        catch (BootloaderNotFoundException ex)
        {
            string hint = ex.StrayText is { } t && t.Contains('{')
                ? " The device is running its application and did not react to the reset request."
                : "";
            return Fail(ex.Message + hint);
        }
        catch (RomTimeoutException ex)
        {
            return Fail(ex.Message + " Check the USB cable and try again.");
        }
        catch (RomException ex)
        {
            return Fail(ex.Message + (written > 0 ? " The device may be left without working firmware until it is flashed again." : ""));
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or InvalidOperationException or TimeoutException)
        {
            state.LinkLost = true;
            return Fail($"The connection to the device was lost: {ex.Message}");
        }
    }

    private static string Capitalize(string s) => s.Length == 0 ? s : char.ToUpperInvariant(s[0]) + s[1..];
}
