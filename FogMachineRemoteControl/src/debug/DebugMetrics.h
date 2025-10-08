// DebugMetrics.h
// Aggregates per-frame and per-loop diagnostic information to avoid serial spam.
#pragma once
#include <Arduino.h>

class DebugMetrics {
public:
    static DebugMetrics& instance();

    void recordDisplayFrame(uint32_t prepMs, uint32_t flushMs, uint32_t totalMs) {
        frameCount++; sumPrep += prepMs; sumFlush += flushMs; sumTotal += totalMs;
        if (flushMs > maxFlush) maxFlush = flushMs;
    }
    void recordSlowFlush() { slowFlushes++; }

    // Accessors for aggregation
    uint32_t getFrameCount() const { return frameCount; }
    uint32_t getAvgPrep() const { return frameCount ? sumPrep / frameCount : 0; }
    uint32_t getAvgFlush() const { return frameCount ? sumFlush / frameCount : 0; }
    uint32_t getAvgTotal() const { return frameCount ? sumTotal / frameCount : 0; }
    uint32_t getMaxFlush() const { return maxFlush; }
    uint32_t getSlowFlushes() const { return slowFlushes; }

    void resetDisplay() { frameCount=0; sumPrep=0; sumFlush=0; sumTotal=0; maxFlush=0; slowFlushes=0; }

    // Progress bar tracking (menu entry hold)
    void recordProgress(float pct) { progressFrames++; lastProgressPct = pct; }
    uint32_t getProgressFrames() const { return progressFrames; }
    float getLastProgressPct() const { return lastProgressPct; }
    void resetProgress() { progressFrames = 0; lastProgressPct = 0.f; }

private:
    DebugMetrics() = default;
    uint32_t frameCount = 0;
    uint32_t sumPrep = 0;
    uint32_t sumFlush = 0;
    uint32_t sumTotal = 0;
    uint32_t maxFlush = 0;
    uint32_t slowFlushes = 0;
    uint32_t progressFrames = 0;
    float lastProgressPct = 0.f;
};
