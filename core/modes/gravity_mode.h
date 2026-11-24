#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/input/input.h"
#include "core/modes/game_mode.h"
#include "core/scene/coordinates.h"

namespace dpp {

enum class GravityDirection : std::uint8_t {
    Down = 0,
    Up = 1,
    Left = 2,
    Right = 3,
};

enum class GravityPreset : std::uint8_t {
    ZeroG = 0,         // 0.00 m/s^2 (0.00g, 太空失重)
    Pluto = 1,         // 0.62 m/s^2 (0.06g, 冥王星表面)
    Moon = 2,          // 1.62 m/s^2 (0.17g, 月球表面)
    Mars = 3,          // 3.72 m/s^2 (0.38g, 火星表面)
    Earth = 4,         // 9.80 m/s^2 (1.00g, 地球标准)
    Jupiter = 5,       // 24.79 m/s^2 (2.53g, 木星表面)
    Centrifuge4G = 6,  // 39.20 m/s^2 (4.00g, 超重离心)
    Custom = 7,
};

inline constexpr float kMinGravityMps2 = 0.0f;
inline constexpr float kMaxGravityMps2 = 40.0f;
inline constexpr float kEarthGravityMps2 = 9.8f;
inline constexpr float kMinGravityRestitution = 0.0f;
inline constexpr float kMaxGravityRestitution = 0.95f;
inline constexpr float kDefaultGravityRestitution = 0.15f;

[[nodiscard]] constexpr float gravityPresetMps2(GravityPreset p) {
    switch (p) {
        case GravityPreset::ZeroG:
            return 0.00f;
        case GravityPreset::Pluto:
            return 0.62f;
        case GravityPreset::Moon:
            return 1.62f;
        case GravityPreset::Mars:
            return 3.72f;
        case GravityPreset::Earth:
            return 9.80f;
        case GravityPreset::Jupiter:
            return 24.79f;
        case GravityPreset::Centrifuge4G:
            return 39.20f;
        case GravityPreset::Custom:
            break;
    }
    return kEarthGravityMps2;
}

[[nodiscard]] inline GravityPreset matchGravityPreset(float mps2, float eps = 0.05f) {
    if (!std::isfinite(mps2)) return GravityPreset::Earth;
    constexpr GravityPreset kKnown[] = {
        GravityPreset::ZeroG, GravityPreset::Pluto,   GravityPreset::Moon,         GravityPreset::Mars,
        GravityPreset::Earth, GravityPreset::Jupiter, GravityPreset::Centrifuge4G,
    };
    for (GravityPreset p : kKnown) {
        if (std::abs(mps2 - gravityPresetMps2(p)) <= eps) return p;
    }
    return GravityPreset::Custom;
}

[[nodiscard]] inline Vec2 gravityVectorFrom(float magnitudeMps2, GravityDirection dir) {
    const float g = std::isfinite(magnitudeMps2) ? std::clamp(magnitudeMps2, kMinGravityMps2, kMaxGravityMps2)
                                                 : kEarthGravityMps2;
    switch (dir) {
        case GravityDirection::Down:
            return {0.0f, -g};
        case GravityDirection::Up:
            return {0.0f, g};
        case GravityDirection::Left:
            return {-g, 0.0f};
        case GravityDirection::Right:
            return {g, 0.0f};
    }
    return {0.0f, -g};
}

struct GravityModeConfig {
    PhysicsConfig physics{{0.0f, -9.8f}, 4, true, 3.0f, 40.0f, true, 0.42f};
    float collisionScale = 0.90f;  // collision box = icon image * scale (labels never collide)
    float minHalfExtentM = 0.08f;
    BodyMaterial material{1.0f, 0.6f, 0.15f, 0.0f, 0.05f, false};
    GrabConfig grab{};
};

enum class SwallowPhase { Normal, Swallowing, Swallowed };
struct SwallowState {
    SwallowPhase phase = SwallowPhase::Normal;
    float elapsed = 0;
};
// Classic: purple ring, edge-drag (Windows, historical). Cinematic: accretion
// disk visual, drag anywhere on the shadow, icons under the pointer win.
enum class BlackHoleStyle { Classic, Cinematic };
struct BlackHoleState {
    static constexpr float kMinScale = .5f, kMaxScale = 2.0f;
    static constexpr float kDefaultElevation = .056f;                                // ~3.2 deg (NASA view)
    static constexpr float kMinElevation = -1.4835298f, kMaxElevation = 1.4835298f;  // +-85 deg
    static constexpr float kMinTilt = -3.14159265f, kMaxTilt = 3.14159265f;          // +-180 deg
    static constexpr float kDevourRampSeconds = 8.0f, kTimeWrap = 1200.0f;

