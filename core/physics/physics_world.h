#pragma once
// Thin RAII owner of one Box2D v3.1.1 world (handle-based C API only).
// All positions here are PHYSICS metres (Y up). Scene conversion is the
// caller's job via SceneToPhysics.

#include <box2d/box2d.h>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "core/scene/types.h"

namespace dpp {

struct PhysicsConfig {
    Vec2 gravity{0.0f, -9.8f};  // m/s^2, physics space (Y up)
    int subSteps = 4;           // Box2D sub-steps per fixed step; measured in benchmarks/
    bool enableSleep = true;
    float maxContactPushSpeed = 3.0f;  // m/s, Box2D default; limits overlap explosions
    float maximumLinearSpeed = 40.0f;  // m/s, far below Box2D default 400
    bool enableHitEvents = false;      // Opt-in Box2D v3.1.1 contact hit events (billiards audio)
    float hitEventThreshold = 0.18f;   // m/s approach speed threshold when enableHitEvents is true
};

using BodyIndex = std::uint32_t;

struct CollisionHit {
    std::optional<BodyIndex> bodyA;  // nullopt => static boundary cushion/wall
    std::optional<BodyIndex> bodyB;  // nullopt => static boundary cushion/wall
    Vec2 point;                      // physics metres
    float approachSpeed = 0.0f;      // m/s, positive
};

struct BodyMaterial {
    float density = 1.0f;
    float friction = 0.6f;
    float restitution = 0.1f;
    float linearDamping = 0.0f;
    float angularDamping = 0.05f;
    bool bullet = false;
};

struct BodyState {
    Vec2 position;        // metres
    float angle = 0.0f;   // radians, CCW positive
    Vec2 linearVelocity;  // m/s
    float angularVelocity = 0.0f;
    bool awake = false;
    bool enabled = true;
    Vec2 unitRotation{1, 0};  // exact Box2D cosine/sine, avoiding angle round-trip drift
};

struct GrabConfig {
    float hertz = 5.0f;
    float dampingRatio = 0.7f;
    float maxForcePerKg = 1000.0f;  // maxForce = mass * this
    float maxReleaseSpeed = 25.0f;  // m/s cap for throw velocity
};

class PhysicsWorld {
   public:
    explicit PhysicsWorld(const PhysicsConfig& config = {});
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;
    PhysicsWorld(PhysicsWorld&&) = delete;
    PhysicsWorld& operator=(PhysicsWorld&&) = delete;

    // Static box walls just outside the rectangle [min, min+size]. thickness in metres.
    void createBoundary(Vec2 minMeters, Vec2 sizeMeters, bool withCeiling, float thickness = 1.0f,
                        float friction = 0.6f, float restitution = 0.1f);
    // Static convex polygon cushion/obstacle attached to the ground body.
    void addStaticPolygon(std::span<const Vec2> points, float cornerRadius = 0.0f, float friction = 0.1f,
                          float restitution = 0.85f);

    BodyIndex addBox(Vec2 position, Vec2 halfExtents, float angle, const BodyMaterial& m,
                     float cornerRadius = 0.0f);
    BodyIndex addCircle(Vec2 position, float radius, const BodyMaterial& m);

    void step(float dt);
    [[nodiscard]] std::span<const CollisionHit> lastHits() const { return lastHits_; }
    void accelerate(BodyIndex i, Vec2 acceleration) {
        b2Body_ApplyForceToCenter(id(i), {acceleration.x * mass(i), acceleration.y * mass(i)}, true);
    }

    [[nodiscard]] std::size_t bodyCount() const { return bodies_.size(); }
    [[nodiscard]] BodyState state(BodyIndex i) const;
    [[nodiscard]] Vec2 position(BodyIndex i) const;
    [[nodiscard]] Vec2 linearVelocity(BodyIndex i) const;
    [[nodiscard]] float angularVelocity(BodyIndex i) const;
    [[nodiscard]] bool isAwake(BodyIndex i) const;
    [[nodiscard]] bool isEnabled(BodyIndex i) const;
    [[nodiscard]] int awakeBodyCount() const;
    [[nodiscard]] bool allAsleep() const { return awakeBodyCount() == 0; }
    [[nodiscard]] float mass(BodyIndex i) const;

    void setTransform(BodyIndex i, Vec2 position, float angle);
    void restoreTransform(BodyIndex i, Vec2 position, const BodyState& frozen);
    void setVelocity(BodyIndex i, Vec2 linear, float angular);
    void setEnabled(BodyIndex i, bool enabled);
    void wake(BodyIndex i);
    void wakeAll();
    void setGravity(Vec2 gravityMetersPerSec2);
    [[nodiscard]] Vec2 gravity() const;
    void setRestitution(float restitution);

    // Returns the topmost (last created) enabled dynamic body containing the point.
    [[nodiscard]] std::optional<BodyIndex> queryPoint(Vec2 point) const;
    // True if a circle of `radius` at `center` overlaps any enabled shape except `ignore`.
    [[nodiscard]] bool overlapsCircle(Vec2 center, float radius, std::optional<BodyIndex> ignore) const;

    // Grabbing via a Box2D mouse joint: no teleporting through other bodies.
    bool beginGrab(BodyIndex i, Vec2 target, const GrabConfig& config = {});
    void updateGrab(Vec2 target);
    // Ends the grab. If releaseVelocity is set it REPLACES the body velocity
    // (clamped); the joint-induced velocity is not added on top.
    void endGrab(std::optional<Vec2> releaseVelocity);
    void cancelGrab() { endGrab(std::nullopt); }
    [[nodiscard]] std::optional<BodyIndex> grabbedBody() const { return grabbed_; }

    [[nodiscard]] b2Profile profile() const;

   private:
    [[nodiscard]] b2BodyId id(BodyIndex i) const { return bodies_.at(i); }

    b2WorldId world_{};
    b2BodyId ground_{};
    std::vector<b2BodyId> bodies_;
    std::vector<CollisionHit> lastHits_;
    PhysicsConfig config_;
    GrabConfig grabConfig_;
    b2JointId grabJoint_{};
    std::optional<BodyIndex> grabbed_;
};

}  // namespace dpp
