namespace FogConsole.Simulator;

/// <summary>
/// A fake 2.4 GHz neighbourhood: a handful of Wi-Fi access points on typical channels. Drives the survey results
/// and the interference-dependent packet loss of the simulated radio link, so that moving the system to a quieter
/// channel really improves the simulated link quality.
/// </summary>
internal sealed class SimBand
{
    private readonly record struct Ap(int Channel, int Rssi);

    private readonly List<Ap> _aps = new();
    private readonly Dictionary<int, int> _injected = new();   // channel -> number of active interference injections

    public SimBand(Random rng)
    {
        // Deliberately busiest on the default home channel 6 so that the demo has a story to tell.
        int[] channels = { 1, 1, 1, 6, 6, 6, 6, 6, 11, 11, 9, 3, 13 };
        int[] rssi = { -47, -63, -74, -42, -53, -61, -70, -78, -55, -68, -76, -82, -86 };
        for (int i = 0; i < channels.Length; i++)
            _aps.Add(new Ap(channels[i], rssi[i] + rng.Next(-2, 3)));
    }

    public int ApCount(int ch) => _aps.Count(a => a.Channel == ch) + (_injected.TryGetValue(ch, out var n) ? n : 0);

    /// <summary>Demo control: a strong jammer (microwave oven, hotspot ...) appears on a channel.</summary>
    public void Inject(int ch) => _injected[ch] = (_injected.TryGetValue(ch, out var n) ? n : 0) + 1;

    public void Remove(int ch)
    {
        if (!_injected.TryGetValue(ch, out var n)) return;
        if (n <= 1) _injected.Remove(ch); else _injected[ch] = n - 1;
    }

    public bool IsInjected(int ch) => _injected.ContainsKey(ch);

    /// <summary>Strongest AP whose primary channel is <paramref name="ch"/>, or 0 when there is none.</summary>
    public int StrongestRssi(int ch)
    {
        int best = int.MinValue;
        foreach (var a in _aps)
            if (a.Channel == ch && a.Rssi > best) best = a.Rssi;
        if (_injected.ContainsKey(ch)) best = Math.Max(best, -36);
        return best == int.MinValue ? 0 : best;
    }

    /// <summary>Spectral overlap between a 22 MHz wide AP channel and a 2 MHz wide ESP-NOW channel.</summary>
    private static double Overlap(int apCh, int ch)
    {
        int d = Math.Abs(apCh - ch);
        return d switch { 0 => 1.0, 1 => 0.75, 2 => 0.5, 3 => 0.3, 4 => 0.12, _ => 0.0 };
    }

    /// <summary>Interference score 0…100 (lower is quieter) before measurement noise.</summary>
    public double BaseScore(int ch)
    {
        double sum = 0;
        foreach (var a in _aps)
        {
            double strength = Math.Pow(10, (a.Rssi + 90) / 20.0); // -90 dBm -> 1, -50 dBm -> 100
            sum += Overlap(a.Channel, ch) * strength;
        }
        foreach (var (apCh, count) in _injected)
            sum += Overlap(apCh, ch) * count * Math.Pow(10, (-36 + 90) / 20.0);
        return Math.Min(100, 100 * (1 - Math.Exp(-sum / 420.0)));
    }

    /// <summary>Extra packet loss probability caused by interference on the given channel.</summary>
    public double InterferenceLoss(int ch)
    {
        double s = BaseScore(ch) / 100.0;
        double loss = 0.07 * s * s;
        // a jammer on (or right next to) the channel really hurts
        foreach (var (apCh, count) in _injected)
            loss += Overlap(apCh, ch) * Math.Min(count, 2) * 0.20;
        return Math.Min(loss, 0.6);
    }

    /// <summary>Average RSSI of foreign frames heard on a channel.</summary>
    public int AverageRssi(int ch)
    {
        double wsum = 0, sum = 0;
        foreach (var a in _aps)
        {
            double w = Overlap(a.Channel, ch);
            if (w <= 0) continue;
            wsum += w;
            sum += w * (a.Rssi - 8 * (1 - w));
        }
        return wsum <= 0 ? -95 : (int)Math.Round(sum / wsum);
    }
}
