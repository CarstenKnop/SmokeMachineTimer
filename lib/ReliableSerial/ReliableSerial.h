#pragma once

#include <Arduino.h>
#include <HardwareSerial.h>
#include <Stream.h>
#include <functional>
#include <type_traits>
#include <vector>
#include "ReliableProtocol.h"

namespace ReliableSerial {

// Upper bound for payload bytes carried per frame. Adjust conservatively for serial buffers.
static constexpr size_t MAX_PAYLOAD_BYTES = 224;

using ReceiveHandler = std::function<ReliableProtocol::HandlerResult(const uint8_t* mac, const uint8_t* payload, size_t len)>;
using AckCallback = std::function<void(const uint8_t* mac, ReliableProtocol::AckType type, uint8_t status, void* context, const char* tag)>;

class Link {
public:
    template <typename SerialLike>
    void attach(SerialLike& serialRef, uint32_t baud, bool waitForConnection = false) {
        beginSerial(serialRef, baud);
        serial = &serialRef;
        pending.clear();
        rxBuffer.clear();
        nextSeq = 1;
        resetStats();
        connectionReady = !waitForConnection;
        lastActivityMs = millis();
    }

    void loop();    

    void setReceiveHandler(ReceiveHandler handler) { receiveHandler = handler; }
    void setAckCallback(AckCallback cb) { ackCallback = cb; }

    template <typename T>
    bool sendStruct(const T& payload, const ReliableProtocol::SendConfig& cfg = ReliableProtocol::SendConfig{}) {
        static_assert(std::is_trivially_copyable<T>::value, "Payload struct must be trivially copyable");
        return queuePacket(&payload, sizeof(T), cfg);
    }

    bool queuePacket(const void* payload, size_t len, const ReliableProtocol::SendConfig& cfg = ReliableProtocol::SendConfig{});

    bool isAttached() const { return serial != nullptr; }
    bool isConnected() const { return connectionReady; }

    const ReliableProtocol::TransportStats& getStats() const { return stats; }
    void resetStats();
private:
    Stream* serial = nullptr;
    ReceiveHandler receiveHandler;
    AckCallback ackCallback;

    std::vector<uint8_t> rxBuffer;

    struct PendingTx {
        std::vector<uint8_t> frame;
        ReliableProtocol::SendConfig cfg;
        uint32_t lastSendMs = 0;
        uint8_t attempts = 0;
        uint8_t seq = 0;
    };

    std::vector<PendingTx> pending;
    uint8_t nextSeq = 1;
    ReliableProtocol::TransportStats stats;
    bool connectionReady = false;
    uint32_t lastActivityMs = 0;

    void processIncoming();
    bool sendFrame(PendingTx& tx);
    bool sendRaw(std::vector<uint8_t>& frame, const char* tag, bool logErrors = true);
    void finalizePending(size_t index, ReliableProtocol::AckType type, uint8_t status);
    void sendAckFrame(uint8_t seq, bool ack, uint8_t status);
    uint8_t reserveSequence();
    bool sequenceInUse(uint8_t seq) const;
    void markConnected();

    template <typename SerialLike>
    static auto beginSerial(SerialLike& serialRef, uint32_t baud) -> decltype(serialRef.begin(baud), void()) {
        serialRef.begin(baud);
    }

    static void beginSerial(...)
    {
        // no-op when the serial-like object does not provide begin()
    }
};

} // namespace ReliableSerial
