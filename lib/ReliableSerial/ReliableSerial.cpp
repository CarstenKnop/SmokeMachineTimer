#include "ReliableSerial.h"

#include <cstring>

namespace ReliableSerial {

namespace {
constexpr size_t MAX_BUFFER_BYTES = 512;
}

void Link::loop() {
    if (!serial) return;

    while (serial->available()) {
        int byteVal = serial->read();
        if (byteVal < 0) break;
        if (rxBuffer.size() >= MAX_BUFFER_BYTES) {
            rxBuffer.erase(rxBuffer.begin(), rxBuffer.begin() + rxBuffer.size() / 2);
        }
        rxBuffer.push_back(static_cast<uint8_t>(byteVal));
        markConnected();
    }

    processIncoming();

    if (pending.empty()) return;
    const uint32_t now = millis();
    size_t index = 0;
    while (index < pending.size()) {
        PendingTx& tx = pending[index];
        const uint32_t elapsed = now - tx.lastSendMs;
        const bool infinite = tx.cfg.maxAttempts == 0;
        const bool attemptsRemaining = infinite || tx.attempts < tx.cfg.maxAttempts;
        if (attemptsRemaining && elapsed >= tx.cfg.retryIntervalMs) {
            if (sendFrame(tx)) {
                ++index;
            } else {
                finalizePending(index, ReliableProtocol::AckType::Timeout, static_cast<uint8_t>(ReliableProtocol::Status::SendError));
            }
        } else if (!attemptsRemaining && elapsed >= tx.cfg.retryIntervalMs) {
            finalizePending(index, ReliableProtocol::AckType::Timeout, static_cast<uint8_t>(ReliableProtocol::Status::Timeout));
        } else {
            ++index;
        }
    }
}

bool Link::queuePacket(const void* payload, size_t len, const ReliableProtocol::SendConfig& cfg) {
    if (!serial) return false;
    if (len > MAX_PAYLOAD_BYTES) {
        Serial.printf("[ReliableSerial] Payload too large (%u > %u)\n", static_cast<unsigned>(len), static_cast<unsigned>(MAX_PAYLOAD_BYTES));
        return false;
    }

    ReliableProtocol::FrameHeader header = {};
    header.magic = ReliableProtocol::FRAME_MAGIC;
    header.version = ReliableProtocol::FRAME_VERSION;
    header.flags = cfg.requireAck ? ReliableProtocol::FLAG_ACK_REQUEST : 0;
    header.seq = cfg.requireAck ? reserveSequence() : 0;
    header.payloadLen = static_cast<uint16_t>(len);
    header.status = static_cast<uint8_t>(ReliableProtocol::Status::Ok);

    std::vector<uint8_t> frame(sizeof(ReliableProtocol::FrameHeader) + len);
    memcpy(frame.data(), &header, sizeof(ReliableProtocol::FrameHeader));
    if (len && payload) {
        memcpy(frame.data() + sizeof(ReliableProtocol::FrameHeader), payload, len);
    }
    const uint16_t crc = ReliableProtocol::crc16(frame.data(), frame.size());
    memcpy(frame.data() + offsetof(ReliableProtocol::FrameHeader, crc), &crc, sizeof(crc));

    if (!cfg.requireAck) {
        if (sendRaw(frame, cfg.tag)) {
            ++stats.txFrames;
            return true;
        }
        return false;
    }

    PendingTx tx;
    tx.frame = std::move(frame);
    tx.cfg = cfg;
    tx.seq = header.seq;
    tx.attempts = 0;
    tx.lastSendMs = millis();

    pending.push_back(std::move(tx));
    PendingTx& stored = pending.back();
    if (!sendFrame(stored)) {
        finalizePending(pending.size() - 1, ReliableProtocol::AckType::Timeout, static_cast<uint8_t>(ReliableProtocol::Status::SendError));
        return false;
    }
    ++stats.txFrames;
    return true;
}

void Link::processIncoming() {
    if (rxBuffer.size() < sizeof(ReliableProtocol::FrameHeader)) return;

    size_t offset = 0;
    while (rxBuffer.size() - offset >= sizeof(ReliableProtocol::FrameHeader)) {
        const uint8_t* base = rxBuffer.data() + offset;
        const ReliableProtocol::FrameHeader* header = reinterpret_cast<const ReliableProtocol::FrameHeader*>(base);
        if (header->magic != ReliableProtocol::FRAME_MAGIC || header->version != ReliableProtocol::FRAME_VERSION) {
            ++offset;
            continue;
        }
        if (header->payloadLen > MAX_PAYLOAD_BYTES) {
            ++stats.rxInvalidLength;
            if (header->flags & ReliableProtocol::FLAG_ACK_REQUEST) {
                sendAckFrame(header->seq, false, static_cast<uint8_t>(ReliableProtocol::Status::InvalidLength));
            }
            offset += 1;
            continue;
        }

        const size_t totalLen = sizeof(ReliableProtocol::FrameHeader) + header->payloadLen;
        if (rxBuffer.size() - offset < totalLen) {
            break; // wait for more data
        }

        std::vector<uint8_t> scratch(totalLen);
        memcpy(scratch.data(), base, totalLen);
        reinterpret_cast<ReliableProtocol::FrameHeader*>(scratch.data())->crc = 0;
        const uint16_t computedCrc = ReliableProtocol::crc16(scratch.data(), scratch.size());
        if (computedCrc != header->crc) {
            ++stats.rxCrcErrors;
            if (header->flags & ReliableProtocol::FLAG_ACK_REQUEST) {
                sendAckFrame(header->seq, false, static_cast<uint8_t>(ReliableProtocol::Status::CrcMismatch));
            }
            offset += 1;
            continue;
        }

        const bool isAck = (header->flags & ReliableProtocol::FLAG_IS_ACK) != 0;
        const bool isNak = (header->flags & ReliableProtocol::FLAG_IS_NAK) != 0;

        if (isAck || isNak) {
            bool matched = false;
            for (size_t i = 0; i < pending.size(); ++i) {
                if (pending[i].seq == header->seq) {
                    finalizePending(i, isAck ? ReliableProtocol::AckType::Ack : ReliableProtocol::AckType::Nak, header->status);
                    matched = true;
                    break;
                }
            }
            if (!matched && ackCallback) {
                ackCallback(nullptr, isAck ? ReliableProtocol::AckType::Ack : ReliableProtocol::AckType::Nak, header->status, nullptr, nullptr);
            }
            offset += totalLen;
            continue;
        }

        ++stats.rxFrames;
        if (header->flags & ReliableProtocol::FLAG_ACK_REQUEST) {
            ++stats.rxAckRequests;
        }

        ReliableProtocol::HandlerResult result{};
        if (receiveHandler) {
            const uint8_t* payload = header->payloadLen ? base + sizeof(ReliableProtocol::FrameHeader) : nullptr;
            result = receiveHandler(nullptr, payload, header->payloadLen);
        }

        if (header->flags & ReliableProtocol::FLAG_ACK_REQUEST) {
            if (!result.ack) {
                ++stats.handlerDeclined;
            }
            sendAckFrame(header->seq, result.ack, result.status);
        }

        offset += totalLen;
    }

    if (offset > 0) {
        rxBuffer.erase(rxBuffer.begin(), rxBuffer.begin() + offset);
    }
}

bool Link::sendFrame(PendingTx& tx) {
    const bool ok = sendRaw(tx.frame, tx.cfg.tag);
    if (ok) {
        tx.lastSendMs = millis();
        if (tx.attempts < 0xFF) {
            ++tx.attempts;
            if (tx.attempts > 1) {
                ++stats.txRetries;
            }
        }
    }
    return ok;
}

bool Link::sendRaw(std::vector<uint8_t>& frame, const char* tag, bool logErrors) {
    if (!serial) return false;
    size_t written = serial->write(frame.data(), frame.size());
    if (written != frame.size()) {
        if (logErrors) {
            Serial.printf("[ReliableSerial] send failed tag=%s wrote=%u expected=%u\n", tag ? tag : "-", static_cast<unsigned>(written), static_cast<unsigned>(frame.size()));
        }
        ++stats.txSendErrors;
        return false;
    }
    markConnected();
    return true;
}

void Link::finalizePending(size_t index, ReliableProtocol::AckType type, uint8_t status) {
    if (index >= pending.size()) return;
    PendingTx tx = std::move(pending[index]);
    pending.erase(pending.begin() + index);
    if (ackCallback) {
        ackCallback(nullptr, type, status, tx.cfg.userContext, tx.cfg.tag);
    }
    stats.lastAckOrNakMs = millis();
    stats.lastStatusCode = status;
    switch (type) {
        case ReliableProtocol::AckType::Ack:
            ++stats.txAcked;
            break;
        case ReliableProtocol::AckType::Nak:
            ++stats.txNak;
            break;
        case ReliableProtocol::AckType::Timeout:
            ++stats.txTimeout;
            break;
    }
}

void Link::sendAckFrame(uint8_t seq, bool ack, uint8_t status) {
    ReliableProtocol::FrameHeader header = {};
    header.magic = ReliableProtocol::FRAME_MAGIC;
    header.version = ReliableProtocol::FRAME_VERSION;
    header.flags = ack ? ReliableProtocol::FLAG_IS_ACK : ReliableProtocol::FLAG_IS_NAK;
    header.seq = seq;
    header.payloadLen = 0;
    header.status = status;
    header.crc = 0;

    std::vector<uint8_t> frame(sizeof(ReliableProtocol::FrameHeader));
    memcpy(frame.data(), &header, sizeof(ReliableProtocol::FrameHeader));
    const uint16_t crc = ReliableProtocol::crc16(frame.data(), frame.size());
    memcpy(frame.data() + offsetof(ReliableProtocol::FrameHeader, crc), &crc, sizeof(crc));
    sendRaw(frame, ack ? "ACK" : "NAK", false);
    if (ack) {
        ++stats.rxAckSent;
    } else {
        ++stats.rxNakSent;
    }
}

uint8_t Link::reserveSequence() {
    for (int attempt = 0; attempt < 255; ++attempt) {
        uint8_t candidate = nextSeq;
        nextSeq = (nextSeq == 255) ? 1 : static_cast<uint8_t>(nextSeq + 1);
        if (!sequenceInUse(candidate)) {
            return candidate;
        }
    }
    return 1;
}

bool Link::sequenceInUse(uint8_t seq) const {
    if (seq == 0) return true;
    for (const auto& tx : pending) {
        if (tx.seq == seq) return true;
    }
    return false;
}

void Link::markConnected() {
    connectionReady = true;
    lastActivityMs = millis();
}

void Link::resetStats() {
    memset(&stats, 0, sizeof(stats));
}

} // namespace ReliableSerial
