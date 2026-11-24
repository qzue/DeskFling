#include "core/modes/air_hockey_mode.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dpp {

namespace {

bool rectContains(const Rect& r, Vec2 p) {
    return p.x >= r.x && p.x <= r.x + r.width && p.y >= r.y && p.y <= r.y + r.height;
}

const char* diffLabel(AirHockeyDifficulty d) {
    switch (d) {
        case AirHockeyDifficulty::Casual:
            return "休闲 AI";
        case AirHockeyDifficulty::Standard:
            return "标准 AI";
        case AirHockeyDifficulty::Master:
            return "大师 AI";
    }
    return "标准 AI";
}

constexpr float kMaxPuckSpeedMps = 13.5f;

PhysicsConfig makeHockeyPhysics() {
    PhysicsConfig pc;
    pc.gravity = {0.0f, 0.0f};
    pc.subSteps = 4;
    pc.enableSleep = false;
    pc.maxContactPushSpeed = 5.0f;
    pc.maximumLinearSpeed = 72.0f;
    pc.enableHitEvents = true;
    pc.hitEventThreshold = 0.25f;
    return pc;
}

}  // namespace

AirHockeyMode::AirHockeyMode(const SceneSetup& setup, const AirHockeyConfig& config)
    : setup_(setup), config_(config), coords_(setup.workAreaSize), world_(makeHockeyPhysics()) {
    config_.targetGoals = std::clamp(config_.targetGoals, 3, 15);
    const float w = setup_.workAreaSize.x;
    const float h = setup_.workAreaSize.y;

    const float availW = std::max(180.0f, w * 0.84f);
    const float availH = std::max(110.0f, h * 0.74f);
    const float maxAspect =
        (w > h * 2.25f) ? std::clamp(availW / std::max(1.0f, availH), 2.0f, 2.85f) : 2.0f;
    float rw = availW;
    float rh = rw / maxAspect;
    if (rh > availH) {
        rh = availH;
        rw = rh * maxAspect;
    }
    rinkSize_ = {rw, rh};
    rinkMin_ = {(w - rw) * 0.5f, (h - rh) * 0.5f + 12.0f};
    puckRadius_ = std::clamp(rh * 0.048f, 12.0f, 24.0f);
    malletRadius_ = std::clamp(rh * 0.068f, 17.0f, 34.0f);
    goalHalfH_ = rh * 0.195f;
    cornerCut_ = rh * 0.085f;

    chromeToggleRect_ = {24.0f, 18.0f, 108.0f, 28.0f};
    diffCycleRect_ = {140.0f, 18.0f, 104.0f, 28.0f};

    // Assign desktop icons to Puck, Left Mallet, Right Mallet, and Spectators.
    const std::size_t n = setup_.icons.size();
    if (n >= 1) puckTex_ = setup_.icons[0].texture;
    if (n >= 2)
        leftTex_ = setup_.icons[1].texture;
    else if (n >= 1)
        leftTex_ = setup_.icons[0].texture;
    if (n >= 3)
        rightTex_ = setup_.icons[2].texture;
    else if (n >= 1)
        rightTex_ = setup_.icons[n - 1].texture;

    for (std::size_t i = 3; i < n; ++i) {
        spectatorIcons_.push_back(i);
    }
    if (!spectatorIcons_.empty()) {
        const std::size_t topCount = (spectatorIcons_.size() + 1) / 2;
        const std::size_t botCount = spectatorIcons_.size() - topCount;
        for (std::size_t i = 0; i < spectatorIcons_.size(); ++i) {
            const bool topRow = (i < topCount);
            const std::size_t rowIdx = topRow ? i : (i - topCount);
            const std::size_t rowLen = std::max<std::size_t>(1, topRow ? topCount : botCount);
            const float u = (static_cast<float>(rowIdx) + 0.5f) / static_cast<float>(rowLen);
            const float sx = rinkMin_.x + 36.0f + u * std::max(40.0f, rinkSize_.x - 72.0f);
            const float sy = topRow ? std::max(54.0f, rinkMin_.y - 30.0f)
                                    : std::min(h - 24.0f, rinkMin_.y + rinkSize_.y + 30.0f);
            spectatorBasePos_.push_back({sx, sy});
        }
    }

    buildRinkWalls();

    BodyMaterial puckMat;
    puckMat.density = 0.85f;
    puckMat.friction = 0.02f;
    puckMat.restitution = 0.84f;
    puckMat.linearDamping = 0.30f;
    puckMat.angularDamping = 0.40f;
    puckMat.bullet = true;

    BodyMaterial malletMat;
    malletMat.density = 4.2f;
    malletMat.friction = 0.04f;
    malletMat.restitution = 0.52f;
    malletMat.linearDamping = 4.0f;
    malletMat.angularDamping = 4.0f;
    malletMat.bullet = true;

    leftTargetScene_ = {rinkMin_.x + rinkSize_.x * 0.14f, rinkMin_.y + rinkSize_.y * 0.50f};
    rightTargetScene_ = {rinkMin_.x + rinkSize_.x * 0.86f, rinkMin_.y + rinkSize_.y * 0.50f};

    puckBody_ = world_.addCircle(
        coords_.toPhysics({rinkMin_.x + rinkSize_.x * 0.50f, rinkMin_.y + rinkSize_.y * 0.50f}),
        coords_.lengthToPhysics(puckRadius_), puckMat);
    leftMalletBody_ = world_.addCircle(coords_.toPhysics(leftTargetScene_),
                                       coords_.lengthToPhysics(malletRadius_), malletMat);
    rightMalletBody_ = world_.addCircle(coords_.toPhysics(rightTargetScene_),
                                        coords_.lengthToPhysics(malletRadius_), malletMat);

    resetFaceOff(0);
}

