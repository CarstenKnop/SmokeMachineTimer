// FogCycle.h - the OFF/ON cycle state machine of a timer. Pure logic, millisecond clock injected,
// so it is unit-tested on a PC and cannot be affected by radio or flash activity.
#pragma once

#include <stdint.h>
#include "FogProto.h"

namespace Fog {

class CycleEngine {
public:
    // startOn selects the phase used right after a power-up. Output is always off in OFF phase.
    void begin(uint32_t nowMs, uint32_t onTenths, uint32_t offTenths, bool startOn);

    // Advance the machine. Call often (every loop). Returns true when output/phase/mode changed.
    bool update(uint32_t nowMs);

    // Configuration (clamped to the valid range). The running phase keeps its elapsed time.
    void setTimes(uint32_t onTenths, uint32_t offTenths);

    // AUTO resumes the cycle (restarting at the beginning of the OFF phase after a force).
    // FORCE_ON keeps the output on until the lease expires; call again to extend (returns false if nothing changed).
    // PAUSED switches the output off and freezes the cycle.
    bool setMode(Mode mode, uint32_t nowMs, uint16_t leaseMs);

    void toggle(uint32_t nowMs);
    void reset(uint32_t nowMs);
    void setPhase(bool on, uint32_t elapsedMs, uint32_t nowMs);

    bool outputOn(uint32_t nowMs) const;
    bool phaseOn() const { return phaseOn_; }
    Mode mode() const { return mode_; }
    uint32_t elapsedMs(uint32_t nowMs) const;
    uint32_t onTenths() const { return onT_; }
    uint32_t offTenths() const { return offT_; }
    uint32_t phaseDurationMs() const { return (phaseOn_ ? onT_ : offT_) * 100u; }

private:
    static constexpr uint16_t kMinLease = 200, kMaxLease = 5000;
    void freeze(uint32_t nowMs);
    uint32_t onT_ = kDefaultOnTenths, offT_ = kDefaultOffTenths;
    bool phaseOn_ = false;
    Mode mode_ = Mode::Auto;
    uint32_t phaseStart_ = 0;
    uint32_t frozenElapsed_ = 0;
    uint32_t forceUntil_ = 0;
};

}  // namespace Fog
