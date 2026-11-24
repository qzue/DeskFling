#include "core/physics/physics_world.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace dpp {
namespace {

b2Vec2 toB2(Vec2 v) { return b2Vec2{v.x, v.y}; }
Vec2 fromB2(b2Vec2 v) { return Vec2{v.x, v.y}; }

// Body user data stores (index + 1) so that nullptr means "not ours" (ground).
void* encodeIndex(BodyIndex i) { return reinterpret_cast<void*>(static_cast<std::uintptr_t>(i) + 1u); }
std::optional<BodyIndex> decodeIndex(void* p) {
    const auto v = reinterpret_cast<std::uintptr_t>(p);
    if (v == 0) return std::nullopt;
    return static_cast<BodyIndex>(v - 1u);
}

struct PointQuery {
    b2Vec2 point;
    std::optional<BodyIndex> best;
};

bool pointQueryCallback(b2ShapeId shapeId, void* context) {
    auto* q = static_cast<PointQuery*>(context);
    if (!b2Shape_TestPoint(shapeId, q->point)) return true;
    const b2BodyId body = b2Shape_GetBody(shapeId);
    if (b2Body_GetType(body) != b2_dynamicBody) return true;
    if (auto idx = decodeIndex(b2Body_GetUserData(body))) {
        if (!q->best || *idx > *q->best) q->best = idx;  // later bodies draw on top
    }
    return true;
}

struct OverlapQuery {
    std::optional<BodyIndex> ignore;
    bool hit = false;
};

bool overlapCallback(b2ShapeId shapeId, void* context) {
    auto* q = static_cast<OverlapQuery*>(context);
    const b2BodyId body = b2Shape_GetBody(shapeId);
    const auto idx = decodeIndex(b2Body_GetUserData(body));
    if (idx && q->ignore && *idx == *q->ignore) return true;
    q->hit = true;
    return false;  // stop
}

}  // namespace

PhysicsWorld::PhysicsWorld(const PhysicsConfig& config) : config_(config) {
    if (!config.gravity.isFinite() || config.subSteps < 1 || config.subSteps > 64 ||
        !std::isfinite(config.maximumLinearSpeed) || config.maximumLinearSpeed <= 0 ||
        !std::isfinite(config.maxContactPushSpeed) || config.maxContactPushSpeed < 0)
        throw std::invalid_argument("invalid physics configuration");
    b2WorldDef def = b2DefaultWorldDef();
    def.gravity = toB2(config.gravity);
    def.enableSleep = config.enableSleep;
    def.maxContactPushSpeed = config.maxContactPushSpeed;
    def.maximumLinearSpeed = config.maximumLinearSpeed;
    if (config.enableHitEvents && std::isfinite(config.hitEventThreshold) &&
        config.hitEventThreshold > 0.0f) {
        def.hitEventThreshold = config.hitEventThreshold;
        lastHits_.reserve(64);
    }
    world_ = b2CreateWorld(&def);

    b2BodyDef groundDef = b2DefaultBodyDef();  // static, no user data
    ground_ = b2CreateBody(world_, &groundDef);
}

PhysicsWorld::~PhysicsWorld() {
    // Destroying the world releases all bodies, shapes and joints it owns.
    if (b2World_IsValid(world_)) b2DestroyWorld(world_);
}

void PhysicsWorld::createBoundary(Vec2 o, Vec2 size, bool withCeiling, float t, float friction,
                                  float restitution) {
    b2ShapeDef sd = b2DefaultShapeDef();
    sd.material.friction = friction;
    sd.material.restitution = restitution;
    sd.enableHitEvents = config_.enableHitEvents;
    const float hw = size.x * 0.5f;
    const float hh = size.y * 0.5f;
    // floor, left, right, (ceiling): thick boxes reduce tunnelling compared to segments.
    const b2Polygon floorBox =
        b2MakeOffsetBox(hw + t, t * 0.5f, b2Vec2{o.x + hw, o.y - t * 0.5f}, b2Rot_identity);
    const b2Polygon leftBox =
        b2MakeOffsetBox(t * 0.5f, hh + t, b2Vec2{o.x - t * 0.5f, o.y + hh}, b2Rot_identity);
    const b2Polygon rightBox =
        b2MakeOffsetBox(t * 0.5f, hh + t, b2Vec2{o.x + size.x + t * 0.5f, o.y + hh}, b2Rot_identity);
    b2CreatePolygonShape(ground_, &sd, &floorBox);
    b2CreatePolygonShape(ground_, &sd, &leftBox);
    b2CreatePolygonShape(ground_, &sd, &rightBox);
    if (withCeiling) {
        const b2Polygon ceilBox =
            b2MakeOffsetBox(hw + t, t * 0.5f, b2Vec2{o.x + hw, o.y + size.y + t * 0.5f}, b2Rot_identity);
        b2CreatePolygonShape(ground_, &sd, &ceilBox);
    }
}

