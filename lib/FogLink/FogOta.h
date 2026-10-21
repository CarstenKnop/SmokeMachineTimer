// FogOta.h - over-the-air firmware update, timer side (docs/OTA_UPDATE.md is the contract).
//
// Pure C++ (no Arduino/IDF includes): the same receiver runs in the timer firmware and in the host tests.
// The PC (Fog Console) runs the transfer and the remote only bridges frames, so everything that has a state
// machine lives here:
//
//     OtaReceiver --- OtaTarget  (the slot that is not running: erase, write, read, activate)
//          |
//          +--------- OtaIo      (send a reply, drive the output off, reboot)
//
// All times are the caller's millis(); call tick() every loop pass. tick() may block for the duration of one flash
// sector erase (about 45 ms) and never more than one per 20 ms.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "FogSha256.h"

#ifndef FOG_OTA_KEY
#define FOG_OTA_KEY "FogLink-OTA-v1"       // override with -DFOG_OTA_KEY=\"...\" (and the same text in Fog Console's settings)
#endif

namespace Fog {

namespace Ota {

enum Op : uint8_t {
    OpBegin = 0x01,
    OpData = 0x02,
    OpPoll = 0x03,
    OpVerify = 0x04,
    OpActivate = 0x05,
    OpAbort = 0x06,
    OpState = 0x81,
};

enum State : uint8_t { Idle = 0, Erasing = 1, Receiving = 2, Verifying = 3, Verified = 4, Activating = 5, Failed = 6 };

enum Error : uint8_t {
    None = 0,
    Busy = 1,           // another session is active
    NoSlot = 2,         // no second application slot / OTA unsupported
    TooBig = 3,
    BadImage = 4,       // not an ESP32-C3 image, or not timer firmware
    HashMismatch = 5,
    FlashError = 6,
    BadRequest = 7,     // arguments, or wrong state for this request
    AuthFailed = 8,
    Timeout = 9,        // the session was dropped after 30 s of silence
    LowResources = 10,
};

constexpr uint8_t kActivateMagic = 0xA7;
constexpr size_t kBeginBodyLen = 58;                    // size u32, sha256[32], chunk u16, fw[3], flags u8, auth[16]
constexpr size_t kBeginPayloadLen = 2 + kBeginBodyLen;
constexpr size_t kStateLen = 14;                        // [0x81][session][state][error][contig u16][mask u32][progress][total u16][slot]
constexpr size_t kAuthLen = 16;
constexpr uint16_t kMinChunk = 32;
constexpr uint16_t kMaxChunk = 232;                     // 2 (op, session) + 2 (seq) + 232 = 236 <= 242 (largest FogLink payload)
constexpr uint32_t kSectorBytes = 4096;
constexpr uint32_t kEraseGapMs = 20;
constexpr uint32_t kVerifySlice = 1024;
constexpr uint32_t kSilenceMs = 30000;
constexpr uint32_t kStateGapMs = 20;                    // at most one self-initiated State per 20 ms
constexpr uint8_t kStateEvery = 8;                      // ... after this many accepted Data frames
constexpr uint32_t kActivateDelayMs = 300;
constexpr uint32_t kMaxChunks = 0x1E0000 / kMinChunk;   // bitmap size: largest slot / smallest chunk
constexpr uint32_t kMaxImageBytes = 0x1E0000;

// Message for the authentication code of a Begin: "FOGOTA1" | session | size | sha256 | chunk | fw[3] | flags.
constexpr size_t kAuthMsgLen = 7 + 1 + 4 + 32 + 2 + 3 + 1;
void buildAuthMessage(uint8_t out[kAuthMsgLen], uint8_t session, const uint8_t* beginBody /* kBeginBodyLen bytes */);
void computeAuth(const uint8_t* key, size_t keyLen, uint8_t session, const uint8_t* beginBody, uint8_t out[kAuthLen]);

// "FOGFW|timer|": a timer firmware carries it (lib/FogLink/FogIdent.h); anything else is refused.
extern const char kTimerIdent[];
constexpr size_t kTimerIdentLen = 12;

}  // namespace Ota

// The application slot that gets the new image. The platform picks the one that is NOT running.
class OtaTarget {
public:
    virtual ~OtaTarget() {}
    virtual bool available() = 0;                   // there is a second application slot (and it is not the running one)
    virtual uint32_t slotSize() = 0;                // bytes
    virtual uint8_t slotIndex() = 0;                // 0 / 1, informational
    virtual bool eraseSector(uint32_t sector) = 0;  // one 4 KiB sector of the slot; blocks for about 45 ms
    virtual bool write(uint32_t offset, const uint8_t* data, size_t len) = 0;
    virtual bool read(uint32_t offset, uint8_t* out, size_t len) = 0;
    virtual bool activate() = 0;                    // let the boot loader verify the image and boot it next time; blocks for about a second
};

class OtaIo {
public:
    virtual ~OtaIo() {}
    // Sends one OTA payload ([op][session][body]) to whoever sent the session's frames. Returns false when the radio queue is full.
    virtual bool otaSend(const uint8_t* mac, const uint8_t* payload, size_t len) = 0;
    virtual void otaQuiesce() {}                    // drive the output off: the next step blocks the loop
    virtual void otaReboot() = 0;
    virtual void otaLog(const char* /*msg*/) {}
};

class OtaReceiver {
public:
    explicit OtaReceiver(OtaIo& io);

