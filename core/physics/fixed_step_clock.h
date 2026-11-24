#pragma once
// Fixed-timestep accumulator. Converts real frame deltas into a bounded number
// of fixed physics steps and drops (does not replay) time beyond that bound.

#include <cstdint>

namespace dpp {

struct FixedStepConfig {
    double stepSeconds = 1.0 / 60.0;
    int maxStepsPerFrame = 4;     // catch-up limit; excess time is discarded
    double maxFrameDelta = 0.25;  // a single frame delta is clamped to this first
};

class FixedStepClock {
   public:
    explicit FixedStepClock(FixedStepConfig config = {});

    // Returns how many fixed steps the caller must run now.
    // Negative, NaN or infinite deltas are treated as zero.
    int advance(double frameDeltaSeconds);

    // Drop all accumulated time (call on pause, resume, mode switch, debugger stall).
    void reset();

    [[nodiscard]] double stepSeconds() const { return config_.stepSeconds; }
    [[nodiscard]] double accumulator() const { return accumulator_; }
    [[nodiscard]] std::uint64_t droppedSteps() const { return droppedSteps_; }
    [[nodiscard]] std::uint64_t totalSteps() const { return totalSteps_; }

   private:
    FixedStepConfig config_;
    double accumulator_ = 0.0;
    std::uint64_t droppedSteps_ = 0;
    std::uint64_t totalSteps_ = 0;
};

}  // namespace dpp