void PhysicsWorld::addStaticPolygon(std::span<const Vec2> points, float cornerRadius, float friction,
                                    float restitution) {
    if (points.size() < 3 || points.size() > B2_MAX_POLYGON_VERTICES) return;
    b2Vec2 raw[B2_MAX_POLYGON_VERTICES]{};
    for (std::size_t i = 0; i < points.size(); ++i) raw[i] = toB2(points[i]);
    const b2Hull hull = b2ComputeHull(raw, static_cast<int>(points.size()));
    if (hull.count < 3) return;
    b2ShapeDef sd = b2DefaultShapeDef();
    sd.material.friction = friction;
    sd.material.restitution = restitution;
    sd.enableHitEvents = config_.enableHitEvents;
    const b2Polygon poly = b2MakePolygon(&hull, std::max(0.0f, cornerRadius));
    b2CreatePolygonShape(ground_, &sd, &poly);
}

BodyIndex PhysicsWorld::addBox(Vec2 position, Vec2 halfExtents, float angle, const BodyMaterial& m,
                               float cornerRadius) {
    const auto index = static_cast<BodyIndex>(bodies_.size());
    b2BodyDef bd = b2DefaultBodyDef();
    bd.type = b2_dynamicBody;
    bd.position = toB2(position);
    bd.rotation = b2MakeRot(angle);
    bd.linearDamping = m.linearDamping;
    bd.angularDamping = m.angularDamping;
    bd.isBullet = m.bullet;
    bd.userData = encodeIndex(index);
    const b2BodyId body = b2CreateBody(world_, &bd);

    b2ShapeDef sd = b2DefaultShapeDef();
    sd.density = m.density;
    sd.material.friction = m.friction;
    sd.material.restitution = m.restitution;
    sd.enableHitEvents = config_.enableHitEvents;
    const float r = std::clamp(cornerRadius, 0.0f, std::min(halfExtents.x, halfExtents.y) * 0.5f);
    const b2Polygon poly = r > 0.0f ? b2MakeRoundedBox(halfExtents.x - r, halfExtents.y - r, r)
                                    : b2MakeBox(halfExtents.x, halfExtents.y);
    b2CreatePolygonShape(body, &sd, &poly);
    bodies_.push_back(body);
    return index;
}

BodyIndex PhysicsWorld::addCircle(Vec2 position, float radius, const BodyMaterial& m) {
    const auto index = static_cast<BodyIndex>(bodies_.size());
    b2BodyDef bd = b2DefaultBodyDef();
    bd.type = b2_dynamicBody;
    bd.position = toB2(position);
    bd.linearDamping = m.linearDamping;
    bd.angularDamping = m.angularDamping;
    bd.isBullet = m.bullet;
    bd.userData = encodeIndex(index);
    const b2BodyId body = b2CreateBody(world_, &bd);

    b2ShapeDef sd = b2DefaultShapeDef();
    sd.density = m.density;
    sd.material.friction = m.friction;
    sd.material.restitution = m.restitution;
    sd.enableHitEvents = config_.enableHitEvents;
    const b2Circle circle{b2Vec2{0.0f, 0.0f}, radius};
    b2CreateCircleShape(body, &sd, &circle);
    bodies_.push_back(body);
    return index;
}

