// ota_tests.inl - over-the-air update tests, included by fog_sim_tests.cpp (it provides CHECK, Harness, SimTimer).
//
//   1. SHA-256 / HMAC / base64 known answers and the test vectors of docs/OTA_UPDATE.md section 8
//   2. the receiver on its own (OtaReceiver + RAM flash): every rule of sections 4 and 6.1
//   3. the receiver against a windowed reference sender over a lossy, reordering, duplicating link
//   4. the whole chain: sender -> RemoteLink bridge -> simulated radio with loss -> TimerNode -> RAM flash
#include <deque>

using OtaTest::Bytes;
using OtaTest::Params;
using OtaTest::RamTarget;
using OtaTest::StateMsg;

namespace otatests {

const uint8_t kSrc[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
const uint8_t kOther[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x02};

struct TestIo : OtaIo {
    std::vector<std::pair<uint32_t, Bytes>> sent;      // (time, payload)
    uint32_t* clock = nullptr;
    int quiesces = 0, reboots = 0;
    uint32_t rebootAt = 0;
    bool otaSend(const uint8_t*, const uint8_t* p, size_t n) override { sent.emplace_back(clock ? *clock : 0, Bytes(p, p + n)); return true; }
    void otaQuiesce() override { ++quiesces; }
    void otaReboot() override { ++reboots; rebootAt = clock ? *clock : 0; }
};

// The receiver alone: you hand it payloads and advance time one millisecond per tick.
struct UnitRig {
    uint32_t now = 1000;
    RamTarget target{0x40000};
    TestIo io;
    std::unique_ptr<OtaReceiver> rx;

    UnitRig() { io.clock = &now; target.clock = [this]() { return now; }; reset(); }
    void reset() {
        rx.reset(new OtaReceiver(io));
        rx->setTarget(&target);
    }
    std::vector<StateMsg> take() {
        std::vector<StateMsg> v;
        for (auto& e : io.sent) v.push_back(OtaTest::parseState(e.second));
        io.sent.clear();
        return v;
    }
    std::vector<StateMsg> send(const Bytes& p, const uint8_t* src = kSrc) {
        rx->onPayload(src, p.data(), p.size(), now);
        return take();
    }
    std::vector<StateMsg> run(uint32_t ms) {
        std::vector<StateMsg> all;
        for (uint32_t i = 0; i < ms; ++i) {
            ++now;
            rx->tick(now);
            auto v = take();
            all.insert(all.end(), v.begin(), v.end());
        }
        return all;
    }
    bool runUntil(uint8_t state, uint32_t maxMs) {
        for (uint32_t i = 0; i < maxMs && rx->state() != state; ++i) { ++now; rx->tick(now); }
        take();
        return rx->state() == state;
    }
    // Begin + wait for the end of the erase.
    bool beginAndErase(const Bytes& image, const Params& p = Params()) {
        auto r = send(OtaTest::beginPayload(image, p));
        if (r.empty() || r[0].error != 0) return false;
        return runUntil(Ota::Receiving, 5000);
    }
    void sendAll(const Bytes& image, const Params& p = Params()) {
        for (uint32_t s = 0; s < OtaTest::chunksOf(image.size(), p.chunk); ++s) send(OtaTest::dataPayload(p.session, s, image, p.chunk));
    }
};

// ------------------------------------------------------------------ 1. known answers
void testKnownAnswers() {
    std::printf("[ota: sha256 / hmac / base64 / vectors]\n");
    auto sha = [](const std::string& s) { uint8_t d[32]; Sha256::hash(reinterpret_cast<const uint8_t*>(s.data()), s.size(), d); return OtaTest::hex(d, 32); };
    CHECK(sha("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "empty");
    CHECK(sha("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "abc");
    CHECK(sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "two blocks");
    {
        Sha256 s;
        const std::string a(1000, 'a');
        for (int i = 0; i < 1000; ++i) s.update(reinterpret_cast<const uint8_t*>(a.data()), a.size());
        uint8_t d[32];
        s.finish(d);
        CHECK(OtaTest::hex(d, 32) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "one million 'a'");
    }
    {   // the result must not depend on how the input is cut
        Bytes data(777);
        for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i * 31 + 7);
        uint8_t whole[32];
        Sha256::hash(data.data(), data.size(), whole);
        bool same = true;
        for (size_t cut = 0; cut <= data.size(); cut += 13) {
            Sha256 s;
            s.update(data.data(), cut);
            s.update(data.data() + cut, data.size() - cut);
            uint8_t d[32];
            s.finish(d);
            if (memcmp(d, whole, 32) != 0) same = false;
        }
        CHECK(same, "split updates");
    }
    auto hmac = [](const Bytes& key, const std::string& msg) { uint8_t d[32]; hmacSha256(key.data(), key.size(), reinterpret_cast<const uint8_t*>(msg.data()), msg.size(), d); return OtaTest::hex(d, 32); };
    CHECK(hmac(Bytes(20, 0x0b), "Hi There") == "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", "RFC 4231 case 1");
    CHECK(hmac(Bytes{'J', 'e', 'f', 'e'}, "what do ya want for nothing?") == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", "RFC 4231 case 2");
    CHECK(hmac(Bytes(131, 0xaa), "Test Using Larger Than Block-Size Key - Hash Key First") == "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54", "RFC 4231 case 6 (key longer than a block)");
    CHECK(constTimeEqual(reinterpret_cast<const uint8_t*>("abcd"), reinterpret_cast<const uint8_t*>("abcd"), 4) && !constTimeEqual(reinterpret_cast<const uint8_t*>("abcd"), reinterpret_cast<const uint8_t*>("abce"), 4), "constTimeEqual");

    // base64 (RFC 4648)
    auto b64 = [](const std::string& s) { char o[64]; const size_t n = base64Encode(reinterpret_cast<const uint8_t*>(s.data()), s.size(), o, sizeof(o)); return std::string(o, n); };
    CHECK(b64("") == "" && b64("f") == "Zg==" && b64("fo") == "Zm8=" && b64("foo") == "Zm9v" && b64("foob") == "Zm9vYg==" && b64("fooba") == "Zm9vYmE=" && b64("foobar") == "Zm9vYmFy", "RFC 4648 vectors");
    {
        uint8_t out[16];
        CHECK(base64Decode("Zm9vYmE=", 8, out, sizeof(out)) == 5 && memcmp(out, "fooba", 5) == 0, "decode");
        CHECK(base64Decode("Zm9vYg==", 8, out, sizeof(out)) == 4, "decode pad 2");
        CHECK(base64Decode("Zm9", 3, out, sizeof(out)) == -1, "length not multiple of 4");
        CHECK(base64Decode("Zm9v!A==", 8, out, sizeof(out)) == -1, "bad character");
        CHECK(base64Decode("Zg==Zg==", 8, out, sizeof(out)) == -1, "padding in the middle");
        CHECK(base64Decode("Zm9vYmFy", 8, out, 4) == -1, "no room");
        CHECK(base64Decode("", 0, out, sizeof(out)) == -1, "empty is not a frame");
        Bytes big(242);
        for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<uint8_t>(i * 7);
        char text[base64EncodedLen(242) + 1];
        const size_t n = base64Encode(big.data(), big.size(), text, sizeof(text));
        uint8_t back[256];
        CHECK(n == 324 && base64Decode(text, n, back, sizeof(back)) == 242 && memcmp(back, big.data(), 242) == 0, "round trip of the largest payload");
        CHECK(base64Encode(big.data(), big.size(), text, 100) == 0, "encode refuses a short buffer");
    }

    // ---- the vectors of docs/OTA_UPDATE.md section 8 ----
    Bytes img(5000);
    for (size_t i = 0; i < img.size(); ++i) img[i] = static_cast<uint8_t>((i * 7 + 3) & 0xFF);
    uint8_t d[32];
    Sha256::hash(img.data(), img.size(), d);
    CHECK(OtaTest::hex(d, 32) == "34398b85297bf7d9dfb59b8d511d8bbb44ab23e891570e4395e7871475fc8afb", "vector: sha256 of the image");
    Params p;           // session 0x42, chunk 224, fw 2.1.0, key FogLink-OTA-v1
    const Bytes begin = OtaTest::beginPayload(img, p);
    CHECK(begin.size() == 60, "begin length");
    CHECK(OtaTest::hex(begin.data() + 2 + 42, 16) == "7e37afee6477849bcd24393f2018a79a", "vector: auth");
    CHECK(OtaTest::hex(begin) == "01428813000034398b85297bf7d9dfb59b8d511d8bbb44ab23e891570e4395e7871475fc8afbe000020100007e37afee6477849bcd24393f2018a79a", "vector: Begin payload");
    uint8_t f[kMaxFrame];
    size_t n = encodeFrame(f, sizeof(f), 1, Type::Ota, 7, begin.data(), begin.size());
    CHECK(OtaTest::hex(f, n) == "f6020105073c01428813000034398b85297bf7d9dfb59b8d511d8bbb44ab23e891570e4395e7871475fc8afbe000020100007e37afee6477849bcd24393f2018a79a643e", "vector: Begin frame");
    FrameView v;
    CHECK(decodeFrame(f, n, v) == Decode::Ok && v.type == Type::Ota && v.len == 60, "the frame decodes as Type::Ota");
    CHECK(std::strcmp(typeName(static_cast<uint8_t>(Type::Ota)), "OTA") == 0, "typeName");
    Params other = p;
    other.key = "other";
    CHECK(OtaTest::hex(OtaTest::beginPayload(img, other).data() + 2 + 42, 16) == "5f2a426305c696639802d693a83bf393", "vector: auth with the key 'other'");

    const Bytes d0 = OtaTest::dataPayload(0x42, 0, img, 224);
    CHECK(d0.size() == 228 && OtaTest::hex(d0.data(), 12) == "02420000030a11181f262d34", "vector: Data(0) head");
    n = encodeFrame(f, sizeof(f), 1, Type::Ota, 8, d0.data(), d0.size());
    CHECK(n == 236 && OtaTest::hex(f + n - 2, 2) == "1793", "vector: Data(0) frame crc");
    const Bytes d22 = OtaTest::dataPayload(0x42, 22, img, 224);
    CHECK(d22.size() == 76 && OtaTest::hex(d22.data(), 12) == "02421600c3cad1d8dfe6edf4", "vector: Data(22) head (last chunk, 72 bytes)");
    n = encodeFrame(f, sizeof(f), 1, Type::Ota, 9, d22.data(), d22.size());
    CHECK(OtaTest::hex(f + n - 2, 2) == "59d8", "vector: Data(22) frame crc");
    n = encodeFrame(f, sizeof(f), 1, Type::Ota, 1, OtaTest::pollPayload(0).data(), 2);
    CHECK(OtaTest::hex(f, n) == "f602010501020300ebb7", "vector: Poll(0) frame");

    // the State message: built by the receiver and compared with the document's vector
    {
        // 5000 bytes = 23 chunks; chunks 0-4 written, chunks 6 and 8 received (5 missing): contig 5, mask 0b101, slot 1
        // (the receiver wants a real-looking image here, the State bytes do not depend on its content)
        const Bytes shaped = OtaTest::makeImage(5000);
        Params q;
        UnitRig r2;
        CHECK(r2.beginAndErase(shaped, q), "state vector: begin");
        for (uint32_t s : {0u, 1u, 2u, 3u, 4u, 6u, 8u}) r2.send(OtaTest::dataPayload(q.session, s, shaped, q.chunk));
        r2.io.sent.clear();
        r2.rx->onPayload(kSrc, OtaTest::pollPayload(q.session).data(), 2, r2.now);
        CHECK(r2.io.sent.size() == 1 && OtaTest::hex(r2.io.sent[0].second) == "8142020005000500000064170001", "vector: State payload built by the receiver");
        uint8_t fr[kMaxFrame];
        const size_t fn = encodeFrame(fr, sizeof(fr), 1, Type::Ota, 3, r2.io.sent[0].second.data(), r2.io.sent[0].second.size());
        CHECK(OtaTest::hex(fr, fn) == "f6020105030e81420200050005000000641700018a02", "vector: State frame");
        char text[32];
        base64Encode(r2.io.sent[0].second.data(), r2.io.sent[0].second.size(), text, sizeof(text));
        CHECK(std::string(text) == "gUICAAUABQAAAGQXAAE=", "vector: State payload in base64");
    }
}

// ------------------------------------------------------------------ 2. the receiver alone
void testReceiverHappyPath() {
    std::printf("[ota: receiver, happy path]\n");
    UnitRig r;
    const Bytes img = OtaTest::makeImage(10000);
    Params p;
    auto s = r.send(OtaTest::beginPayload(img, p));
    CHECK(s.size() == 1 && s[0].ok && s[0].session == 0x42 && s[0].state == Ota::Erasing && s[0].error == 0, "Begin -> Erasing");
    CHECK(s[0].total == OtaTest::chunksOf(img.size(), 224) && s[0].slot == 1, "total and slot reported");

    auto more = r.run(200);
    CHECK(r.rx->state() == Ota::Receiving, "erase finished");
    CHECK(r.target.erases == 3, "10000 bytes = 3 sectors, %d erased", r.target.erases);
    bool sawReceiving = false;
    for (auto& m : more) if (m.state == Ota::Receiving) sawReceiving = true;
    CHECK(sawReceiving, "a State announces the end of the erase");

    r.sendAll(img, p);
    CHECK(r.rx->contig() == r.rx->total(), "all chunks received");
    CHECK(std::equal(img.begin(), img.end(), r.target.flash.begin()), "the data is in the slot");
    auto s2 = r.send(OtaTest::pollPayload(0x42));
    CHECK(s2.size() == 1 && s2[0].state == Ota::Receiving && s2[0].contig == s2[0].total && s2[0].mask == 0, "Poll: everything there");

    s2 = r.send(OtaTest::verifyPayload(0x42));
    CHECK(s2.size() == 1 && s2[0].state == Ota::Verifying, "Verify -> Verifying");
    CHECK(r.runUntil(Ota::Verified, 200), "verified");
    CHECK(r.target.reads >= 9 && r.target.reads <= 11, "hashed in 1 KiB slices: %d reads", r.target.reads);

    CHECK(r.io.quiesces == 0 && r.target.activations == 0, "nothing activated yet");
    s2 = r.send(OtaTest::activatePayload(0x42));
    CHECK(s2.size() == 1 && s2[0].state == Ota::Activating && s2[0].error == 0, "Activate -> Activating");
    CHECK(r.io.quiesces == 1 && r.target.activations == 1, "output quiesced, boot slot selected");
    CHECK(r.io.reboots == 0, "no reboot yet");
    r.run(299);
    CHECK(r.io.reboots == 0, "not within 300 ms");
    r.run(5);
    CHECK(r.io.reboots == 1, "reboot after about 300 ms");
    r.run(1000);
    CHECK(r.io.reboots == 1, "only once");
}

void testStateRules() {
    std::printf("[ota: State reporting rules]\n");
    UnitRig r;
    const Bytes img = OtaTest::makeImage(40000);
    Params p;
    CHECK(r.beginAndErase(img, p), "ready");
    r.run(30);
    r.take();
    // in-order data: one State per 8 accepted chunks
    int states = 0;
    for (uint32_t s = 0; s < 24; ++s) {
        r.now += 21;                                         // slow enough that the 20 ms limit never matters
        states += static_cast<int>(r.send(OtaTest::dataPayload(p.session, s, img, p.chunk)).size());
    }
    CHECK(states == 3, "24 chunks -> 3 States, got %d", states);

    // an out-of-order chunk is reported at once (the limiter allows it)
    r.now += 25;
    auto st = r.send(OtaTest::dataPayload(p.session, 40, img, p.chunk));
    CHECK(st.size() == 1 && st[0].contig == 24 && (st[0].mask >> (40 - 24 - 1)) % 2 == 1, "gap reported with the mask");

    // ... but never more than one self-initiated State per 20 ms
    r.now += 25;
    int burst = 0;
    for (uint32_t s = 100; s < 130; ++s) burst += static_cast<int>(r.send(OtaTest::dataPayload(p.session, s, img, p.chunk)).size());
    CHECK(burst == 1, "a burst of out-of-order chunks in one millisecond -> 1 State, got %d", burst);
    auto later = r.run(60);
    CHECK(later.size() >= 1 && later.size() <= 3, "the pending report follows within the next 20 ms windows: %zu", later.size());

    // replies to direct requests are never suppressed
    int polls = 0;
    for (int i = 0; i < 5; ++i) polls += static_cast<int>(r.send(OtaTest::pollPayload(p.session)).size());
    CHECK(polls == 5, "every Poll is answered");

    // a duplicate chunk makes the receiver repeat itself (its State may have been lost)
    r.now += 25;
    st = r.send(OtaTest::dataPayload(p.session, 3, img, p.chunk));
    CHECK(st.size() == 1, "duplicate -> State");
    CHECK(r.rx->dupChunks == 1, "counted");
    CHECK(std::equal(img.begin(), img.begin() + 224 * 3, r.target.flash.begin()), "the data was not disturbed");

    // completion is reported
    UnitRig c;
    const Bytes small = OtaTest::makeImage(1000);
    c.beginAndErase(small, p);
    c.run(30);
    c.take();
    std::vector<StateMsg> all;
    for (uint32_t s = 0; s < 5; ++s) { c.now += 21; auto v = c.send(OtaTest::dataPayload(p.session, s, small, p.chunk)); all.insert(all.end(), v.begin(), v.end()); }
    CHECK(!all.empty() && all.back().contig == 5 && all.back().total == 5, "the last chunk produces a State with contig == total");
}

void testReceiverErrors() {
    std::printf("[ota: receiver, refusals]\n");
    const Bytes img = OtaTest::makeImage(5000);
    Params p;
    {   // wrong key
        UnitRig r;
        Params bad = p;
        bad.key = "not the key";
        auto s = r.send(OtaTest::beginPayload(img, bad));
        CHECK(s.size() == 1 && s[0].error == Ota::AuthFailed && s[0].state == Ota::Idle, "wrong key -> authFailed");
        CHECK(r.target.erases == 0, "nothing was touched");
        r.run(100);
        CHECK(r.target.erases == 0 && r.rx->state() == Ota::Idle, "no erase after a refused Begin");
        s = r.send(OtaTest::beginPayload(img, p));
        CHECK(s[0].error == 0 && s[0].state == Ota::Erasing, "the right key works afterwards");
        // any field is covered by the code: flip a bit in the announced size
        UnitRig r2;
        Bytes b = OtaTest::beginPayload(img, p);
        b[2] ^= 0x01;
        s = r2.send(b);
        CHECK(s[0].error == Ota::AuthFailed, "a changed field breaks the auth");
    }
    {   // a custom key
        UnitRig r;
        const std::string key = "my own key";
        r.rx->setKey(reinterpret_cast<const uint8_t*>(key.data()), key.size());
        Params mine = p;
        mine.key = key;
        CHECK(r.send(OtaTest::beginPayload(img, mine))[0].error == 0, "custom key accepted");
        UnitRig r2;
        CHECK(r2.send(OtaTest::beginPayload(img, mine))[0].error == Ota::AuthFailed, "...and refused by a receiver with the default key");
    }
    {   // too big, zero, chunk sizes, malformed
        UnitRig r;
        auto s = r.send(OtaTest::beginPayload(OtaTest::makeImage(0x40000 + 1), p));
        CHECK(s[0].error == Ota::TooBig, "larger than the slot -> tooBig");
        s = r.send(OtaTest::beginPayload(OtaTest::makeImage(0x40000), p));
        CHECK(s[0].error == 0, "exactly the slot size is fine");
        UnitRig r2;
        Params c = p;
        c.chunk = 16;
        CHECK(r2.send(OtaTest::beginPayload(img, c))[0].error == Ota::BadRequest, "chunk 16 -> badRequest");
        c.chunk = 233;
        CHECK(r2.send(OtaTest::beginPayload(img, c))[0].error == Ota::BadRequest, "chunk 233 -> badRequest");
        c.chunk = 232;
        CHECK(r2.send(OtaTest::beginPayload(img, c))[0].error == 0, "chunk 232 is the largest");
        UnitRig r3;
        Bytes zero = OtaTest::beginPayload(img, p, 0);
        putU32(zero.data() + 2, 0);
        Ota::computeAuth(reinterpret_cast<const uint8_t*>(p.key.data()), p.key.size(), p.session, zero.data() + 2, zero.data() + 2 + 42);
        CHECK(r3.send(zero)[0].error == Ota::BadRequest, "size 0 -> badRequest");
        Bytes shortBegin = OtaTest::beginPayload(img, p);
        shortBegin.pop_back();
        CHECK(r3.send(shortBegin)[0].error == Ota::BadRequest, "truncated Begin -> badRequest");
        Params s0 = p;
        s0.session = 0;
        CHECK(r3.send(OtaTest::beginPayload(img, s0))[0].error == Ota::BadRequest, "session 0 cannot be used");
    }
    {   // image header
        UnitRig r;
        Bytes bad = img;
        bad[0] = 0x7F;
        CHECK(r.beginAndErase(bad, p), "begin");
        r.now += 25;
        auto s = r.send(OtaTest::dataPayload(p.session, 0, bad, p.chunk));
        CHECK(s.size() == 1 && s[0].state == Ota::Failed && s[0].error == Ota::BadImage, "no ESP image magic -> failed/badImage");
        CHECK(r.target.writes == 0, "chunk 0 was not written");
        UnitRig r2;
        const Bytes s3 = OtaTest::makeImage(5000, "timer", 1, 9);
        r2.beginAndErase(s3, p);
        r2.now += 25;
        s = r2.send(OtaTest::dataPayload(p.session, 0, s3, p.chunk));
        CHECK(!s.empty() && s[0].state == Ota::Failed && s[0].error == Ota::BadImage, "chip id 9 (ESP32-S3) -> badImage");
        // Failed stays Failed and is reported until Abort / Begin
        s = r2.send(OtaTest::pollPayload(p.session));
        CHECK(!s.empty() && s[0].state == Ota::Failed && s[0].error == Ota::BadImage, "Failed is reported again");
        s = r2.send(OtaTest::abortPayload(p.session));
        CHECK(!s.empty() && s[0].state == Ota::Idle && s[0].error == 0, "Abort clears it");
    }
    {   // a remote image on a timer
        UnitRig r;
        const Bytes remote = OtaTest::makeImage(5000, "remote");
        r.beginAndErase(remote, p);
        r.sendAll(remote, p);
        r.send(OtaTest::verifyPayload(p.session));
        r.runUntil(Ota::Failed, 500);
        CHECK(r.rx->state() == Ota::Failed && r.rx->error() == Ota::BadImage, "remote firmware on a timer -> badImage");
        auto s = r.send(OtaTest::activatePayload(p.session));
        CHECK(s[0].error == Ota::BadRequest && r.target.activations == 0, "and it cannot be activated");
        // no identity at all
        UnitRig r2;
        const Bytes none = OtaTest::makeImage(5000, "timer", 3, 5, 100000);       // identity string placed outside the image
        r2.beginAndErase(none, p);
        r2.sendAll(none, p);
        r2.send(OtaTest::verifyPayload(p.session));
        r2.runUntil(Ota::Failed, 500);
        CHECK(r2.rx->error() == Ota::BadImage, "an image without identity is refused");
    }
    {   // hash mismatch
        UnitRig r;
        Params lie = p;
        lie.wrongSha = true;
        r.beginAndErase(img, lie);
        r.sendAll(img, lie);
        r.send(OtaTest::verifyPayload(p.session));
        r.runUntil(Ota::Failed, 500);
        CHECK(r.rx->state() == Ota::Failed && r.rx->error() == Ota::HashMismatch, "wrong hash -> hashMismatch");
        CHECK(r.target.activations == 0, "never activated");
    }
    {   // damaged data on the way (the sender's hash is right, one chunk differs)
        UnitRig r;
        Bytes broken = img;
        broken[3000] ^= 0x20;
        r.beginAndErase(img, p);
        r.sendAll(broken, p);
        r.send(OtaTest::verifyPayload(p.session));
        r.runUntil(Ota::Failed, 500);
        CHECK(r.rx->error() == Ota::HashMismatch, "corrupted payload -> hashMismatch");
    }
    {   // flash failures
        UnitRig r;
        r.target.failWriteAt = 2;
        r.beginAndErase(img, p);
        r.send(OtaTest::dataPayload(p.session, 0, img, p.chunk));
        r.send(OtaTest::dataPayload(p.session, 1, img, p.chunk));
        auto s = r.send(OtaTest::dataPayload(p.session, 2, img, p.chunk));
        CHECK(r.rx->state() == Ota::Failed && r.rx->error() == Ota::FlashError, "write error -> failed/flashError");
        UnitRig e;
        e.target.failEraseAt = 1;
        e.send(OtaTest::beginPayload(img, p));
        e.run(200);
        CHECK(e.rx->state() == Ota::Failed && e.rx->error() == Ota::FlashError, "erase error -> failed/flashError");
        UnitRig a;
        a.beginAndErase(img, p);
        a.sendAll(img, p);
        a.send(OtaTest::verifyPayload(p.session));
        a.runUntil(Ota::Verified, 500);
        a.target.failActivate = true;
        s = a.send(OtaTest::activatePayload(p.session));
        CHECK(s[0].state == Ota::Failed && s[0].error == Ota::FlashError && a.io.reboots == 0, "activation refused by the boot loader -> failed, no reboot");
        a.run(1000);
        CHECK(a.io.reboots == 0, "still no reboot");
        UnitRig v;
        v.beginAndErase(img, p);
        v.sendAll(img, p);
        v.target.failRead = true;
        v.send(OtaTest::verifyPayload(p.session));
        v.run(100);
        CHECK(v.rx->state() == Ota::Failed && v.rx->error() == Ota::FlashError, "read error while hashing -> flashError");
    }
}

void testRequestsInWrongState() {
    std::printf("[ota: receiver, requests in the wrong state]\n");
    const Bytes img = OtaTest::makeImage(5000);
    Params p;
    {
        UnitRig r;
        r.beginAndErase(img, p);
        auto s = r.send(OtaTest::activatePayload(p.session));
        CHECK(s[0].error == Ota::BadRequest && r.target.activations == 0 && r.io.quiesces == 0, "Activate while Receiving -> badRequest, nothing happens");
        s = r.send(OtaTest::verifyPayload(p.session));
        CHECK(s[0].error == Ota::BadRequest && s[0].state == Ota::Receiving, "Verify with chunks missing -> badRequest");
        r.sendAll(img, p);
        r.send(OtaTest::verifyPayload(p.session));
        r.runUntil(Ota::Verified, 500);
        s = r.send(OtaTest::activatePayload(p.session, 0x00));
        CHECK(s[0].error == Ota::BadRequest && r.target.activations == 0, "wrong magic -> badRequest");
        Bytes noMagic = {Ota::OpActivate, p.session};
        s = r.send(noMagic);
        CHECK(s[0].error == Ota::BadRequest, "no magic -> badRequest");
        s = r.send(OtaTest::verifyPayload(p.session));
        CHECK(s[0].state == Ota::Verified && s[0].error == 0, "a repeated Verify just reports Verified");
        s = r.send(OtaTest::activatePayload(p.session));
        CHECK(s[0].state == Ota::Activating, "now it works");
        s = r.send(OtaTest::activatePayload(p.session));
        CHECK(s[0].state == Ota::Activating && r.target.activations == 1, "a repeated Activate does not activate twice");
        s = r.send(OtaTest::abortPayload(p.session));
        CHECK(s[0].state == Ota::Activating, "too late to abort");
    }
    {   // data while erasing is ignored but answered
        UnitRig r;
        r.send(OtaTest::beginPayload(img, p));
        auto s = r.send(OtaTest::dataPayload(p.session, 0, img, p.chunk));
        CHECK(r.target.writes == 0 && (s.empty() || s[0].state == Ota::Erasing), "no data accepted before the erase is done");
    }
    {   // malformed data
        UnitRig r;
        r.beginAndErase(img, p);
        Bytes wrong = OtaTest::dataPayload(p.session, 1, img, p.chunk);
        wrong.pop_back();
        r.send(wrong);
        Bytes beyond = OtaTest::dataPayload(p.session, 999, img, p.chunk);
        r.send(beyond);
        r.send(Bytes{Ota::OpData, p.session, 0});
        r.send(Bytes{0x7E, p.session, 1, 2, 3});
        r.send(Bytes{0x01});
        CHECK(r.target.writes == 0 && r.rx->contig() == 0, "nothing of that was written");
        CHECK(r.rx->state() == Ota::Receiving, "and the session is unharmed");
    }
}

void testSessions() {
    std::printf("[ota: sessions]\n");
    const Bytes img = OtaTest::makeImage(8000);
    Params p;
    {   // a second sender
        UnitRig r;
        r.beginAndErase(img, p);
        for (uint32_t s = 0; s < 4; ++s) r.send(OtaTest::dataPayload(p.session, s, img, p.chunk));
        Params b = p;
        b.session = 0x55;
        auto s = r.send(OtaTest::beginPayload(img, b), kOther);
        CHECK(s.size() == 1 && s[0].error == Ota::Busy && s[0].session == 0x42 && s[0].state == Ota::Receiving, "other session Begin -> busy, shows the active session");
        r.now += 25;                                                                  // stray frames are answered once per 20 ms
        s = r.send(OtaTest::dataPayload(0x55, 0, img, p.chunk), kOther);
        CHECK(s.size() == 1 && s[0].error == Ota::Busy, "Data of the other session -> busy");
        CHECK(r.target.writes == 4, "its data was not written");
        r.now += 25;
        s = r.send(OtaTest::abortPayload(0x55), kOther);
        CHECK(s.size() == 1 && s[0].error == Ota::Busy && r.rx->state() == Ota::Receiving, "it cannot abort our transfer either");
        r.now += 25;
        s = r.send(OtaTest::dataPayload(p.session, 4, img, p.chunk), kOther);       // right session id, wrong sender
        CHECK(s.size() == 1 && s[0].error == Ota::Busy && r.rx->contig() == 4, "the session is bound to the sender's address");
        s = r.send(OtaTest::pollPayload(0), kOther);
        CHECK(s.size() == 1 && s[0].error == Ota::Busy && s[0].session == 0x42, "the capability probe of another sender: busy");
        s = r.send(OtaTest::pollPayload(p.session));
        CHECK(s[0].contig == 4, "our own session carries on");
    }
    {   // resume: the same Begin again
        UnitRig r;
        r.beginAndErase(img, p);
        for (uint32_t s : {0u, 1u, 2u, 3u, 4u, 6u}) r.send(OtaTest::dataPayload(p.session, s, img, p.chunk));
        const int erases = r.target.erases;
        auto s = r.send(OtaTest::beginPayload(img, p));
        CHECK(s.size() == 1 && s[0].error == 0 && s[0].state == Ota::Receiving && s[0].contig == 5 && s[0].mask == 1, "same Begin -> the State, nothing reset");
        CHECK(r.target.erases == erases && r.rx->contig() == 5, "no new erase");
        Params different = p;
        different.fw[2] = 9;
        s = r.send(OtaTest::beginPayload(img, different));
        CHECK(s[0].error == Ota::BadRequest && r.rx->contig() == 5, "same session, other parameters -> badRequest, transfer kept");
        for (uint32_t c : {5u, 7u}) r.send(OtaTest::dataPayload(p.session, c, img, p.chunk));
        r.sendAll(img, p);
        r.send(OtaTest::verifyPayload(p.session));
        CHECK(r.runUntil(Ota::Verified, 500), "and it can be finished");
        CHECK(std::equal(img.begin(), img.end(), r.target.flash.begin()), "intact");
    }
    {   // begin again while idle after Abort; Begin after Failed
        UnitRig r;
        r.beginAndErase(img, p);
        auto s = r.send(OtaTest::abortPayload(p.session));
        CHECK(s[0].state == Ota::Idle, "abort");
        CHECK(r.beginAndErase(img, p), "a new transfer can start");
        UnitRig f;
        Bytes bad = img;
        bad[0] = 0;
        f.beginAndErase(bad, p);
        f.send(OtaTest::dataPayload(p.session, 0, bad, p.chunk));
        CHECK(f.rx->state() == Ota::Failed, "failed");
        Params again = p;
        again.session = 0x77;
        CHECK(f.beginAndErase(img, again), "a Begin replaces a failed transfer");
    }
    {   // capability probe
        UnitRig r;
        auto s = r.send(OtaTest::pollPayload(0));
        CHECK(s.size() == 1 && s[0].state == Ota::Idle && s[0].error == 0 && s[0].session == 0 && s[0].slot == 1, "probe: idle, ready, slot 1");
        // stray frames in idle
        s = r.send(OtaTest::pollPayload(0x42));
        CHECK(s.size() == 1 && s[0].state == Ota::Idle && s[0].error == 0, "Poll of an unknown session: idle");
        s = r.send(OtaTest::verifyPayload(0x42));
        CHECK(s[0].error == Ota::BadRequest, "Verify when idle: badRequest");
        int stray = 0;
        for (int i = 0; i < 50; ++i) stray += static_cast<int>(r.send(OtaTest::dataPayload(0x42, 1, img, 224)).size());
        CHECK(stray <= 1, "stale Data frames are answered at most once per 20 ms: %d", stray);
    }
    {   // abort in all phases
        UnitRig r;
        r.beginAndErase(img, p);
        r.sendAll(img, p);
        r.send(OtaTest::verifyPayload(p.session));
        auto s = r.send(OtaTest::abortPayload(p.session));
        CHECK(s[0].state == Ota::Idle, "abort while verifying");
        r.run(100);
        CHECK(r.rx->state() == Ota::Idle, "the verification did not continue");
    }
}

void testNoSlot() {
    std::printf("[ota: no second slot]\n");
    const Bytes img = OtaTest::makeImage(3000);
    Params p;
    {
        UnitRig r;
        r.target.avail = false;
        auto s = r.send(OtaTest::pollPayload(0));
        CHECK(s.size() == 1 && s[0].error == Ota::NoSlot && s[0].state == Ota::Idle, "probe: noSlot");
        s = r.send(OtaTest::beginPayload(img, p));
        CHECK(s[0].error == Ota::NoSlot && r.target.erases == 0, "Begin: noSlot, nothing erased");
        r.run(100);
        CHECK(r.target.erases == 0 && r.rx->state() == Ota::Idle, "still nothing");
        s = r.send(OtaTest::dataPayload(p.session, 0, img, p.chunk));
        CHECK(s[0].error == Ota::NoSlot, "Data: noSlot");
    }
    {   // no target at all (a platform without OTA)
        TestIo io;
        OtaReceiver rx(io);
        rx.onPayload(kSrc, OtaTest::pollPayload(0).data(), 2, 10);
        CHECK(io.sent.size() == 1 && OtaTest::parseState(io.sent[0].second).error == Ota::NoSlot, "no target: the probe says noSlot");
        rx.tick(20);
    }
}

void testTimeoutAndPowerCut() {
    std::printf("[ota: silence, power cut]\n");
    const Bytes img = OtaTest::makeImage(8000);
    Params p;
    {   // 30 s of silence
        UnitRig r;
        r.beginAndErase(img, p);
        for (uint32_t s = 0; s < 3; ++s) r.send(OtaTest::dataPayload(p.session, s, img, p.chunk));
        r.run(29000);
        CHECK(r.rx->state() == Ota::Receiving, "still waiting after 29 s");
        r.send(OtaTest::pollPayload(p.session));            // any frame of the session restarts the clock
        r.run(29000);
        CHECK(r.rx->state() == Ota::Receiving, "the Poll restarted the clock");
        r.run(1500);
        CHECK(r.rx->state() == Ota::Idle, "idle after 30 s without a frame");
        auto s = r.send(OtaTest::pollPayload(p.session));
        CHECK(s.size() == 1 && s[0].state == Ota::Idle && s[0].error == Ota::Timeout, "the next State says timeout");
        s = r.send(OtaTest::pollPayload(p.session));
        CHECK(s[0].error == 0, "only once");
        // the same holds in Verified
        UnitRig v;
        v.beginAndErase(img, p);
        v.sendAll(img, p);
        v.send(OtaTest::verifyPayload(p.session));
        v.runUntil(Ota::Verified, 500);
        v.run(31000);
        CHECK(v.rx->state() == Ota::Idle, "a verified image is dropped after 30 s of silence too");
        CHECK(v.target.activations == 0, "without activating");
    }
    {   // erase interrupted by silence
        UnitRig r;
        r.send(OtaTest::beginPayload(OtaTest::makeImage(0x40000), p));
        r.run(31000);
        CHECK(r.rx->state() == Ota::Idle, "a long erase does not count as activity of the sender");
    }
    {   // a power cut: the RAM state is gone, the flash stays; a new transfer starts cleanly over the half-written slot
        UnitRig r;
        r.beginAndErase(img, p);
        for (uint32_t s = 0; s < 10; ++s) r.send(OtaTest::dataPayload(p.session, s, img, p.chunk));
        r.reset();
        auto s = r.send(OtaTest::pollPayload(p.session));
        CHECK(s[0].state == Ota::Idle && s[0].session == p.session, "after the cut the timer is idle and says so");
        s = r.send(OtaTest::dataPayload(p.session, 11, img, p.chunk));
        CHECK(s.size() == 1 && s[0].state == Ota::Idle, "stray data: answered idle");
        CHECK(r.beginAndErase(img, p), "a new Begin erases the slot again");
        r.sendAll(img, p);
        r.send(OtaTest::verifyPayload(p.session));
        CHECK(r.runUntil(Ota::Verified, 500), "and the image comes out right (the NOR semantics of the test flash would show a missing erase)");
    }
}

void testEraseAndVerifyPacing() {
    std::printf("[ota: pacing]\n");
    UnitRig r;
    const Bytes img = OtaTest::makeImage(60000);
    Params p;
    r.send(OtaTest::beginPayload(img, p));
    // tick as the real loop does: every millisecond; erases may not come closer than 20 ms
    for (int i = 0; i < 2000 && r.rx->state() == Ota::Erasing; ++i) { ++r.now; r.rx->tick(r.now); }
    CHECK(r.target.erases == 15, "60000 bytes = 15 sectors, %d", r.target.erases);
    uint32_t minGap = 100000;
    for (size_t i = 1; i < r.target.eraseTimes.size(); ++i) minGap = std::min<uint32_t>(minGap, r.target.eraseTimes[i] - r.target.eraseTimes[i - 1]);
    CHECK(minGap >= 20, "at most one sector per 20 ms (smallest gap %u)", minGap);
    // never more than one erase in a tick, even if the loop was stalled
    UnitRig s;
    s.send(OtaTest::beginPayload(img, p));
    s.now += 5000;
    s.rx->tick(s.now);
    CHECK(s.target.erases == 1, "a late tick still erases one sector only (%d)", s.target.erases);
    // progress is reported while erasing
    UnitRig t;
    t.send(OtaTest::beginPayload(img, p));
    t.run(100);
    auto st = t.send(OtaTest::pollPayload(p.session));
    CHECK(st[0].state == Ota::Erasing && st[0].progress > 0 && st[0].progress < 100, "erase progress %u %%", st[0].progress);
    // verification: one 1 KiB slice per tick, progress reported
    UnitRig v;
    v.beginAndErase(img, p);
    v.sendAll(img, p);
    v.send(OtaTest::verifyPayload(p.session));
    v.rx->tick(v.now + 1);
    CHECK(v.target.reads == 1, "one slice per tick");
    v.now += 1;
    for (int i = 0; i < 20; ++i) { ++v.now; v.rx->tick(v.now); }
    auto vs = v.send(OtaTest::pollPayload(p.session));
    CHECK(vs[0].state == Ota::Verifying && vs[0].progress > 0 && vs[0].progress < 100, "verify progress %u %%", vs[0].progress);
    CHECK(v.runUntil(Ota::Verified, 500), "done");
}

void testIdentityAcrossSlices() {
    std::printf("[ota: identity string across slice boundaries]\n");
    Params p;
    // the identity string straddles the 1 KiB boundary at every possible split point
    int found = 0, tried = 0;
    for (size_t at = 1024 - 12; at <= 1024; ++at) {
        UnitRig r;
        const Bytes img = OtaTest::makeImage(3000, "timer", 7, 5, at);
        r.beginAndErase(img, p);
        r.sendAll(img, p);
        r.send(OtaTest::verifyPayload(p.session));
        ++tried;
        if (r.runUntil(Ota::Verified, 500)) ++found;
    }
    CHECK(found == tried, "found in %d of %d positions", found, tried);
    // a partial prefix followed by the real thing ("FOGFOGFW|timer|") still matches
    UnitRig r;
    Bytes img = OtaTest::makeImage(3000, "timer", 8, 5, 5000);        // no regular identity
    const char* tricky = "FOGFOGFW|timer|";
    memcpy(img.data() + 1021, tricky, strlen(tricky));
    r.beginAndErase(img, p);
    r.sendAll(img, p);
    r.send(OtaTest::verifyPayload(p.session));
    CHECK(r.runUntil(Ota::Verified, 500), "the search backs up correctly after a false start");
}

// ------------------------------------------------------------------ 3. a reference sender (what Fog Console does, section 4.3)
struct RefSender {
    struct Wire { virtual ~Wire() {} virtual bool tx(const Bytes& payload) = 0; };

    Wire& wire;
    Bytes image;
    Params prm;
    uint32_t total = 0;
    enum class Phase { Begin, Erase, Stream, Verify, Activate, Done, Failed } phase = Phase::Begin;
    uint8_t failError = 0;
    int restarts = 0, maxRestarts = 3;
    uint32_t contig = 0, mask = 0, window = 24, rto = 150;
    uint32_t lastBegin = 0, lastPoll = 0, lastState = 0, lastCtl = 0, started = 0, dataSent = 0, resent = 0;
    bool first = true;
    std::vector<uint32_t> sentAt;
    std::mt19937 rng{99};

    RefSender(Wire& w, const Bytes& img, Params p, uint32_t now) : wire(w), image(img), prm(p) { restart(now, false); }

    void restart(uint32_t now, bool count) {
        if (count) ++restarts;
        if (count) prm.session = static_cast<uint8_t>(1 + rng() % 250);
        total = OtaTest::chunksOf(image.size(), prm.chunk);
        sentAt.assign(total, 0);
        contig = mask = 0;
        phase = Phase::Begin;
        first = true;
        lastState = lastBegin = lastPoll = lastCtl = now;
        started = now;
    }

    bool confirmed(uint32_t seq) const {
        if (seq < contig) return true;
        if (seq == contig) return false;
        const uint32_t bit = seq - contig - 1;
        return bit < 32 && ((mask >> bit) & 1u);
    }

    void fail(uint8_t e) { phase = Phase::Failed; failError = e; }

    void onPayload(const uint8_t* p, size_t n, uint32_t now) {
        const StateMsg s = OtaTest::parseState(p, n);
        if (!s.ok || s.session != prm.session) return;
        lastState = now;
        if (s.error != 0 && s.state != Ota::Failed) {
            if (s.error == Ota::BadRequest && (phase == Phase::Verify || phase == Phase::Activate)) { phase = Phase::Stream; return; }
            if (s.error == Ota::Timeout) { restart(now, true); return; }
            fail(s.error);
            return;
        }
        switch (s.state) {
            case Ota::Idle:
                if (phase != Phase::Begin && phase != Phase::Done) { if (restarts < maxRestarts) restart(now, true); else fail(Ota::Timeout); }
                break;
            case Ota::Erasing: if (phase == Phase::Begin) phase = Phase::Erase; break;
            case Ota::Receiving:
                contig = s.contig; mask = s.mask;
                if (phase == Phase::Begin || phase == Phase::Erase || phase == Phase::Verify) phase = Phase::Stream;
                break;
            case Ota::Verifying: if (phase == Phase::Stream || phase == Phase::Verify) phase = Phase::Verify; contig = total; break;
            case Ota::Verified: if (phase != Phase::Done) phase = Phase::Activate; break;
            case Ota::Activating: phase = Phase::Done; break;
            case Ota::Failed: fail(s.error); break;
        }
    }

    void tick(uint32_t now) {
        switch (phase) {
            case Phase::Begin:
                if (first || now - lastBegin >= 400) { first = false; lastBegin = now; wire.tx(OtaTest::beginPayload(image, prm)); }
                if (now - started > 10000) fail(Ota::Timeout);
                break;
            case Phase::Erase:
                if (now - lastPoll >= 500) { lastPoll = now; wire.tx(OtaTest::pollPayload(prm.session)); }
                if (now - started > 70000) fail(Ota::Timeout);
                break;
            case Phase::Stream:
                if (contig >= total) { phase = Phase::Verify; lastCtl = 0; return; }
                for (uint32_t seq = contig; seq < total && seq < contig + window; ++seq) {
                    if (confirmed(seq)) continue;
                    if (sentAt[seq] != 0 && now - sentAt[seq] < rto) continue;
                    if (!wire.tx(OtaTest::dataPayload(prm.session, seq, image, prm.chunk))) break;
                    if (sentAt[seq] != 0) ++resent;
                    ++dataSent;
                    sentAt[seq] = now ? now : 1;
                }
                if (now - lastState >= 400 && now - lastPoll >= 400) { lastPoll = now; wire.tx(OtaTest::pollPayload(prm.session)); }
                break;
            case Phase::Verify:
                if (now - lastCtl >= 500) { lastCtl = now; wire.tx(OtaTest::verifyPayload(prm.session)); }
                break;
            case Phase::Activate:
                if (now - lastCtl >= 500) { lastCtl = now; wire.tx(OtaTest::activatePayload(prm.session)); }
                break;
            default: break;
        }
        if ((phase == Phase::Stream || phase == Phase::Verify || phase == Phase::Activate || phase == Phase::Erase) && now - lastState > 15000) fail(Ota::Timeout);
    }
    bool done() const { return phase == Phase::Done; }
    bool failed() const { return phase == Phase::Failed; }
};

// the receiver against the sender over a link that loses, duplicates and reorders
struct DirectRig : RefSender::Wire {
    uint32_t now = 1000;
    RamTarget target{0x40000};
    TestIo io;
    std::unique_ptr<OtaReceiver> rx;
    std::unique_ptr<RefSender> sender;
    std::mt19937 rng{4242};
    double loss = 0.0, dup = 0.0;
    uint32_t jitter = 0;
    struct Item { uint32_t due; uint64_t order; Bytes payload; bool toTimer; };
    std::vector<Item> q;
    uint64_t order = 0;
    int dropped = 0, delivered = 0;

    DirectRig() { io.clock = &now; target.clock = [this]() { return now; }; resetReceiver(); }
    void resetReceiver() { rx.reset(new OtaReceiver(io)); rx->setTarget(&target); }
    double r01() { return std::uniform_real_distribution<double>(0, 1)(rng); }
    void post(const Bytes& b, bool toTimer) {
        if (r01() < loss) { ++dropped; return; }
        const int copies = r01() < dup ? 2 : 1;
        for (int i = 0; i < copies; ++i) q.push_back({now + 1 + (jitter ? static_cast<uint32_t>(rng() % (jitter + 1)) : 0), order++, b, toTimer});
    }
    bool tx(const Bytes& payload) override { post(payload, true); return true; }
    void step() {
        ++now;
        std::vector<Item> due;
        for (size_t i = 0; i < q.size();) {
            if (static_cast<int32_t>(now - q[i].due) >= 0) { due.push_back(q[i]); q.erase(q.begin() + static_cast<long>(i)); }
            else ++i;
        }
        std::sort(due.begin(), due.end(), [](const Item& a, const Item& b) { return a.due != b.due ? a.due < b.due : a.order < b.order; });
        for (Item& it : due) {
            ++delivered;
            if (it.toTimer) rx->onPayload(kSrc, it.payload.data(), it.payload.size(), now);
            else sender->onPayload(it.payload.data(), it.payload.size(), now);
        }
        for (auto& e : io.sent) post(e.second, false);
        io.sent.clear();
        if (sender) sender->tick(now);
        rx->tick(now);
        for (auto& e : io.sent) post(e.second, false);
        io.sent.clear();
    }
    bool run(uint32_t maxMs) {
        for (uint32_t i = 0; i < maxMs && !sender->done() && !sender->failed(); ++i) step();
        return sender->done();
    }
};

void testTransferOverBadLink(double loss, double dup, uint32_t jitter, uint32_t size, const char* name) {
    std::printf("[ota: transfer, %s]\n", name);
    DirectRig d;
    d.loss = loss; d.dup = dup; d.jitter = jitter;
    const Bytes img = OtaTest::makeImage(size, "timer", 11);
    Params p;
    d.sender.reset(new RefSender(d, img, p, d.now));
    const bool ok = d.run(600000);
    CHECK(ok, "transfer finished (phase %d, error %u, restarts %d)", static_cast<int>(d.sender->phase), d.sender->failError, d.sender->restarts);
    CHECK(d.rx->state() == Ota::Activating && d.target.activations == 1, "activated exactly once");
    CHECK(std::equal(img.begin(), img.end(), d.target.flash.begin()), "the slot holds the image");
    CHECK(d.sender->restarts == 0, "no restart needed");
    std::printf("  %zu bytes, %u data frames sent (%u re-sent), %d frames lost, %u ms virtual\n", img.size(), d.sender->dataSent, d.sender->resent, d.dropped, d.now - 1000);
}

void testPowerCutDuringTransfer() {
    std::printf("[ota: power cut during the transfer]\n");
    DirectRig d;
    d.loss = 0.1;
    const Bytes img = OtaTest::makeImage(30000, "timer", 21);
    Params p;
    d.sender.reset(new RefSender(d, img, p, d.now));
    bool cut = false;
    for (uint32_t i = 0; i < 400000 && !d.sender->done() && !d.sender->failed(); ++i) {
        d.step();
        if (!cut && d.rx->contig() >= 60) { d.resetReceiver(); cut = true; }       // the machine heats up: power gone, RAM lost, flash stays
    }
    CHECK(cut, "the cut happened");
    CHECK(d.sender->done(), "finished (phase %d error %u)", static_cast<int>(d.sender->phase), d.sender->failError);
    CHECK(d.sender->restarts == 1, "restarted once: %d", d.sender->restarts);
    CHECK(std::equal(img.begin(), img.end(), d.target.flash.begin()), "the second attempt wrote a clean image over the half-written slot");
    CHECK(d.target.activations == 1, "activated once");
}

} // namespace otatests

// ------------------------------------------------------------------ 4. the whole chain
namespace otatests {

// RefSender -> RemoteLink::submitOta -> simulated radio -> TimerNode -> RAM flash, and back through onOtaFrame
struct TunnelWire : RefSender::Wire {
    Harness& h;
    int dev;
    uint32_t nextTag = 1;
    int accepted = 0, refused = 0;
    TunnelWire(Harness& hh, int d) : h(hh), dev(d) {}
    bool tx(const Bytes& payload) override {
        const auto r = h.link->submitOta(dev, payload.data(), payload.size(), nextTag);
        if (r == RemoteLink::OtaSubmit::Ok) { ++nextTag; ++accepted; return true; }
        ++refused;
        return false;
    }
};

struct TunnelRun {
    bool done = false;
    int restarts = 0;
    uint32_t ms = 0;
    uint32_t dataSent = 0, resent = 0;
};

// Updates one timer; keeps the clock of the harness running; optional hook once per millisecond.
TunnelRun runTunnel(Harness& h, int dev, const Bytes& img, uint32_t maxMs, std::function<void(uint32_t /*ms since start*/, RefSender&)> hook = nullptr) {
    TunnelWire wire(h, dev);
    Params p;
    p.session = static_cast<uint8_t>(0x30 + dev);
    RefSender sender(wire, img, p, h.net.now);
    h.otaRxHook = [&](int from, const Bytes& payload) { if (from == dev) sender.onPayload(payload.data(), payload.size(), h.net.now); };
    const uint32_t t0 = h.net.now;
    for (uint32_t i = 0; i < maxMs && !sender.done() && !sender.failed(); ++i) {
        h.run(1);
        sender.tick(h.net.now);
        if (hook) hook(h.net.now - t0, sender);
    }
    h.otaRxHook = nullptr;
    TunnelRun r;
    r.done = sender.done();
    r.restarts = sender.restarts;
    r.ms = h.net.now - t0;
    r.dataSent = sender.dataSent;
    r.resent = sender.resent;
    if (sender.failed()) std::printf("  sender failed with error %u in phase %d\n", sender.failError, static_cast<int>(sender.phase));
    return r;
}

void testTunnel(double loss, double burst, const char* name) {
    std::printf("[ota: tunnel through the remote, %s]\n", name);
    Harness h;
    for (int i = 0; i < 3; ++i) h.addTimer();
    h.net.loss = loss;
    h.net.burst = burst;
    h.begin();
    h.run(3000);
    CHECK(h.link->onlineCount(h.net.now) == 3, "three timers online before the update");

    std::vector<Bytes> images;
    for (int dev = 0; dev < 3; ++dev) images.push_back(OtaTest::makeImage(30000 + 5000 * dev, "timer", 100 + dev));
    int toggles = 0, toggleOk = 0;
    for (int dev = 0; dev < 3; ++dev) {
        const bool heldDuring = false;
        (void)heldDuring;
        bool heldSeen = true, refusedSeen = true;
        const size_t jobsBefore = h.done.size();
        TunnelRun r = runTunnel(h, dev, images[dev], 300000, [&](uint32_t ms, RefSender&) {
            if (ms == 300) {
                // a user command to another timer in the middle of the transfer
                h.link->submitToggle((dev + 1) % 3);
                ++toggles;
            }
            if (ms > 500 && ms % 997 == 0) {
                if (!h.link->otaHeld()) heldSeen = false;
                if (h.link->startDiscovery(true) || h.link->startRecover(-1) || h.link->startMigrate(6, true) || h.link->startLinkTest(0, 10, 40)) refusedSeen = false;
            }
        });
        CHECK(r.done, "timer %d: transfer finished", dev);
        CHECK(r.restarts == 0, "timer %d: no restart (%d)", dev, r.restarts);
        SimTimer& t = *h.timers[dev];
        CHECK(std::equal(images[dev].begin(), images[dev].end(), t.otaFlash.flash.begin()), "timer %d: slot holds its image", dev);
        CHECK(t.otaFlash.activations == 1 && t.quiesces == 1, "timer %d: activated once, output quiesced once", dev);
        CHECK(heldSeen, "the radio stayed held during the transfer");
        CHECK(refusedSeen, "discovery / recovery / channel move / link test were refused during the transfer");
        for (size_t i = jobsBefore; i < h.done.size(); ++i) if (h.done[i].op == static_cast<uint8_t>(Op::Toggle) && h.done[i].result == JobResult::Ok) ++toggleOk;
        // the timer reboots into the new firmware (power cycle in the simulation)
        h.run(400);
        CHECK(t.rebootRequests >= 1, "timer %d asked for the reboot", dev);
        t.powerOff();
        h.run(300);
        t.powerOn();
        h.run(2500);
        CHECK(h.link->isOnline(dev, h.net.now), "timer %d is back online after the restart", dev);
        std::printf("  timer %d: %u ms, %u data frames (%u re-sent)\n", dev, r.ms, r.dataSent, r.resent);
    }
    CHECK(toggles == 3 && toggleOk == 3, "commands to other timers kept working during the updates: %d of %d", toggleOk, toggles);
    CHECK(h.link->onlineCount(h.net.now) == 3, "all timers online at the end");

    // the hold ends by itself
    CHECK(h.link->otaHeld(), "still held right after the last frame");
    h.run(RemoteLink::kOtaHoldMs + 500);
    CHECK(!h.link->otaHeld(), "the hold expired after 15 s of silence");
    CHECK(h.link->startRecover(-1), "recovery is allowed again");
    // erase pacing in the real chain
    uint32_t minGap = 100000;
    for (int dev = 0; dev < 3; ++dev) {
        const auto& et = h.timers[dev]->otaFlash.eraseTimes;
        for (size_t i = 1; i < et.size(); ++i) minGap = std::min<uint32_t>(minGap, et[i] - et[i - 1]);
    }
    CHECK(minGap >= 20, "erases at least 20 ms apart (%u)", minGap);
    // heartbeats were not starved: the transferring timer's own status kept arriving
    CHECK(h.link->counters().otaTx > 100 && h.link->counters().otaRx > 10, "bridge counters: tx %u rx %u", h.link->counters().otaTx, h.link->counters().otaRx);
}

void testTunnelPowerCut() {
    std::printf("[ota: tunnel, the machine cuts the timer's power]\n");
    Harness h;
    h.addTimer();
    h.addTimer();
    h.net.loss = 0.15;
    h.begin();
    h.run(3000);
    const Bytes img = OtaTest::makeImage(40000, "timer", 55);
    bool cut = false;
    TunnelRun r = runTunnel(h, 0, img, 400000, [&](uint32_t, RefSender&) {
        if (!cut && h.timers[0]->logic && h.timers[0]->logic->ota().contig() >= 80) {
            cut = true;
            h.timers[0]->powerOff();
        } else if (cut && !h.timers[0]->node.powered && h.net.now % 1000 == 0) {
            h.timers[0]->powerOn();                        // the machine is warm again
        }
    });
    CHECK(cut, "power was cut");
    CHECK(r.done, "the update finished anyway");
    CHECK(r.restarts >= 1, "the sender noticed and started over (%d restarts)", r.restarts);
    SimTimer& t = *h.timers[0];
    CHECK(std::equal(img.begin(), img.end(), t.otaFlash.flash.begin()), "clean image in the slot");
    CHECK(t.otaFlash.activations == 1, "activated once");
    h.run(200);
    CHECK(h.link->isOnline(1, h.net.now), "the other timer was never disturbed");
}

void testBridgeQueue() {
    std::printf("[ota: bridge queue, priorities, device removal]\n");
    Harness h;
    h.addTimer();
    h.addTimer();
    h.addTimer();
    h.begin();
    h.run(3000);
    const uint8_t frame[] = {Ota::OpPoll, 0};

    // queue limits and results
    std::vector<std::pair<uint32_t, std::pair<bool, bool>>> results;
    h.otaTxHook = [&](uint32_t tag, bool sent, bool acked) { results.push_back({tag, {sent, acked}}); };
    int ok = 0;
    for (uint32_t i = 1; i <= 12; ++i) if (h.link->submitOta(0, frame, sizeof(frame), i) == RemoteLink::OtaSubmit::Ok) ++ok;
    CHECK(ok == 8, "queue holds 8 frames, accepted %d", ok);
    CHECK(h.link->submitOta(9, frame, sizeof(frame), 99) == RemoteLink::OtaSubmit::BadDevice, "unknown device");
    CHECK(h.link->submitOta(0, frame, 0, 99) == RemoteLink::OtaSubmit::TooLong, "empty payload refused");
    uint8_t big[kMaxPayload + 1] = {0};
    CHECK(h.link->submitOta(0, big, sizeof(big), 99) == RemoteLink::OtaSubmit::TooLong, "243 bytes refused");
    CHECK(h.link->submitOta(0, big, kMaxPayload, 98) == RemoteLink::OtaSubmit::Full, "242 bytes would fit a frame, the queue is full");
    h.run(200);
    CHECK(results.size() == 8, "every accepted frame reports its result: %zu", results.size());
    bool inOrder = true;
    for (size_t i = 0; i < results.size(); ++i) if (results[i].first != i + 1) inOrder = false;
    CHECK(inOrder, "results come in submission order");
    bool allSent = true;
    for (auto& r : results) if (!r.second.first || !r.second.second) allSent = false;
    CHECK(allSent, "all sent and acknowledged on a clean link");
    CHECK(h.link->otaQueued() == 0, "queue is empty");
    CHECK(h.sentByKind.count("OTA/uni/ch1") && h.sentByKind["OTA/uni/ch1"] == 8, "eight Type::Ota frames on the home channel");

    // commands keep priority: a toggle between a full queue of update frames is sent at once
    results.clear();
    for (uint32_t i = 1; i <= 8; ++i) h.link->submitOta(0, frame, sizeof(frame), i);
    const int job = h.link->submitToggle(1);
    CHECK(job > 0, "job queued");
    h.run(120);
    const JobDone* d = h.lastDone(1, Op::Toggle);
    CHECK(d && d->result == JobResult::Ok, "the command to another timer was confirmed within 120 ms although 8 update frames were queued");
    h.run(200);

    // a frame to a timer that is switched off is sent, not acknowledged, and reported
    results.clear();
    h.timers[2]->powerOff();
    h.link->submitOta(2, frame, sizeof(frame), 7);
    h.run(300);
    CHECK(results.size() == 1 && results[0].second.first && !results[0].second.second, "unacknowledged frame: sent=true acked=false");
    h.timers[2]->powerOn();

    // removing a timer: its queued frames are dropped (sent=false), the others follow the index shift
    results.clear();
    h.link->submitOta(0, frame, sizeof(frame), 21);        // may already be in flight
    h.link->submitOta(1, frame, sizeof(frame), 22);
    h.link->submitOta(2, frame, sizeof(frame), 23);
    h.link->removeDevice(1);
    h.run(200);
    bool t22 = false, t23ok = false;
    for (auto& r : results) {
        if (r.first == 22 && !r.second.first) t22 = true;
        if (r.first == 23 && r.second.first && r.second.second) t23ok = true;
    }
    CHECK(t22, "frame of the removed timer reported as not sent");
    CHECK(t23ok, "frame of the timer behind it still reaches it (index shifted)");

    // commands keep priority even when the update queue never runs dry (otherwise a flood of update frames would starve them)
    {
        Harness g;
        g.addTimer();
        g.addTimer();
        g.begin();
        g.run(3000);
        uint32_t tag = 1;
        g.link->submitToggle(1);
        for (int i = 0; i < 400; ++i) {
            while (g.link->submitOta(0, frame, sizeof(frame), tag) == RemoteLink::OtaSubmit::Ok) ++tag;           // keep the queue full
            g.run(1);
        }
        const JobDone* t = g.lastDone(1, Op::Toggle);
        CHECK(t && t->result == JobResult::Ok && t->ms < 300, "a command is confirmed within 300 ms although the update queue is always full (%u ms)", t ? t->ms : 0);
        CHECK(g.link->counters().otaTx > 100, "while the update frames kept flowing: %u", g.link->counters().otaTx);
    }

    // no update frame goes out while a channel move is under way (the timers are changing channel)
    {
        Harness g;
        g.addTimer();
        g.addTimer();
        g.addTimer();
        g.begin();
        g.run(3000);
        auto otaSent = [&]() { uint32_t n = 0; for (auto& kv : g.sentByKind) if (kv.first.rfind("OTA/", 0) == 0) n += static_cast<uint32_t>(kv.second); return n; };
        g.timers[2]->powerOff();                       // still counted as online: its job keeps the move busy for the length of the command budget
        CHECK(g.link->startMigrate(6, false), "migration started");
        g.link->submitOta(0, frame, sizeof(frame), 1);
        g.link->submitOta(1, frame, sizeof(frame), 2);
        uint32_t during = 0;
        for (int i = 0; i < 5000 && g.link->migrating(); ++i) { during = otaSent(); g.run(1); }      // what had been sent while the move was still running
        CHECK(!g.link->migrating(), "migration finished");
        CHECK(during == 0, "no update frame during the channel move (%u)", during);
        g.run(300);
        CHECK(otaSent() == 2, "both went out afterwards: %u", otaSent());
    }

    // received frames are handed up with the device index
    std::vector<std::pair<int, Bytes>> got;
    h.otaRxHook = [&](int dev, const Bytes& b) { got.emplace_back(dev, b); };
    h.timers[0]->logic->runOtaUsb(OtaTest::pollPayload(0).data(), 2, h.net.now);        // a reply must go to the USB sink: no sink -> dropped, no crash
    const uint8_t probe[] = {Ota::OpPoll, 0};
    h.link->submitOta(1, probe, sizeof(probe), 31);     // timer index 1 is now the former third timer
    h.run(300);
    CHECK(got.size() == 1 && got[0].first == 1 && OtaTest::parseState(got[0].second).ok, "the timer's State arrives as onOtaFrame(dev 1)");
    CHECK(OtaTest::parseState(got[0].second).error == 0 && OtaTest::parseState(got[0].second).state == Ota::Idle, "capable and idle");
    h.otaRxHook = nullptr;
}

void testHoldSemantics() {
    std::printf("[ota: radio hold]\n");
    Harness h;
    h.addTimer();
    h.begin();
    h.run(3000);
    CHECK(!h.link->otaHeld(), "free at the start");
    h.link->setNow(h.net.now);
    h.link->setOtaHold(true);
    CHECK(h.link->otaHeld(), "held");
    CHECK(!h.link->startDiscovery(false) && !h.link->startDiscovery(true), "discovery refused");
    CHECK(!h.link->startRecover(-1) && !h.link->startRecover(0), "recovery refused");
    CHECK(!h.link->startMigrate(6, false), "channel move refused");
    CHECK(!h.link->startLinkTest(0, 10, 40), "link test refused");
    h.run(14000);
    CHECK(h.link->otaHeld(), "still held after 14 s");
    h.run(1500);
    CHECK(!h.link->otaHeld(), "released after 15 s");
    CHECK(h.link->startLinkTest(0, 3, 40), "link test allowed again");
    h.run(1000);
    h.link->setOtaHold(true);
    h.link->setOtaHold(false);
    CHECK(!h.link->otaHeld(), "explicit release");

    // submitOta refreshes the hold
    const uint8_t frame[] = {Ota::OpPoll, 0};
    h.link->submitOta(0, frame, sizeof(frame), 1);
    h.run(10000);
    h.link->submitOta(0, frame, sizeof(frame), 2);
    h.run(10000);
    CHECK(h.link->otaHeld(), "the second frame renewed the hold");
    h.run(6000);
    CHECK(!h.link->otaHeld(), "and it ended 15 s after the last frame");

    // auto recovery is skipped while held: a timer that went offline is not searched for during an update
    Harness g;
    g.addTimer();
    g.addTimer();
    g.begin();
    g.run(3000);
    g.timers[1]->powerOff();
    g.run(6000);                                         // declared offline
    g.link->setNow(g.net.now);
    g.link->setOtaHold(true);
    const uint32_t sweepsBefore = g.link->counters().sweeps;
    for (int i = 0; i < 12; ++i) { g.link->setOtaHold(true); g.run(1000); }
    CHECK(g.link->counters().sweeps == sweepsBefore, "no recovery sweep while held");
    CHECK(g.remoteNode.channel == 1, "the radio stayed on the home channel");
    g.run(16000);
    g.run(30000);
    CHECK(g.link->counters().sweeps > sweepsBefore, "the recovery resumes after the hold");

    // an excursion that is already running delays the update frames instead of sending them on another channel
    Harness e;
    e.addTimer();
    e.begin();
    e.run(3000);
    e.link->startDiscovery(true);
    for (int i = 0; i < 6000 && !e.link->busyExcursion(); ++i) e.run(1);
    CHECK(e.link->busyExcursion(), "an excursion started");
    const uint8_t f2[] = {Ota::OpPoll, 0};
    e.link->submitOta(0, f2, sizeof(f2), 1);
    uint32_t sentAway = 0;
    for (int i = 0; i < 4000 && e.link->busyExcursion(); ++i) {
        e.run(1);
        for (auto& kv : e.sentByKind) if (kv.first.rfind("OTA/", 0) == 0 && kv.first != "OTA/uni/ch1") ++sentAway;
    }
    CHECK(sentAway == 0, "no update frame left on another channel");
    e.link->stopDiscovery();
    e.run(300);
    CHECK(e.sentByKind["OTA/uni/ch1"] >= 1, "it went out on the home channel afterwards");
}

void testTimerNodeOta() {
    std::printf("[ota: TimerNode]\n");
    // the timer keeps cycling and heartbeating while it erases and receives
    Harness h;
    SimTimer& t = h.addTimer();
    t.saved.onTenths = 5;
    t.saved.offTenths = 10;
    t.powerOff();
    t.powerOn();
    h.begin();
    h.run(3000);
    const Bytes img = OtaTest::makeImage(60000, "timer", 77);
    const int edgesBefore = t.outputEdges;
    const uint32_t beatsBefore = t.logic->txBeats;
    TunnelRun r = runTunnel(h, 0, img, 120000);
    CHECK(r.done, "transfer finished");
    const int edges = t.outputEdges - edgesBefore;                 // ON 0.5 s / OFF 1.0 s: two edges per 1.5 s
    CHECK(edges >= static_cast<int>(r.ms / 1500 * 2) - 1 && edges >= 2, "the cycle kept running during the update: %d edges in %u ms", edges, r.ms);
    CHECK(t.logic->txBeats >= beatsBefore + r.ms / 1500, "heartbeats kept coming: %u in %u ms", t.logic->txBeats - beatsBefore, r.ms);
    // the output may not come on between activation and reboot
    h.run(100);
    bool everOn = false;
    for (int i = 0; i < 400; ++i) { h.run(1); if (t.logic->outputOn(h.net.now)) everOn = true; }
    CHECK(!everOn, "output stays off between activation and the reboot");
    CHECK(t.quiesces == 1, "quiesce hook called once");

    // a timer without a second slot answers noSlot through the radio
    Harness g;
    SimTimer& n = g.addTimer();
    n.otaFlash.avail = false;
    g.begin();
    g.run(2500);
    std::vector<StateMsg> states;
    g.otaRxHook = [&](int, const Bytes& b) { states.push_back(OtaTest::parseState(b)); };
    const uint8_t probe[] = {Ota::OpPoll, 0};
    g.link->submitOta(0, probe, sizeof(probe), 1);
    g.run(300);
    CHECK(states.size() == 1 && states[0].error == Ota::NoSlot, "noSlot over the radio");
    g.otaRxHook = nullptr;

    // a platform without any OTA target at all
    struct NoOtaTimer : TimerPlatform {
        std::vector<Bytes> sent;
        bool send(const uint8_t*, const uint8_t* d, size_t l) override { sent.emplace_back(d, d + l); return true; }
        void setChannel(uint8_t) override {}
        void saveSettings(const TimerSettings&) override {}
        void eraseSettings() override {}
        void reboot() override {}
        void identify(uint16_t) override {}
        void fillInfo(InfoPayload&) override {}
        uint32_t random(uint32_t) override { return 0; }
    } plain;
    TimerNode node(plain);
    node.begin(1000, TimerSettings(), 1);
    uint8_t buf[kMaxFrame];
    const size_t fl = encodeFrame(buf, sizeof(buf), kDefaultNet, Type::Ota, 1, probe, sizeof(probe));
    plain.sent.clear();
    node.onFrame(kSrc, -50, buf, fl, 1100);
    node.tick(1100);
    bool answered = false;
    for (auto& s : plain.sent) {
        FrameView fv;
        if (decodeFrame(s.data(), s.size(), fv) == Decode::Ok && fv.type == Type::Ota) answered = OtaTest::parseState(fv.payload, fv.len).error == Ota::NoSlot;
    }
    CHECK(answered, "a platform without OTA answers noSlot");
    CHECK(node.rxOta == 1, "frame counted");

    // USB path: the same receiver behind runOtaUsb, replies to the sink
    struct Sink : OtaUsbSink { std::vector<Bytes> got; void onOtaReply(const uint8_t* p, size_t n) override { got.emplace_back(p, p + n); } } sink;
    Harness u;
    u.addTimer(true, false);
    u.timers[0]->logic->setOtaUsbSink(&sink);
    u.timers[0]->logic->runOtaUsb(probe, sizeof(probe), u.net.now);
    CHECK(sink.got.size() == 1 && OtaTest::parseState(sink.got[0]).ok && OtaTest::parseState(sink.got[0]).error == 0, "USB: probe answered to the sink");
    const Bytes usbImg = OtaTest::makeImage(5000, "timer", 5);
    Params p;
    u.timers[0]->logic->runOtaUsb(OtaTest::beginPayload(usbImg, p).data(), Ota::kBeginPayloadLen, u.net.now);
    u.run(200);
    for (uint32_t s = 0; s < OtaTest::chunksOf(usbImg.size(), p.chunk); ++s) {
        const Bytes d = OtaTest::dataPayload(p.session, s, usbImg, p.chunk);
        u.timers[0]->logic->runOtaUsb(d.data(), d.size(), u.net.now);
    }
    const Bytes vv = OtaTest::verifyPayload(p.session);
    u.timers[0]->logic->runOtaUsb(vv.data(), vv.size(), u.net.now);
    u.run(300);
    const Bytes aa = OtaTest::activatePayload(p.session);
    u.timers[0]->logic->runOtaUsb(aa.data(), aa.size(), u.net.now);
    CHECK(u.timers[0]->otaFlash.activations == 1 && std::equal(usbImg.begin(), usbImg.end(), u.timers[0]->otaFlash.flash.begin()), "USB: a full transfer activated the image");
    StateMsg last = OtaTest::parseState(sink.got.back());
    CHECK(last.state == Ota::Activating, "USB: last reply says Activating");
}

} // namespace otatests

void testOtaAll() {
    using namespace otatests;
    testKnownAnswers();
    testReceiverHappyPath();
    testStateRules();
    testReceiverErrors();
    testRequestsInWrongState();
    testSessions();
    testNoSlot();
    testTimeoutAndPowerCut();
    testEraseAndVerifyPacing();
    testIdentityAcrossSlices();
    testTransferOverBadLink(0.0, 0.0, 0, 60000, "clean link");
    testTransferOverBadLink(0.25, 0.10, 6, 60000, "25 % loss, duplicates, reordering");
    testTransferOverBadLink(0.40, 0.0, 12, 20000, "40 % loss");
    testPowerCutDuringTransfer();
    testBridgeQueue();
    testHoldSemantics();
    testTunnel(0.0, 0.0, "clean radio");
    testTunnel(0.10, 0.05, "10 % loss");
    testTunnel(0.25, 0.10, "25 % loss with bursts");
    testTunnelPowerCut();
    testTimerNodeOta();
}
