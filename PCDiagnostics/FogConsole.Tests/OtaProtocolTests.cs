using System.Text.Json.Nodes;
using FogConsole.Hub;
using FogConsole.Ota;
using FogConsole.Protocol;
using Xunit;

namespace FogConsole.Tests;

/// <summary>The wire format against the test vectors of docs/OTA_UPDATE.md (section 8).</summary>
public class OtaProtocolTests
{
    private static byte[] VectorImage() => Enumerable.Range(0, 5000).Select(i => (byte)((i * 7 + 3) & 0xFF)).ToArray();

    private static OtaImage Image() => new(VectorImage(), "2.1.0", 224);

    private static byte[] Hex(string h) => Convert.FromHexString(h);

    private const string Sha = "34398b85297bf7d9dfb59b8d511d8bbb44ab23e891570e4395e7871475fc8afb";
    private const string BeginPayload =
        "01428813000034398b85297bf7d9dfb59b8d511d8bbb44ab23e891570e4395e7871475fc8afbe000020100007e37afee6477849bcd24393f2018a79a";

    private static OtaBegin VectorBegin() => new(0x42, 5000, Hex(Sha), 224, 2, 1, 0, 0);

    [Fact]
    public void The_image_hash_and_chunking_match_the_vectors()
    {
        var img = Image();
        Assert.Equal(Sha, Convert.ToHexString(img.Sha256).ToLowerInvariant());
        Assert.Equal(23, img.Chunks);
        Assert.Equal(72, img.ChunkBytes(22).Length);                      // the last chunk
        Assert.Equal(224, img.ChunkBytes(0).Length);
        Assert.Equal((2, 1, 0), (img.Major, img.Minor, img.Patch));
    }

    [Fact]
    public void The_authentication_input_and_value_match_the_vectors()
    {
        var input = OtaAuth.Input(VectorBegin());
        Assert.Equal("464f474f544131" + "42" + "88130000" + Sha + "e000" + "020100" + "00", Convert.ToHexString(input).ToLowerInvariant());
        Assert.Equal("7e37afee6477849bcd24393f2018a79a", Convert.ToHexString(OtaAuth.Compute(OtaAuth.KeyBytes("FogLink-OTA-v1"), VectorBegin())).ToLowerInvariant());
        Assert.Equal("5f2a426305c696639802d693a83bf393", Convert.ToHexString(OtaAuth.Compute("other"u8.ToArray(), VectorBegin())).ToLowerInvariant());
    }

    [Fact]
    public void The_default_key_is_used_when_none_is_given()
    {
        Assert.Equal(OtaAuth.KeyBytes("FogLink-OTA-v1"), OtaAuth.KeyBytes(null));
        Assert.Equal(OtaAuth.KeyBytes("FogLink-OTA-v1"), OtaAuth.KeyBytes(""));
    }

    [Fact]
    public void Begin_is_byte_for_byte_the_vector()
    {
        var p = OtaFrames.Begin(VectorBegin(), OtaAuth.KeyBytes(null));
        Assert.Equal(BeginPayload, Convert.ToHexString(p).ToLowerInvariant());
        Assert.Equal(60, p.Length);
    }

    [Fact]
    public void The_begin_frame_on_the_radio_matches_the_vector_including_its_crc()
    {
        var frame = FogFrame.Encode(1, FogFrame.TypeOta, 7, Hex(BeginPayload));
        Assert.Equal("f6020105073c" + BeginPayload + "643e", Convert.ToHexString(frame).ToLowerInvariant());
    }

    [Fact]
    public void Begin_parses_back_and_the_authentication_verifies_only_with_the_right_key()
    {
        var p = OtaFrames.Begin(VectorBegin(), OtaAuth.KeyBytes(null));
        Assert.True(OtaFrames.TryParseBegin(p, out var b, out var auth));
        Assert.Equal(0x42, b.Session);
        Assert.Equal(5000u, b.Size);
        Assert.Equal(224, b.Chunk);
        Assert.True(OtaAuth.Verify(OtaAuth.KeyBytes(null), b, auth));
        Assert.False(OtaAuth.Verify("other"u8.ToArray(), b, auth));
        var tampered = b with { Size = 5001 };
        Assert.False(OtaAuth.Verify(OtaAuth.KeyBytes(null), tampered, auth));
    }

    [Fact]
    public void Data_frames_match_the_vectors()
    {
        var img = Image();
        var d0 = OtaFrames.Data(0x42, 0, img.ChunkBytes(0));
        Assert.Equal(228, d0.Length);
        Assert.Equal("02420000030a11181f262d34", Convert.ToHexString(d0.AsSpan(0, 12)).ToLowerInvariant());
        Assert.Equal("1793", Convert.ToHexString(FogFrame.Encode(1, 5, 8, d0)[^2..]).ToLowerInvariant());

        var d22 = OtaFrames.Data(0x42, 22, img.ChunkBytes(22));
        Assert.Equal(76, d22.Length);
        Assert.Equal("02421600c3cad1d8dfe6edf4", Convert.ToHexString(d22.AsSpan(0, 12)).ToLowerInvariant());
        Assert.Equal("59d8", Convert.ToHexString(FogFrame.Encode(1, 5, 9, d22)[^2..]).ToLowerInvariant());

        Assert.True(OtaFrames.TryParseData(d22, out var s, out var seq, out var bytes));
        Assert.Equal((byte)0x42, s);
        Assert.Equal(22, seq);
        Assert.Equal(72, bytes.Length);
    }

