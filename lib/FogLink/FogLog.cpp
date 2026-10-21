#include "FogLog.h"

#include <stdio.h>
#include <string.h>

namespace Fog {

const char* logLevelName(LogLevel l) {
    switch (l) {
        case LogLevel::Error: return "error";
        case LogLevel::Warn:  return "warn";
        case LogLevel::Info:  return "info";
        case LogLevel::Debug: return "debug";
        default:              return "off";
    }
}

LogLevel logLevelFromName(const char* name, LogLevel fallback) {
    if (!name) return fallback;
    if (!strcmp(name, "error")) return LogLevel::Error;
    if (!strcmp(name, "warn")) return LogLevel::Warn;
    if (!strcmp(name, "info")) return LogLevel::Info;
    if (!strcmp(name, "debug")) return LogLevel::Debug;
    if (!strcmp(name, "off")) return LogLevel::Off;
    return fallback;
}

void LogRing::add(LogLevel level, const char* src, const char* fmt, ...) {
    const uint8_t next = static_cast<uint8_t>((head_ + 1) % kSize);
    if (next == tail_) { ++dropped_; return; }         // full: drop the newest, never block
    Entry& e = ring_[head_];
    e.ms = millis();
    e.level = level;
    strncpy(e.src, src ? src : "", sizeof(e.src) - 1);
    e.src[sizeof(e.src) - 1] = 0;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e.msg, sizeof(e.msg), fmt, ap);
    va_end(ap);
    head_ = next;
}

bool LogRing::pop(Entry& out) {
    if (tail_ == head_) return false;
    out = ring_[tail_];
    tail_ = static_cast<uint8_t>((tail_ + 1) % kSize);
    return true;
}

LogRing& logRing() {
    static LogRing ring;
    return ring;
}

}  // namespace Fog
