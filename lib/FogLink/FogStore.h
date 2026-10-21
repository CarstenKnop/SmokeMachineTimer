// FogStore.h - tiny validated blob storage on top of NVS (Preferences).
//
// NVS is log-structured and power-fail safe: an interrupted write leaves the previous value intact, which is
// exactly what a timer that loses mains power at random needs. Each blob carries a magic, a layout version
// and a CRC, and save() skips the write when nothing changed (flash wear).
#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <type_traits>
#include "FogProto.h"

namespace Fog {

template <typename T>
class BlobStore {
public:
    BlobStore(const char* ns, const char* key, uint8_t version) : ns_(ns), key_(key), version_(version) {}

    // Returns true and fills 'out' only if a valid blob of the right version was found.
    bool load(T& out) const {
        Preferences p;
        if (!p.begin(ns_, true)) return false;
        Packed packed;
        const size_t n = p.getBytes(key_, &packed, sizeof(packed));
        p.end();
        if (n != sizeof(packed)) return false;
        if (packed.magic != kMagic || packed.version != version_) return false;
        if (crc16(reinterpret_cast<const uint8_t*>(&packed.value), sizeof(T)) != packed.crc) return false;
        out = packed.value;
        return true;
    }

    // Returns true when the stored data now equals 'value'.
    bool save(const T& value) const {
        Packed packed;
        memset(static_cast<void*>(&packed), 0, sizeof(packed));   // also zeroes padding so the compare below is exact
        packed.magic = kMagic;
        packed.version = version_;
        packed.value = value;
        packed.crc = crc16(reinterpret_cast<const uint8_t*>(&packed.value), sizeof(T));
        Preferences p;
        if (!p.begin(ns_, false)) return false;
        Packed current;
        const size_t n = p.getBytes(key_, &current, sizeof(current));
        bool ok = true;
        if (n != sizeof(packed) || memcmp(&current, &packed, sizeof(packed)) != 0) {
            ok = p.putBytes(key_, &packed, sizeof(packed)) == sizeof(packed);
        }
        p.end();
        return ok;
    }

    void erase() const {
        Preferences p;
        if (p.begin(ns_, false)) { p.remove(key_); p.end(); }
    }

private:
    static constexpr uint8_t kMagic = 0xF6;
    struct Packed {
        uint8_t magic;
        uint8_t version;
        uint16_t crc;
        T value;
    };
    static_assert(std::is_trivially_copyable<T>::value, "BlobStore needs a trivially copyable type");
    const char* ns_;
    const char* key_;
    uint8_t version_;
};

// Small persistent counters (boot count, reset-cause statistics) that survive everything except a full erase.
struct LifeCounters {
    uint32_t boots = 0;
    uint16_t rstPower = 0, rstBrownout = 0, rstWdt = 0, rstPanic = 0, rstSw = 0, rstDeepSleep = 0, rstOther = 0;
};

}  // namespace Fog
