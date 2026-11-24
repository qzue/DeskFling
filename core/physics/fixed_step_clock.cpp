#include "core/physics/fixed_step_clock.h"

#include <cmath>
#include <stdexcept>

namespace dpp {

FixedStepClock::FixedStepClock(FixedStepConfig config) : config_(config) {
    if (!std::isfinite(config_.stepSeconds) || config_.stepSeconds < 0.001 || config_.stepSeconds > 1.0 ||
        config_.maxStepsPerFrame < 1 || config_.maxStepsPerFrame > 1000 ||
        !std::isfinite(config_.maxFrameDelta) || config_.maxFrameDelta <= 0 || config_.maxFrameDelta > 60)
        throw std::invalid_argument("invalid fixed step configuration");
}

int FixedStepClock::advance(double dt) {
    if (!std::isfinite(dt) || dt <= 0.0) return 0;
    if (dt > config_.maxFrameDelta) dt = config_.maxFrameDelta;
    accumulator_ += dt;

    int steps = static_cast<int>(std::floor(accumulator_ / config_.stepSeconds));
    if (steps > config_.maxStepsPerFrame) {
        droppedSteps_ += static_cast<std::uint64_t>(steps - config_.maxStepsPerFrame);
        steps = config_.maxStepsPerFrame;
        accumulator_ = 0.0;  // discard backlog instead of spiralling
    } else {
        accumulator_ -= steps * config_.stepSeconds;
        // Guard against floating error leaving a tiny negative remainder.
        if (accumulator_ < 0.0) accumulator_ = 0.0;
    }
    totalSteps_ += static_cast<std::uint64_t>(steps);
    return steps;
}

void FixedStepClock::reset() { accumulator_ = 0.0; }

}  // namespace dpp
