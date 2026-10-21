using System.Text;
using FogConsole.Transport;
using Xunit;

namespace FogConsole.Tests;

public class LineFramerTests
{
    private static (LineFramer Framer, List<string> Lines, List<int> Overflows) Create(int maxLineBytes = LineFramer.DefaultMaxLineBytes)
    {
        var f = new LineFramer(maxLineBytes);
        var lines = new List<string>();
        var over = new List<int>();
        f.LineReady += lines.Add;
        f.Overflow += over.Add;
        return (f, lines, over);
    }

    private static byte[] B(string s) => Encoding.UTF8.GetBytes(s);

    [Fact]
    public void A_complete_line_is_delivered()
    {
        var (f, lines, _) = Create();
        f.Feed(B("{\"t\":\"ping\"}\n"));
        Assert.Equal(new[] { "{\"t\":\"ping\"}" }, lines);
    }

    [Fact]
    public void A_line_split_across_reads_is_reassembled()
    {
        var (f, lines, _) = Create();
        f.Feed(B("{\"t\":\"fl"));
        Assert.Empty(lines);
        Assert.Equal(8, f.PendingBytes);
        f.Feed(B("eet\",\"seq\":1}"));
        Assert.Empty(lines);
        f.Feed(B("\n"));
        Assert.Equal(new[] { "{\"t\":\"fleet\",\"seq\":1}" }, lines);
        Assert.Equal(0, f.PendingBytes);
    }

    [Fact]
    public void Every_byte_boundary_works()
    {
        var data = B("{\"t\":\"a\"}\r\nplain text\n{\"t\":\"b\"}\n");
        for (int split = 0; split <= data.Length; split++)
        {
            var (f, lines, _) = Create();
            f.Feed(data.AsSpan(0, split));
            f.Feed(data.AsSpan(split));
            Assert.Equal(new[] { "{\"t\":\"a\"}", "plain text", "{\"t\":\"b\"}" }, lines);
        }
    }

    [Fact]
    public void One_byte_at_a_time()
    {
        var (f, lines, _) = Create();
        foreach (var b in B("one\ntwo\nthree\n")) f.Feed(new[] { b });
        Assert.Equal(new[] { "one", "two", "three" }, lines);
    }

    [Fact]
    public void Multiple_lines_in_one_read()
    {
        var (f, lines, _) = Create();
        f.Feed(B("a\nb\nc\nd"));
        Assert.Equal(new[] { "a", "b", "c" }, lines);
        Assert.Equal(1, f.PendingBytes);
        f.Feed(B("\n"));
        Assert.Equal(new[] { "a", "b", "c", "d" }, lines);
    }

    [Fact]
    public void Crlf_and_empty_lines()
    {
        var (f, lines, _) = Create();
        f.Feed(B("a\r\n\r\n\nb\r\n"));
        Assert.Equal(new[] { "a", "b" }, lines);
    }

    [Fact]
    public void Plain_text_interleaved_with_json()
    {
        var (f, lines, _) = Create();
        f.Feed(B("ESP-ROM:esp32c3-api1-20210207\r\nBuild:Feb  7 2021\r\n{\"t\":\"fleet\",\"seq\":1,\"devs\":[]}\r\nI (321) wifi: x\n{\"t\":\"remote\"}\n"));
        Assert.Equal(5, lines.Count);
        Assert.StartsWith("ESP-ROM", lines[0]);
        Assert.Equal("{\"t\":\"fleet\",\"seq\":1,\"devs\":[]}", lines[2]);
        Assert.Equal("{\"t\":\"remote\"}", lines[4]);
    }

    [Fact]
    public void Utf8_characters_split_across_reads_survive()
    {
        var bytes = B("{\"name\":\"Kühl€\"}\n");
        for (int split = 1; split < bytes.Length; split++)
        {
            var (f, lines, _) = Create();
            f.Feed(bytes.AsSpan(0, split));
            f.Feed(bytes.AsSpan(split));
            Assert.Equal("{\"name\":\"Kühl€\"}", Assert.Single(lines));
        }
    }

    [Fact]
    public void An_oversized_line_is_dropped_and_the_next_line_is_fine()
    {
        var (f, lines, over) = Create(maxLineBytes: 64);
        f.Feed(B(new string('x', 200) + "\n"));
        f.Feed(B("{\"t\":\"ok\"}\n"));
        Assert.Equal(new[] { "{\"t\":\"ok\"}" }, lines);
        Assert.Single(over);
        Assert.True(over[0] >= 200);
    }

    [Fact]
    public void An_oversized_line_delivered_in_many_chunks_is_still_dropped_once()
    {
        var (f, lines, over) = Create(maxLineBytes: 32);
        for (int i = 0; i < 50; i++) f.Feed(B("0123456789"));
        Assert.Empty(lines);
        Assert.Empty(over); // not finished yet
        f.Feed(B("\nafter\n"));
        Assert.Equal(new[] { "after" }, lines);
        Assert.Single(over);
        Assert.Equal(500, over[0]);
    }

    [Fact]
    public void A_line_of_exactly_the_maximum_length_is_accepted()
    {
        var (f, lines, over) = Create(maxLineBytes: 40);
        string s = new('y', 40);
        f.Feed(B(s + "\r\n"));
        Assert.Equal(new[] { s }, lines);
        Assert.Empty(over);
    }

    [Fact]
    public void Invalid_utf8_and_binary_garbage_do_not_throw()
    {
        var (f, lines, _) = Create();
        f.Feed(new byte[] { 0xFF, 0xFE, 0x00, 0x01, (byte)'A', (byte)'\n', 0x80, 0x81, (byte)'\n', (byte)'o', (byte)'k', (byte)'\n' });
        Assert.Equal(3, lines.Count);
        Assert.Equal("ok", lines[2]);
        Assert.DoesNotContain('\0', lines[0]);
    }

    [Fact]
    public void Reset_discards_a_partial_line()
    {
        var (f, lines, _) = Create();
        f.Feed(B("{\"t\":\"par"));
        f.Reset();
        f.Feed(B("x\n"));
        Assert.Equal(new[] { "x" }, lines);
    }

    [Fact]
    public void Reset_also_ends_discard_mode()
    {
        var (f, lines, over) = Create(maxLineBytes: 16);
        f.Feed(B(new string('z', 100)));
        f.Reset();
        f.Feed(B("fine\n"));
        Assert.Equal(new[] { "fine" }, lines);
        Assert.Empty(over);
    }

    [Fact]
    public void Tabs_are_kept()
    {
        var (f, lines, _) = Create();
        f.Feed(B("a\tb\n"));
        Assert.Equal("a\tb", Assert.Single(lines));
    }
}