    void setTarget(OtaTarget* target) { tgt_ = target; }
    void setKey(const uint8_t* key, size_t len);    // default: FOG_OTA_KEY

    // One OTA payload from the sender. srcMac is where the replies go.
    void onPayload(const uint8_t* srcMac, const uint8_t* payload, size_t len, uint32_t nowMs);
    void tick(uint32_t nowMs);

    // ---- introspection (tests, diagnostics) ----
    uint8_t state() const { return state_; }
    uint8_t error() const { return error_; }
    uint8_t session() const { return session_; }
    uint16_t contig() const { return static_cast<uint16_t>(contig_); }
    uint32_t total() const { return total_; }
    uint32_t imageSize() const { return size_; }
    bool capable() { return tgt_ && tgt_->available(); }
    uint32_t framesRx = 0, framesBad = 0, statesTx = 0, dupChunks = 0, chunksWritten = 0, sectorsErased = 0;

private:
    bool active() const { return state_ != Ota::Idle; }
    void sendState(const uint8_t* mac, uint8_t sessionField, uint8_t errorField, uint32_t now);
    void wantState() { wantState_ = true; }
    void maybeSendWanted(uint32_t now);
    void rateLimitedState(const uint8_t* mac, uint8_t sessionField, uint8_t errorField, uint32_t now);
    void fail(uint8_t error);
    void goIdle();
    bool sameSender(const uint8_t* mac) const;
    bool haveChunk(uint32_t i) const { return (bitmap_[i >> 3] >> (i & 7)) & 1u; }
    void setChunk(uint32_t i) { bitmap_[i >> 3] |= static_cast<uint8_t>(1u << (i & 7)); }
    uint32_t chunkLen(uint32_t seq) const { return seq + 1 == total_ ? size_ - seq * chunk_ : chunk_; }
    uint32_t mask() const;
    uint8_t progress() const;

    void onBegin(const uint8_t* mac, const uint8_t* p, size_t len, uint32_t now);
    void onData(const uint8_t* mac, const uint8_t* p, size_t len, uint32_t now);
    void onVerify(const uint8_t* mac, uint32_t now);
    void onActivate(const uint8_t* mac, const uint8_t* p, size_t len, uint32_t now);
    void onAbort(const uint8_t* mac, uint32_t now);
    void stepErase(uint32_t now);
    void stepVerify();

    OtaIo& io_;
    OtaTarget* tgt_ = nullptr;
    uint8_t key_[64];
    size_t keyLen_ = 0;

    uint8_t state_ = Ota::Idle;
    uint8_t error_ = 0;
    uint8_t session_ = 0;
    uint8_t srcMac_[6] = {0};
    uint8_t begin_[Ota::kBeginBodyLen] = {0};
    uint32_t size_ = 0, total_ = 0;
    uint16_t chunk_ = 0;
    uint32_t contig_ = 0, received_ = 0;
    uint8_t bitmap_[(Ota::kMaxChunks + 7) / 8];

    uint32_t sectors_ = 0, erased_ = 0, eraseAt_ = 0;
    uint32_t verOff_ = 0;
    Sha256 sha_;
    uint8_t identMatch_ = 0;
    bool identFound_ = false;
    uint32_t activateAt_ = 0;
    bool rebootSent_ = false;

    uint32_t lastRxMs_ = 0, lastStateMs_ = 0;
    bool stateSentOnce_ = false;
    bool wantState_ = false;
    uint8_t dataSince_ = 0;
    bool timeoutPending_ = false;
    uint32_t lastWrongMs_ = 0;
    bool wrongSentOnce_ = false;
};

}  // namespace Fog
