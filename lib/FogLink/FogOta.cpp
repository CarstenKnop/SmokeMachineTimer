#include "FogOta.h"

#include <string.h>
#include "FogProto.h"

namespace Fog {

namespace Ota {

const char kTimerIdent[] = "FOGFW|timer|";

void buildAuthMessage(uint8_t out[kAuthMsgLen], uint8_t session, const uint8_t* b) {
    size_t o = 0;
    memcpy(out + o, "FOGOTA1", 7); o += 7;
    out[o++] = session;
    memcpy(out + o, b, 4); o += 4;            // size
    memcpy(out + o, b + 4, 32); o += 32;      // sha256
    memcpy(out + o, b + 36, 2); o += 2;       // chunk
    memcpy(out + o, b + 38, 3); o += 3;       // fw
    out[o++] = b[41];                         // flags
}

void computeAuth(const uint8_t* key, size_t keyLen, uint8_t session, const uint8_t* beginBody, uint8_t out[kAuthLen]) {
    uint8_t msg[kAuthMsgLen];
    uint8_t mac[Sha256::kDigestLen];
    buildAuthMessage(msg, session, beginBody);
    hmacSha256(key, keyLen, msg, sizeof(msg), mac);
    memcpy(out, mac, kAuthLen);
}

}  // namespace Ota

namespace {
// Failure function of "FOGFW|timer|" for the streaming search (the 'F' of "FOGFW" can start a new match).
void identFailure(uint8_t* fail) {
    fail[0] = 0;
    uint8_t k = 0;
    for (size_t i = 1; i < Ota::kTimerIdentLen; ++i) {
        while (k > 0 && Ota::kTimerIdent[i] != Ota::kTimerIdent[k]) k = fail[k - 1];
        if (Ota::kTimerIdent[i] == Ota::kTimerIdent[k]) ++k;
        fail[i] = k;
    }
}
}  // namespace

OtaReceiver::OtaReceiver(OtaIo& io) : io_(io) {
    memset(bitmap_, 0, sizeof(bitmap_));
    setKey(reinterpret_cast<const uint8_t*>(FOG_OTA_KEY), strlen(FOG_OTA_KEY));
}

void OtaReceiver::setKey(const uint8_t* key, size_t len) {
    // keys longer than the buffer are reduced by HMAC itself (it hashes them), so store a digest of an over-long key
    if (len > sizeof(key_)) {
        Sha256::hash(key, len, key_);
        keyLen_ = Sha256::kDigestLen;
    } else {
        memcpy(key_, key, len);
        keyLen_ = len;
    }
}

bool OtaReceiver::sameSender(const uint8_t* mac) const { return memcmp(mac, srcMac_, 6) == 0; }

uint32_t OtaReceiver::mask() const {
    if (!active()) return 0;
    uint32_t m = 0;
    for (uint32_t i = 0; i < 32; ++i) {
        const uint32_t c = contig_ + 1 + i;
        if (c < total_ && haveChunk(c)) m |= 1u << i;
    }
    return m;
}

uint8_t OtaReceiver::progress() const {
    if (state_ == Ota::Erasing) return sectors_ ? static_cast<uint8_t>(erased_ * 100 / sectors_) : 100;
    if (state_ == Ota::Verifying) return size_ ? static_cast<uint8_t>(static_cast<uint64_t>(verOff_) * 100 / size_) : 100;
    return 100;
}

void OtaReceiver::sendState(const uint8_t* mac, uint8_t sessionField, uint8_t errorField, uint32_t now) {
    uint8_t err = errorField;
    if (err == 0 && state_ == Ota::Failed) err = error_;
    if (err == 0 && state_ == Ota::Idle && timeoutPending_) { err = Ota::Timeout; timeoutPending_ = false; }
    uint8_t p[Ota::kStateLen];
    p[0] = Ota::OpState;
    p[1] = sessionField;
    p[2] = state_;
    p[3] = err;
    putU16(p + 4, active() ? static_cast<uint16_t>(contig_) : 0);
    putU32(p + 6, mask());
    p[10] = progress();
    putU16(p + 11, active() ? static_cast<uint16_t>(total_) : 0);
    p[13] = tgt_ ? tgt_->slotIndex() : 0;
    io_.otaSend(mac, p, sizeof(p));
    ++statesTx;
    lastStateMs_ = now;
    stateSentOnce_ = true;
    wantState_ = false;
    dataSince_ = 0;
}

void OtaReceiver::maybeSendWanted(uint32_t now) {
    if (!wantState_ || !active()) return;
    if (stateSentOnce_ && static_cast<uint32_t>(now - lastStateMs_) < Ota::kStateGapMs) return;
    sendState(srcMac_, session_, 0, now);
}

void OtaReceiver::rateLimitedState(const uint8_t* mac, uint8_t sessionField, uint8_t errorField, uint32_t now) {
    // a stray or stale sender must not be able to make us transmit without limit
    if (wrongSentOnce_ && static_cast<uint32_t>(now - lastWrongMs_) < Ota::kStateGapMs) return;
    wrongSentOnce_ = true;
    lastWrongMs_ = now;
    sendState(mac, sessionField, errorField, now);
}

void OtaReceiver::fail(uint8_t error) {
    state_ = Ota::Failed;
    error_ = error;
    wantState_ = true;
    io_.otaLog("OTA failed");
}

void OtaReceiver::goIdle() {
    state_ = Ota::Idle;
    error_ = 0;
    session_ = 0;
    size_ = total_ = 0;
    contig_ = received_ = 0;
    erased_ = sectors_ = 0;
    verOff_ = 0;
    wantState_ = false;
    dataSince_ = 0;
    rebootSent_ = false;
}

// ------------------------------------------------------------------ requests
void OtaReceiver::onPayload(const uint8_t* mac, const uint8_t* p, size_t len, uint32_t now) {
    ++framesRx;
    if (len < 2) { ++framesBad; return; }
    const uint8_t op = p[0];
    const uint8_t session = p[1];

    if (!capable()) {                                   // no second slot: every request is answered "noSlot"
        if (op >= Ota::OpBegin && op <= Ota::OpAbort) sendState(mac, session, Ota::NoSlot, now);
        else ++framesBad;
        return;
    }

    if (op == Ota::OpPoll && session == 0) {            // capability probe
        if (state_ == Ota::Idle || state_ == Ota::Failed) sendState(mac, 0, 0, now);
        else sendState(mac, session_, Ota::Busy, now);
        return;
    }

    switch (op) {
        case Ota::OpBegin:
            onBegin(mac, p, len, now);
            return;
        case Ota::OpData:
        case Ota::OpPoll:
        case Ota::OpVerify:
        case Ota::OpActivate:
        case Ota::OpAbort:
            break;
        default:
            ++framesBad;
            return;
    }

    // everything else belongs to the active session
    if (state_ == Ota::Idle) {
        // nothing is going on: tell the sender (a stale frame, or a transfer that was dropped on silence)
        if (op == Ota::OpData) rateLimitedState(mac, session, 0, now);
        else sendState(mac, session, (op == Ota::OpVerify || op == Ota::OpActivate) ? Ota::BadRequest : 0, now);
        return;
    }
    if (session != session_ || !sameSender(mac)) { rateLimitedState(mac, session_, Ota::Busy, now); return; }
    lastRxMs_ = now;

    switch (op) {
        case Ota::OpData: onData(mac, p, len, now); break;
        case Ota::OpPoll: sendState(mac, session_, 0, now); break;
        case Ota::OpVerify: onVerify(mac, now); break;
        case Ota::OpActivate: onActivate(mac, p, len, now); break;
        case Ota::OpAbort: onAbort(mac, now); break;
    }
}

void OtaReceiver::onBegin(const uint8_t* mac, const uint8_t* p, size_t len, uint32_t now) {
    const uint8_t session = p[1];
    const bool running = state_ != Ota::Idle && state_ != Ota::Failed;

    if (running) {
        if (session == session_ && sameSender(mac)) {
            lastRxMs_ = now;
            if (len == Ota::kBeginPayloadLen && memcmp(p + 2, begin_, Ota::kBeginBodyLen) == 0) sendState(mac, session_, 0, now);   // same Begin again: resume
            else sendState(mac, session_, Ota::BadRequest, now);
        } else {
            sendState(mac, session_, Ota::Busy, now);
        }
        return;
    }

    if (len != Ota::kBeginPayloadLen || session == 0) { sendState(mac, session, Ota::BadRequest, now); return; }
    const uint8_t* b = p + 2;

    uint8_t auth[Ota::kAuthLen];
    Ota::computeAuth(key_, keyLen_, session, b, auth);
    if (!constTimeEqual(auth, b + 42, Ota::kAuthLen)) { sendState(mac, session, Ota::AuthFailed, now); return; }

    const uint32_t size = getU32(b);
    const uint16_t chunk = getU16(b + 36);
    if (size == 0 || chunk < Ota::kMinChunk || chunk > Ota::kMaxChunk) { sendState(mac, session, Ota::BadRequest, now); return; }
    const uint32_t slot = tgt_->slotSize();
    const uint32_t total = (size + chunk - 1) / chunk;
    if (size > slot || size > Ota::kMaxImageBytes || total > Ota::kMaxChunks || total > 0xFFFF) { sendState(mac, session, Ota::TooBig, now); return; }

    // accept: from now on this is the active session
    session_ = session;
    memcpy(srcMac_, mac, 6);
    memcpy(begin_, b, Ota::kBeginBodyLen);
    size_ = size;
    chunk_ = chunk;
    total_ = total;
    contig_ = received_ = 0;
    memset(bitmap_, 0, (total + 7) / 8);
    sectors_ = (size + Ota::kSectorBytes - 1) / Ota::kSectorBytes;
    erased_ = 0;
    eraseAt_ = now;
    verOff_ = 0;
    error_ = 0;
    state_ = Ota::Erasing;
    lastRxMs_ = now;
    timeoutPending_ = false;
    wantState_ = false;
    dataSince_ = 0;
    io_.otaLog("OTA begin");
    sendState(mac, session_, 0, now);
}

void OtaReceiver::onData(const uint8_t* mac, const uint8_t* p, size_t len, uint32_t now) {
    (void)mac;
    if (state_ != Ota::Receiving) {                      // still erasing, or already past the data phase: let the sender know where we are
        wantState_ = true;
        maybeSendWanted(now);
        return;
    }
    if (len < 4) { ++framesBad; return; }
    const uint32_t seq = getU16(p + 2);
    const size_t n = len - 4;
    if (seq >= total_ || n != chunkLen(seq)) { ++framesBad; wantState_ = true; maybeSendWanted(now); return; }

    if (haveChunk(seq)) {                                // our State was probably lost: say it again soon
        ++dupChunks;
        wantState_ = true;
        maybeSendWanted(now);
        return;
    }

    const uint8_t* data = p + 4;
    if (seq == 0 && (n < 14 || data[0] != 0xE9 || getU16(data + 12) != 5)) { fail(Ota::BadImage); maybeSendWanted(now); return; }
    if (!tgt_->write(seq * static_cast<uint32_t>(chunk_), data, n)) { fail(Ota::FlashError); maybeSendWanted(now); return; }

    setChunk(seq);
    ++received_;
    ++chunksWritten;
    while (contig_ < total_ && haveChunk(contig_)) ++contig_;
    if (++dataSince_ >= Ota::kStateEvery) wantState_ = true;
    if (seq > contig_) wantState_ = true;                // a gap below this chunk
    if (contig_ == total_) wantState_ = true;            // everything is here
    maybeSendWanted(now);
}

void OtaReceiver::onVerify(const uint8_t* mac, uint32_t now) {
    if (state_ == Ota::Verifying || state_ == Ota::Verified) { sendState(mac, session_, 0, now); return; }   // again: idempotent
    if (state_ != Ota::Receiving || contig_ != total_) { sendState(mac, session_, Ota::BadRequest, now); return; }
    sha_.reset();
    verOff_ = 0;
    identMatch_ = 0;
    identFound_ = false;
    state_ = Ota::Verifying;
    sendState(mac, session_, 0, now);
}

void OtaReceiver::onActivate(const uint8_t* mac, const uint8_t* p, size_t len, uint32_t now) {
    if (state_ == Ota::Activating) { sendState(mac, session_, 0, now); return; }
    if (len != 3 || p[2] != Ota::kActivateMagic || state_ != Ota::Verified) { sendState(mac, session_, Ota::BadRequest, now); return; }
    io_.otaQuiesce();                                    // the next call blocks the loop for about a second
    if (!tgt_->activate()) {
        fail(Ota::FlashError);
        sendState(mac, session_, 0, now);
        return;
    }
    state_ = Ota::Activating;
    activateAt_ = now + Ota::kActivateDelayMs;
    rebootSent_ = false;
    io_.otaLog("OTA activated");
    sendState(mac, session_, 0, now);
}

void OtaReceiver::onAbort(const uint8_t* mac, uint32_t now) {
    if (state_ == Ota::Activating) { sendState(mac, session_, 0, now); return; }   // too late: the new image is selected already
    const uint8_t session = session_;
    goIdle();
    sendState(mac, session, 0, now);
}

// ------------------------------------------------------------------ background work
void OtaReceiver::stepErase(uint32_t now) {
    if (static_cast<int32_t>(now - eraseAt_) < 0) return;
    if (erased_ < sectors_) {
        if (!tgt_->eraseSector(erased_)) { fail(Ota::FlashError); return; }
        ++erased_;
        ++sectorsErased;
        eraseAt_ = now + Ota::kEraseGapMs;
    }
    if (erased_ >= sectors_) {
        state_ = Ota::Receiving;
        wantState_ = true;
    }
}

void OtaReceiver::stepVerify() {
    static uint8_t kmp[Ota::kTimerIdentLen];
    static bool kmpReady = false;
    if (!kmpReady) { identFailure(kmp); kmpReady = true; }

    uint8_t buf[Ota::kVerifySlice];
    const uint32_t n = size_ - verOff_ < Ota::kVerifySlice ? size_ - verOff_ : Ota::kVerifySlice;
    if (n) {
        if (!tgt_->read(verOff_, buf, n)) { fail(Ota::FlashError); return; }
        sha_.update(buf, n);
        for (uint32_t i = 0; i < n && !identFound_; ++i) {
            while (identMatch_ > 0 && buf[i] != static_cast<uint8_t>(Ota::kTimerIdent[identMatch_])) identMatch_ = kmp[identMatch_ - 1];
            if (buf[i] == static_cast<uint8_t>(Ota::kTimerIdent[identMatch_])) ++identMatch_;
            if (identMatch_ == Ota::kTimerIdentLen) identFound_ = true;
        }
        verOff_ += n;
    }
    if (verOff_ < size_) return;

    uint8_t digest[Sha256::kDigestLen];
    sha_.finish(digest);
    if (!constTimeEqual(digest, begin_ + 4, Sha256::kDigestLen)) { fail(Ota::HashMismatch); return; }
    if (!identFound_) { fail(Ota::BadImage); return; }
    state_ = Ota::Verified;
    wantState_ = true;
}

void OtaReceiver::tick(uint32_t now) {
    if (!tgt_) return;

    if (state_ == Ota::Erasing || state_ == Ota::Receiving || state_ == Ota::Verifying || state_ == Ota::Verified) {
        if (static_cast<uint32_t>(now - lastRxMs_) > Ota::kSilenceMs) {
            io_.otaLog("OTA session timed out");
            goIdle();
            timeoutPending_ = true;
            return;
        }
    }

    switch (state_) {
        case Ota::Erasing: stepErase(now); break;
        case Ota::Verifying: stepVerify(); break;
        case Ota::Activating:
            if (!rebootSent_ && static_cast<int32_t>(now - activateAt_) >= 0) {
                rebootSent_ = true;
                io_.otaReboot();
            }
            break;
        default: break;
    }
    maybeSendWanted(now);
}

}  // namespace Fog