    bool enabled = false;
    Vec2 center{};
    float radius = 42, range = 280, angle = 0;
    float scale = 1;                      // user size factor, clamped to [kMinScale, kMaxScale]
    float elevation = kDefaultElevation;  // 3D pitch above disk plane, radians
    float tilt = 0;                       // 2D screen roll, radians
    BlackHoleStyle style = BlackHoleStyle::Classic;
    // Devour-all (cinematic toggle): the attraction range grows smoothly over
    // kDevourRampSeconds until it covers the whole work area.
    bool devourAll = false;
    float devourElapsed = 0;
    float time = 0;  // visual clock, wrapped at kTimeWrap (multiple of renderer periods)
    std::vector<SwallowState> objects;
    // Radius/range for a work area; scale only multiplies, never changes deadlines.
    void fit(Vec2 area);
    // Classic keeps the accepted 0.55 s; cinematic dissolves into particles more slowly.
    [[nodiscard]] float swallowSeconds() const { return style == BlackHoleStyle::Cinematic ? 1.5f : .55f; }
    // Scene-point range actually used for attraction (base range unless devouring).
    [[nodiscard]] float effectiveRange(Vec2 area) const;
    // Visual approach progress in [0, 1] for an icon centre at distance d (cinematic only).
    [[nodiscard]] float tidal(float distance) const;
};
class GravityMode final : public GameMode {
   public:
    explicit GravityMode(const SceneSetup& setup, const GravityModeConfig& config = {});

    [[nodiscard]] ModeKind kind() const override { return ModeKind::Gravity; }
    void handleInput(const InputEvent& e) override;
    void stepPhysics(float dt) override;
    void updateRules(float dt) override;
    BlackHoleState blackHole() const { return hole_; }
    void restoreBlackHole(BlackHoleState state) { hole_ = std::move(state); }
    void enableBlackHole(bool enabled);
    void setBlackHoleStyle(BlackHoleStyle style) {
        hole_.style = style;
        hole_.fit(area_);
    }
    void setBlackHoleScale(float scale);
    void setBlackHoleAngles(float elevation, float tilt);
    void setBlackHoleDevour(bool devour) {
        hole_.devourAll = devour;
        hole_.devourElapsed = 0;
    }
    void setGravityParameters(float magnitudeMps2, GravityDirection direction, float restitution);
    [[nodiscard]] float gravityMagnitude() const { return gravityMagnitude_; }
    [[nodiscard]] GravityDirection gravityDirection() const { return gravityDirection_; }
    [[nodiscard]] float restitution() const { return config_.material.restitution; }
    bool blackHoleEdge(Vec2 p, bool alternate = false) const;
    bool draggingBlackHole() const { return holeDragging_ || holeOrbiting_; }
    void buildDrawList(DrawList& out) const override;
    void drainSounds(std::vector<SoundEvent>& out) override;
    [[nodiscard]] bool isIdle() const override;
    [[nodiscard]] ModeStats stats() const override;
    [[nodiscard]] const PhysicsWorld& world() const override { return world_; }

    std::vector<BodyState> bodyStates() const;
    void restoreStates(std::span<const BodyState> states, std::span<const Vec2> centers);
    const GravityModeConfig& config() const { return config_; }
    // Test/benchmark access.
    PhysicsWorld& mutableWorld() { return world_; }
    [[nodiscard]] const SceneToPhysics& coords() const { return coords_; }
    [[nodiscard]] Vec2 bodySceneCenter(std::size_t i) const;

   private:
    struct Entry {
        BodyIndex body;
        TextureId texture;
        Vec2 visualSize;  // scene points
    };

    void emitSound(SoundKind kind, float gain, float sceneX, float pitch = 1.0f);

    BlackHoleState hole_;
    Vec2 area_, holeOffset_, orbitStart_;
    float orbitElevation0_ = 0, orbitTilt0_ = 0;
    bool holeDragging_ = false, holeOrbiting_ = false;
    SceneToPhysics coords_;
    GravityModeConfig config_;
    PhysicsWorld world_;
    std::vector<Entry> entries_;
    std::vector<float> bodySoundCooldown_;
    std::vector<SoundEvent> sounds_;
    float softHitCooldown_ = 0.0f;
    float humTimer_ = 0.0f;
    float swallowSoundCooldown_ = 0.0f;
    float gravityMagnitude_ = kEarthGravityMps2;
    GravityDirection gravityDirection_ = GravityDirection::Down;
    PointerVelocityTracker tracker_;
    Vec2 pointer_;
};

}  // namespace dpp