void PhysicsWorld::step(float dt) {
    lastHits_.clear();
    if (!(dt > 0.0f) || !std::isfinite(dt) || dt > 0.25f) return;
    b2World_Step(world_, dt, config_.subSteps);
    if (config_.enableHitEvents) {
        const b2ContactEvents ce = b2World_GetContactEvents(world_);
        const int limit = std::min(ce.hitCount, 64);
        for (int k = 0; k < limit; ++k) {
            const b2ContactHitEvent& h = ce.hitEvents[k];
            if (!b2Shape_IsValid(h.shapeIdA) || !b2Shape_IsValid(h.shapeIdB)) continue;
            if (!std::isfinite(h.approachSpeed) || h.approachSpeed <= 0.0f) continue;
            const b2BodyId ba = b2Shape_GetBody(h.shapeIdA);
            const b2BodyId bb = b2Shape_GetBody(h.shapeIdB);
            lastHits_.push_back(CollisionHit{
                decodeIndex(b2Body_GetUserData(ba)),
                decodeIndex(b2Body_GetUserData(bb)),
                fromB2(h.point),
                h.approachSpeed,
            });
        }
    }
}

BodyState PhysicsWorld::state(BodyIndex i) const {
    const b2BodyId b = id(i);
    BodyState s;
    const b2Transform xf = b2Body_GetTransform(b);
    s.position = fromB2(xf.p);
    s.angle = b2Rot_GetAngle(xf.q);
    s.unitRotation = {xf.q.c, xf.q.s};
    s.linearVelocity = fromB2(b2Body_GetLinearVelocity(b));
    s.angularVelocity = b2Body_GetAngularVelocity(b);
    s.awake = b2Body_IsAwake(b);
    s.enabled = b2Body_IsEnabled(b);
    return s;
}

Vec2 PhysicsWorld::position(BodyIndex i) const { return fromB2(b2Body_GetPosition(id(i))); }

Vec2 PhysicsWorld::linearVelocity(BodyIndex i) const { return fromB2(b2Body_GetLinearVelocity(id(i))); }

float PhysicsWorld::angularVelocity(BodyIndex i) const { return b2Body_GetAngularVelocity(id(i)); }

bool PhysicsWorld::isAwake(BodyIndex i) const { return b2Body_IsAwake(id(i)); }

bool PhysicsWorld::isEnabled(BodyIndex i) const { return b2Body_IsEnabled(id(i)); }

int PhysicsWorld::awakeBodyCount() const { return b2World_GetAwakeBodyCount(world_); }

float PhysicsWorld::mass(BodyIndex i) const { return b2Body_GetMass(id(i)); }

void PhysicsWorld::setTransform(BodyIndex i, Vec2 position, float angle) {
    b2Body_SetTransform(id(i), toB2(position), b2MakeRot(angle));
}

void PhysicsWorld::restoreTransform(BodyIndex i, Vec2 position, const BodyState& frozen) {
    b2Body_SetTransform(id(i), toB2(position), {frozen.unitRotation.x, frozen.unitRotation.y});
}
void PhysicsWorld::setVelocity(BodyIndex i, Vec2 linear, float angular) {
    b2Body_SetLinearVelocity(id(i), toB2(linear));
    b2Body_SetAngularVelocity(id(i), angular);
}

void PhysicsWorld::setEnabled(BodyIndex i, bool enabled) {
    if (grabbed_ && *grabbed_ == i && !enabled) cancelGrab();
    const b2BodyId b = id(i);
    if (enabled && !b2Body_IsEnabled(b)) b2Body_Enable(b);
    if (!enabled && b2Body_IsEnabled(b)) b2Body_Disable(b);
}

void PhysicsWorld::wake(BodyIndex i) { b2Body_SetAwake(id(i), true); }

void PhysicsWorld::wakeAll() {
    for (const b2BodyId b : bodies_) {
        if (b2Body_IsValid(b) && b2Body_IsEnabled(b)) b2Body_SetAwake(b, true);
    }
}

void PhysicsWorld::setGravity(Vec2 gravityMetersPerSec2) {
    if (!gravityMetersPerSec2.isFinite()) return;
    config_.gravity = gravityMetersPerSec2;
    b2World_SetGravity(world_, toB2(gravityMetersPerSec2));
    wakeAll();
}

