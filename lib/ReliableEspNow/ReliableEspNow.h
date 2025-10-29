#pragma once

#include <Arduino.h>
#include <functional>
#include <type_traits>
#include <vector>
#include <stdint.h>
#include "ReliableProtocol.h"

namespace ReliableEspNow {

// Maximum payload size available after link headers are applied.
static constexpr size_t MAX_PAYLOAD_BYTES = 200; // conservative guard; validated at runtime

using ReceiveHandler = std::function<ReliableProtocol::HandlerResult(const uint8_t* mac, const uint8_t* payload, size_t len)>;
using AckCallback = std::function<void(const uint8_t* mac, ReliableProtocol::AckType type, uint8_t status, void* context, const char* tag)>;
using EnsurePeerCallback = std::function<void(const uint8_t* mac)>;
using SendHook = std::function<void(const uint8_t* mac)>;

class Link {
public:
    void begin();
    void loop();

    void setReceiveHandler(ReceiveHandler handler) { receiveHandler = handler; }
    void setAckCallback(AckCallback cb) { ackCallback = cb; }
    void setEnsurePeerCallback(EnsurePeerCallback cb) { ensurePeer = cb; }
    void setSendHook(SendHook hook) { sendHook = hook; }

    bool queuePacket(const uint8_t* mac, const void* payload, size_t len, const ReliableProtocol::SendConfig& cfg = ReliableProtocol::SendConfig{});
    void onReceive(const uint8_t* mac, const uint8_t* data, int len);

    template <typename T>
    bool sendStruct(const uint8_t* mac, const T& payload, const ReliableProtocol::SendConfig& cfg = ReliableProtocol::SendConfig{}) {
        static_assert(std::is_trivially_copyable<T>::value, "Payload struct must be trivially copyable");
        return queuePacket(mac, &payload, sizeof(T), cfg);
    }

    const ReliableProtocol::TransportStats& getStats() const { return stats; }
    void resetStats();

private:
    struct PendingTx {
        uint8_t mac[6] = {0};
        std::vector<uint8_t> frame; // header + payload
        ReliableProtocol::SendConfig cfg;
        uint32_t lastSendMs = 0;
        uint8_t attempts = 0;
        uint8_t seq = 0;
    };

    ReceiveHandler receiveHandler;
    AckCallback ackCallback;
    EnsurePeerCallback ensurePeer;
    SendHook sendHook;

    std::vector<PendingTx> pending;
    uint8_t nextSeq = 1;
    ReliableProtocol::TransportStats stats;

    uint8_t reserveSequence();
    bool sequenceInUse(uint8_t seq) const;
    bool sendFrame(PendingTx& tx);
    bool sendRaw(const uint8_t* mac, std::vector<uint8_t>& frame, const char* tag, bool logErrors = true);
    void finalizePending(size_t index, ReliableProtocol::AckType type, uint8_t status);
    void sendAckFrame(const uint8_t* mac, uint8_t seq, bool ack, uint8_t status);
};

} // namespace ReliableEspNow
