#include "core/modes/gravity_mode.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace dpp {

namespace {
float wrapTilt(float rad) {
    constexpr float kPi = 3.14159265f, kTwoPi = 6.2831853f;
    rad = std::fmod(rad + kPi, kTwoPi);
    if (rad < 0) rad += kTwoPi;
    return std::clamp(rad - kPi, BlackHoleState::kMinTilt, BlackHoleState::kMaxTilt);
}
}  // namespace

void BlackHoleState::fit(Vec2 area) {
    scale = std::isfinite(scale) ? std::clamp(scale, kMinScale, kMaxScale) : 1.0f;
    elevation =
        std::isfinite(elevation) ? std::clamp(elevation, kMinElevation, kMaxElevation) : kDefaultElevation;
    tilt = std::isfinite(tilt) ? std::clamp(tilt, kMinTilt, kMaxTilt) : 0.0f;
    const float side = std::min(area.x, area.y);
    if (style == BlackHoleStyle::Cinematic) {
        radius = (side * 0.115f) * scale;
        range = (side * 0.45f) * scale;
    } else {
        radius = std::min(42.0f, side * .15f) * scale;
        range = std::min(280.0f, side * .45f) * scale;
    }
    center = {std::clamp(center.x, radius, std::max(radius, area.x - radius)),
              std::clamp(center.y, radius, std::max(radius, area.y - radius))};
}
float BlackHoleState::effectiveRange(Vec2 area) const {
    if (!devourAll) return range;
    // Far enough that every point of the work area keeps >= 60% of the pull.
    const float target = std::max(range, Vec2{area.x, area.y}.length() * 2.5f);
    const float t = std::clamp(devourElapsed / kDevourRampSeconds, 0.0f, 1.0f);
    return range + (target - range) * t * t * (3 - 2 * t);
}
float BlackHoleState::tidal(float distance) const {
    if (!enabled || style != BlackHoleStyle::Cinematic || !std::isfinite(distance)) return 0;
    const float outer = radius * 3.2f, inner = radius * .65f;
    return std::clamp((outer - distance) / (outer - inner), 0.0f, 1.0f);
}