    [Fact]
    public void The_capability_probe_frame_matches_the_vector()
    {
        Assert.Equal(Hex("f602010501020300ebb7"), FogFrame.Encode(1, 5, 1, OtaFrames.Poll(0)));
    }

    [Fact]
    public void The_state_report_matches_the_vectors()
    {
        var payload = Hex("8142020005000500000064170001");
        Assert.True(OtaStateReport.TryParse(payload, out var r));
        Assert.Equal(new OtaStateReport(0x42, OtaState.Receiving, OtaError.None, 5, 0b101, 100, 23, 1), r);
        Assert.Equal(payload, r.ToBytes());
        Assert.Equal("gUICAAUABQAAAGQXAAE=", Convert.ToBase64String(payload));
        Assert.Equal(Hex("f6020105030e81420200050005000000641700018a02"), FogFrame.Encode(1, 5, 3, payload));
    }

    [Fact]
    public void The_mask_marks_the_chunks_after_the_first_missing_one()
    {
        var r = new OtaStateReport(1, OtaState.Receiving, OtaError.None, 5, 0b101, 100, 23, 0);
        Assert.True(r.Has(0));
        Assert.True(r.Has(4));
        Assert.False(r.Has(5));          // the first missing chunk
        Assert.True(r.Has(6));           // bit 0
        Assert.False(r.Has(7));
        Assert.True(r.Has(8));           // bit 2
        Assert.False(r.Has(9));
        Assert.Equal(7, r.ReceivedCount);
        var edge = r with { Mask = 0x8000_0000u };
        Assert.True(edge.Has(5 + 1 + 31));
        Assert.False(edge.Has(5 + 1 + 32));
    }

    [Fact]
    public void Only_a_complete_state_report_parses()
    {
        Assert.False(OtaStateReport.TryParse(ReadOnlySpan<byte>.Empty, out _));
        Assert.False(OtaStateReport.TryParse(Hex("81"), out _));
        Assert.False(OtaStateReport.TryParse(Hex("814202000500050000006417"), out _));          // 13 bytes
        Assert.True(OtaStateReport.TryParse(Hex("8142020005000500000064170001"), out _));
        Assert.True(OtaStateReport.TryParse(Hex("8142020005000500000064170001ff"), out _));     // a longer report (future fields) still parses
    }

    [Fact]
    public void A_frame_of_another_op_is_not_a_state_report()
    {
        var b = Hex("8142020005000500000064170001");
        b[0] = 0x82;
        Assert.False(OtaStateReport.TryParse(b, out _));
    }

    [Fact]
    public void Poll_verify_activate_and_abort_are_two_or_three_bytes()
    {
        Assert.Equal(new byte[] { 3, 9 }, OtaFrames.Poll(9));
        Assert.Equal(new byte[] { 4, 9 }, OtaFrames.Verify(9));
        Assert.Equal(new byte[] { 5, 9, 0xA7 }, OtaFrames.Activate(9));
        Assert.Equal(new byte[] { 6, 9 }, OtaFrames.Abort(9));
    }

    [Fact]
    public void Every_error_code_has_a_plain_explanation()
    {
        foreach (OtaError e in Enum.GetValues<OtaError>())
            Assert.False(string.IsNullOrWhiteSpace(OtaText.Error(e)));
        Assert.Contains("USB", OtaText.Error(OtaError.NoSlot));
        Assert.Contains("key", OtaText.Error(OtaError.AuthFailed));
        Assert.Contains("30 s", OtaText.Error(OtaError.Timeout));
    }

    [Theory]
    [InlineData("2.1.0", "2.1.0", true)]
    [InlineData("2.1.0", "2.1", true)]            // the real remote reports major.minor
    [InlineData("2.1.0", "2.0.0", false)]
    [InlineData("2.1.0", "2.1.1", false)]
    [InlineData("2.1.0", "", true)]               // nothing to compare with
    [InlineData("2.1.0", "x", false)]
    public void The_reported_version_is_compared_with_the_image_version(string image, string reported, bool expected)
    {
        Assert.Equal(expected, new OtaImage(new byte[100], image).IsVersion(reported));
    }

    [Fact]
    public void An_image_must_not_be_empty_and_the_chunk_size_must_fit_a_frame()
    {
        Assert.Throws<ArgumentException>(() => new OtaImage(Array.Empty<byte>(), "1.0.0"));
        Assert.Throws<ArgumentOutOfRangeException>(() => new OtaImage(new byte[10], "1.0.0", 233));
        Assert.Throws<ArgumentOutOfRangeException>(() => new OtaImage(new byte[10], "1.0.0", 31));
    }
}

