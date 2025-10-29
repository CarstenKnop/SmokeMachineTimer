#include "DebugProtocol.h"

#include <cstring>

namespace DebugProtocol {

bool isValid(const Packet& packet) {
    if (packet.magic != PACKET_MAGIC) return false;
    if (packet.dataLength > MAX_DATA_BYTES) return false;
    return true;
}

void clearData(Packet& packet) {
    packet.dataLength = 0;
    memset(packet.data, 0, sizeof(packet.data));
}

bool setData(Packet& packet, const void* payload, size_t len) {
    if (!payload && len > 0) return false;
    if (len > MAX_DATA_BYTES) {
        return false;
    }
    packet.dataLength = static_cast<uint16_t>(len);
    if (len) {
        memcpy(packet.data, payload, len);
    } else {
        memset(packet.data, 0, sizeof(packet.data));
    }
    return true;
}

const char* commandToString(Command cmd) {
    switch (cmd) {
        case Command::Ping: return "Ping";
        case Command::GetRemoteStats: return "GetRemoteStats";
        case Command::GetTimerStats: return "GetTimerStats";
        case Command::SetChannel: return "SetChannel";
        case Command::ForceChannel: return "ForceChannel";
        case Command::GetRssi: return "GetRssi";
        case Command::ReadConfig: return "ReadConfig";
        case Command::WriteConfig: return "WriteConfig";
        case Command::GetDeviceInfo: return "GetDeviceInfo";
        case Command::GetLogSnapshot: return "GetLogSnapshot";
        case Command::GetDeviceInventory: return "GetDeviceInventory";
        case Command::SelectDevice: return "SelectDevice";
        case Command::StartDiscovery: return "StartDiscovery";
        case Command::StopDiscovery: return "StopDiscovery";
        case Command::GetDiscoveredDevices: return "GetDiscoveredDevices";
        case Command::PairDiscoveredDevice: return "PairDiscoveredDevice";
        case Command::UnpairDevice: return "UnpairDevice";
        case Command::RenameDevice: return "RenameDevice";
        case Command::SetTimerValues: return "SetTimerValues";
        case Command::SetTimerOutput: return "SetTimerOutput";
        default: return "Unknown";
    }
}

const char* statusToString(Status status) {
    switch (status) {
        case Status::Ok: return "Ok";
        case Status::Busy: return "Busy";
        case Status::InvalidArgument: return "InvalidArgument";
        case Status::Unsupported: return "Unsupported";
        case Status::TransportError: return "TransportError";
        case Status::Timeout: return "Timeout";
        case Status::NotReady: return "NotReady";
        default: return "?";
    }
}

} // namespace DebugProtocol
