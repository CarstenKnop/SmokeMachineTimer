using System.Text;

namespace FogConsole.Flashing;

/// <summary>SLIP framing as used by the ESP32 ROM bootloader (RFC 1055: 0xC0 delimits, 0xDB escapes).</summary>
public static class Slip
{
    public const byte End = 0xC0;
    public const byte Esc = 0xDB;
    public const byte EscEnd = 0xDC;
    public const byte EscEsc = 0xDD;

    /// <summary>Wraps one packet in a SLIP frame.</summary>
    public static byte[] Encode(ReadOnlySpan<byte> packet)
    {
        var output = new List<byte>(packet.Length + 16) { End };
        foreach (byte b in packet)
        {
            switch (b)
            {
                case End: output.Add(Esc); output.Add(EscEnd); break;
                case Esc: output.Add(Esc); output.Add(EscEsc); break;
                default: output.Add(b); break;
            }
        }
        output.Add(End);
        return output.ToArray();
    }
}

/// <summary>
/// Incremental SLIP decoder. Bytes outside a frame (the ROM prints a text banner after a reset, an application that is
/// still running prints its log) are not part of any packet; the last few of them are kept for diagnostics.
/// </summary>
public sealed class SlipDecoder
{
    private const int StrayLimit = 600;

    private readonly List<byte> _frame = new();
    private readonly StringBuilder _stray = new();
    private bool _inFrame;
    private bool _escaped;
    private bool _bad;

    /// <summary>Frames dropped because of a broken escape sequence.</summary>
    public int BadFrames { get; private set; }

    /// <summary>Printable text that arrived outside of frames (most recent part).</summary>
    public string StrayText => _stray.ToString();

    public void ClearStray() => _stray.Clear();

    public void Reset()
    {
        _frame.Clear();
        _inFrame = _escaped = _bad = false;
    }

    /// <summary>Feeds received bytes; every complete frame is handed to <paramref name="onFrame"/>.</summary>
    public void Feed(ReadOnlySpan<byte> data, Action<byte[]> onFrame)
    {
        foreach (byte b in data)
        {
            if (!_inFrame)
            {
                if (b == Slip.End)
                {
                    _inFrame = true;
                    _escaped = _bad = false;
                    _frame.Clear();
                }
                else
                {
                    AddStray(b);
                }
                continue;
            }

            if (_escaped)
            {
                _escaped = false;
                if (b == Slip.EscEnd) _frame.Add(Slip.End);
                else if (b == Slip.EscEsc) _frame.Add(Slip.Esc);
                else _bad = true;
                continue;
            }

            switch (b)
            {
                case Slip.End:
                    if (_frame.Count > 0 || _bad)
                    {
                        if (_bad) BadFrames++;
                        else onFrame(_frame.ToArray());
                    }
                    // an END directly after an END is the start of the next frame, not an empty frame
                    _frame.Clear();
                    _bad = false;
                    _inFrame = false;
                    break;
                case Slip.Esc:
                    _escaped = true;
                    break;
                default:
                    _frame.Add(b);
                    break;
            }
        }
    }

    private void AddStray(byte b)
    {
        char c = b switch
        {
            (byte)'\r' => '\0',
            (byte)'\n' => '\n',
            >= 0x20 and < 0x7F => (char)b,
            _ => '\0',
        };
        if (c == '\0') return;
        _stray.Append(c);
        if (_stray.Length > StrayLimit) _stray.Remove(0, _stray.Length - StrayLimit);
    }
}
