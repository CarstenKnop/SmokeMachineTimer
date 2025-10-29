#include "ReliableProtocol.h"

namespace ReliableProtocol {

namespace {
const char* builtinStatusToString(Status status) {
    switch (status) {
        case Status::Ok: return "OK";
        case Status::CrcMismatch: return "CRC_MISMATCH";
        case Status::InvalidLength: return "INVALID_LENGTH";
        case Status::HandlerDeclined: return "HANDLER_DECLINED";
        case Status::Timeout: return "TIMEOUT";
        case Status::SendError: return "SEND_ERROR";
        default: return nullptr;
    }
}
} // namespace

uint16_t crc16(const uint8_t* data, size_t len, uint16_t seed) {
    uint16_t crc = seed;
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (int j = 0; j < 8; ++j) {
            if (crc & 0x8000) {
                crc = static_cast<uint16_t>((crc << 1) ^ 0x1021);
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

const char* statusToString(uint8_t status) {
    auto builtIn = builtinStatusToString(static_cast<Status>(status));
    return builtIn;
}

} // namespace ReliableProtocol
