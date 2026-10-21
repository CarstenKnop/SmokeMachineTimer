#include "FogProto.h"

namespace Fog {

uint16_t crc16(const uint8_t* data, size_t len, uint16_t seed) {
    uint16_t crc = seed;
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (int b = 0; b < 8; ++b) {
            crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021) : static_cast<uint16_t>(crc << 1);
        }
    }
    return crc;
}

size_t encodeFrame(uint8_t* out, size_t cap, uint8_t net, Type type, uint8_t seq,
                   const void* payload, size_t payloadLen) {
    const size_t total = kHeaderLen + payloadLen + kCrcLen;
    if (!out || total > cap || total > kMaxFrame || payloadLen > 255) return 0;
    out[0] = kMagic;
    out[1] = kProtoVersion;
    out[2] = net;
    out[3] = static_cast<uint8_t>(type);
    out[4] = seq;
    out[5] = static_cast<uint8_t>(payloadLen);
    if (payloadLen && payload) memcpy(out + kHeaderLen, payload, payloadLen);
    const uint16_t crc = crc16(out, kHeaderLen + payloadLen);
    putU16(out + kHeaderLen + payloadLen, crc);
    return total;
}

Decode decodeFrame(const uint8_t* in, size_t n, FrameView& out) {
    if (!in || n < kHeaderLen + kCrcLen) return Decode::TooShort;
    if (in[0] != kMagic) return Decode::BadMagic;
    if (in[1] != kProtoVersion) return Decode::BadVersion;
    const size_t len = in[5];
    if (n != kHeaderLen + len + kCrcLen) return Decode::BadLength;
    const uint16_t expect = getU16(in + kHeaderLen + len);
    if (crc16(in, kHeaderLen + len) != expect) return Decode::BadCrc;
    out.net = in[2];
    out.type = static_cast<Type>(in[3]);
    out.seq = in[4];
    out.payload = in + kHeaderLen;
    out.len = len;
    return Decode::Ok;
}

const char* typeName(uint8_t type) {
    switch (static_cast<Type>(type)) {
        case Type::Status: return "STATUS";
        case Type::Probe:  return "PROBE";
        case Type::Cmd:    return "CMD";
        case Type::Info:   return "INFO";
    }
    return "?";
}

void sanitizeName(char* dst, const char* src, size_t srcMax) {
    memset(dst, 0, kNameLen);
    size_t o = 0;
    for (size_t i = 0; i < srcMax && src[i] && o < kNameLen - 1; ++i) {
        const uint8_t c = static_cast<uint8_t>(src[i]);
        if (c < 0x20 || c >= 0x7F) continue;          // printable ASCII only
        if (c == ' ' && o == 0) continue;             // no leading blanks
        dst[o++] = static_cast<char>(c);
    }
    while (o > 0 && dst[o - 1] == ' ') dst[--o] = 0;   // no trailing blanks
}

}  // namespace Fog
