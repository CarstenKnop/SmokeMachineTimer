#include "FogCycle.h"

namespace Fog {

namespace {
uint32_t clampTenths(uint32_t t) {
    if (t < kMinTenths) return kMinTenths;
    if (t > kMaxTenths) return kMaxTenths;
    return t;
}
}  // namespace

void CycleEngine::begin(uint32_t nowMs, uint32_t onTenths, uint32_t offTenths, bool startOn) {
    onT_ = clampTenths(onTenths);
    offT_ = clampTenths(offTenths);
    mode_ = Mode::Auto;
    phaseOn_ = startOn;
    phaseStart_ = nowMs;
    frozenElapsed_ = 0;
    forceUntil_ = 0;
}

void CycleEngine::freeze(uint32_t nowMs) {
    frozenElapsed_ = elapsedMs(nowMs);
}

bool CycleEngine::update(uint32_t nowMs) {
    switch (mode_) {
        case Mode::Paused:
            return false;
        case Mode::ForceOn:
            if (static_cast<int32_t>(forceUntil_ - nowMs) > 0) return false;
            // Lease ran out (remote gone / button released): go back to a fresh OFF phase.
            mode_ = Mode::Auto;
            phaseOn_ = false;
            phaseStart_ = nowMs;
            return true;
        case Mode::Auto:
            break;
    }
    bool changed = false;
    for (int guard = 0; guard < 4; ++guard) {
        const uint32_t dur = phaseDurationMs();
        const int32_t sdiff = static_cast<int32_t>(nowMs - phaseStart_);
        if (sdiff < 0 || static_cast<uint32_t>(sdiff) < dur) return changed;   // tolerate a stale 'nowMs'
        phaseStart_ += dur;      // keep the cadence exact: no drift from loop jitter
        phaseOn_ = !phaseOn_;
        changed = true;
    }
    phaseStart_ = nowMs;         // we were stalled for a very long time: resynchronise
    return changed;
}

void CycleEngine::setTimes(uint32_t onTenths, uint32_t offTenths) {
    onT_ = clampTenths(onTenths);
    offT_ = clampTenths(offTenths);
}

bool CycleEngine::setMode(Mode mode, uint32_t nowMs, uint16_t leaseMs) {
    switch (mode) {
        case Mode::Auto:
            if (mode_ == Mode::Auto) return false;
            if (mode_ == Mode::Paused) {
                phaseStart_ = nowMs - frozenElapsed_;   // resume where we stopped
            } else {                                    // leaving a force: fresh OFF phase
                phaseOn_ = false;
                phaseStart_ = nowMs;
            }
            mode_ = Mode::Auto;
            return true;
        case Mode::ForceOn: {
            uint16_t lease = leaseMs < kMinLease ? kMinLease : (leaseMs > kMaxLease ? kMaxLease : leaseMs);
            const bool already = (mode_ == Mode::ForceOn);
            if (mode_ == Mode::Auto) freeze(nowMs);
            mode_ = Mode::ForceOn;
            forceUntil_ = nowMs + lease;
            return !already;
        }
        case Mode::Paused:
            if (mode_ == Mode::Paused) return false;
            if (mode_ == Mode::Auto) freeze(nowMs);
            mode_ = Mode::Paused;
            return true;
    }
    return false;
}

void CycleEngine::toggle(uint32_t nowMs) {
    if (mode_ == Mode::ForceOn) phaseOn_ = true;        // output was on  -> toggle to off
    else if (mode_ == Mode::Paused) phaseOn_ = false;   // output was off -> toggle to on
    mode_ = Mode::Auto;
    phaseOn_ = !phaseOn_;
    phaseStart_ = nowMs;
}

void CycleEngine::reset(uint32_t nowMs) {
    mode_ = Mode::Auto;
    phaseOn_ = false;
    phaseStart_ = nowMs;
}

void CycleEngine::setPhase(bool on, uint32_t elapsedMs, uint32_t nowMs) {
    mode_ = Mode::Auto;
    phaseOn_ = on;
    const uint32_t dur = phaseDurationMs();
    if (elapsedMs >= dur) elapsedMs = dur ? dur - 1 : 0;
    phaseStart_ = nowMs - elapsedMs;
}

bool CycleEngine::outputOn(uint32_t nowMs) const {
    switch (mode_) {
        case Mode::Paused:  return false;
        case Mode::ForceOn: return static_cast<int32_t>(forceUntil_ - nowMs) > 0;
        case Mode::Auto:    return phaseOn_;
    }
    return false;
}

uint32_t CycleEngine::elapsedMs(uint32_t nowMs) const {
    if (mode_ != Mode::Auto) return frozenElapsed_;
    const int32_t d = static_cast<int32_t>(nowMs - phaseStart_);
    if (d < 0) return 0;
    const uint32_t e = static_cast<uint32_t>(d), dur = phaseDurationMs();
    return e > dur ? dur : e;
}

}  // namespace Fog
