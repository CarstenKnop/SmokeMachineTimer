#include "ReliableEspNow.h"

#include <algorithm>
#include <esp_now.h>

namespace ReliableEspNow {

namespace {

size_t maxPayload() {
    return ESP_NOW_MAX_DATA_LEN > sizeof(ReliableProtocol::FrameHeader)
        ? (ESP_NOW_MAX_DATA_LEN - sizeof(ReliableProtocol::FrameHeader))
        : 0;
}

} // namespace

void Link::begin() {
    pending.clear();
    nextSeq = 1;
    resetStats();
}

void Link::loop() {
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

bool Link::queuePacket(const uint8_t* mac, const void* payload, size_t len, const ReliableProtocol::SendConfig& cfg) {
    if (!mac) return false;
    const size_t maxPayloadBytes = maxPayload();
    if (len > maxPayloadBytes) {
        Serial.printf("[ReliableEspNow] Payload too large (%u > %u)\n", static_cast<unsigned>(len), static_cast<unsigned>(maxPayloadBytes));
        return false;
    }

    ReliableProtocol::FrameHeader header = {};
    header.magic = ReliableProtocol::FRAME_MAGIC;
    header.version = ReliableProtocol::FRAME_VERSION;
    header.flags = cfg.requireAck ? ReliableProtocol::FLAG_ACK_REQUEST : 0;
    header.seq = cfg.requireAck ? reserveSequence() : 0;
    header.payloadLen = static_cast<uint16_t>(len);
    header.status = static_cast<uint8_t>(ReliableProtocol::Status::Ok);
    header.crc = 0;

    std::vector<uint8_t> frame(sizeof(ReliableProtocol::FrameHeader) + len);
    memcpy(frame.data(), &header, sizeof(ReliableProtocol::FrameHeader));
    if (len && payload) {
        memcpy(frame.data() + sizeof(ReliableProtocol::FrameHeader), payload, len);
    }
    const uint16_t crc = ReliableProtocol::crc16(frame.data(), frame.size());
    memcpy(frame.data() + offsetof(ReliableProtocol::FrameHeader, crc), &crc, sizeof(crc));

    if (!cfg.requireAck) {
        if (sendRaw(mac, frame, cfg.tag)) {
            ++stats.txFrames;
            return true;
        }
        return false;
    }

    PendingTx tx;
    memcpy(tx.mac, mac, 6);
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

void Link::onReceive(const uint8_t* mac, const uint8_t* data, int len) {
    if (!mac || !data || len < static_cast<int>(sizeof(ReliableProtocol::FrameHeader))) {
        return;
    }

    ReliableProtocol::FrameHeader header;
    memcpy(&header, data, sizeof(ReliableProtocol::FrameHeader));
    if (header.magic != ReliableProtocol::FRAME_MAGIC || header.version != ReliableProtocol::FRAME_VERSION) {
        return;
    }

    const size_t totalLen = sizeof(ReliableProtocol::FrameHeader) + header.payloadLen;
    if (header.payloadLen > maxPayload() || len < static_cast<int>(totalLen)) {
        ++stats.rxInvalidLength;
        if (header.flags & ReliableProtocol::FLAG_ACK_REQUEST) {
            sendAckFrame(mac, header.seq, false, static_cast<uint8_t>(ReliableProtocol::Status::InvalidLength));
        }
        return;
    }

    std::vector<uint8_t> scratch(totalLen);
    memcpy(scratch.data(), data, totalLen);
    reinterpret_cast<ReliableProtocol::FrameHeader*>(scratch.data())->crc = 0;
    const uint16_t computedCrc = ReliableProtocol::crc16(scratch.data(), scratch.size());
    if (computedCrc != header.crc) {
        ++stats.rxCrcErrors;
        if (header.flags & ReliableProtocol::FLAG_ACK_REQUEST) {
            sendAckFrame(mac, header.seq, false, static_cast<uint8_t>(ReliableProtocol::Status::CrcMismatch));
        }
        return;
    }

    const bool isAck = (header.flags & ReliableProtocol::FLAG_IS_ACK) != 0;
    const bool isNak = (header.flags & ReliableProtocol::FLAG_IS_NAK) != 0;

    if (isAck || isNak) {
        for (size_t i = 0; i < pending.size(); ++i) {
            if (pending[i].seq == header.seq && memcmp(pending[i].mac, mac, 6) == 0) {
                finalizePending(i, isAck ? ReliableProtocol::AckType::Ack : ReliableProtocol::AckType::Nak, header.status);
                return;
            }
        }
        if (ackCallback) {
            ackCallback(mac, isAck ? ReliableProtocol::AckType::Ack : ReliableProtocol::AckType::Nak, header.status, nullptr, nullptr);
        }
        return;
    }

    ++stats.rxFrames;
    if (header.flags & ReliableProtocol::FLAG_ACK_REQUEST) {
        ++stats.rxAckRequests;
    }

    ReliableProtocol::HandlerResult result{};
    if (receiveHandler) {
        const uint8_t* payload = header.payloadLen ? data + sizeof(ReliableProtocol::FrameHeader) : nullptr;
        result = receiveHandler(mac, payload, header.payloadLen);
    }

    if (header.flags & ReliableProtocol::FLAG_ACK_REQUEST) {
        if (!result.ack) {
            ++stats.handlerDeclined;
        }
        sendAckFrame(mac, header.seq, result.ack, result.status);
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

bool Link::sendFrame(PendingTx& tx) {
    const bool ok = sendRaw(tx.mac, tx.frame, tx.cfg.tag);
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

bool Link::sendRaw(const uint8_t* mac, std::vector<uint8_t>& frame, const char* tag, bool logErrors) {
    if (ensurePeer) {
        ensurePeer(mac);
    }
    if (sendHook) {
        sendHook(mac);
    }
    const esp_err_t err = esp_now_send(mac, frame.data(), frame.size());
    if (err != ESP_OK) {
        if (logErrors) {
            Serial.printf("[ReliableEspNow] send failed (%d) tag=%s\n", static_cast<int>(err), tag ? tag : "-" );
        }
        ++stats.txSendErrors;
        return false;
    }
    return true;
}

void Link::finalizePending(size_t index, ReliableProtocol::AckType type, uint8_t status) {
    if (index >= pending.size()) return;
    PendingTx tx = std::move(pending[index]);
    pending.erase(pending.begin() + index);
    if (ackCallback) {
        ackCallback(tx.mac, type, status, tx.cfg.userContext, tx.cfg.tag);
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

void Link::sendAckFrame(const uint8_t* mac, uint8_t seq, bool ack, uint8_t status) {
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
    sendRaw(mac, frame, ack ? "ACK" : "NAK", false);
    if (ack) {
        ++stats.rxAckSent;
    } else {
        ++stats.rxNakSent;
    }
}

void Link::resetStats() {
    memset(&stats, 0, sizeof(stats));
}

} // namespace ReliableEspNow
