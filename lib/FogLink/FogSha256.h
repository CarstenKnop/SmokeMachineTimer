// FogSha256.h - SHA-256 and HMAC-SHA256 in plain C++ (no Arduino / IDF includes), so the over-the-air update code runs
// unchanged in the ESP32 firmware and in the PC tests. Not tuned for speed: about 30 cycles per byte is plenty for 2 MB.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace Fog {

class Sha256 {
public:
    static constexpr size_t kDigestLen = 32;
    static constexpr size_t kBlockLen = 64;

    Sha256() { reset(); }
    void reset();
    void update(const uint8_t* data, size_t len);
    void finish(uint8_t out[kDigestLen]);          // the object must be reset() before it is used again

    static void hash(const uint8_t* data, size_t len, uint8_t out[kDigestLen]);

private:
    void block(const uint8_t* p);

    uint32_t h_[8];
    uint8_t buf_[kBlockLen];
    uint64_t total_;
    size_t fill_;
};

// HMAC-SHA256 of msg with an arbitrary-length key.
void hmacSha256(const uint8_t* key, size_t keyLen, const uint8_t* msg, size_t msgLen, uint8_t out[Sha256::kDigestLen]);

// Comparison whose run time does not depend on where the first difference is.
bool constTimeEqual(const uint8_t* a, const uint8_t* b, size_t len);

}  // namespace Fog
