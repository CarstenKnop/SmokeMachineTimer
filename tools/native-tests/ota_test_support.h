// ota_test_support.h - pieces shared by the over-the-air update tests (fog_sim_tests.cpp) and the cross-language
// peer (ota_peer.cpp): a RAM flash slot with NOR semantics, builders for the OTA payloads of docs/OTA_UPDATE.md and
// a parser for the State reply.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "FogOta.h"
#include "FogProto.h"
#include "FogSha256.h"

namespace OtaTest {

using namespace Fog;
using Bytes = std::vector<uint8_t>;

// A flash slot in RAM. Writes can only clear bits (like NOR flash), so a missing erase shows up as corrupted data.
struct RamTarget : OtaTarget {
    Bytes flash;
    bool avail = true;
    uint8_t slot = 1;
    int failEraseAt = -1, failWriteAt = -1;          // fail the n-th erase / write call (0 based)
    bool failActivate = false, failRead = false;
    int erases = 0, writes = 0, reads = 0, activations = 0;
    std::vector<uint32_t> eraseTimes;
    std::function<uint32_t()> clock;

    explicit RamTarget(uint32_t size = 0x1E0000) : flash(size, 0xFF) {}

    bool available() override { return avail; }
    uint32_t slotSize() override { return static_cast<uint32_t>(flash.size()); }
    uint8_t slotIndex() override { return slot; }
    bool eraseSector(uint32_t sector) override {
        const int n = erases++;
        if (failEraseAt >= 0 && n == failEraseAt) return false;
        if (static_cast<size_t>(sector + 1) * Ota::kSectorBytes > flash.size()) return false;
        memset(flash.data() + static_cast<size_t>(sector) * Ota::kSectorBytes, 0xFF, Ota::kSectorBytes);
        eraseTimes.push_back(clock ? clock() : 0);
        return true;
    }
    bool write(uint32_t offset, const uint8_t* data, size_t len) override {
        const int n = writes++;
        if (failWriteAt >= 0 && n == failWriteAt) return false;
        if (static_cast<size_t>(offset) + len > flash.size()) return false;
        for (size_t i = 0; i < len; ++i) flash[offset + i] &= data[i];
        return true;
    }
    bool read(uint32_t offset, uint8_t* out, size_t len) override {
        ++reads;
        if (failRead || static_cast<size_t>(offset) + len > flash.size()) return false;
        memcpy(out, flash.data() + offset, len);
        return true;
    }
    bool activate() override {
        if (failActivate) return false;
        ++activations;
        return true;
    }
};

// ---- helpers ------------------------------------------------------------------------------------------------------
inline std::string hex(const uint8_t* p, size_t n) {
    std::string s;
    char b[3];
    for (size_t i = 0; i < n; ++i) { snprintf(b, sizeof(b), "%02x", p[i]); s += b; }
    return s;
}
inline std::string hex(const Bytes& v) { return hex(v.data(), v.size()); }
inline Bytes unhex(const std::string& s) {
    Bytes out;
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        auto val = [](char c) { return c >= '0' && c <= '9' ? c - '0' : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : (c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0)); };
        out.push_back(static_cast<uint8_t>(val(s[i]) * 16 + val(s[i + 1])));
    }
    return out;
}

// An image that looks like firmware to the receiver: ESP image magic, chip id 5, identity string, then pseudo-random filler.
// identAt: where the identity string sits (default: early, like a real image).
inline Bytes makeImage(size_t size, const char* kind = "timer", uint32_t seed = 1, uint16_t chipId = 5, size_t identAt = 64) {
    Bytes img(size);
    std::mt19937 rng(seed);
    for (auto& b : img) b = static_cast<uint8_t>(rng());
    if (size > 14) { img[0] = 0xE9; img[12] = static_cast<uint8_t>(chipId); img[13] = static_cast<uint8_t>(chipId >> 8); }
    char ident[32];
    snprintf(ident, sizeof(ident), "FOGFW|%s|2.1.0|", kind);
    const size_t n = strlen(ident);
    if (identAt + n <= size) memcpy(img.data() + identAt, ident, n);
    return img;
}

struct Params {
    uint8_t session = 0x42;
    uint16_t chunk = 224;
    uint8_t fw[3] = {2, 1, 0};
    uint8_t flags = 0;
    std::string key = "FogLink-OTA-v1";
    bool wrongSha = false;          // announce the hash of other data (the transfer must end in hashMismatch)
};

inline uint32_t chunksOf(size_t size, uint16_t chunk) { return static_cast<uint32_t>((size + chunk - 1) / chunk); }

inline Bytes beginPayload(const Bytes& image, const Params& p, uint32_t sizeOverride = 0) {
    Bytes b(Ota::kBeginPayloadLen, 0);
    b[0] = Ota::OpBegin;
    b[1] = p.session;
    uint8_t* body = b.data() + 2;
    putU32(body, sizeOverride ? sizeOverride : static_cast<uint32_t>(image.size()));
    Bytes hashed = image;
    if (p.wrongSha && !hashed.empty()) hashed[hashed.size() / 2] ^= 0x55;
    Sha256::hash(hashed.data(), hashed.size(), body + 4);
    putU16(body + 36, p.chunk);
    memcpy(body + 38, p.fw, 3);
    body[41] = p.flags;
    Ota::computeAuth(reinterpret_cast<const uint8_t*>(p.key.data()), p.key.size(), p.session, body, body + 42);
    return b;
}

inline Bytes dataPayload(uint8_t session, uint32_t seq, const Bytes& image, uint16_t chunk) {
    const size_t off = static_cast<size_t>(seq) * chunk;
    const size_t n = off >= image.size() ? 0 : (image.size() - off < chunk ? image.size() - off : chunk);
    Bytes b(4 + n);
    b[0] = Ota::OpData;
    b[1] = session;
    putU16(b.data() + 2, static_cast<uint16_t>(seq));
    if (n) memcpy(b.data() + 4, image.data() + off, n);
    return b;
}

inline Bytes simplePayload(uint8_t op, uint8_t session) { return Bytes{op, session}; }
inline Bytes pollPayload(uint8_t session) { return simplePayload(Ota::OpPoll, session); }
inline Bytes verifyPayload(uint8_t session) { return simplePayload(Ota::OpVerify, session); }
inline Bytes abortPayload(uint8_t session) { return simplePayload(Ota::OpAbort, session); }
inline Bytes activatePayload(uint8_t session, uint8_t magic = Ota::kActivateMagic) { return Bytes{Ota::OpActivate, session, magic}; }

struct StateMsg {
    bool ok = false;
    uint8_t session = 0, state = 0, error = 0, progress = 0, slot = 0;
    uint16_t contig = 0, total = 0;
    uint32_t mask = 0;
};

inline StateMsg parseState(const uint8_t* p, size_t n) {
    StateMsg s;
    if (n != Ota::kStateLen || p[0] != Ota::OpState) return s;
    s.ok = true;
    s.session = p[1]; s.state = p[2]; s.error = p[3];
    s.contig = getU16(p + 4); s.mask = getU32(p + 6); s.progress = p[10]; s.total = getU16(p + 11); s.slot = p[13];
    return s;
}
inline StateMsg parseState(const Bytes& b) { return parseState(b.data(), b.size()); }

}  // namespace OtaTest
