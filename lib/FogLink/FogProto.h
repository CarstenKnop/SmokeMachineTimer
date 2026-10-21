// FogProto.h - FogLink radio wire format shared by the remote and the timers.
//
// Pure C++ (no Arduino/IDF includes) so the same code is unit-tested on a PC.
// See docs/RADIO_PROTOCOL.md for the design rationale.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace Fog {

// ---- Constants -------------------------------------------------------------
constexpr uint8_t  kMagic          = 0xF6;
constexpr uint8_t  kProtoVersion   = 2;
constexpr uint8_t  kDefaultNet     = 1;
constexpr uint8_t  kMinChannel     = 1;
constexpr uint8_t  kMaxChannel     = 13;
constexpr uint8_t  kDefaultChannel = 1;
constexpr size_t   kMaxFrame       = 250;   // ESP-NOW payload limit
constexpr size_t   kNameLen        = 10;    // 9 characters + NUL
constexpr uint8_t  kMaxTimers      = 10;

// Firmware version shared by remote and timer.
constexpr uint8_t  kFwMajor = 2, kFwMinor = 1, kFwPatch = 0;
#define FOG_FW_STRING "2.1.0"

constexpr uint32_t kMinTenths      = 1;       // 0.1 s
constexpr uint32_t kMaxTenths      = 99999;   // 9999.9 s
constexpr uint32_t kDefaultOnTenths  = 10;    // 1.0 s
constexpr uint32_t kDefaultOffTenths = 100;   // 10.0 s

constexpr uint32_t kHeartbeatMs    = 1000;    // timer -> broadcast status period
constexpr uint32_t kOnlineWindowMs = 4500;    // heard within => "online" (about 4 missed heartbeats before we call it offline)
constexpr uint32_t kBootFlagMs     = 10000;   // STATUS_JUST_BOOTED lasts this long

inline bool validChannel(uint8_t ch) { return ch >= kMinChannel && ch <= kMaxChannel; }
inline bool validTenths(uint32_t t) { return t >= kMinTenths && t <= kMaxTenths; }

// ---- Frame -----------------------------------------------------------------
//   [magic][ver][net][type][seq][len] payload[len] [crc16 LE]
// CRC-16/CCITT-FALSE over everything before the CRC.
enum class Type : uint8_t {
    Status = 1,   // timer -> remote(s): heartbeat / event / command echo
    Probe  = 2,   // remote -> broadcast or unicast: "please send STATUS"
    Cmd    = 3,   // remote -> timer
    Info   = 4,   // timer -> remote: extended diagnostics (reply to GetInfo)
    Ota    = 5,   // firmware update transfer, both directions (docs/OTA_UPDATE.md, lib/FogLink/FogOta.h)
};

constexpr size_t kHeaderLen = 6;
constexpr size_t kCrcLen    = 2;
constexpr size_t kMaxPayload = kMaxFrame - kHeaderLen - kCrcLen;

uint16_t crc16(const uint8_t* data, size_t len, uint16_t seed = 0xFFFF);

// Writes a complete frame into out. Returns the total length, or 0 if it does not fit.
size_t encodeFrame(uint8_t* out, size_t cap, uint8_t net, Type type, uint8_t seq,
                   const void* payload, size_t payloadLen);

enum class Decode : uint8_t { Ok, TooShort, BadMagic, BadVersion, BadLength, BadCrc };

struct FrameView {
    uint8_t net = 0;
    Type type = Type::Status;
    uint8_t seq = 0;
    const uint8_t* payload = nullptr;
    size_t len = 0;
};

Decode decodeFrame(const uint8_t* in, size_t n, FrameView& out);
const char* typeName(uint8_t type);

// ---- Payloads --------------------------------------------------------------
#pragma pack(push, 1)

enum StatusFlags : uint8_t {
    ST_OUTPUT_ON   = 0x01,   // relay energised right now
    ST_PHASE_ON    = 0x02,   // cycle is in its ON phase
    ST_JUST_BOOTED = 0x04,   // first kBootFlagMs after power-up
    ST_IDENTIFY    = 0x08,   // identify blink running
    ST_HEARTBEAT   = 0x10,   // periodic beat (not an event / reply)
    ST_REPLY       = 0x20,   // direct answer to a Probe or Cmd
};

enum class Mode : uint8_t { Auto = 0, ForceOn = 1, Paused = 2 };

enum class CmdResult : uint8_t { Ok = 0, Invalid = 1, Unsupported = 2, Busy = 3, Failed = 4, Stale = 5 /* issued for an earlier boot of this timer */ };

struct StatusPayload {
    uint8_t  flags;          // StatusFlags
    uint8_t  mode;           // Mode
    uint32_t onTenths;
    uint32_t offTenths;
    uint32_t elapsedMs;      // time spent in the current phase
    uint8_t  channel;        // timer's current radio channel
    int8_t   rssiAtTimer;    // RSSI of the last frame the timer heard from a remote (0 = none)
    uint8_t  lastCmdId;      // echo of the last command processed (0 = none)
    uint8_t  lastCmdResult;  // CmdResult
    uint8_t  lastCmdSrc;     // low byte of the sender MAC of that command
    uint8_t  fwMajor;
    uint8_t  fwMinor;
    uint8_t  startPhaseOn;   // persisted: phase to start in after a power-up
    uint16_t bootCount;      // persisted power-up counter (low 16 bits)
    uint32_t uptimeS;
    char     name[kNameLen];
};
static_assert(sizeof(StatusPayload) == 38, "StatusPayload layout");

