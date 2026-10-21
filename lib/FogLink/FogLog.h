// FogLog.h - allocation-free log ring. Producers (main loop only) write, the USB link drains it.
// Logging never blocks and never touches the serial port directly.
#pragma once

#include <Arduino.h>
#include <stdarg.h>

namespace Fog {

enum class LogLevel : uint8_t { Off = 0, Error = 1, Warn = 2, Info = 3, Debug = 4 };

const char* logLevelName(LogLevel l);
LogLevel logLevelFromName(const char* name, LogLevel fallback);

class LogRing {
public:
    struct Entry {
        uint32_t ms;
        LogLevel level;
        char src[8];
        char msg[110];
    };

    void add(LogLevel level, const char* src, const char* fmt, ...) __attribute__((format(printf, 4, 5)));
    bool pop(Entry& out);
    uint32_t dropped() const { return dropped_; }

private:
    static constexpr uint8_t kSize = 32;
    Entry ring_[kSize];
    uint8_t head_ = 0, tail_ = 0;
    uint32_t dropped_ = 0;
};

LogRing& logRing();

}  // namespace Fog

#define FLOGE(src, ...) ::Fog::logRing().add(::Fog::LogLevel::Error, src, __VA_ARGS__)
#define FLOGW(src, ...) ::Fog::logRing().add(::Fog::LogLevel::Warn, src, __VA_ARGS__)
#define FLOGI(src, ...) ::Fog::logRing().add(::Fog::LogLevel::Info, src, __VA_ARGS__)
#define FLOGD(src, ...) ::Fog::logRing().add(::Fog::LogLevel::Debug, src, __VA_ARGS__)