GravityMode::GravityMode(const SceneSetup& setup, const GravityModeConfig& config)
    : coords_(setup.workAreaSize), config_(config), world_(config.physics) {
    area_ = setup.workAreaSize;
    const Vec2 gv = config_.physics.gravity;
    gravityMagnitude_ = std::clamp(gv.length(), kMinGravityMps2, kMaxGravityMps2);
    if (std::abs(gv.x) > std::abs(gv.y)) {
        gravityDirection_ = gv.x >= 0.0f ? GravityDirection::Right : GravityDirection::Left;
    } else {
        gravityDirection_ = gv.y > 0.0f ? GravityDirection::Up : GravityDirection::Down;
    }
    hole_.center = (setup.primaryArenaRect.width > 0.0f && setup.primaryArenaRect.height > 0.0f)
                       ? rectCenter(setup.primaryArenaRect)
                       : (area_ * 0.5f);
    hole_.fit(area_);
    hole_.objects.resize(setup.icons.size());
    const Vec2 worldSize = coords_.worldSizeMeters();
    const float boundaryRestitution =
        std::clamp(std::min(0.1f, config_.material.restitution), kMinGravityRestitution, kMaxGravityRestitution);
    world_.createBoundary({0.0f, 0.0f}, worldSize, /*withCeiling=*/true, 1.0f, 0.6f, boundaryRestitution);
    if (!setup.screenWorkAreas.empty()) {
        for (const Rect& v : computeMultiScreenVoidRects(setup.workAreaSize, setup.screenWorkAreas)) {
            if (v.width < 1.0f || v.height < 1.0f) continue;
            const Vec2 bl = coords_.toPhysics({v.x, v.y + v.height});
            const Vec2 br = coords_.toPhysics({v.x + v.width, v.y + v.height});
            const Vec2 tr = coords_.toPhysics({v.x + v.width, v.y});
            const Vec2 tl = coords_.toPhysics({v.x, v.y});
            const Vec2 quad[4] = {bl, br, tr, tl};
            world_.addStaticPolygon(quad, 0.0f, 0.6f, boundaryRestitution);
        }
    }

    entries_.reserve(setup.icons.size());
    bodySoundCooldown_.assign(setup.icons.size(), 0.0f);
    sounds_.reserve(16);
    for (const IconSeed& icon : setup.icons) {
        if (!icon.iconSize.isFinite() || icon.iconSize.x <= 0 || icon.iconSize.y <= 0 ||
            icon.iconSize.x > setup.workAreaSize.x || icon.iconSize.y > setup.workAreaSize.y)
            throw std::invalid_argument("icon size outside work area");
        Vec2 half{coords_.lengthToPhysics(icon.iconSize.x) * 0.5f * config_.collisionScale,
                  coords_.lengthToPhysics(icon.iconSize.y) * 0.5f * config_.collisionScale};
        half.x = std::max(half.x, config_.minHalfExtentM);
        half.y = std::max(half.y, config_.minHalfExtentM);
        // Keep shapes fully inside the boundary (and inside the nearest screen work area on multi-screen
        // span so no seed overlaps a static void-rect obstacle on frame 0).
        Vec2 seedScene = icon.center.isFinite() ? icon.center : Vec2{0, 0};
        if (!setup.screenWorkAreas.empty()) {
            const Vec2 halfSc = icon.iconSize * 0.5f;
            std::size_t bestIdx = 0;
            float bestDist2 = 1e30f;
            for (std::size_t si = 0; si < setup.screenWorkAreas.size(); ++si) {
                const Rect& sw = setup.screenWorkAreas[si];
                if (sw.width <= 0.0f || sw.height <= 0.0f) continue;
                const float cx = std::clamp(seedScene.x, sw.x, sw.x + sw.width);
                const float cy = std::clamp(seedScene.y, sw.y, sw.y + sw.height);
                const float dx = seedScene.x - cx;
                const float dy = seedScene.y - cy;
                const float d2 = dx * dx + dy * dy;
                if (d2 < bestDist2) {
                    bestDist2 = d2;
                    bestIdx = si;
                }
            }
            const Rect& owner = setup.screenWorkAreas[bestIdx];
            if (owner.width >= icon.iconSize.x && owner.height >= icon.iconSize.y) {
                seedScene.x = std::clamp(seedScene.x, owner.x + halfSc.x + 0.5f,
                                         std::max(owner.x + halfSc.x + 0.5f, owner.x + owner.width - halfSc.x - 0.5f));
                seedScene.y = std::clamp(seedScene.y, owner.y + halfSc.y + 0.5f,
                                         std::max(owner.y + halfSc.y + 0.5f, owner.y + owner.height - halfSc.y - 0.5f));
            }
        }
        Vec2 p = coords_.toPhysics(seedScene);
        p.x = std::clamp(p.x, half.x, std::max(half.x, worldSize.x - half.x));
        p.y = std::clamp(p.y, half.y, std::max(half.y, worldSize.y - half.y));
        const float corner = std::min(half.x, half.y) * 0.2f;
        const BodyIndex b = world_.addBox(p, half, 0.0f, config_.material, corner);
        entries_.push_back(Entry{b, icon.texture, icon.iconSize});
    }
}