void AirHockeyMode::buildRinkWalls() {
    const float x0 = rinkMin_.x;
    const float y0 = rinkMin_.y;
    const float x1 = rinkMin_.x + rinkSize_.x;
    const float y1 = rinkMin_.y + rinkSize_.y;
    const float midY = (y0 + y1) * 0.5f;
    const float wt = 24.0f;
    const float goalDepth = puckRadius_ * 3.4f;

    auto addSceneRect = [&](float left, float top, float right, float bottom, float rest = 0.85f) {
        const std::array<Vec2, 4> pts = {
            coords_.toPhysics({left, bottom}),
            coords_.toPhysics({right, bottom}),
            coords_.toPhysics({right, top}),
            coords_.toPhysics({left, top}),
        };
        world_.addStaticPolygon(pts, 0.01f, 0.02f, rest);
    };

    // Top & bottom long cushions
    addSceneRect(x0 - wt, y0 - wt, x1 + wt, y0);
    addSceneRect(x0 - wt, y1, x1 + wt, y1 + wt);

    // Left & right goal shoulders
    addSceneRect(x0 - wt, y0 - wt, x0, midY - goalHalfH_);
    addSceneRect(x0 - wt, midY + goalHalfH_, x0, y1 + wt);
    addSceneRect(x1, y0 - wt, x1 + wt, midY - goalHalfH_);
    addSceneRect(x1, midY + goalHalfH_, x1 + wt, y1 + wt);

    // Goal pocket backstops & side sleeves so puck stays bounded when scoring
    addSceneRect(x0 - goalDepth - wt, midY - goalHalfH_ - wt, x0 - goalDepth, midY + goalHalfH_ + wt, 0.2f);
    addSceneRect(x0 - goalDepth - wt, midY - goalHalfH_ - wt, x0, midY - goalHalfH_, 0.5f);
    addSceneRect(x0 - goalDepth - wt, midY + goalHalfH_, x0, midY + goalHalfH_ + wt, 0.5f);

    addSceneRect(x1 + goalDepth, midY - goalHalfH_ - wt, x1 + goalDepth + wt, midY + goalHalfH_ + wt, 0.2f);
    addSceneRect(x1, midY - goalHalfH_ - wt, x1 + goalDepth + wt, midY - goalHalfH_, 0.5f);
    addSceneRect(x1, midY + goalHalfH_, x1 + goalDepth + wt, midY + goalHalfH_ + wt, 0.5f);

    // 45-degree corner bevel wedges to prevent corner deadlocks
    auto addTri = [&](Vec2 a, Vec2 b, Vec2 c) {
        const std::array<Vec2, 3> pts = {coords_.toPhysics(a), coords_.toPhysics(b), coords_.toPhysics(c)};
        world_.addStaticPolygon(pts, 0.008f, 0.02f, 0.92f);
    };
    const float cc = cornerCut_;
    addTri({x0, y0}, {x0, y0 + cc}, {x0 + cc, y0});
    addTri({x1, y0}, {x1 - cc, y0}, {x1, y0 + cc});
    addTri({x0, y1}, {x0 + cc, y1}, {x0, y1 - cc});
    addTri({x1, y1}, {x1, y1 - cc}, {x1 - cc, y1});
}

Vec2 AirHockeyMode::clampLeftMalletScene(Vec2 p) const {
    const float r = malletRadius_ + 2.0f;
    const float cx = rinkMin_.x + rinkSize_.x * 0.5f;
    return {std::clamp(p.x, rinkMin_.x + r, cx - r),
            std::clamp(p.y, rinkMin_.y + r, rinkMin_.y + rinkSize_.y - r)};
}

Vec2 AirHockeyMode::clampRightMalletScene(Vec2 p) const {
    const float r = malletRadius_ + 2.0f;
    const float cx = rinkMin_.x + rinkSize_.x * 0.5f;
    return {std::clamp(p.x, cx + r, rinkMin_.x + rinkSize_.x - r),
            std::clamp(p.y, rinkMin_.y + r, rinkMin_.y + rinkSize_.y - r)};
}

Vec2 AirHockeyMode::sceneToRinkNorm(Vec2 scene) const {
    return {(scene.x - rinkMin_.x) / std::max(1.0f, rinkSize_.x),
            (scene.y - rinkMin_.y) / std::max(1.0f, rinkSize_.y)};
}

Vec2 AirHockeyMode::rinkNormToScene(Vec2 norm) const {
    return {rinkMin_.x + norm.x * rinkSize_.x, rinkMin_.y + norm.y * rinkSize_.y};
}

void AirHockeyMode::resetFaceOff(int serveTowardPlayer) {
    const float midY = rinkMin_.y + rinkSize_.y * 0.5f;
    const float midX = rinkMin_.x + rinkSize_.x * 0.5f;
    const float offset = serveTowardPlayer == 0 ? (-rinkSize_.x * 0.07f)
                                                : (serveTowardPlayer == 1 ? (rinkSize_.x * 0.07f) : 0.0f);
    const Vec2 puckScene = {midX + offset, midY};
    world_.setTransform(puckBody_, coords_.toPhysics(puckScene), 0.0f);
    world_.setVelocity(puckBody_, {0.0f, 0.0f}, 0.0f);
    aiServeTimer_ = (serveTowardPlayer == 1) ? 0.35f : 0.0f;

    leftTargetScene_ = clampLeftMalletScene({rinkMin_.x + rinkSize_.x * 0.14f, midY});
    rightTargetScene_ = clampRightMalletScene({rinkMin_.x + rinkSize_.x * 0.86f, midY});
    world_.setTransform(leftMalletBody_, coords_.toPhysics(leftTargetScene_), 0.0f);
    world_.setVelocity(leftMalletBody_, {0.0f, 0.0f}, 0.0f);
    world_.setTransform(rightMalletBody_, coords_.toPhysics(rightTargetScene_), 0.0f);
    world_.setVelocity(rightMalletBody_, {0.0f, 0.0f}, 0.0f);

    for (auto& tr : puckTrail_) tr = puckScene;
    stuckTimer_ = 0.0f;
}

void AirHockeyMode::emitSound(SoundKind kind, float gain, Vec2 scenePos, float pitch) {
    if (pendingSounds_.size() >= 16) return;
    const float pan =
        std::clamp((scenePos.x / std::max(1.0f, setup_.workAreaSize.x)) * 2.0f - 1.0f, -1.0f, 1.0f);
    pendingSounds_.push_back({kind, std::clamp(gain, 0.05f, 1.0f), pan, std::clamp(pitch, 0.82f, 1.25f)});
}

void AirHockeyMode::setVisualOptions(bool showRinkChrome, bool showSpectators) {
    config_.showRinkChrome = showRinkChrome;
    config_.showSpectators = showSpectators;
}

void AirHockeyMode::setPlayer2KeyDirection(Vec2 dir) {
    const float len = dir.length();
    p2KeyDir_ = (len > 1.0f) ? (dir * (1.0f / len)) : dir;
}

void AirHockeyMode::setLanLocalPlayer(int role) { lanLocalPlayer_ = (role == 0 || role == 1) ? role : -1; }

bool AirHockeyMode::consumePendingRemoteInput(AirHockeyRemoteInput& out) {
    if (!hasPendingRemoteInput_) return false;
    out = pendingRemoteInput_;
    hasPendingRemoteInput_ = false;
    return true;
}

