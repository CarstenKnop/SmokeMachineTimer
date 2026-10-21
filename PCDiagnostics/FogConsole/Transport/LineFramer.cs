using System.Text;

namespace FogConsole.Transport;

/// <summary>
/// Turns arbitrary byte chunks into text lines. Handles lines split across reads, several lines per read,
/// CRLF, empty lines, invalid UTF-8 and oversized lines (dropped up to the next newline).
/// </summary>
public sealed class LineFramer
{
    /// <summary>The protocol allows 4096 bytes per line; we tolerate twice that before declaring a line garbage.</summary>
    public const int DefaultMaxLineBytes = 8192;

    private readonly int _maxLineBytes;
    private byte[] _buf = new byte[256];
    private int _len;
    private bool _discarding;
    private int _discarded;

    public LineFramer(int maxLineBytes = DefaultMaxLineBytes)
    {
        ArgumentOutOfRangeException.ThrowIfLessThan(maxLineBytes, 16);
        _maxLineBytes = maxLineBytes;
    }

    /// <summary>A complete, non-empty line.</summary>
    public event Action<string>? LineReady;

    /// <summary>A line longer than the limit was dropped; the argument is its total length in bytes.</summary>
    public event Action<int>? Overflow;

    /// <summary>Bytes of an incomplete line currently buffered.</summary>
    public int PendingBytes => _discarding ? 0 : _len;

    public void Reset()
    {
        _len = 0;
        _discarding = false;
        _discarded = 0;
    }

    public void Feed(ReadOnlySpan<byte> data)
    {
        while (!data.IsEmpty)
        {
            int nl = data.IndexOf((byte)'\n');
            ReadOnlySpan<byte> chunk = nl < 0 ? data : data[..nl];
            Append(chunk);
            if (nl < 0) return;
            EndLine();
            data = data[(nl + 1)..];
        }
    }

    private void Append(ReadOnlySpan<byte> chunk)
    {
        if (chunk.IsEmpty) return;
        if (_discarding)
        {
            _discarded += chunk.Length;
            return;
        }
        if (_len + chunk.Length > _maxLineBytes + 1) // +1: tolerate a trailing \r
        {
            _discarding = true;
            _discarded = _len + chunk.Length;
            _len = 0;
            return;
        }
        if (_len + chunk.Length > _buf.Length)
            Array.Resize(ref _buf, Math.Max(_buf.Length * 2, _len + chunk.Length));
        chunk.CopyTo(_buf.AsSpan(_len));
        _len += chunk.Length;
    }

    private void EndLine()
    {
        if (_discarding)
        {
            int total = _discarded;
            _discarding = false;
            _discarded = 0;
            _len = 0;
            Overflow?.Invoke(total);
            return;
        }

        int n = _len;
        _len = 0;
        if (n > 0 && _buf[n - 1] == (byte)'\r') n--;
        if (n == 0) return;

        string text = Encoding.UTF8.GetString(_buf, 0, n);
        // Binary garbage (wrong baud rate during ROM boot) must not break the console.
        if (HasControlChars(text)) text = Sanitize(text);
        if (text.Length > 0) LineReady?.Invoke(text);
    }

    private static bool HasControlChars(string s)
    {
        foreach (char c in s)
            if (c < 0x20 && c != '\t') return true;
        return false;
    }

    private static string Sanitize(string s)
    {
        var sb = new StringBuilder(s.Length);
        foreach (char c in s)
            sb.Append(c < 0x20 && c != '\t' ? '�' : c);
        return sb.ToString();
    }
}
