// DebugHooks.cpp - lightweight global C-style hooks to avoid including headers in DisplayManager
#include "debug/DebugMetrics.h"

extern "C" void debugRecordDisplayFrame(uint32_t prep, uint32_t flush, uint32_t total) {
    DebugMetrics::instance().recordDisplayFrame(prep, flush, total);
}

extern "C" void debugRecordSlowFlush() {
    DebugMetrics::instance().recordSlowFlush();
}