void GravityMode::handleInput(const InputEvent& e) {
    if (!e.scenePos.isFinite() || !std::isfinite(e.time)) return;
    if (e.type == InputType::PointerCancel || e.type == InputType::PointerUp)
        holeDragging_ = holeOrbiting_ = false;
    if (e.type == InputType::PointerDown && hole_.style == BlackHoleStyle::Cinematic && e.alternate &&
        blackHoleEdge(e.scenePos, true)) {
        world_.cancelGrab();
        holeOrbiting_ = true;
        orbitStart_ = e.scenePos;
        orbitElevation0_ = hole_.elevation;
        orbitTilt0_ = hole_.tilt;
        emitSound(SoundKind::BlackHoleHum, 0.20f, hole_.center.x, 1.08f);
        return;
    }
    if (e.type == InputType::PointerDown && blackHoleEdge(e.scenePos) &&
        (hole_.style == BlackHoleStyle::Classic || e.targetTexture == kNoTexture)) {
        world_.cancelGrab();
        holeDragging_ = true;
        holeOffset_ = hole_.center - e.scenePos;
        emitSound(SoundKind::BlackHoleHum, 0.22f, hole_.center.x, 1.04f);
        return;
    }
    if (e.type == InputType::PointerMove && holeOrbiting_) {
        const Vec2 d = e.scenePos - orbitStart_;
        setBlackHoleAngles(orbitElevation0_ + d.y * .012f, wrapTilt(orbitTilt0_ + d.x * .012f));
        return;
    }
    if (e.type == InputType::PointerMove && holeDragging_) {
        const auto p = e.scenePos + holeOffset_;
        hole_.center = {std::clamp(p.x, hole_.radius, area_.x - hole_.radius),
                        std::clamp(p.y, hole_.radius, area_.y - hole_.radius)};
        return;
    }
    switch (e.type) {
        case InputType::PointerDown: {
            pointer_ = e.scenePos;
            tracker_.reset();
            tracker_.addSample(e.scenePos, e.time);
            const Vec2 p = coords_.toPhysics(e.scenePos);
            bool grabbed = false;
            if (e.targetTexture != kNoTexture) {
                for (const auto& entry : entries_)
                    if (entry.texture == e.targetTexture && world_.state(entry.body).enabled) {
                        grabbed = world_.beginGrab(entry.body, p, config_.grab);
                        break;
                    }
            } else if (auto hit = world_.queryPoint(p))
                grabbed = world_.beginGrab(*hit, p, config_.grab);
            if (grabbed) emitSound(SoundKind::IconGrab, 0.22f, e.scenePos.x, 1.05f);
            break;
        }
        case InputType::PointerMove:
            pointer_ = e.scenePos;
            if (world_.grabbedBody()) {
                tracker_.addSample(e.scenePos, e.time);
                world_.updateGrab(coords_.toPhysics(e.scenePos));
            }
            break;
        case InputType::PointerUp:
            if (world_.grabbedBody()) {
                tracker_.addSample(e.scenePos, e.time);
                const Vec2 v = coords_.velocityToPhysics(tracker_.velocity(e.time));
                world_.endGrab(v);
                const float spd = v.length();
                if (spd >= 4.2f) {
                    const float g = std::clamp((spd - 3.5f) / 18.0f, 0.20f, 0.62f);
                    const float pt = std::clamp(0.89f + 0.010f * spd, 0.89f, 1.12f);
                    emitSound(SoundKind::IconGrab, g, e.scenePos.x, pt);
                }
            }
            tracker_.reset();
            break;
        case InputType::PointerCancel:
            world_.cancelGrab();  // no throw on focus loss / unknown button state
            tracker_.reset();
            break;
        default:
            break;
    }
}

