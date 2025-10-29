#pragma once

#include <Arduino.h>
#include <stdint.h>

namespace ReliableProtocol {

static constexpr uint8_t FRAME_MAGIC = 0xA5;
static constexpr uint8_t FRAME_VERSION = 1;
static constexpr uint8_t FLAG_ACK_REQUEST = 0x01;
static constexpr uint8_t FLAG_IS_ACK = 0x02;
static constexpr uint8_t FLAG_IS_NAK = 0x04;

#pragma pack(push, 1)
struct FrameHeader {
    uint8_t magic;
    uint8_t version;
    uint8_t flags;
    uint8_t seq;
    uint16_t payloadLen;
    uint16_t crc;
    uint8_t status;
};
#pragma pack(pop)

enum class AckType : uint8_t {
    Ack,
    Nak,
    Timeout
};

enum class Status : uint8_t {
    Ok = 0,
    CrcMismatch = 1,
    InvalidLength = 2,
    HandlerDeclined = 3,
    Timeout = 4,
    SendError = 5
};

struct HandlerResult {
    bool ack = true;
    uint8_t status = static_cast<uint8_t>(Status::Ok);
};

struct SendConfig {
    bool requireAck = true;
    uint16_t retryIntervalMs = 200;
    uint8_t maxAttempts = 0; // 0 => infinite retries
    const char* tag = nullptr; // optional human readable label (must remain valid)
    void* userContext = nullptr; // optional opaque pointer echoed in ack callback
};

struct TransportStats {
    uint32_t txFrames = 0;
    uint32_t txAcked = 0;
    uint32_t txNak = 0;
    uint32_t txTimeout = 0;
    uint32_t txRetries = 0;
    uint32_t txSendErrors = 0;
    uint32_t rxFrames = 0;
    uint32_t rxAckRequests = 0;
    uint32_t rxAckSent = 0;
    uint32_t rxNakSent = 0;
    uint32_t rxCrcErrors = 0;
    uint32_t rxInvalidLength = 0;
    uint32_t handlerDeclined = 0;
    uint32_t lastAckOrNakMs = 0;
    uint8_t lastStatusCode = 0;
    uint8_t reserved[3] = {0};
};

uint16_t crc16(const uint8_t* data, size_t len, uint16_t seed = 0xFFFF);
const char* statusToString(uint8_t status);

} // namespace ReliableProtocol