Vec2 PhysicsWorld::gravity() const { return fromB2(b2World_GetGravity(world_)); }

void PhysicsWorld::setRestitution(float restitution) {
    if (!std::isfinite(restitution)) return;
    const float clamped = std::clamp(restitution, 0.0f, 0.98f);
    if (b2Body_IsValid(ground_)) {
        b2ShapeId groundShapes[16]{};
        const int count = b2Body_GetShapes(ground_, groundShapes, 16);
        for (int i = 0; i < count; ++i) {
            if (b2Shape_IsValid(groundShapes[i])) b2Shape_SetRestitution(groundShapes[i], clamped);
        }
    }
    for (const b2BodyId b : bodies_) {
        if (!b2Body_IsValid(b)) continue;
        b2ShapeId shapes[4]{};
        const int count = b2Body_GetShapes(b, shapes, 4);
        for (int i = 0; i < count; ++i) {
            if (b2Shape_IsValid(shapes[i])) b2Shape_SetRestitution(shapes[i], clamped);
        }
    }
    wakeAll();
}

std::optional<BodyIndex> PhysicsWorld::queryPoint(Vec2 point) const {
    PointQuery q{toB2(point), std::nullopt};
    const float e = 0.001f;
    const b2AABB box{b2Vec2{point.x - e, point.y - e}, b2Vec2{point.x + e, point.y + e}};
    b2World_OverlapAABB(world_, box, b2DefaultQueryFilter(), pointQueryCallback, &q);
    return q.best;
}

bool PhysicsWorld::overlapsCircle(Vec2 center, float radius, std::optional<BodyIndex> ignore) const {
    const b2Vec2 c = toB2(center);
    const b2ShapeProxy proxy = b2MakeProxy(&c, 1, radius);
    OverlapQuery q{ignore, false};
    b2World_OverlapShape(world_, &proxy, b2DefaultQueryFilter(), overlapCallback, &q);
    return q.hit;
}

bool PhysicsWorld::beginGrab(BodyIndex i, Vec2 target, const GrabConfig& config) {
    if (i >= bodies_.size() || !target.isFinite()) return false;
    if (grabbed_) cancelGrab();
    const b2BodyId b = id(i);
    if (!b2Body_IsEnabled(b)) return false;
    grabConfig_ = config;
    b2MouseJointDef jd = b2DefaultMouseJointDef();
    jd.bodyIdA = ground_;
    jd.bodyIdB = b;
    jd.target = toB2(target);  // anchor on B = this world point at creation
    jd.hertz = config.hertz;
    jd.dampingRatio = config.dampingRatio;
    jd.maxForce = config.maxForcePerKg * b2Body_GetMass(b);
    grabJoint_ = b2CreateMouseJoint(world_, &jd);
    if (B2_IS_NULL(grabJoint_)) return false;
    b2Body_SetAwake(b, true);
    grabbed_ = i;
    return true;
}

void PhysicsWorld::updateGrab(Vec2 target) {
    if (!grabbed_ || !target.isFinite()) return;
    b2MouseJoint_SetTarget(grabJoint_, toB2(target));
    b2Body_SetAwake(id(*grabbed_), true);
}

void PhysicsWorld::endGrab(std::optional<Vec2> releaseVelocity) {
    if (!grabbed_) return;
    const BodyIndex i = *grabbed_;
    if (!B2_IS_NULL(grabJoint_)) b2DestroyJoint(grabJoint_);
    grabJoint_ = b2JointId{};
    grabbed_.reset();
    if (releaseVelocity && releaseVelocity->isFinite()) {
        Vec2 v = *releaseVelocity;
        const float speed = v.length();
        if (speed > grabConfig_.maxReleaseSpeed) v = v * (grabConfig_.maxReleaseSpeed / speed);
        b2Body_SetLinearVelocity(id(i), toB2(v));
        b2Body_SetAwake(id(i), true);
    }
}

b2Profile PhysicsWorld::profile() const { return b2World_GetProfile(world_); }

}  // namespace dpp
