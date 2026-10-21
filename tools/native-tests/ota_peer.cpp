// ota_peer.cpp - the timer's over-the-air update receiver as a stand-alone program, for cross-language tests.
//
// Fog Console's tests drive it through stdin/stdout (text lines, lower-case hex, no spaces) to prove that the C# sender and the
// C++ receiver speak the same protocol (docs/OTA_UPDATE.md):
//
//   arguments:  --key <text>          OTA key (default FogLink-OTA-v1)
//               --slot-size <bytes>   size of the slot that is written (default 0x1E0000)
//               --no-slot             there is no second application slot (the probe answers noSlot)
//               --running-slot <0|1>  the slot that is running; the other one is written (default 0)
//   stdin       P <hex>   an OTA payload [op][session][body] as received from the remote
//                         -> zero or more "P <hex>" lines (payloads the timer sends back), then "OK"
//               T <ms>    set the virtual time (ms since start, monotonic) and run the receiver's tick
//                         -> "P <hex>" lines for what the tick sent, then "OK"
//               R         power cut: the receiver loses its RAM state, the flash content stays            -> "OK"
//               H         "H <sha256 of the first <size> bytes of the slot>" (size of the last Begin, "H 0" if none),
//                         then "S <state> <error> <contig> <total> <slot>"
//               Q         "OK" and exit
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include "ota_test_support.h"

using namespace OtaTest;

namespace {
struct PeerIo : OtaIo {
    std::vector<Bytes> out;
    int reboots = 0;
    bool otaSend(const uint8_t*, const uint8_t* payload, size_t len) override { out.emplace_back(payload, payload + len); return true; }
    void otaReboot() override { ++reboots; }
};

void flushReplies(PeerIo& io) {
    for (const Bytes& b : io.out) std::printf("P %s\n", hex(b).c_str());
    io.out.clear();
}
}  // namespace

int main(int argc, char** argv) {
    std::string key = "FogLink-OTA-v1";
    uint32_t slotSize = 0x1E0000;
    bool noSlot = false;
    int running = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--key" && i + 1 < argc) key = argv[++i];
        else if (a == "--slot-size" && i + 1 < argc) slotSize = static_cast<uint32_t>(std::strtoull(argv[++i], nullptr, 0));
        else if (a == "--no-slot") noSlot = true;
        else if (a == "--running-slot" && i + 1 < argc) running = std::atoi(argv[++i]) ? 1 : 0;
    }

    RamTarget target(slotSize);
    target.avail = !noSlot;
    target.slot = static_cast<uint8_t>(1 - running);
    PeerIo io;
    auto make = [&]() {
        std::unique_ptr<OtaReceiver> r(new OtaReceiver(io));
        r->setTarget(&target);
        r->setKey(reinterpret_cast<const uint8_t*>(key.data()), key.size());
        return r;
    };
    auto rx = make();

    const uint8_t src[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    uint32_t now = 0, lastSize = 0;
    std::string line;
    while (std::getline(std::cin, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) continue;
        const char cmd = line[0];
        const std::string arg = line.size() > 2 ? line.substr(2) : "";
        if (cmd == 'P') {
            const Bytes p = unhex(arg);
            if (p.size() == Ota::kBeginPayloadLen && p[0] == Ota::OpBegin) lastSize = getU32(p.data() + 2);
            rx->onPayload(src, p.data(), p.size(), now);
            flushReplies(io);
            std::printf("OK\n");
        } else if (cmd == 'T') {
            const uint32_t t = static_cast<uint32_t>(std::strtoull(arg.c_str(), nullptr, 10));
            if (static_cast<int32_t>(t - now) > 0) now = t;
            rx->tick(now);
            flushReplies(io);
            std::printf("OK\n");
        } else if (cmd == 'R') {
            rx = make();
            io.out.clear();
            std::printf("OK\n");
        } else if (cmd == 'H') {
            if (lastSize == 0 || lastSize > target.flash.size()) {
                std::printf("H 0\n");
            } else {
                uint8_t d[Sha256::kDigestLen];
                Sha256::hash(target.flash.data(), lastSize, d);
                std::printf("H %s\n", hex(d, sizeof(d)).c_str());
            }
            std::printf("S %u %u %u %u %u\n", rx->state(), rx->error(), rx->contig(), rx->total(), target.slot);
        } else if (cmd == 'Q') {
            std::printf("OK\n");
            std::fflush(stdout);
            return 0;
        } else {
            std::printf("ERR\n");
        }
        std::fflush(stdout);
    }
    return 0;
}