bool GravityMode::blackHoleEdge(Vec2 p, bool alternate) const {
    if (!hole_.enabled || !p.isFinite()) return false;
    const float d = (p - hole_.center).length();
    // Cinematic: the whole black shadow is the move handle; Option-drag also accepts
    // the surrounding accretion ring so 3D angle orbiting is easy to grab.
    if (hole_.style == BlackHoleStyle::Cinematic)
        return d <= (alternate ? hole_.radius * 2.8f + 10 : hole_.radius * 1.4f + 6);
    return std::abs(d - hole_.radius) <= 10;
}
void GravityMode::enableBlackHole(bool enabled) {
    const bool wasEnabled = hole_.enabled;
    hole_.enabled = enabled;
    if (enabled && !wasEnabled) {
        humTimer_ = 0.24f;
        swallowSoundCooldown_ = 0.0f;
        emitSound(SoundKind::BlackHoleHum, 0.26f, hole_.center.x, 0.95f);
    }
}
void GravityMode::emitSound(SoundKind kind, float gain, float sceneX, float pitch) {
    if (!std::isfinite(gain) || gain <= 0.02f) return;
    const float w = std::max(100.0f, area_.x);
    const float pan = std::isfinite(sceneX) ? std::clamp((sceneX / w) * 2.0f - 1.0f, -0.88f, 0.88f) : 0.0f;
    const SoundEvent ev{kind, std::clamp(gain, 0.04f, 1.0f), pan, std::clamp(pitch, 0.88f, 1.14f)};
    if (sounds_.size() < 16u) {
        sounds_.push_back(ev);
    } else {
        auto it = std::min_element(sounds_.begin(), sounds_.end(),
                                   [](const SoundEvent& a, const SoundEvent& b) { return a.gain < b.gain; });
        if (it != sounds_.end() && ev.gain > it->gain) *it = ev;
    }
}
void GravityMode::drainSounds(std::vector<SoundEvent>& out) {
    out.insert(out.end(), sounds_.begin(), sounds_.end());
    sounds_.clear();
}
void GravityMode::setBlackHoleScale(float scale) {
    if (!std::isfinite(scale)) return;
    hole_.scale = scale;
    hole_.fit(area_);
}
void GravityMode::setBlackHoleAngles(float elevation, float tilt) {
    if (std::isfinite(elevation))
        hole_.elevation = std::clamp(elevation, BlackHoleState::kMinElevation, BlackHoleState::kMaxElevation);
    if (std::isfinite(tilt))
        hole_.tilt = std::clamp(tilt, BlackHoleState::kMinTilt, BlackHoleState::kMaxTilt);
}
void GravityMode::setGravityParameters(float magnitudeMps2, GravityDirection direction, float restitution) {
    if (std::isfinite(magnitudeMps2)) {
        gravityMagnitude_ = std::clamp(magnitudeMps2, kMinGravityMps2, kMaxGravityMps2);
    }
    if (static_cast<unsigned>(direction) <= 3u) {
        gravityDirection_ = direction;
    }
    if (std::isfinite(restitution)) {
        config_.material.restitution =
            std::clamp(restitution, kMinGravityRestitution, kMaxGravityRestitution);
    }
    config_.physics.gravity = gravityVectorFrom(gravityMagnitude_, gravityDirection_);
    world_.setGravity(config_.physics.gravity);
    world_.setRestitution(config_.material.restitution);
}
void GravityMode::stepPhysics(float dt) {
    if (dt > 0.0f && std::isfinite(dt) && dt <= 0.25f) {
        if (bodySoundCooldown_.size() != entries_.size()) bodySoundCooldown_.assign(entries_.size(), 0.0f);
        for (float& cd : bodySoundCooldown_) cd = std::max(0.0f, cd - dt);
        softHitCooldown_ = std::max(0.0f, softHitCooldown_ - dt);
    }
    if (hole_.enabled) {
        const float rangeScene = hole_.effectiveRange(area_);
        for (const auto& e : entries_) {
            const auto s = world_.state(e.body);
            if (!s.enabled || world_.grabbedBody() == e.body) continue;
            const auto d = coords_.toPhysics(hole_.center) - s.position;
            const float distance = d.length(), range = coords_.lengthToPhysics(rangeScene);
            if (distance < range) {
                // Soft core, bounded acceleration, mild tangential orbit, velocity damping.
                const auto n = d / std::max(distance, coords_.lengthToPhysics(hole_.radius));
                auto a =
                    (n * 24.0f + Vec2{-n.y, n.x} * 5.0f) * (1 - distance / range) - s.linearVelocity * 2.5f;
                const float length = a.length();
                if (length > 30) a = a * (30 / length);
                world_.accelerate(e.body, a);
            }
            const float speed = s.linearVelocity.length();
            if (speed > 8) world_.setVelocity(e.body, s.linearVelocity * (8 / speed), s.angularVelocity);
        }
    }
    world_.step(dt);
    const auto hits = world_.lastHits();
    if (!hits.empty()) {
        int emittedThisStep = 0;
        for (const CollisionHit& hit : hits) {
            if (emittedThisStep >= 3) break;
            if (!std::isfinite(hit.approachSpeed) || hit.approachSpeed < 0.42f) continue;
            const bool aOk =
                hit.bodyA && *hit.bodyA < bodySoundCooldown_.size() && bodySoundCooldown_[*hit.bodyA] <= 0.0f;
            const bool bOk =
                hit.bodyB && *hit.bodyB < bodySoundCooldown_.size() && bodySoundCooldown_[*hit.bodyB] <= 0.0f;
            if (hit.bodyA && hit.bodyB) {
                if (!aOk && !bOk) continue;
            } else {
                if (!aOk && !bOk) continue;
            }
            if (hit.approachSpeed < 1.35f && softHitCooldown_ > 0.0f) continue;

            const bool iconToIcon = hit.bodyA.has_value() && hit.bodyB.has_value();
            const float norm = std::clamp((hit.approachSpeed - 0.42f) / 8.5f, 0.0f, 1.0f);
            const float gain = std::clamp(0.08f + 0.76f * std::pow(norm, 0.72f), 0.08f, 0.84f);
            const std::uint32_t seed = (hit.bodyA.value_or(7u) * 31u + hit.bodyB.value_or(13u) * 17u) % 9u;
            const float detune = static_cast<float>(static_cast<int>(seed) - 4) * 0.014f;
            const float pitch =
                std::clamp((iconToIcon ? 0.99f : 0.93f) + detune + 0.08f * norm, 0.88f, 1.13f);
            const float sx = coords_.toScene(hit.point).x;
            emitSound(iconToIcon ? SoundKind::IconTap : SoundKind::IconThud, gain, sx, pitch);

            if (hit.bodyA && *hit.bodyA < bodySoundCooldown_.size()) bodySoundCooldown_[*hit.bodyA] = 0.068f;
            if (hit.bodyB && *hit.bodyB < bodySoundCooldown_.size()) bodySoundCooldown_[*hit.bodyB] = 0.068f;
            if (hit.approachSpeed < 1.35f) softHitCooldown_ = 0.024f;
            ++emittedThisStep;
        }
    }
}
void GravityMode::updateRules(float dt) {
    if (!hole_.enabled) return;
    hole_.angle = std::fmod(hole_.angle + dt, 6.2831853f);
    hole_.time = std::fmod(hole_.time + dt, BlackHoleState::kTimeWrap);
    if (hole_.devourAll) hole_.devourElapsed = std::min(hole_.devourElapsed + dt, 600.0f);
    if (dt > 0.0f && std::isfinite(dt)) {
        swallowSoundCooldown_ = std::max(0.0f, swallowSoundCooldown_ - dt);
    }
    const float duration = hole_.swallowSeconds();
    const float effRange = std::max(hole_.radius + 1.0f, hole_.effectiveRange(area_));
    float maxPull = 0.0f;
    for (size_t i = 0; i < entries_.size(); ++i) {
        auto& state = hole_.objects[i];
        const auto body = entries_[i].body;
        if (world_.grabbedBody() == body) continue;
        const float dist = (bodySceneCenter(i) - hole_.center).length();
        if (state.phase == SwallowPhase::Normal && std::isfinite(dist) && dist < effRange) {
            maxPull = std::max(maxPull, std::clamp(1.0f - dist / effRange, 0.0f, 1.0f));
        }
        if (state.phase == SwallowPhase::Normal && dist < hole_.radius * .65f) {
            state.phase = SwallowPhase::Swallowing;
            // Rules run after Step: no world mutation from a Box2D callback.
            world_.setEnabled(body, false);
            if (swallowSoundCooldown_ <= 0.0f) {
                const float pt = std::clamp(0.92f + 0.032f * static_cast<float>(i % 6u), 0.90f, 1.12f);
                emitSound(SoundKind::BlackHoleSwallow, 0.58f, hole_.center.x, pt);
                swallowSoundCooldown_ = 0.055f;
            }
        }
        if (state.phase == SwallowPhase::Swallowing) {
            state.elapsed = std::min(duration, state.elapsed + dt);
            if (state.elapsed >= duration) state.phase = SwallowPhase::Swallowed;
        }
    }
    if (dt > 0.0f && std::isfinite(dt)) {
        humTimer_ -= dt;
        if (humTimer_ <= 0.0f) {
            humTimer_ = 0.25f;
            const float humGain =
                std::clamp(0.12f + 0.14f * maxPull + (hole_.devourAll ? 0.08f : 0.0f), 0.10f, 0.36f);
            const float sizeNorm = std::clamp((hole_.scale - 0.5f) / 1.5f, 0.0f, 1.0f);
            const float humPitch = std::clamp(1.06f - 0.11f * sizeNorm + 0.06f * maxPull, 0.89f, 1.12f);
            emitSound(SoundKind::BlackHoleHum, humGain, hole_.center.x, humPitch);
        }
    }
}