enum class Op : uint8_t {
    Nop          = 0,
    SetTimes     = 1,   // u32 onTenths, u32 offTenths
    SetMode      = 2,   // u8 mode, u16 leaseMs
    Toggle       = 3,
    Reset        = 4,
    SetPhase     = 5,   // u8 phaseOn, u32 elapsedMs
    SetName      = 6,   // char[10]
    SetChannel   = 7,   // u8 channel
    FactoryReset = 8,   // u8 0xA5
    Identify     = 9,   // u16 ms
    Reboot       = 10,  // u8 0x5A
    SetStartup   = 11,  // u8 startPhaseOn
    GetInfo      = 12,
};

constexpr uint8_t kFactoryMagic = 0xA5;
constexpr uint8_t kRebootMagic  = 0x5A;
constexpr size_t  kMaxCmdArgs   = 12;

struct CmdPayload {          // variable length: 3 + argument bytes
    uint8_t cmdId;           // 1..255, 0 reserved
    uint8_t op;              // Op
    uint8_t epoch;           // boot the command was issued for (0 = any boot), see opBoundToBoot()
    uint8_t args[kMaxCmdArgs];
};
constexpr size_t kCmdFixedLen = 3;

// A command that is retried across a power cut must not run again on the rebooted timer (a replayed Toggle would
// switch the output ON right after power-up). Such commands carry the boot epoch the remote last saw; the timer
// rejects them as Stale if it has rebooted since. Idempotent commands (times, name, channel, auto...) carry 0.
inline uint8_t epochOf(uint16_t bootCount) { const uint8_t e = static_cast<uint8_t>(bootCount & 0xFF); return e ? e : 1; }

struct ProbePayload {
    uint8_t nonce;
    uint8_t flags;           // bit0: answer after a random 0..60 ms delay (broadcast probes)
};
constexpr uint8_t kProbeJitter = 0x01;

struct InfoPayload {         // timer -> remote: everything needed to judge the timer's health
    uint8_t  kind;           // 2 = timer
    uint8_t  fwMajor, fwMinor, fwPatch;
    uint8_t  resetReason;    // esp_reset_reason_t of the last boot
    uint8_t  startPhaseOn;
    uint8_t  channel;
    uint8_t  net;
    uint32_t bootCount;
    uint32_t uptimeS;
    uint32_t freeHeap;
    uint32_t minFreeHeap;
    uint32_t loopMaxUs;      // slowest main-loop pass since boot
    uint32_t rxFrames;       // valid FogLink frames
    uint32_t rxBad;          // CRC / length / version failures
    uint32_t rxForeign;      // other networks or other ESP-NOW users
    uint32_t rxDropped;      // receive queue overflow
    uint32_t rxProbes;
    uint32_t rxCmds;
    uint32_t txFrames;
    uint32_t txFail;         // unicast frames the receiver's MAC did not acknowledge
    uint32_t txDropped;      // transmit queue full
    uint32_t txBeats;
    uint32_t txReplies;
    uint32_t cmdsRun;
    uint32_t cmdsDup;        // retransmitted commands recognised and not executed twice
    uint32_t cmdsRejected;
    uint16_t rstPower, rstBrownout, rstWdt, rstPanic, rstSw, rstOther;   // lifetime reset counts
    int8_t   rssiMin, rssiAvg, rssiMax, rssiLast;   // RSSI of remote frames as heard by the timer
};
static_assert(sizeof(InfoPayload) == 100, "InfoPayload layout");

#pragma pack(pop)

inline bool opBoundToBoot(Op op, uint8_t firstArg) {
    switch (op) {
        case Op::Toggle: case Op::SetPhase: case Op::Reboot: case Op::FactoryReset: return true;
        case Op::SetMode: return firstArg == static_cast<uint8_t>(Mode::ForceOn);
        default: return false;
    }
}

// ---- Helpers ---------------------------------------------------------------
template <typename T>
inline bool readPayload(const FrameView& f, T& out, size_t minLen = sizeof(T)) {
    if (f.len < minLen || f.len > sizeof(T)) return false;
    memset(&out, 0, sizeof(T));
    memcpy(&out, f.payload, f.len);
    return true;
}

inline void putU32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24); }
inline uint32_t getU32(const uint8_t* p) { return p[0] | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
inline void putU16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
inline uint16_t getU16(const uint8_t* p) { return uint16_t(p[0] | (uint16_t(p[1]) << 8)); }

// Copies a name into a fixed field: printable ASCII only, trimmed, NUL padded.
void sanitizeName(char* dst, const char* src, size_t srcMax);

inline uint8_t macTag(const uint8_t* mac) { return mac[5]; }

}  // namespace Fog