public class OtaMessagesTests
{
    [Fact]
    public void Hello_lists_the_capabilities()
    {
        var m = (ResponseMessage)ProtocolParser.Parse("""{"t":"res","id":1,"ok":true,"kind":"remote","fw":"2.1.0","proto":2,"mac":"aa:bb:cc:00:11:22","name":"R","ch":6,"net":1,"caps":["ota","future"]}""");
        Assert.NotNull(m.Hello);
        Assert.True(m.Hello!.SupportsOta);
        Assert.True(m.Hello.Has("FUTURE"));
        Assert.False(m.Hello.Has("nope"));
    }

    [Fact]
    public void A_hello_without_capabilities_means_no_ota()
    {
        var m = (ResponseMessage)ProtocolParser.Parse("""{"t":"res","id":1,"ok":true,"kind":"remote","fw":"2.0.0","proto":2,"mac":"aa:bb:cc:00:11:22","name":"R","ch":6,"net":1}""");
        Assert.False(m.Hello!.SupportsOta);
        Assert.Null(m.Hello.Caps);
    }

    [Fact]
    public void An_ota_rx_event_carries_the_payload_the_mac_and_the_signal()
    {
        var m = ProtocolParser.Parse("""{"t":"otaRx","dev":3,"mac":"aa:bb:cc:dd:ee:03","rssi":-61,"d":"gUICAAUABQAAAGQXAAE="}""");
        var rx = Assert.IsType<OtaRxMessage>(m);
        Assert.Equal(3, rx.Dev);
        Assert.Equal("AA:BB:CC:DD:EE:03", rx.Mac);
        Assert.Equal(-61, rx.Rssi);
        Assert.Equal(Convert.FromHexString("8142020005000500000064170001"), rx.Payload);
    }

    [Fact]
    public void A_damaged_ota_rx_event_is_kept_as_unknown_not_thrown()
    {
        Assert.IsType<UnknownMessage>(ProtocolParser.Parse("""{"t":"otaRx","dev":3,"d":"!!!not base64!!!"}"""));
        Assert.IsType<UnknownMessage>(ProtocolParser.Parse("""{"t":"otaRx","dev":3,"d":"abc"}"""));
    }

    [Fact]
    public void The_ota_tx_request_has_the_shape_of_the_spec()
    {
        var o = Requests.OtaTx(CommandTarget.Dev(3), new byte[] { 3, 0 });
        Assert.Equal("otaTx", (string?)o["t"]);
        Assert.Equal(3, (int?)o["dev"]);
        Assert.Equal("AwA=", (string?)o["d"]);
        var byMac = Requests.OtaTx(CommandTarget.ByMac("aa:bb:cc:dd:ee:03"), new byte[] { 3, 0 });
        Assert.Equal("AA:BB:CC:DD:EE:03", (string?)byMac["mac"]);
        Assert.Null(byMac["dev"]);
        Assert.Equal("""{"t":"otaTx","id":17,"dev":3,"d":"AwA="}""", Requests.Serialize(o, 17));
        Assert.Equal("""{"t":"otaHold","id":18,"on":true}""", Requests.Serialize(Requests.OtaHold(true), 18));
    }

    [Fact]
    public void The_response_to_ota_tx_tells_whether_the_mac_acknowledged()
    {
        var yes = (ResponseMessage)ProtocolParser.Parse("""{"t":"res","id":17,"ok":true,"acked":true}""");
        var no = (ResponseMessage)ProtocolParser.Parse("""{"t":"res","id":17,"ok":true,"acked":false}""");
        var none = (ResponseMessage)ProtocolParser.Parse("""{"t":"res","id":17,"ok":true}""");
        Assert.True(yes.Acked);
        Assert.False(no.Acked);
        Assert.Null(none.Acked);
    }

    [Fact]
    public void The_raw_console_shows_ota_lines_with_the_data_cut_short()
    {
        string line = "{\"t\":\"otaTx\",\"id\":17,\"dev\":3,\"d\":\"" + Convert.ToBase64String(new byte[228]) + "\"}";
        string shown = HubClient.ShortenOta(line);
        Assert.True(shown.Length < 120, shown);
        Assert.Contains("(228 bytes)", shown);
        Assert.StartsWith("{\"t\":\"otaTx\",\"id\":17,\"dev\":3,\"d\":\"AAAAAAAAAAAAAAAA", shown);
        Assert.Equal("""{"t":"ping","id":1}""", HubClient.ShortenOta("""{"t":"ping","id":1}"""));
        string rx = "{\"t\":\"otaRx\",\"dev\":3,\"d\":\"" + Convert.ToBase64String(new byte[14]) + "\"}";
        Assert.Equal(rx, HubClient.ShortenOta(rx));          // short data: nothing to cut
    }

    [Fact]
    public void Requests_stay_valid_json()
    {
        var json = JsonNode.Parse(Requests.Serialize(Requests.OtaTx(CommandTarget.Dev(0), new byte[240]), 5));
        Assert.NotNull(json);
        Assert.Equal(320, ((string?)json!["d"])!.Length);
    }
}