void AirHockeyMode::applyRemoteInput(const AirHockeyRemoteInput& in) {
    if (in.requestRematch && winner_ != -1) {
        scores_ = {0, 0};
        winner_ = -1;
        goalPauseTimer_ = 0.0f;
        resetFaceOff(0);
        return;
    }
    const Vec2 clampedNorm = {std::clamp(in.normTarget.x, 0.50f, 1.0f),
                              std::clamp(in.normTarget.y, 0.0f, 1.0f)};
    rightTargetScene_ = clampRightMalletScene(rinkNormToScene(clampedNorm));
}

AirHockeyLanState AirHockeyMode::exportLanState() const {
    AirHockeyLanState st;
    const BodyState ps = world_.state(puckBody_);
    st.puckNorm = sceneToRinkNorm(coords_.toScene(ps.position));
    const Vec2 vScene = coords_.velocityToScene(ps.linearVelocity);
    st.puckVelNorm = {vScene.x / std::max(1.0f, rinkSize_.x), vScene.y / std::max(1.0f, rinkSize_.y)};
    st.leftMalletNorm = sceneToRinkNorm(coords_.toScene(world_.state(leftMalletBody_).position));
    st.rightMalletNorm = sceneToRinkNorm(coords_.toScene(world_.state(rightMalletBody_).position));
    st.scores = scores_;
    st.targetGoals = config_.targetGoals;
    st.winner = winner_;
    st.goalFlash = goalCelebration_;
    return st;
}

void AirHockeyMode::applyLanState(const AirHockeyLanState& st) {
    scores_ = st.scores;
    config_.targetGoals = std::clamp(st.targetGoals, 3, 15);
    winner_ = st.winner;
    goalCelebration_ = std::max(goalCelebration_, st.goalFlash);

    const Vec2 puckSc = rinkNormToScene(st.puckNorm);
    const Vec2 puckVelSc = {st.puckVelNorm.x * rinkSize_.x, st.puckVelNorm.y * rinkSize_.y};
    world_.setTransform(puckBody_, coords_.toPhysics(puckSc), 0.0f);
    world_.setVelocity(puckBody_, coords_.velocityToPhysics(puckVelSc), 0.0f);

    const Vec2 leftSc = clampLeftMalletScene(rinkNormToScene(st.leftMalletNorm));
    world_.setTransform(leftMalletBody_, coords_.toPhysics(leftSc), 0.0f);
    leftTargetScene_ = leftSc;

    if (lanLocalPlayer_ != 1) {
        const Vec2 rightSc = clampRightMalletScene(rinkNormToScene(st.rightMalletNorm));
        world_.setTransform(rightMalletBody_, coords_.toPhysics(rightSc), 0.0f);
        rightTargetScene_ = rightSc;
    }
}

void AirHockeyMode::handleInput(const InputEvent& event) {
    if (event.type == InputType::PointerCancel) return;

    if (event.type == InputType::PointerDown) {
        if (rectContains(chromeToggleRect_, event.scenePos)) {
            config_.showRinkChrome = !config_.showRinkChrome;
            return;
        }
        if (config_.matchMode == AirHockeyMatchMode::SoloAI && rectContains(diffCycleRect_, event.scenePos)) {
            const int next = (static_cast<int>(config_.difficulty) + 1) % 3;
            config_.difficulty = static_cast<AirHockeyDifficulty>(next);
            return;
        }
        if (winner_ != -1) {
            if (config_.matchMode == AirHockeyMatchMode::Lan && lanLocalPlayer_ == 1) {
                pendingRemoteInput_.requestRematch = true;
                hasPendingRemoteInput_ = true;
                return;
            }
            scores_ = {0, 0};
            winner_ = -1;
            goalPauseTimer_ = 0.0f;
            resetFaceOff(0);
            return;
        }
    }

    if (event.type == InputType::PointerMove || event.type == InputType::PointerDown) {
        const float pad = malletRadius_ * 2.2f;
        const bool nearRink =
            event.scenePos.x >= rinkMin_.x - pad && event.scenePos.x <= rinkMin_.x + rinkSize_.x + pad &&
            event.scenePos.y >= rinkMin_.y - pad && event.scenePos.y <= rinkMin_.y + rinkSize_.y + pad;
        if (!nearRink) return;

        if (config_.matchMode == AirHockeyMatchMode::Lan && lanLocalPlayer_ == 1) {
            rightTargetScene_ = clampRightMalletScene(event.scenePos);
            pendingRemoteInput_.normTarget = sceneToRinkNorm(rightTargetScene_);
            pendingRemoteInput_.requestRematch = false;
            hasPendingRemoteInput_ = true;
            return;
        }

        if (config_.matchMode == AirHockeyMatchMode::Hotseat2P &&
            event.scenePos.x > rinkMin_.x + rinkSize_.x * 0.52f && p2KeyDir_.lengthSquared() < 1e-4f) {
            rightTargetScene_ = clampRightMalletScene(event.scenePos);
            return;
        }

        leftTargetScene_ = clampLeftMalletScene(event.scenePos);
    }
}

void AirHockeyMode::updateAiTarget(float dt) {
    if (aiServeTimer_ > 0.0f) {
        aiServeTimer_ = std::max(0.0f, aiServeTimer_ - dt);
    }
    const BodyState ps = world_.state(puckBody_);
    const Vec2 puckSc = coords_.toScene(ps.position);
    const Vec2 puckVelSc = coords_.velocityToScene(ps.linearVelocity);
    const float midX = rinkMin_.x + rinkSize_.x * 0.5f;
    const float midY = rinkMin_.y + rinkSize_.y * 0.5f;
    const float homeX = rinkMin_.x + rinkSize_.x * 0.87f;

    float trackBlend = 0.65f;
    float attackReach = rinkMin_.x + rinkSize_.x * 0.64f;
    if (config_.difficulty == AirHockeyDifficulty::Casual) {
        trackBlend = 0.48f;
        attackReach = rinkMin_.x + rinkSize_.x * 0.72f;
    } else if (config_.difficulty == AirHockeyDifficulty::Master) {
        trackBlend = 0.88f;
        attackReach = rinkMin_.x + rinkSize_.x * 0.56f;
    }

    const Vec2 aiNowSc = coords_.toScene(world_.state(rightMalletBody_).position);
    Vec2 desired = {homeX, midY + (puckSc.y - midY) * trackBlend};

    // If puck is in AI's half (including face-off serve spot at midX + 0.07 * W)
    if (puckSc.x > midX - puckRadius_ * 0.25f) {
        // Avoid accidental own-goal if puck slipped behind AI mallet!
        if (puckSc.x > aiNowSc.x + puckRadius_ * 0.5f) {
            const float dodgeY =
                (puckSc.y < midY) ? (puckSc.y + malletRadius_ * 2.1f) : (puckSc.y - malletRadius_ * 2.1f);
            desired = {rinkMin_.x + rinkSize_.x - malletRadius_ - 4.0f, dodgeY};
        } else if (puckVelSc.x > 65.0f) {
            // Predict Y at strike X with single wall reflection!
            const float strikeX = std::clamp(puckSc.x + 36.0f, attackReach, homeX);
            const float t = std::clamp((strikeX - puckSc.x) / std::max(60.0f, puckVelSc.x), 0.0f, 0.55f);
            float predY = puckSc.y + puckVelSc.y * t;
            const float topLim = rinkMin_.y + puckRadius_;
            const float botLim = rinkMin_.y + rinkSize_.y - puckRadius_;
            if (config_.difficulty != AirHockeyDifficulty::Casual) {
                if (predY < topLim) predY = topLim + (topLim - predY);
                if (predY > botLim) predY = botLim - (predY - botLim);
            }
            desired = {strikeX, predY};
        } else if (aiServeTimer_ > 0.0f) {
            // Brief natural wind-up behind face-off puck before serving
            desired = {std::min(homeX, puckSc.x + malletRadius_ * 2.3f), puckSc.y + malletRadius_ * 0.16f};
        } else {
            // Stationary face-off serve or slow puck in AI half: step all the way up to centerline
            // and drive cleanly through the puck across ALL difficulties (Casual / Standard / Master)!
            const float clearReach = midX + malletRadius_ + 2.0f;
            const float angleBias = (puckSc.y <= midY ? -1.0f : 1.0f) * malletRadius_ * 0.16f;
            desired = {std::max(clearReach, puckSc.x - malletRadius_ * 0.95f), puckSc.y + angleBias};
        }
    }

    rightTargetScene_ = clampRightMalletScene(desired);
}

