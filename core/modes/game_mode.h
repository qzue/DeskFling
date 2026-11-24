#pragma once
// GameMode: one playable scene. A mode OWNS its PhysicsWorld, so destroying a
// mode releases every body/joint it created (mode switch and reset rebuild the
// mode). Modes never touch the system desktop; that is SessionCoordinator's job.

#include <cstdint>
#include <memory>
#include <vector>

#include "core/input/input.h"
#include "core/physics/physics_world.h"
#include "core/render/draw_list.h"
#include "core/scene/types.h"

namespace dpp {

enum class ModeKind { Gravity, Billiards, Slingshot, AirHockey };

const char* modeName(ModeKind kind);

enum class SoundKind : std::uint8_t {
    CueStrike,         // Cue tip strikes white ball
    BallCollide,       // Phenolic ball-to-ball collision
    RailBounce,        // Cushion rubber bounce
    PocketDrop,        // Ball enters pocket & drops into return tube
    SlingStretch,      // Elastic slingshot stretch creak
    SlingRelease,      // Elastic slingshot release twang
    WoodImpact,        // Wooden plank knock / fracture
    StoneImpact,       // Stone block heavy thud
    GlassShatter,      // Ice / glass strut shatter chime
    TargetPop,         // Defender icon KO / victory pop
    MalletStrike,      // Air hockey mallet-puck strike
    HockeyGoal,        // Air hockey goal celebration chord
    IconTap,           // Gravity mode icon-to-icon frosted tile clack
    IconThud,          // Gravity mode icon hitting desktop floor / wall
    IconGrab,          // Gravity mode tactile icon pickup / throw whoosh
    BlackHoleHum,      // Black hole Hann-windowed gravitational drone grain
    BlackHoleSwallow,  // Black hole event-horizon spaghettification chirp
};

struct SoundEvent {
    SoundKind kind = SoundKind::BallCollide;
    float gain = 1.0f;   // Normalized intensity in [0.05, 1.0]
    float pan = 0.0f;    // Stereo pan in [-1.0, +1.0]
    float pitch = 1.0f;  // Natural pitch variation in [0.90, 1.10]
};

struct ModeStats {
    int bodies = 0;
    int awakeBodies = 0;
    int pocketed = 0;       // billiards only
    int targets = 0;        // billiards only: object balls
    int shots = 0;          // billiards only
    bool finished = false;  // billiards only
    bool grabbing = false;  // gravity only
};

class GameMode {
   public:
    virtual ~GameMode() = default;
    [[nodiscard]] virtual ModeKind kind() const = 0;

    // Pointer events only (pause/reset/exit are handled by Playground).
    virtual void handleInput(const InputEvent& event) = 0;
    // One fixed physics step; then rule evaluation for that step.
    virtual void stepPhysics(float dt) = 0;
    virtual void updateRules(float dt) = 0;
    // Appends this mode's visuals (scene points) to an already-cleared list.
    virtual void buildDrawList(DrawList& out) const = 0;
    // Moves pending procedural sound events emitted since the last drain.
    virtual void drainSounds(std::vector<SoundEvent>& /*out*/) {}

    // True when nothing would change without new input; hosts may stop ticking.
    [[nodiscard]] virtual bool isIdle() const = 0;
    [[nodiscard]] virtual ModeStats stats() const = 0;
    [[nodiscard]] virtual const PhysicsWorld& world() const = 0;
};

std::unique_ptr<GameMode> createMode(ModeKind kind, const SceneSetup& setup);

}  // namespace dpp