Vec2 GravityMode::bodySceneCenter(std::size_t i) const {
    return coords_.toScene(world_.state(entries_.at(i).body).position);
}

void GravityMode::buildDrawList(DrawList& out) const {
    const bool cinematic = hole_.enabled && hole_.style == BlackHoleStyle::Cinematic;
    if (hole_.enabled && !cinematic) {
        out.add(CircleItem{hole_.center, hole_.radius, {9, 5, 24, 240}, {154, 113, 255, 255}, 3});
        for (int i = 0; i < 24; ++i) {
            const float a = hole_.angle + static_cast<float>(i) * 6.2831853f / 24;
            out.add(LineItem{hole_.center + Vec2{std::cos(a), std::sin(a)} * (hole_.radius - 5),
                             hole_.center + Vec2{std::cos(a + .15f), std::sin(a + .15f)} * (hole_.radius - 5),
                             {199, 168, 255, static_cast<uint8_t>(80 + (i % 8) * 20)},
                             2});
        }
    }
    const auto sprite = [&](size_t index) {
        const Entry& e = entries_[index];
        const auto swallow = hole_.objects[index];
        const BodyState s = world_.state(e.body);
        SpriteItem item;
        item.texture = e.texture;
        item.center = coords_.toScene(s.position);
        item.size = e.visualSize;
        item.rotation = SceneToPhysics::angleToScene(s.angle);
        if (swallow.phase == SwallowPhase::Normal) {
            if (cinematic) {
                item.tidal = hole_.tidal((item.center - hole_.center).length());
                item.grabbed = world_.grabbedBody() == e.body;
            }
            return item;
        }
        const float t = std::clamp(swallow.elapsed / hole_.swallowSeconds(), 0.0f, 1.0f);
        item.interactive = false;
        if (!cinematic) {
            item.center = item.center * (1 - t) + hole_.center * t;
            item.size = item.size * (1 - t);
            item.opacity = 1 - t;
            item.rotation += t * 3;
            return item;
        }
        // Gentle orbit toward the centre while the renderer dissolves the copy
        // into particles (consume); renderers without particles fade it out.
        const Vec2 d = item.center - hole_.center;
        const float turn = -t * 2.2f, shrink = std::pow(1 - t, 1.1f);
        const float c = std::cos(turn), sn = std::sin(turn);
        Vec2 offset{c * d.x - sn * d.y, sn * d.x + c * d.y};
        offset = offset * shrink;
        offset.y *= 1 - .6f * t;
        item.center = hole_.center + offset;
        item.size = e.visualSize * (1 - .3f * t);
        item.rotation *= 1 - t;
        item.consume = t;
        item.tidal = 1;
        if (swallow.phase == SwallowPhase::Swallowed) item.opacity = 0;
        return item;
    };
    if (cinematic) {
        BlackHoleItem back{hole_.center,
                           hole_.radius,
                           hole_.angle,
                           hole_.time,
                           hole_.elevation,
                           hole_.tilt,
                           BlackHoleItem::Layer::Back};
        out.add(back);
        // Swallowing copies pass between the far and near halves of the disk.
        for (size_t i = 0; i < entries_.size(); ++i)
            if (hole_.objects[i].phase != SwallowPhase::Normal) out.add(sprite(i));
        back.layer = BlackHoleItem::Layer::Front;
        out.add(back);
        for (size_t i = 0; i < entries_.size(); ++i)
            if (hole_.objects[i].phase == SwallowPhase::Normal) out.add(sprite(i));
    } else {
        for (size_t i = 0; i < entries_.size(); ++i) out.add(sprite(i));
    }
    if (auto g = world_.grabbedBody()) {
        const Vec2 c = coords_.toScene(world_.state(*g).position);
        out.add(LineItem{c, pointer_, Color{255, 255, 255, 140}, 1.5f, true});
    }
}