void AirHockeyMode::stepPhysics(float dt) {
    if (dt <= 0.0f) return;

    if (config_.matchMode == AirHockeyMatchMode::Lan && lanLocalPlayer_ == 1) {
        // Guest updates local right mallet visually; Host is authoritative for puck.
        const Vec2 curPhys = world_.state(rightMalletBody_).position;
        const Vec2 tgtPhys = coords_.toPhysics(rightTargetScene_);
        Vec2 v = (tgtPhys - curPhys) * std::min(1.0f / dt, 100.0f);
        const float maxSpd = 45.0f;
        if (v.length() > maxSpd) v = v * (maxSpd / v.length());
        world_.setVelocity(rightMalletBody_, v, 0.0f);
        world_.step(dt);
        return;
    }

    if (winner_ == -1 && goalPauseTimer_ <= 0.0f) {
        if (config_.matchMode == AirHockeyMatchMode::SoloAI) {
            updateAiTarget(dt);
        } else if (config_.matchMode == AirHockeyMatchMode::Hotseat2P && p2KeyDir_.lengthSquared() > 1e-4f) {
            rightTargetScene_ = clampRightMalletScene(rightTargetScene_ + p2KeyDir_ * (580.0f * dt));
        }
    }

    auto driveMallet = [&](BodyIndex body, Vec2 targetScene, float maxSpeedMeters) {
        const Vec2 curPhys = world_.state(body).position;
        const Vec2 tgtPhys = coords_.toPhysics(targetScene);
        const Vec2 diff = tgtPhys - curPhys;
        if (diff.length() < 0.0010f) {
            world_.setVelocity(body, {0.0f, 0.0f}, 0.0f);
            return;
        }
        Vec2 vel = diff * std::min(1.0f / dt, 100.0f);
        if (vel.length() > maxSpeedMeters) vel = vel * (maxSpeedMeters / vel.length());
        world_.setVelocity(body, vel, 0.0f);
    };

    const float aiMaxSpeed =
        (config_.matchMode == AirHockeyMatchMode::SoloAI)
            ? (config_.difficulty == AirHockeyDifficulty::Casual
                   ? 5.6f
                   : (config_.difficulty == AirHockeyDifficulty::Standard ? 8.8f : 12.8f))
            : 65.0f;

    driveMallet(leftMalletBody_, leftTargetScene_, 65.0f);
    driveMallet(rightMalletBody_, rightTargetScene_, aiMaxSpeed);

    world_.step(dt);

    // Keep puck speed within crisp, human-reactable arcade bounds even after a fast cursor flick!
    {
        const BodyState pst = world_.state(puckBody_);
        const float spd = pst.linearVelocity.length();
        if (spd > kMaxPuckSpeedMps) {
            world_.setVelocity(puckBody_, pst.linearVelocity * (kMaxPuckSpeedMps / spd), pst.angularVelocity);
        }
    }

    // Strictly enforce half-rink boundaries on mallets after collision response.
    auto enforceMalletBounds = [&](BodyIndex body, bool isLeft) {
        const BodyState st = world_.state(body);
        const Vec2 sc = coords_.toScene(st.position);
        const Vec2 clamped = isLeft ? clampLeftMalletScene(sc) : clampRightMalletScene(sc);
        if ((clamped - sc).lengthSquared() > 0.25f) {
            world_.setTransform(body, coords_.toPhysics(clamped), 0.0f);
        }
    };
    enforceMalletBounds(leftMalletBody_, true);
    enforceMalletBounds(rightMalletBody_, false);
}

