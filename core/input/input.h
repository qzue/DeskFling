#pragma once
// Platform-neutral input commands. Platform layers translate native events
// (WM_*, NSEvent) into these; game modes never see native types.

#include <array>
#include <cstddef>

#include "core/scene/types.h"

namespace dpp {

enum class InputType {
    PointerDown,    // primary button pressed at scenePos
    PointerMove,    // pointer moved (button state irrelevant)
    PointerUp,      // primary button released normally
    PointerCancel,  // focus lost / capture lost / button state unknown: abort any drag WITHOUT throwing
    TogglePause,    // Space
    Reset,          // R
    RequestExit,    // Esc (only when the game owns input) or emergency hotkey
};

struct InputEvent {
    InputType type = InputType::PointerMove;
    Vec2 scenePos;                         // scene points
    double time = 0.0;                     // seconds, monotonic clock of the platform
    TextureId targetTexture = kNoTexture;  // Optional native draw-order hit, captured before activation.
    bool alternate = false;  // Optional modifier (Option/Alt): orbits cinematic black hole 3D view.
};

// Estimates pointer velocity from recent samples (scene points / second).
// Uses a least-squares fit over a short time window so a single jittery
// sample or a pause before release does not produce a wild throw.
class PointerVelocityTracker {
   public:
    static constexpr std::size_t kCapacity = 16;
    static constexpr double kWindowSeconds = 0.10;
    // If the pointer was stationary this long before release, velocity is zero.
    static constexpr double kStaleSeconds = 0.08;

    void reset() {
        count_ = 0;
        head_ = 0;
    }
    void addSample(Vec2 pos, double time);
    [[nodiscard]] Vec2 velocity(double now) const;
    [[nodiscard]] std::size_t sampleCount() const { return count_; }

   private:
    struct Sample {
        Vec2 pos;
        double time;
    };
    std::array<Sample, kCapacity> samples_{};
    std::size_t count_ = 0;
    std::size_t head_ = 0;  // next write index
};

}  // namespace dpp