bool GravityMode::isIdle() const { return !hole_.enabled && !world_.grabbedBody() && world_.allAsleep(); }

ModeStats GravityMode::stats() const {
    ModeStats s;
    s.bodies = static_cast<int>(entries_.size());
    s.awakeBodies = world_.awakeBodyCount();
    s.grabbing = holeDragging_ || holeOrbiting_ || world_.grabbedBody().has_value();
    return s;
}

std::vector<BodyState> GravityMode::bodyStates() const {
    std::vector<BodyState> result;
    for (const auto& entry : entries_) result.push_back(world_.state(entry.body));
    return result;
}
void GravityMode::restoreStates(std::span<const BodyState> states, std::span<const Vec2> centers) {
    if (states.size() != entries_.size() || centers.size() != entries_.size())
        throw std::invalid_argument("migration identity count mismatch");
    for (size_t i = 0; i < entries_.size(); ++i) {
        const auto b = entries_[i].body;
        world_.restoreTransform(b, coords_.toPhysics(centers[i]), states[i]);
        world_.setVelocity(b, states[i].linearVelocity, states[i].angularVelocity);
        world_.setEnabled(b, states[i].enabled);
        // Contacts have changed. Waking bodies never changes Playground's user pause.
        if (states[i].enabled) world_.wake(b);
    }
}
}  // namespace dpp