void AirHockeyMode::updateRules(float dt) {
    animTime_ += dt;
    if (goalCelebration_ > 0.0f) {
        goalCelebration_ = std::max(0.0f, goalCelebration_ - dt);
    }

    // Sample puck trail
    trailSampleAccum_ += dt;
    if (trailSampleAccum_ >= 0.018f) {
        trailSampleAccum_ = 0.0f;
        puckTrail_[trailHead_] = coords_.toScene(world_.state(puckBody_).position);
        trailHead_ = (trailHead_ + 1) % puckTrail_.size();
    }

    if (config_.matchMode == AirHockeyMatchMode::Lan && lanLocalPlayer_ == 1) {
        return;
    }

    // Collision audio
    for (const CollisionHit& hit : world_.lastHits()) {
        const Vec2 sc = coords_.toScene(hit.point);
        const bool involvesPuck =
            (hit.bodyA && *hit.bodyA == puckBody_) || (hit.bodyB && *hit.bodyB == puckBody_);
        if (!involvesPuck) continue;
        const bool involvesMallet =
            (hit.bodyA && (*hit.bodyA == leftMalletBody_ || *hit.bodyA == rightMalletBody_)) ||
            (hit.bodyB && (*hit.bodyB == leftMalletBody_ || *hit.bodyB == rightMalletBody_));
        const float gain = std::clamp(hit.approachSpeed / 18.0f, 0.12f, 0.96f);
        if (involvesMallet) {
            emitSound(SoundKind::MalletStrike, std::max(0.28f, gain), sc,
                      0.96f + 0.14f * std::min(1.0f, gain));
        } else {
            emitSound(SoundKind::RailBounce, gain * 0.80f, sc, 1.12f);
        }
    }

    if (winner_ != -1) return;

    if (goalPauseTimer_ > 0.0f) {
        goalPauseTimer_ -= dt;
        if (goalPauseTimer_ <= 0.0f) {
            resetFaceOff(nextServe_);
        }
        return;
    }

    // Goal detection!
    const BodyState ps = world_.state(puckBody_);
    const Vec2 puckSc = coords_.toScene(ps.position);
    const float midY = rinkMin_.y + rinkSize_.y * 0.5f;
    const bool inGoalY = std::abs(puckSc.y - midY) <= (goalHalfH_ + puckRadius_ * 0.65f);

    int scorer = -1;
    if (puckSc.x < rinkMin_.x - puckRadius_ * 0.55f && inGoalY) {
        scorer = 1;  // Right player / AI scored on Left goal!
    } else if (puckSc.x > rinkMin_.x + rinkSize_.x + puckRadius_ * 0.55f && inGoalY) {
        scorer = 0;  // Left player scored on Right goal!
    }

    if (scorer != -1) {
        ++scores_[static_cast<std::size_t>(scorer)];
        goalCelebration_ = 1.35f;
        emitSound(SoundKind::HockeyGoal, 0.92f, puckSc, scorer == 0 ? 1.06f : 0.90f);
        world_.setVelocity(puckBody_, {0.0f, 0.0f}, 0.0f);
        world_.setTransform(puckBody_, coords_.toPhysics({rinkMin_.x + rinkSize_.x * 0.5f, -200.0f}),
                            0.0f);  // hide puck briefly during goal celebration
        if (scores_[static_cast<std::size_t>(scorer)] >= config_.targetGoals) {
            winner_ = scorer;
        } else {
            nextServe_ = 1 - scorer;  // Conceding player receives face-off!
            goalPauseTimer_ = 0.95f;
        }
        return;
    }

    // Safety anti-stuck nudge if puck somehow leaves rink bounds or wedges in corner
    if (puckSc.x < rinkMin_.x - puckRadius_ * 4.0f ||
        puckSc.x > rinkMin_.x + rinkSize_.x + puckRadius_ * 4.0f ||
        puckSc.y < rinkMin_.y - puckRadius_ * 2.0f ||
        puckSc.y > rinkMin_.y + rinkSize_.y + puckRadius_ * 2.0f) {
        resetFaceOff(0);
        return;
    }

    if (ps.linearVelocity.length() < 0.15f) {
        const float dxEdge = std::min(puckSc.x - rinkMin_.x, (rinkMin_.x + rinkSize_.x) - puckSc.x);
        const float dyEdge = std::min(puckSc.y - rinkMin_.y, (rinkMin_.y + rinkSize_.y) - puckSc.y);
        if (dxEdge < puckRadius_ * 1.6f && dyEdge < puckRadius_ * 1.6f) {
            stuckTimer_ += dt;
            if (stuckTimer_ > 1.2f) {
                const Vec2 centerPhys =
                    coords_.toPhysics({rinkMin_.x + rinkSize_.x * 0.5f, rinkMin_.y + rinkSize_.y * 0.5f});
                Vec2 nudge = centerPhys - ps.position;
                if (nudge.length() > 1e-3f) nudge = nudge * (3.5f / nudge.length());
                world_.setVelocity(puckBody_, nudge, 0.0f);
                stuckTimer_ = 0.0f;
            }
        } else {
            stuckTimer_ = 0.0f;
        }
    } else {
        stuckTimer_ = 0.0f;
    }
}

bool AirHockeyMode::isIdle() const {
    if (!pendingSounds_.empty() || goalCelebration_ > 0.0f || goalPauseTimer_ > 0.0f) return false;
    // Keep the 120Hz loop hot throughout a live match so cursor micro-pauses at face-off
    // never drop the frame timer to 200ms (5Hz) or reset the fixed-step accumulator.
    return winner_ != -1;
}

ModeStats AirHockeyMode::stats() const {
    ModeStats s;
    s.bodies = static_cast<int>(world_.bodyCount());
    s.awakeBodies = isIdle() ? 0 : s.bodies;
    s.pocketed = scores_[0] + scores_[1];
    s.targets = config_.targetGoals;
    s.shots = scores_[0];
    s.finished = (winner_ != -1);
    s.grabbing = false;
    return s;
}

void AirHockeyMode::drainSounds(std::vector<SoundEvent>& out) {
    out.insert(out.end(), pendingSounds_.begin(), pendingSounds_.end());
    pendingSounds_.clear();
}

Vec2 AirHockeyMode::iconCurrentSceneCenter(std::size_t iconIndex) const {
    if (iconIndex >= setup_.icons.size()) return {};
    if (iconIndex == 0) {
        if (goalPauseTimer_ > 0.0f) return {rinkMin_.x + rinkSize_.x * 0.5f, rinkMin_.y + rinkSize_.y * 0.5f};
        return coords_.toScene(world_.state(puckBody_).position);
    }
    if (iconIndex == 1) return coords_.toScene(world_.state(leftMalletBody_).position);
    if (iconIndex == 2) return coords_.toScene(world_.state(rightMalletBody_).position);
    for (std::size_t i = 0; i < spectatorIcons_.size(); ++i) {
        if (spectatorIcons_[i] == iconIndex) {
            Vec2 p = spectatorBasePos_[i];
            if (goalCelebration_ > 0.0f) {
                const float hop =
                    std::abs(std::sin(static_cast<float>(animTime_ * 14.0 + static_cast<double>(i) * 0.65))) *
                    14.0f * std::min(1.0f, goalCelebration_);
                p.y -= hop;
            }
            return p;
        }
    }
    return setup_.icons[iconIndex].center;
}

bool AirHockeyMode::interactiveContains(Vec2 p) const {
    if (rectContains(chromeToggleRect_, p)) return true;
    if (config_.matchMode == AirHockeyMatchMode::SoloAI && rectContains(diffCycleRect_, p)) return true;
    if (winner_ != -1) return true;
    const float pad = malletRadius_ * 1.6f;
    if (p.x >= rinkMin_.x - pad && p.x <= rinkMin_.x + rinkSize_.x + pad && p.y >= rinkMin_.y - pad &&
        p.y <= rinkMin_.y + rinkSize_.y + pad) {
        return true;
    }
    if (config_.showSpectators) {
        for (std::size_t idx : spectatorIcons_) {
            if ((p - iconCurrentSceneCenter(idx)).length() <= 24.0f) return true;
        }
    }
    return false;
}

