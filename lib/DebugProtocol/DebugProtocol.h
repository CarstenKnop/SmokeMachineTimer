#pragma once

#include <Arduino.h>
#include "ReliableProtocol.h"

namespace DebugProtocol {

static constexpr uint8_t PACKET_MAGIC = 0xD1;
static constexpr size_t MAX_DATA_BYTES = 96;

enum class PacketFlags : uint8_t {
    None = 0x00,
    Response = 0x01,
    RequiresTimer = 0x02,
    Streaming = 0x04
};

inline PacketFlags operator|(PacketFlags a, PacketFlags b) {
    return static_cast<PacketFlags>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}
inline PacketFlags operator&(PacketFlags a, PacketFlags b) {
    return static_cast<PacketFlags>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}
inline PacketFlags& operator|=(PacketFlags& a, PacketFlags b) {
    a = a | b;
    return a;
}

enum class Command : uint8_t {
    Ping = 1,
    GetRemoteStats = 2,
    GetTimerStats = 3,
    SetChannel = 4,
    ForceChannel = 5,
    GetRssi = 6,
    ReadConfig = 7,
    WriteConfig = 8,
    GetDeviceInfo = 9,
    GetLogSnapshot = 10
};

enum class Status : uint8_t {
    Ok = 0,
    Busy = 1,
    InvalidArgument = 2,
    Unsupported = 3,
    TransportError = 4,
    Timeout = 5,
    NotReady = 6
};

#pragma pack(push, 1)
struct Packet {
    uint8_t magic = PACKET_MAGIC;
    Command command = Command::Ping;
    Status status = Status::Ok;
    uint8_t flags = static_cast<uint8_t>(PacketFlags::None);
    uint16_t requestId = 0;
    uint16_t dataLength = 0;
    uint8_t data[MAX_DATA_BYTES] = {0};
};
#pragma pack(pop)

struct DeviceInfo {
    uint32_t firmwareVersion = 0;
    uint32_t buildTimestamp = 0;
    uint8_t deviceKind = 0; // 0=Remote, 1=Timer
    uint8_t reserved[11] = {0};
};

struct LinkHealth {
    ReliableProtocol::TransportStats transport;
    int8_t rssiLocal = 0;    // RSSI observed at device executing the command
    int8_t rssiPeer = 0;     // Last known RSSI of the opposite endpoint (if available)
    uint8_t reserved[2] = {0};
};

bool isValid(const Packet& packet);
void clearData(Packet& packet);
bool setData(Packet& packet, const void* payload, size_t len);
const char* commandToString(Command cmd);
const char* statusToString(Status status);

} // namespace DebugProtocol