void AirHockeyMode::buildDrawList(DrawList& out) const {
    const float w = setup_.workAreaSize.x;
    const float x0 = rinkMin_.x;
    const float y0 = rinkMin_.y;
    const float x1 = x0 + rinkSize_.x;
    const float y1 = y0 + rinkSize_.y;
    const float midX = (x0 + x1) * 0.5f;
    const float midY = (y0 + y1) * 0.5f;
    const float railW = std::clamp(rinkSize_.y * 0.048f, 18.0f, 28.0f);
    const float goalTop = midY - goalHalfH_;
    const float goalBot = midY + goalHalfH_;
    const float goalFlash = std::clamp(goalCelebration_, 0.0f, 1.0f);

    auto addPoly = [&](std::initializer_list<Vec2> pts, Color fill, Color stroke = {0, 0, 0, 0},
                       float sw = 0.0f) {
        PolygonItem p;
        p.fill = fill;
        p.stroke = stroke;
        p.strokeWidth = sw;
        for (const Vec2& v : pts) {
            if (p.count < 8) p.points[p.count++] = v;
        }
        if (p.count >= 3) out.add(p);
    };

    // 1. Sculpted 3D Brushed Titanium-Carbon Arcade Cabinet & Integrated Recessed Goals
    if (config_.showRinkChrome) {
        // Soft drop shadow + outer anodized carbon-titanium frame (goals sit cleanly INSIDE this wide rail!)
        out.add(BoxItem{{midX, midY + 4.0f},
                        {rinkSize_.x + railW * 2.35f, rinkSize_.y + railW * 2.15f},
                        0.0f,
                        cornerCut_ + railW * 0.95f,
                        {6, 10, 18, 138},
                        {0, 0, 0, 0},
                        0.0f});
        out.add(BoxItem{{midX, midY},
                        {rinkSize_.x + railW * 2.20f, rinkSize_.y + railW * 2.00f},
                        0.0f,
                        cornerCut_ + railW * 0.90f,
                        {24, 31, 46, 248},
                        {74, 92, 122, 235},
                        2.0f});
        // Inner brushed metallic bevel step
        out.add(BoxItem{{midX, midY},
                        {rinkSize_.x + railW * 1.15f, rinkSize_.y + railW * 1.05f},
                        0.0f,
                        cornerCut_ + railW * 0.48f,
                        {36, 46, 66, 246},
                        {18, 24, 36, 235},
                        1.4f});

        // 8 machined brushed-steel rail bolts along top & bottom rails
        const float boltOffY = rinkSize_.y * 0.5f + railW * 0.52f;
        for (float frac : {0.16f, 0.36f, 0.64f, 0.84f}) {
            const float bx = x0 + rinkSize_.x * frac;
            for (float sy : {midY - boltOffY, midY + boltOffY}) {
                out.add(CircleItem{{bx, sy}, 3.2f, {16, 22, 32, 235}, {118, 138, 168, 200}, 1.1f});
            }
        }
    }

    // 2. Crystalline Glacial Ice / Air-Cushion Playfield Deck
    const std::uint8_t iceAlpha = config_.showRinkChrome ? 236u : 124u;
    out.add(BoxItem{
        {midX, midY}, rinkSize_, 0.0f, cornerCut_, {232, 244, 255, iceAlpha}, {62, 136, 198, 220}, 2.2f});
    // Subtle cool center ice reflection sheen + inner frost border
    out.add(BoxItem{{midX, midY},
                    {rinkSize_.x - 10.0f, rinkSize_.y - 10.0f},
                    0.0f,
                    std::max(8.0f, cornerCut_ - 5.0f),
                    {244, 250, 255, static_cast<std::uint8_t>(config_.showRinkChrome ? 115 : 48)},
                    {165, 212, 246, 145},
                    1.2f});
    addPoly({{x0 + rinkSize_.x * 0.18f, y0 + 6.0f},
             {x0 + rinkSize_.x * 0.44f, y0 + 6.0f},
             {x0 + rinkSize_.x * 0.30f, y1 - 6.0f},
             {x0 + rinkSize_.x * 0.08f, y1 - 6.0f}},
            {255, 255, 255, static_cast<std::uint8_t>(config_.showRinkChrome ? 58 : 24)});

    // 3. Pro Ice-Hockey & Arcade Markings (Zone Blue Lines, Center Red Line, 4 Face-Off Circles, Goal Creases)
    const float zoneW = rinkSize_.x * 0.30f;
    const float blueL = x0 + zoneW;
    const float blueR = x1 - zoneW;
    out.add(LineItem{{blueL, y0 + 4.0f}, {blueL, y1 - 4.0f}, {42, 132, 228, 132}, 3.2f, false});
    out.add(LineItem{{blueR, y0 + 4.0f}, {blueR, y1 - 4.0f}, {42, 132, 228, 132}, 3.2f, false});

    // Center Red Line (soft outer band + crisp dashed core) & Dual Center Face-off Crest
    out.add(LineItem{{midX, y0 + 4.0f}, {midX, y1 - 4.0f}, {228, 56, 84, 72}, 5.0f, false});
    out.add(LineItem{{midX, y0 + 6.0f}, {midX, y1 - 6.0f}, {224, 42, 72, 196}, 2.0f, true});
    const float centerR = rinkSize_.y * 0.175f;
    out.add(CircleItem{{midX, midY}, centerR, {52, 142, 232, 18}, {42, 128, 222, 175}, 2.2f});
    out.add(CircleItem{{midX, midY}, centerR * 0.72f, {0, 0, 0, 0}, {228, 58, 86, 115}, 1.3f});
    out.add(CircleItem{{midX, midY}, 5.5f, {224, 44, 74, 235}, {255, 255, 255, 230}, 1.5f});

    // 4 Corner/Zone Face-off Circles with Crosshairs
    const float foR = rinkSize_.y * 0.105f;
    for (float fx : {x0 + rinkSize_.x * 0.17f, x1 - rinkSize_.x * 0.17f}) {
        const Color foCol = (fx < midX) ? Color{32, 148, 226, 128} : Color{226, 62, 102, 128};
        for (float fy : {y0 + rinkSize_.y * 0.25f, y1 - rinkSize_.y * 0.25f}) {
            out.add(CircleItem{{fx, fy}, foR, {0, 0, 0, 0}, foCol, 1.5f});
            out.add(CircleItem{{fx, fy}, 3.0f, foCol, {0, 0, 0, 0}, 0.0f});
            out.add(LineItem{{fx - 7.0f, fy}, {fx + 7.0f, fy}, foCol, 1.2f, false});
            out.add(LineItem{{fx, fy - 7.0f}, {fx, fy + 7.0f}, foCol, 1.2f, false});
        }
    }

    // Authentic Air-Jet Micro-Perforation Matrix across playfield
    if (config_.showRinkChrome) {
        const float stepX = std::max(28.0f, rinkSize_.x / 20.0f);
        const float stepY = std::max(26.0f, rinkSize_.y / 10.0f);
        for (float gx = x0 + stepX * 0.85f; gx < x1 - stepX * 0.65f; gx += stepX) {
            for (float gy = y0 + stepY * 0.80f; gy < y1 - stepY * 0.65f; gy += stepY) {
                out.add(CircleItem{{gx, gy}, 1.25f, {116, 156, 192, 92}, {0, 0, 0, 0}, 0.0f});
            }
        }
    }

    // Semicircular Goal Creases (`D-Zone`) inside left & right goals
    auto drawGoalCrease = [&](float gx, float dirSign, Color fillCol, Color strokeCol) {
        const float cr = goalHalfH_ * 1.02f;
        PolygonItem dPoly;
        dPoly.fill = fillCol;
        dPoly.stroke = {0, 0, 0, 0};
        dPoly.points[0] = {gx, midY - cr};
        for (int k = 0; k < 6; ++k) {
            const float ang = -1.5707963f + (static_cast<float>(k) / 5.0f) * 3.1415926f;
            dPoly.points[k + 1] = {gx + dirSign * std::cos(ang) * cr * 0.78f, midY + std::sin(ang) * cr};
        }
        dPoly.points[7] = {gx, midY + cr};
        dPoly.count = 8;
        out.add(dPoly);
        for (int k = 0; k < 5; ++k) {
            const float a0 = -1.5707963f + (static_cast<float>(k) / 5.0f) * 3.1415926f;
            const float a1 = -1.5707963f + (static_cast<float>(k + 1) / 5.0f) * 3.1415926f;
            out.add(LineItem{{gx + dirSign * std::cos(a0) * cr * 0.78f, midY + std::sin(a0) * cr},
                             {gx + dirSign * std::cos(a1) * cr * 0.78f, midY + std::sin(a1) * cr},
                             strokeCol,
                             2.0f,
                             false});
        }
    };
    drawGoalCrease(x0 + 1.0f, 1.0f, {54, 168, 248, 38}, {32, 146, 232, 185});
    drawGoalCrease(x1 - 1.0f, -1.0f, {248, 72, 112, 38}, {228, 56, 96, 185});

    // Split Team Neon LED Rail Strips & Integrated Recessed Goal Pockets (zero protruding outer ears!)
    const auto boost = static_cast<std::uint8_t>(goalFlash * 60.0f);
    const Color leftNeon{38, static_cast<std::uint8_t>(std::min(255, 195 + boost)), 255, 242};
    const Color rightNeon{255, static_cast<std::uint8_t>(std::min(255, 78 + boost)), 126, 242};
    if (config_.showRinkChrome) {
        out.add(LineItem{{x0 + cornerCut_, y0}, {midX - 3.0f, y0}, leftNeon, 3.0f, false});
        out.add(LineItem{{x0 + cornerCut_, y1}, {midX - 3.0f, y1}, leftNeon, 3.0f, false});
        out.add(LineItem{{midX + 3.0f, y0}, {x1 - cornerCut_, y0}, rightNeon, 3.0f, false});
        out.add(LineItem{{midX + 3.0f, y1}, {x1 - cornerCut_, y1}, rightNeon, 3.0f, false});
        out.add(LineItem{{x0, y0 + cornerCut_}, {x0, goalTop}, leftNeon, 3.0f, false});
        out.add(LineItem{{x0, goalBot}, {x0, y1 - cornerCut_}, leftNeon, 3.0f, false});
        out.add(LineItem{{x1, y0 + cornerCut_}, {x1, goalTop}, rightNeon, 3.0f, false});
        out.add(LineItem{{x1, goalBot}, {x1, y1 - cornerCut_}, rightNeon, 3.0f, false});
    }

    const float pocketDepth = std::min(railW * 0.92f, puckRadius_ * 1.35f);
    // Left recessed goal well (integrated flush inside left cabinet rail)
    out.add(BoxItem{{x0 - pocketDepth * 0.5f, midY},
                    {pocketDepth, goalHalfH_ * 2.0f},
                    0.0f,
                    5.0f,
                    {10, 18, 30, 245},
                    leftNeon,
                    2.0f});
    out.add(LineItem{{x0, goalTop + 2.0f}, {x0, goalBot - 2.0f}, {120, 232, 255, 235}, 2.4f, true});
    // Right recessed goal well (integrated flush inside right cabinet rail)
    out.add(BoxItem{{x1 + pocketDepth * 0.5f, midY},
                    {pocketDepth, goalHalfH_ * 2.0f},
                    0.0f,
                    5.0f,
                    {28, 12, 22, 245},
                    rightNeon,
                    2.0f});
    out.add(LineItem{{x1, goalTop + 2.0f}, {x1, goalBot - 2.0f}, {255, 138, 168, 235}, 2.4f, true});
    // Goal post caps
    for (float gy : {goalTop, goalBot}) {
        out.add(CircleItem{{x0, gy}, 4.5f, {235, 248, 255, 252}, leftNeon, 2.0f});
        out.add(CircleItem{{x1, gy}, 4.5f, {255, 238, 244, 252}, rightNeon, 2.0f});
    }

    // 4. Cheering spectator desktop icons along top & bottom stadium seats
    if (config_.showSpectators) {
        for (std::size_t idx : spectatorIcons_) {
            const Vec2 pos = iconCurrentSceneCenter(idx);
            out.add(CircleItem{pos + Vec2{0.0f, 3.0f}, 17.0f, {8, 14, 24, 95}, {0, 0, 0, 0}, 0.0f});
            SpriteItem sp;
            sp.texture = setup_.icons[idx].texture;
            sp.center = pos;
            sp.size = {32.0f, 32.0f};
            sp.opacity = 0.94f;
            out.add(sp);
        }
    }

    // 5. Layered Glossy Arcade Puck & Comet Velocity Wake
    if (goalPauseTimer_ <= 0.0f && winner_ == -1) {
        const BodyState ps = world_.state(puckBody_);
        const Vec2 puckSc = coords_.toScene(ps.position);
        const float spd = ps.linearVelocity.length();
        if (spd > 3.0f) {
            for (std::size_t i = 0; i < puckTrail_.size(); ++i) {
                const std::size_t idx = (trailHead_ + i) % puckTrail_.size();
                const float frac = static_cast<float>(i + 1) / static_cast<float>(puckTrail_.size() + 1);
                const auto a = static_cast<std::uint8_t>(frac * std::min(1.0f, spd / 16.0f) * 125.0f);
                out.add(CircleItem{puckTrail_[idx],
                                   puckRadius_ * (0.48f + 0.42f * frac),
                                   {56, 196, 255, a},
                                   {0, 0, 0, 0},
                                   0.0f});
            }
        }
        // Ice contact shadow + beveled crimson/graphite arcade puck + crisp center badge
        out.add(
            CircleItem{puckSc + Vec2{1.5f, 2.5f}, puckRadius_ + 2.5f, {16, 28, 46, 88}, {0, 0, 0, 0}, 0.0f});
        out.add(CircleItem{puckSc, puckRadius_ + 1.8f, {28, 36, 52, 252}, {238, 56, 88, 252}, 3.0f});
        out.add(CircleItem{puckSc, puckRadius_ * 0.78f, {245, 249, 255, 246}, {92, 114, 146, 195}, 1.4f});
        if (puckTex_ != kNoTexture) {
            SpriteItem psp;
            psp.texture = puckTex_;
            psp.center = puckSc;
            psp.size = {puckRadius_ * 1.42f, puckRadius_ * 1.42f};
            psp.clipCircle = true;
            out.add(psp);
        }
    }

    // 6. Sculpted 3D Tactile Arcade Strikers (Mallets: Bumper Ring + Finger Well + Central Knob)
    auto drawMallet = [&](BodyIndex body, TextureId tex, Color neonRim, Color bodyFill, Color wellFill) {
        const Vec2 sc = coords_.toScene(world_.state(body).position);
        // Ice reflection & drop shadow
        out.add(CircleItem{sc + Vec2{0.0f, 2.5f}, malletRadius_ + 4.5f,
                           Color{neonRim.r, neonRim.g, neonRim.b, 46}, {0, 0, 0, 0}, 0.0f});
        // High-impact outer polymer striker skirt
        out.add(CircleItem{sc, malletRadius_, bodyFill, neonRim, 3.6f});
        // Recessed dark ergonomic finger well
        out.add(CircleItem{sc, malletRadius_ * 0.74f, wellFill, {255, 255, 255, 82}, 1.5f});
        // Central raised 3D grip handle + icon badge
        out.add(CircleItem{sc + Vec2{0.0f, -1.0f}, malletRadius_ * 0.50f, bodyFill, neonRim, 2.0f});
        if (tex != kNoTexture) {
            SpriteItem msp;
            msp.texture = tex;
            msp.center = sc + Vec2{0.0f, -1.0f};
            msp.size = {malletRadius_ * 0.88f, malletRadius_ * 0.88f};
            msp.clipCircle = true;
            out.add(msp);
        }
        // Crisp specular highlight dot on top-left of striker handle
        out.add(CircleItem{sc + Vec2{-malletRadius_ * 0.18f, -malletRadius_ * 0.22f},
                           malletRadius_ * 0.11f,
                           {255, 255, 255, 138},
                           {0, 0, 0, 0},
                           0.0f});
    };
    drawMallet(leftMalletBody_, leftTex_, {42, 212, 255, 255}, {24, 118, 196, 248}, {14, 32, 56, 242});
    drawMallet(rightMalletBody_, rightTex_, {255, 92, 132, 255}, {198, 38, 78, 248}, {54, 16, 30, 242});

    // 7. Top-left HUD pills & Broadcast Arcade Scoreboard Pill
    const Color pillBg{18, 24, 38, 218};
    out.add(BoxItem{{chromeToggleRect_.x + chromeToggleRect_.width * 0.5f,
                     chromeToggleRect_.y + chromeToggleRect_.height * 0.5f},
                    {chromeToggleRect_.width, chromeToggleRect_.height},
                    0.0f,
                    13.0f,
                    pillBg,
                    {92, 206, 255, 215},
                    1.5f});
    out.addText({chromeToggleRect_.x + chromeToggleRect_.width * 0.5f, chromeToggleRect_.y + 6.0f},
                config_.showRinkChrome ? "球台外观: 街机" : "球台外观: 透明", {175, 235, 255, 255}, 12.0f,
                true);

    if (config_.matchMode == AirHockeyMatchMode::SoloAI) {
        out.add(BoxItem{
            {diffCycleRect_.x + diffCycleRect_.width * 0.5f, diffCycleRect_.y + diffCycleRect_.height * 0.5f},
            {diffCycleRect_.width, diffCycleRect_.height},
            0.0f,
            13.0f,
            pillBg,
            {255, 148, 178, 215},
            1.5f});
        out.addText({diffCycleRect_.x + diffCycleRect_.width * 0.5f, diffCycleRect_.y + 6.0f},
                    diffLabel(config_.difficulty), {255, 208, 222, 255}, 12.5f, true);
    }

    const char* rightName =
        config_.matchMode == AirHockeyMatchMode::SoloAI
            ? diffLabel(config_.difficulty)
            : (config_.matchMode == AirHockeyMatchMode::Lan ? "P2(客机)" : "P2(WASD)");
    const char* leftName = (config_.matchMode == AirHockeyMatchMode::Lan) ? "P1(房主)" : "P1(鼠标)";

    char scoreText[128]{};
    const float bannerY = std::max(58.0f, (y0 - (config_.showRinkChrome ? railW : 0.0f)) * 0.50f);
    out.add(BoxItem{{w * 0.5f, bannerY + 9.0f},
                    {396.0f, 30.0f},
                    0.0f,
                    15.0f,
                    {14, 20, 34, 216},
                    goalFlash > 0.25f ? Color{255, 218, 92, 235} : Color{96, 148, 210, 185},
                    1.5f});
    if (winner_ != -1) {
        std::snprintf(scoreText, sizeof(scoreText), "%s 获胜！比分 %d : %d · 点击球台再来一局",
                      winner_ == 0 ? leftName : rightName, scores_[0], scores_[1]);
        out.addText({w * 0.5f, bannerY}, scoreText, {135, 255, 175, 255}, 14.0f, true);
    } else if (goalFlash > 0.25f) {
        std::snprintf(scoreText, sizeof(scoreText), "GOAL!!   %s  %d  :  %d  %s   (抢%d)", leftName,
                      scores_[0], scores_[1], rightName, config_.targetGoals);
        out.addText({w * 0.5f, bannerY}, scoreText, {255, 232, 108, 255}, 14.0f, true);
    } else {
        std::snprintf(scoreText, sizeof(scoreText), "%s   %d  :  %d   %s   ·  抢 %d 胜", leftName,
                      scores_[0], scores_[1], rightName, config_.targetGoals);
        out.addText({w * 0.5f, bannerY}, scoreText, {242, 248, 255, 250}, 13.5f, true);
    }
}

}  // namespace dpp
