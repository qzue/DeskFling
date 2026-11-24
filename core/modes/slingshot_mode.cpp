#include "core/modes/slingshot_mode.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>

namespace dpp {

namespace {

constexpr float kPi = 3.14159265358979323846f;

std::uint32_t nextLcg(std::uint32_t& state) {
    state = state * 1664525u + 1013904223u;
    return state;
}

float randRange(std::uint32_t& state, float lo, float hi) {
    const float u = static_cast<float>(nextLcg(state) & 0x00FFFFFFu) / static_cast<float>(0x01000000u);
    return lo + (hi - lo) * u;
}

bool rectContains(const Rect& r, Vec2 p) {
    return p.x >= r.x && p.x <= r.x + r.width && p.y >= r.y && p.y <= r.y + r.height;
}

const char* presetLabel(SlingshotPreset p) {
    switch (p) {
        case SlingshotPreset::TwinTowers:
            return "经典双塔";
        case SlingshotPreset::PyramidFort:
            return "金字塔要塞";
        case SlingshotPreset::GlassBridge:
            return "冰晶悬廊";
        case SlingshotPreset::StoneCitadel:
            return "巨石碉堡";
        case SlingshotPreset::RandomSiege:
            return "随机攻城";
        case SlingshotPreset::CustomBuilder:
            return "自建工坊";
    }
    return "经典双塔";
}

const char* starBadge(int stars) {
    switch (std::clamp(stars, 0, 3)) {
        case 3:
            return "★★★";
        case 2:
            return "★★☆";
        case 1:
            return "★☆☆";
        default:
            return "☆☆☆";
    }
}

PhysicsConfig makeSlingshotPhysics() {
    PhysicsConfig pc;
    pc.gravity = {0.0f, -9.8f};
    pc.subSteps = 8;
    pc.enableSleep = true;
    pc.maxContactPushSpeed = 2.5f;
    pc.maximumLinearSpeed = 48.0f;
    pc.enableHitEvents = true;
    pc.hitEventThreshold = 0.45f;
    return pc;
}

}  // namespace

int SlingshotProgress::totalStars() const {
    int sum = 0;
    for (std::uint8_t s : stageStars) sum += std::clamp<int>(s, 0, 3);
    return sum;
}

int SlingshotProgress::starsForStage(int stageIndex) const {
    if (stageIndex < 1 || stageIndex > static_cast<int>(stageStars.size())) return 0;
    return std::clamp<int>(stageStars[static_cast<std::size_t>(stageIndex - 1)], 0, 3);
}

int SlingshotProgress::starsForPreset(SlingshotPreset preset) const {
    const auto idx = static_cast<std::size_t>(preset);
    if (idx >= presetBestStars.size()) return 0;
    return std::clamp<int>(presetBestStars[idx], 0, 3);
}

void SlingshotProgress::recordClear(SlingshotPreset preset, int stageIndex, int stars, int finalScore) {
    const auto clampedStars = static_cast<std::uint8_t>(std::clamp(stars, 1, 3));
    highScore = std::max(highScore, std::max(0, finalScore));
    maxUnlockedStage = std::clamp(std::max(maxUnlockedStage, stageIndex + 1), 1, 99);
    const auto pIdx = static_cast<std::size_t>(preset);
    if (pIdx < presetBestStars.size()) {
        presetBestStars[pIdx] = std::max(presetBestStars[pIdx], clampedStars);
    }
    if (stageIndex >= 1 && stageIndex <= static_cast<int>(stageStars.size())) {
        auto& slot = stageStars[static_cast<std::size_t>(stageIndex - 1)];
        slot = std::max(slot, clampedStars);
    }
}

std::string encodeSlingshotProgress(const SlingshotProgress& p) {
    char buf[256]{};
    std::string out;
    out.reserve(128);
    std::snprintf(buf, sizeof(buf), "v1:%d:%d:", std::max(0, p.highScore), std::clamp(p.maxUnlockedStage, 1, 99));
    out += buf;
    for (std::size_t i = 0; i < p.presetBestStars.size(); ++i) {
        if (i > 0) out.push_back(',');
        out.push_back(static_cast<char>('0' + std::clamp<int>(p.presetBestStars[i], 0, 3)));
    }
    out.push_back(':');
    for (std::size_t i = 0; i < p.stageStars.size(); ++i) {
        if (i > 0) out.push_back(',');
        out.push_back(static_cast<char>('0' + std::clamp<int>(p.stageStars[i], 0, 3)));
    }
    return out;
}

SlingshotProgress decodeSlingshotProgress(std::string_view encoded) {
    SlingshotProgress out{};
    if (encoded.size() < 6 || encoded.substr(0, 3) != "v1:") return out;
    const auto c1 = encoded.find(':', 3);
    if (c1 == std::string_view::npos) return out;
    const auto c2 = encoded.find(':', c1 + 1);
    if (c2 == std::string_view::npos) return out;
    const auto c3 = encoded.find(':', c2 + 1);
    if (c3 == std::string_view::npos) return out;

    auto parsePositive = [](std::string_view sv, int fallback) -> int {
        if (sv.empty() || sv.size() > 10) return fallback;
        long v = 0;
        for (char ch : sv) {
            if (ch < '0' || ch > '9') return fallback;
            v = v * 10 + (ch - '0');
            if (v > 999999999L) return 999999999;
        }
        return static_cast<int>(v);
    };

    out.highScore = std::max(0, parsePositive(encoded.substr(3, c1 - 3), 0));
    out.maxUnlockedStage = std::clamp(parsePositive(encoded.substr(c1 + 1, c2 - c1 - 1), 1), 1, 99);

    auto parseDigits = [](std::string_view sv, std::uint8_t* dst, std::size_t cap) {
        std::size_t idx = 0;
        for (char ch : sv) {
            if (ch == ',') continue;
            if (idx >= cap) break;
            if (ch >= '0' && ch <= '3') {
                dst[idx++] = static_cast<std::uint8_t>(ch - '0');
            } else {
                dst[idx++] = 0;
            }
        }
    };
    parseDigits(encoded.substr(c2 + 1, c3 - c2 - 1), out.presetBestStars.data(), out.presetBestStars.size());
    parseDigits(encoded.substr(c3 + 1), out.stageStars.data(), out.stageStars.size());
    return out;
}

std::vector<SlingshotCustomPiece> defaultSlingshotCustomPieces() {
    return {
        {SlingshotCustomPieceKind::WoodPillar, -0.65f, 0.59f},
        {SlingshotCustomPieceKind::WoodPillar, 0.65f, 0.59f},
        {SlingshotCustomPieceKind::DefenderSlot, 0.0f, 0.34f},
        {SlingshotCustomPieceKind::WoodBeam, 0.0f, 1.30f},
        {SlingshotCustomPieceKind::GlassBeam, 0.0f, 1.56f},
        {SlingshotCustomPieceKind::DefenderSlot, 0.0f, 2.00f},
        {SlingshotCustomPieceKind::StoneBlock, -1.35f, 0.45f},
        {SlingshotCustomPieceKind::DefenderSlot, -1.35f, 1.24f},
    };
}

std::string encodeSlingshotCustomPieces(std::span<const SlingshotCustomPiece> pieces) {
    std::string out = "c1";
    const std::size_t cap = std::min<std::size_t>(pieces.size(), 48u);
    char buf[64]{};
    for (std::size_t i = 0; i < cap; ++i) {
        const auto& p = pieces[i];
        const int k = std::clamp(static_cast<int>(p.kind), 0, 4);
        const float nx = std::isfinite(p.normX) ? std::clamp(p.normX, -2.8f, 2.4f) : 0.0f;
        const float nh = std::isfinite(p.normHeight) ? std::clamp(p.normHeight, 0.15f, 4.8f) : 0.6f;
        std::snprintf(buf, sizeof(buf), ";%d,%.3f,%.3f", k, static_cast<double>(nx), static_cast<double>(nh));
        for (char* c = buf; *c != '\0'; ++c) {
            if (*c == ',' && c > buf + 3 && *(c - 1) >= '0' && *(c - 1) <= '9' && *(c + 1) >= '0' &&
                *(c + 1) <= '9') {
                // locale decimal guard: snprintf format uses explicit commas only as field delimiters
            }
        }
        out += buf;
    }
    return out;
}

std::vector<SlingshotCustomPiece> decodeSlingshotCustomPieces(std::string_view encoded) {
    std::vector<SlingshotCustomPiece> out;
    if (encoded.size() < 2 || encoded.substr(0, 2) != "c1") return out;
    std::size_t pos = 2;
    while (pos < encoded.size() && out.size() < 48u) {
        if (encoded[pos] == ';') ++pos;
        const auto next = encoded.find(';', pos);
        const std::string_view token =
            (next == std::string_view::npos) ? encoded.substr(pos) : encoded.substr(pos, next - pos);
        pos = (next == std::string_view::npos) ? encoded.size() : (next + 1);
        if (token.empty()) continue;

        const auto c1 = token.find(',');
        const auto c2 = (c1 == std::string_view::npos) ? std::string_view::npos : token.find(',', c1 + 1);
        if (c1 == std::string_view::npos || c2 == std::string_view::npos) continue;

        auto parseF = [](std::string_view sv, float& val) -> bool {
            if (sv.empty() || sv.size() > 16) return false;
            char tmp[20]{};
            std::copy(sv.begin(), sv.end(), tmp);
            char* endp = nullptr;
            const float f = std::strtof(tmp, &endp);
            if (!endp || *endp != '\0' || !std::isfinite(f)) return false;
            val = f;
            return true;
        };
        float kf = 0.0f, nx = 0.0f, nh = 0.6f;
        if (!parseF(token.substr(0, c1), kf) || !parseF(token.substr(c1 + 1, c2 - c1 - 1), nx) ||
            !parseF(token.substr(c2 + 1), nh)) {
            continue;
        }
        const int ki = std::clamp(static_cast<int>(std::lround(kf)), 0, 4);
        out.push_back({static_cast<SlingshotCustomPieceKind>(ki), std::clamp(nx, -2.8f, 2.4f),
                       std::clamp(nh, 0.15f, 4.8f)});
    }
    return out;
}

SlingshotMode::SlingshotMode(const SceneSetup& setup, const SlingshotConfig& config, int stageIndex,
                             int carryScore, int highScore, const SlingshotProgress& progress)
    : setup_(setup),
      config_(config),
      coords_(setup.workAreaSize),
      world_(std::make_unique<PhysicsWorld>(makeSlingshotPhysics())),
      stageIndex_(std::max(1, stageIndex)),
      score_(std::max(0, carryScore)),
      highScore_(std::max({highScore, carryScore, progress.highScore})),
      editing_(config.startInEditor),
      progress_(progress) {
    progress_.highScore = highScore_;
    editorPieces_ = config_.customPieces.empty() ? defaultSlingshotCustomPieces() : config_.customPieces;
    if (config_.preset == SlingshotPreset::CustomBuilder && config_.customPieces.empty()) {
        config_.customPieces = editorPieces_;
    }
    const float w = setup_.workAreaSize.x;
    const float h = setup_.workAreaSize.y;

    groundY_ = std::clamp(h * 0.86f, 120.0f, h - 28.0f);
    const float floorPhysY = coords_.toPhysics({0.0f, groundY_}).y;
    const Vec2 worldMeters = coords_.worldSizeMeters();
    world_->createBoundary({0.0f, floorPhysY}, {worldMeters.x, std::max(0.5f, worldMeters.y - floorPhysY)},
                           true, 1.0f, 0.72f, 0.22f);

    const float slingH = std::clamp(h * 0.19f, 92.0f, 162.0f);
    slingBase_ = {std::clamp(w * 0.175f, 118.0f, w * 0.28f), groundY_};
    forkBackTip_ = {slingBase_.x + slingH * 0.21f, groundY_ - slingH * 1.05f};
    forkFrontTip_ = {slingBase_.x - slingH * 0.23f, groundY_ - slingH * 0.99f};
    slingAnchor_ = {(forkBackTip_.x + forkFrontTip_.x) * 0.5f,
                    (forkBackTip_.y + forkFrontTip_.y) * 0.5f + slingH * 0.08f};
    maxPull_ = std::clamp(slingH * 0.96f, 72.0f, 148.0f);
    projRadius_ = std::clamp(std::min(w, h) * 0.025f, 15.0f, 24.0f);
    fortUnit_ = std::clamp(std::min(w * 0.055f, h * 0.095f), 42.0f, 84.0f);
    fortAnchorX_ = std::clamp(w * 0.71f, slingBase_.x + fortUnit_ * 3.8f, w - fortUnit_ * 2.2f);

    trajToggleRect_ = {24.0f, 18.0f, 108.0f, 28.0f};
    nextStageRect_ = {140.0f, 18.0f, 96.0f, 28.0f};
    editorToggleRect_ = {244.0f, 18.0f, 104.0f, 28.0f};

    // Partition icons into ammo vs fortress defenders.
    const std::size_t n = setup_.icons.size();
    std::vector<std::size_t> order(n);
    std::iota(order.begin(), order.end(), std::size_t{0});

    std::uint32_t seed = config_.randomSeed ^ (static_cast<std::uint32_t>(stageIndex_) * 2654435761u);
    if (config_.iconSelection == IconSelectionPolicy::Random && n > 1) {
        for (std::size_t i = n - 1; i > 0; --i) {
            const std::size_t j = static_cast<std::size_t>(nextLcg(seed) % (i + 1));
            std::swap(order[i], order[j]);
        }
    } else if (config_.iconSelection == IconSelectionPolicy::Custom && !config_.customIconIndices.empty()) {
        std::vector<std::size_t> picked;
        std::vector<bool> used(n, false);
        for (std::size_t idx : config_.customIconIndices) {
            if (idx < n && !used[idx]) {
                used[idx] = true;
                picked.push_back(idx);
            }
        }
        for (std::size_t i = 0; i < n; ++i) {
            if (!used[i]) picked.push_back(i);
        }
        order = std::move(picked);
    }

    const int desiredAmmo = std::clamp(config_.ammoCount, 3, 6);
    std::vector<std::optional<std::size_t>> chosenAmmo;
    std::vector<std::size_t> defenderIcons;
    const std::size_t realAmmoCount =
        n == 0 ? 0 : (n <= 2 ? 1 : std::min<std::size_t>(static_cast<std::size_t>(desiredAmmo), n / 2));
    for (std::size_t i = 0; i < realAmmoCount; ++i) chosenAmmo.push_back(order[i]);
    while (static_cast<int>(chosenAmmo.size()) < desiredAmmo) chosenAmmo.push_back(std::nullopt);

    constexpr std::size_t kMaxFortDefenders = 8;
    for (std::size_t i = realAmmoCount; i < n; ++i) {
        if (defenderIcons.size() < kMaxFortDefenders) {
            defenderIcons.push_back(order[i]);
        } else {
            parkedIcons_.push_back(order[i]);
        }
    }

    // Create disabled projectile bodies for ammo slots.
    BodyMaterial projMat;
    projMat.density = 2.5f;
    projMat.friction = 0.48f;
    projMat.restitution = 0.38f;
    projMat.linearDamping = 0.08f;
    projMat.angularDamping = 0.25f;
    projMat.bullet = true;

    const float queueSpacing = projRadius_ * 2.25f;
    for (std::size_t i = 0; i < chosenAmmo.size(); ++i) {
        AmmoSlot slot;
        slot.iconIndex = chosenAmmo[i];
        slot.texture = chosenAmmo[i] ? setup_.icons[*chosenAmmo[i]].texture
                                     : (n > 0 ? setup_.icons[i % n].texture : TextureId{1});
        const float qx =
            std::max(projRadius_ + 12.0f, slingBase_.x - 42.0f - static_cast<float>(i) * queueSpacing);
        slot.waitPosScene = {qx, groundY_ - projRadius_};
        slot.body =
            world_->addCircle(coords_.toPhysics(slingAnchor_), coords_.lengthToPhysics(projRadius_), projMat);
        world_->setEnabled(slot.body, false);
        ammo_.push_back(slot);
    }

    // Park any overflow icons (> 8 defenders) along a neat reserve shelf at bottom-right.
    for (std::size_t i = 0; i < parkedIcons_.size(); ++i) {
        const float px = w - 34.0f - static_cast<float>(i % 10) * 40.0f;
        const float py = std::min(h - 20.0f, groundY_ + 22.0f + static_cast<float>(i / 10) * 26.0f);
        parkedPositions_.push_back({std::max(34.0f, px), py});
    }

    buildFortress(resolvedPreset(), defenderIcons);

    // Put all blocks and defenders cleanly to sleep on frame 0 so nothing wobbles before aim.
    for (const auto& b : blocks_) world_->setVelocity(b.body, {0.0f, 0.0f}, 0.0f);
    for (const auto& d : defenders_) world_->setVelocity(d.body, {0.0f, 0.0f}, 0.0f);

    loadNextAmmo();
}

SlingshotPreset SlingshotMode::resolvedPreset() const {
    if (config_.preset == SlingshotPreset::CustomBuilder) {
        return SlingshotPreset::CustomBuilder;
    }
    if (config_.preset != SlingshotPreset::RandomSiege) {
        if (stageIndex_ == 1) return config_.preset;
        const int base = static_cast<int>(config_.preset);
        return static_cast<SlingshotPreset>((base + stageIndex_ - 1) % 4);
    }
    return SlingshotPreset::RandomSiege;
}

Vec2 SlingshotMode::customPieceSceneSize(SlingshotCustomPieceKind kind) const {
    const float thick = std::clamp(fortUnit_ * 0.22f, 10.0f, 18.0f);
    switch (kind) {
        case SlingshotCustomPieceKind::WoodPillar:
            return {thick, fortUnit_ * 1.18f};
        case SlingshotCustomPieceKind::WoodBeam:
            return {fortUnit_ * 1.48f, thick};
        case SlingshotCustomPieceKind::StoneBlock:
            return {fortUnit_ * 0.86f, fortUnit_ * 0.86f};
        case SlingshotCustomPieceKind::GlassBeam:
            return {fortUnit_ * 1.36f, thick};
        case SlingshotCustomPieceKind::DefenderSlot: {
            const float d = projRadius_ * 1.88f;
            return {d, d};
        }
    }
    return {thick, fortUnit_};
}

Vec2 SlingshotMode::customPieceSceneCenter(const SlingshotCustomPiece& p) const {
    const Vec2 sz = customPieceSceneSize(p.kind);
    const float cx = std::clamp(fortAnchorX_ + p.normX * fortUnit_, slingBase_.x + fortUnit_ * 1.9f,
                                setup_.workAreaSize.x - sz.x * 0.5f - 18.0f);
    const float cy = std::clamp(groundY_ - p.normHeight * fortUnit_, 92.0f + sz.y * 0.5f,
                                groundY_ - sz.y * 0.5f - 0.5f);
    return {cx, cy};
}

Rect SlingshotMode::customBuildZoneScene() const {
    const float left = std::min(setup_.workAreaSize.x - 120.0f, slingBase_.x + fortUnit_ * 1.8f);
    const float right = std::max(left + 80.0f, setup_.workAreaSize.x - 20.0f);
    const float top = 92.0f;
    return {left, top, right - left, std::max(60.0f, groundY_ - top)};
}

SlingshotCustomPiece SlingshotMode::sceneToCustomPiece(SlingshotCustomPieceKind kind, Vec2 scenePos) const {
    const Vec2 sz = customPieceSceneSize(kind);
    const Rect zone = customBuildZoneScene();
    const float clampedX = std::clamp(scenePos.x, zone.x + sz.x * 0.5f, zone.x + zone.width - sz.x * 0.5f);
    const float clampedY = std::clamp(scenePos.y, zone.y + sz.y * 0.5f, groundY_ - sz.y * 0.5f - 0.5f);
    return {kind, std::clamp((clampedX - fortAnchorX_) / fortUnit_, -2.8f, 2.4f),
            std::clamp((groundY_ - clampedY) / fortUnit_, 0.15f, 4.8f)};
}

Rect SlingshotMode::editorToolPillRect(int toolIndex) const {
    const float x = 356.0f + static_cast<float>(toolIndex) * 58.0f;
    return {x, 18.0f, 54.0f, 28.0f};
}

void SlingshotMode::addBlockScene(Vec2 centerScene, Vec2 sizeScene, SlingshotBlockKind mat, float angleRad) {
    BodyMaterial bm;
    float hp = 12.0f;
    switch (mat) {
        case SlingshotBlockKind::Glass:
            bm.density = 0.78f;
            bm.friction = 0.38f;
            bm.restitution = 0.14f;
            hp = 2.8f;
            break;
        case SlingshotBlockKind::Wood:
            bm.density = 1.18f;
            bm.friction = 0.62f;
            bm.restitution = 0.10f;
            hp = 6.2f;
            break;
        case SlingshotBlockKind::Stone:
            bm.density = 2.35f;
            bm.friction = 0.75f;
            bm.restitution = 0.06f;
            hp = 13.5f;
            break;
    }
    bm.linearDamping = 0.10f;
    bm.angularDamping = 0.35f;

    const Vec2 halfPhys = {coords_.lengthToPhysics(sizeScene.x * 0.5f),
                           coords_.lengthToPhysics(sizeScene.y * 0.5f)};
    const BodyIndex body = world_->addBox(coords_.toPhysics(centerScene), halfPhys, -angleRad, bm, 0.002f);
    const std::uint32_t vSeed =
        static_cast<std::uint32_t>(blocks_.size() * 2654435761u) ^
        static_cast<std::uint32_t>(std::lround(centerScene.x * 13.0f + centerScene.y * 37.0f));
    blocks_.push_back({body, mat, sizeScene, hp, hp, false, vSeed, {0.0f, 0.0f}});
}

void SlingshotMode::addDefenderScene(Vec2 centerScene, std::optional<std::size_t> iconIdx, TextureId tex) {
    BodyMaterial dm;
    dm.density = 1.05f;
    dm.friction = 0.55f;
    dm.restitution = 0.24f;
    dm.linearDamping = 0.14f;
    dm.angularDamping = 0.40f;

    const float r = projRadius_ * 0.94f;
    const BodyIndex body = world_->addCircle(coords_.toPhysics(centerScene), coords_.lengthToPhysics(r), dm);
    const std::size_t slot = defenders_.size();
    const Vec2 trophy = {setup_.workAreaSize.x - 36.0f - static_cast<float>(slot) * (r * 2.15f), 32.0f};
    defenders_.push_back({body, iconIdx, tex, r, 1.35f, false, trophy, 1.0f, centerScene});
}

void SlingshotMode::buildFortress(SlingshotPreset effectivePreset,
                                  const std::vector<std::size_t>& defenderIcons) {
    const float w = setup_.workAreaSize.x;
    const float h = setup_.workAreaSize.y;
    const float defR = projRadius_ * 0.94f;
    const float unit = std::clamp(std::min(w * 0.055f, h * 0.095f), 46.0f, 84.0f);
    const float thick = std::clamp(unit * 0.24f, 12.0f, 20.0f);
    const float pillarH = std::max(unit * 1.18f, defR * 2.35f + 8.0f);
    const float p2BaseH = std::max(pillarH * 0.86f, defR * 2.25f + 6.0f);
    const float minSafeSpan = defR + thick * 0.5f + 5.0f;
    const float beamW = std::max(unit * 1.48f, (minSafeSpan + thick * 0.5f) * 2.08f);
    const float fortX = std::clamp(w * 0.72f, slingBase_.x + unit * 4.2f, w - beamW * 1.65f - 20.0f);

    std::size_t nextDef = 0;
    auto spawnSlotDefender = [&](Vec2 pos) {
        std::optional<std::size_t> idx;
        TextureId tex = TextureId{static_cast<std::uint32_t>((defenders_.size() % 6) + 1)};
        if (nextDef < defenderIcons.size()) {
            idx = defenderIcons[nextDef++];
            tex = setup_.icons[*idx].texture;
        } else if (!setup_.icons.empty()) {
            tex = setup_.icons[defenders_.size() % setup_.icons.size()].texture;
        }
        addDefenderScene(pos, idx, tex);
    };

    std::vector<Vec2> extraPerches;
    extraPerches.reserve(6);

    if (effectivePreset == SlingshotPreset::TwinTowers) {
        const float towerSep = std::max(beamW * 0.84f, unit * 0.55f + beamW * 0.55f);
        const float span1 = std::max(beamW * 0.37f, minSafeSpan);
        const float span2 = std::max(beamW * 0.33f, minSafeSpan);
        for (int t = -1; t <= 1; t += 2) {
            const float cx = fortX + static_cast<float>(t) * towerSep;
            // Story 1
            addBlockScene({cx - span1, groundY_ - pillarH * 0.5f}, {thick, pillarH}, SlingshotBlockKind::Wood);
            addBlockScene({cx + span1, groundY_ - pillarH * 0.5f}, {thick, pillarH}, SlingshotBlockKind::Wood);
            spawnSlotDefender({cx, groundY_ - defR - 0.5f});
            const float roof1Y = groundY_ - pillarH - thick * 0.5f;
            addBlockScene({cx, roof1Y}, {std::max(beamW, (span1 + thick * 0.5f) * 2.04f), thick},
                          SlingshotBlockKind::Wood);
            // Story 2
            const float base2 = roof1Y - thick * 0.5f;
            addBlockScene({cx - span2, base2 - p2BaseH * 0.5f}, {thick, p2BaseH}, SlingshotBlockKind::Glass);
            addBlockScene({cx + span2, base2 - p2BaseH * 0.5f}, {thick, p2BaseH}, SlingshotBlockKind::Glass);
            spawnSlotDefender({cx, base2 - defR - 0.5f});
            const float roof2Y = base2 - p2BaseH - thick * 0.5f;
            addBlockScene({cx, roof2Y}, {std::max(beamW * 0.92f, (span2 + thick * 0.5f) * 2.04f), thick},
                          SlingshotBlockKind::Stone);
            extraPerches.push_back({cx, roof2Y - thick * 0.5f - defR - 0.5f});
        }
        // Center crystal monolith pedestal (strictly non-overlapping between the two towers)
        const float centerColW = std::min(unit * 0.68f, (towerSep - span1 - thick * 0.5f) * 1.55f);
        if (centerColW >= 18.0f) {
            addBlockScene({fortX, groundY_ - pillarH * 0.42f}, {centerColW, pillarH * 0.84f},
                          SlingshotBlockKind::Glass);
            spawnSlotDefender({fortX, groundY_ - pillarH * 0.84f - defR - 0.5f});
        } else {
            spawnSlotDefender({fortX, groundY_ - defR - 0.5f});
        }
    } else if (effectivePreset == SlingshotPreset::PyramidFort) {
        // 3 non-overlapping bays on bottom, 2 in middle, 1 on top (with guaranteed >= 4pt horizontal roof gaps!)
        const float span1 = std::max(beamW * 0.35f, minSafeSpan);
        const float roof1W = (span1 + thick * 0.5f) * 2.06f;
        const float bayW = roof1W + 4.5f;
        for (int b = -1; b <= 1; ++b) {
            const float cx = fortX + static_cast<float>(b) * bayW;
            const auto mat = (b == 0) ? SlingshotBlockKind::Stone : SlingshotBlockKind::Wood;
            addBlockScene({cx - span1, groundY_ - pillarH * 0.5f}, {thick, pillarH}, mat);
            addBlockScene({cx + span1, groundY_ - pillarH * 0.5f}, {thick, pillarH}, mat);
            spawnSlotDefender({cx, groundY_ - defR - 0.5f});
            addBlockScene({cx, groundY_ - pillarH - thick * 0.5f}, {roof1W, thick}, SlingshotBlockKind::Wood);
        }
        const float lv2Base = groundY_ - pillarH - thick;
        const float span2 = std::max(beamW * 0.33f, minSafeSpan);
        const float roof2W = std::min(bayW - 4.5f, (span2 + thick * 0.5f) * 2.04f);
        for (int b = 0; b < 2; ++b) {
            const float cx = fortX + (static_cast<float>(b) - 0.5f) * bayW;
            addBlockScene({cx - span2, lv2Base - p2BaseH * 0.5f}, {thick, p2BaseH}, SlingshotBlockKind::Glass);
            addBlockScene({cx + span2, lv2Base - p2BaseH * 0.5f}, {thick, p2BaseH}, SlingshotBlockKind::Glass);
            spawnSlotDefender({cx, lv2Base - defR - 0.5f});
            addBlockScene({cx, lv2Base - p2BaseH - thick * 0.5f}, {roof2W, thick}, SlingshotBlockKind::Wood);
            extraPerches.push_back(
                {cx + (b == 0 ? -roof2W * 0.26f : roof2W * 0.26f), lv2Base - p2BaseH - thick - defR - 0.5f});
        }
        // Apex crown platform spanning the two level-2 roofs cleanly
        const float lv3Base = lv2Base - p2BaseH - thick;
        addBlockScene({fortX, lv3Base - thick * 0.5f}, {bayW * 0.72f, thick}, SlingshotBlockKind::Stone);
        spawnSlotDefender({fortX, lv3Base - thick - defR - 0.5f});
    } else if (effectivePreset == SlingshotPreset::GlassBridge) {
        // Two stone abutments + non-overlapping center glass suspension bay
        const float sideSpan = std::max(beamW * 0.33f, minSafeSpan);
        const float sideRoofW = (sideSpan + thick * 0.6f) * 2.04f;
        const float centerSpan = std::max(beamW * 0.31f, minSafeSpan);
        const float centerDeckW = (centerSpan + thick * 0.5f) * 2.08f;
        const float spanX = (sideRoofW + centerDeckW) * 0.5f + 5.0f;
        for (int side = -1; side <= 1; side += 2) {
            const float cx = fortX + static_cast<float>(side) * spanX;
            addBlockScene({cx - sideSpan, groundY_ - pillarH * 0.5f}, {thick * 1.15f, pillarH},
                          SlingshotBlockKind::Stone);
            addBlockScene({cx + sideSpan, groundY_ - pillarH * 0.5f}, {thick * 1.15f, pillarH},
                          SlingshotBlockKind::Stone);
            spawnSlotDefender({cx, groundY_ - defR - 0.5f});
            addBlockScene({cx, groundY_ - pillarH - thick * 0.5f}, {sideRoofW, thick},
                          SlingshotBlockKind::Stone);
            extraPerches.push_back({cx, groundY_ - pillarH - thick - defR - 0.5f});
        }
        // Center glass stilts + bridge deck (strictly >= 5pt clear of stone abutment roofs!)
        addBlockScene({fortX - centerSpan, groundY_ - pillarH * 0.5f}, {thick * 0.95f, pillarH},
                      SlingshotBlockKind::Glass);
        addBlockScene({fortX + centerSpan, groundY_ - pillarH * 0.5f}, {thick * 0.95f, pillarH},
                      SlingshotBlockKind::Glass);
        spawnSlotDefender({fortX, groundY_ - defR - 0.5f});
        const float deckY = groundY_ - pillarH - thick * 0.5f;
        addBlockScene({fortX, deckY}, {centerDeckW, thick}, SlingshotBlockKind::Glass);
        const float upperBase = deckY - thick * 0.5f;
        addBlockScene({fortX - centerSpan, upperBase - p2BaseH * 0.5f}, {thick, p2BaseH},
                      SlingshotBlockKind::Wood);
        addBlockScene({fortX + centerSpan, upperBase - p2BaseH * 0.5f}, {thick, p2BaseH},
                      SlingshotBlockKind::Wood);
        spawnSlotDefender({fortX, upperBase - defR - 0.5f});
        const float topRoofY = upperBase - p2BaseH - thick * 0.5f;
        addBlockScene({fortX, topRoofY}, {centerDeckW * 0.96f, thick}, SlingshotBlockKind::Wood);
        extraPerches.push_back({fortX, topRoofY - thick * 0.5f - defR - 0.5f});
    } else if (effectivePreset == SlingshotPreset::StoneCitadel) {
        // Heavy outer stone bunker with central glass divider & strictly non-overlapping twin roofs
        const float chamberInner = std::max(unit * 0.95f, defR * 2.35f + 10.0f);
        const float span = chamberInner + thick * 1.15f;
        const float h1 = pillarH * 1.08f;
        addBlockScene({fortX - span, groundY_ - h1 * 0.5f}, {thick * 1.30f, h1}, SlingshotBlockKind::Stone);
        addBlockScene({fortX + span, groundY_ - h1 * 0.5f}, {thick * 1.30f, h1}, SlingshotBlockKind::Stone);
        addBlockScene({fortX, groundY_ - h1 * 0.5f}, {thick, h1}, SlingshotBlockKind::Glass);
        spawnSlotDefender({fortX - span * 0.5f, groundY_ - defR - 0.5f});
        spawnSlotDefender({fortX + span * 0.5f, groundY_ - defR - 0.5f});
        const float roofY = groundY_ - h1 - thick * 0.5f;
        const float halfRoofW = span + thick * 0.55f - 2.5f;
        const float halfRoofOff = span * 0.5f + thick * 0.25f + 1.5f;
        addBlockScene({fortX - halfRoofOff, roofY}, {halfRoofW, thick}, SlingshotBlockKind::Wood);
        addBlockScene({fortX + halfRoofOff, roofY}, {halfRoofW, thick}, SlingshotBlockKind::Glass);
        extraPerches.push_back({fortX - span * 0.78f, roofY - thick * 0.5f - defR - 0.5f});
        extraPerches.push_back({fortX + span * 0.78f, roofY - thick * 0.5f - defR - 0.5f});
        const float lv2 = roofY - thick * 0.5f;
        const float span2 = std::max(beamW * 0.36f, minSafeSpan);
        addBlockScene({fortX - span2, lv2 - p2BaseH * 0.5f}, {thick, p2BaseH}, SlingshotBlockKind::Stone);
        addBlockScene({fortX + span2, lv2 - p2BaseH * 0.5f}, {thick, p2BaseH}, SlingshotBlockKind::Stone);
        spawnSlotDefender({fortX, lv2 - defR - 0.5f});
        const float topCapW = (span2 + thick * 0.5f) * 2.06f;
        addBlockScene({fortX, lv2 - p2BaseH - thick * 0.58f}, {topCapW, thick * 1.15f},
                      SlingshotBlockKind::Stone);
        spawnSlotDefender({fortX, lv2 - p2BaseH - thick * 1.15f - defR - 0.5f});
    } else if (effectivePreset == SlingshotPreset::CustomBuilder) {
        const auto& pieces = config_.customPieces.empty() ? editorPieces_ : config_.customPieces;
        bool spawnedAnyDefender = false;
        for (const auto& pc : pieces) {
            const Vec2 c = customPieceSceneCenter(pc);
            const Vec2 sz = customPieceSceneSize(pc.kind);
            switch (pc.kind) {
                case SlingshotCustomPieceKind::WoodPillar:
                case SlingshotCustomPieceKind::WoodBeam:
                    addBlockScene(c, sz, SlingshotBlockKind::Wood);
                    break;
                case SlingshotCustomPieceKind::StoneBlock:
                    addBlockScene(c, sz, SlingshotBlockKind::Stone);
                    break;
                case SlingshotCustomPieceKind::GlassBeam:
                    addBlockScene(c, sz, SlingshotBlockKind::Glass);
                    break;
                case SlingshotCustomPieceKind::DefenderSlot:
                    spawnSlotDefender(c);
                    spawnedAnyDefender = true;
                    break;
            }
        }
        if (!spawnedAnyDefender) {
            spawnSlotDefender({fortX, groundY_ - defR - 0.5f});
        }
        return;
    } else {
        // Procedural RandomSiege with guaranteed non-overlapping bay spacing
        std::uint32_t rng = config_.randomSeed ^ (static_cast<std::uint32_t>(stageIndex_) * 0x9E3779B9u);
        const int cols = 2 + static_cast<int>(nextLcg(rng) % 2);  // 2 or 3 columns
        const float span = std::max(beamW * 0.34f, minSafeSpan);
        const float roofW = (span + thick * 0.5f) * 2.04f;
        const float bayW = roofW + 5.0f;
        for (int c = 0; c < cols; ++c) {
            const float cx = fortX + (static_cast<float>(c) - static_cast<float>(cols - 1) * 0.5f) * bayW;
            const int stories = 1 + static_cast<int>(nextLcg(rng) % 3);  // 1..3 stories
            float base = groundY_;
            for (int s = 0; s < stories; ++s) {
                const float sh = (s == 0) ? pillarH : p2BaseH;
                const auto wallMat = static_cast<SlingshotBlockKind>(nextLcg(rng) % 3);
                const auto roofMat = static_cast<SlingshotBlockKind>(nextLcg(rng) % 3);
                addBlockScene({cx - span, base - sh * 0.5f}, {thick, sh}, wallMat);
                addBlockScene({cx + span, base - sh * 0.5f}, {thick, sh}, wallMat);
                if (s == 0 || (nextLcg(rng) % 2 == 0)) spawnSlotDefender({cx, base - defR - 0.5f});
                addBlockScene({cx, base - sh - thick * 0.5f}, {roofW, thick}, roofMat);
                base -= (sh + thick);
            }
            extraPerches.push_back({cx, base - defR - 0.5f});
        }
    }

    // Place any remaining real defender icons on safe roof perches or non-overlapping flanking outposts
    // (NEVER inside existing fortress bays!).
    auto spotClear = [&](Vec2 pt, float halfW, float halfH) {
        if (pt.x - halfW < slingBase_.x + maxPull_ + 28.0f || pt.x + halfW > w - 18.0f) return false;
        for (const auto& blk : blocks_) {
            const Vec2 bc = coords_.toScene(world_->state(blk.body).position);
            if (std::abs(pt.x - bc.x) < (halfW + blk.sizeScene.x * 0.5f + 3.0f) &&
                std::abs(pt.y - bc.y) < (halfH + blk.sizeScene.y * 0.5f + 3.0f)) {
                return false;
            }
        }
        for (const auto& def : defenders_) {
            const Vec2 dc = coords_.toScene(world_->state(def.body).position);
            if ((pt - dc).length() < (std::max(halfW, halfH) + def.radiusScene + 4.0f)) return false;
        }
        return true;
    };

    for (const Vec2& perch : extraPerches) {
        if (nextDef >= defenderIcons.size()) break;
        if (spotClear(perch, defR, defR)) {
            spawnSlotDefender(perch);
        }
    }

    float fortMinX = fortX, fortMaxX = fortX;
    for (const auto& blk : blocks_) {
        const float bx = coords_.toScene(world_->state(blk.body).position).x;
        fortMinX = std::min(fortMinX, bx - blk.sizeScene.x * 0.5f);
        fortMaxX = std::max(fortMaxX, bx + blk.sizeScene.x * 0.5f);
    }

    int flankStep = 0;
    while (nextDef < defenderIcons.size() && flankStep < 12) {
        const float pedW = std::max(unit * 0.62f, defR * 2.1f);
        const float pedH = unit * 0.52f;
        const bool tryLeft = (flankStep % 2 == 0);
        const float dist = (static_cast<float>(flankStep / 2) + 0.75f) * (pedW + 10.0f);
        const float px = tryLeft ? (fortMinX - dist) : (fortMaxX + dist);
        ++flankStep;
        const Vec2 pedCenter{px, groundY_ - pedH * 0.5f};
        const Vec2 defCenter{px, groundY_ - pedH - defR - 0.5f};
        if (spotClear(pedCenter, pedW * 0.5f, pedH * 0.5f) && spotClear(defCenter, defR, defR)) {
            addBlockScene(pedCenter, {pedW, pedH}, SlingshotBlockKind::Wood);
            spawnSlotDefender(defCenter);
        } else if (spotClear({px, groundY_ - defR - 0.5f}, defR, defR)) {
            spawnSlotDefender({px, groundY_ - defR - 0.5f});
        }
    }
    // Any still-unplaced defender icons safely join the reserve shelf rather than overlapping physics blocks!
    while (nextDef < defenderIcons.size()) {
        const std::size_t overflowIdx = defenderIcons[nextDef++];
        const std::size_t slot = parkedIcons_.size();
        const float px = w - 34.0f - static_cast<float>(slot % 10) * 40.0f;
        const float py = std::min(h - 20.0f, groundY_ + 22.0f + static_cast<float>(slot / 10) * 26.0f);
        parkedIcons_.push_back(overflowIdx);
        parkedPositions_.push_back({std::max(34.0f, px), py});
    }
}

void SlingshotMode::loadNextAmmo() {
    while (currentAmmoIdx_ < ammo_.size() && ammo_[currentAmmoIdx_].launched) {
        ++currentAmmoIdx_;
    }
    if (currentAmmoIdx_ < ammo_.size()) {
        phase_ = Phase::ReadyToAim;
        pullPosScene_ = slingAnchor_;
        dashUsedThisShot_ = false;
        flightElapsed_ = 0.0;
        settleIdleElapsed_ = 0.0;
    }
}

Vec2 SlingshotMode::launchVelocityPhysics(Vec2 pullFromAnchorScene) const {
    const float dist = pullFromAnchorScene.length();
    if (dist < 1e-4f) return {};
    const float ratio = std::clamp(dist / maxPull_, 0.0f, 1.0f);
    // Scale ballistic range to the current desktop width so 65%-90% pull hits the fortress at any DPI/size.
    const float spanMeters = coords_.lengthToPhysics(setup_.workAreaSize.x * 0.56f);
    const float maxSpeedMeters = std::clamp(std::sqrt(9.8f * spanMeters * 1.65f), 9.5f, 36.0f);
    const float minSpeedMeters = maxSpeedMeters * 0.24f;
    const Vec2 dirScene = (pullFromAnchorScene * (-1.0f / dist));
    const float speedMeters = minSpeedMeters + (maxSpeedMeters - minSpeedMeters) * std::pow(ratio, 1.05f);
    return {dirScene.x * speedMeters, -dirScene.y * speedMeters};
}

void SlingshotMode::emitSound(SoundKind kind, float gain, Vec2 scenePos, float pitch) {
    if (pendingSounds_.size() >= 16) return;
    const float pan =
        std::clamp((scenePos.x / std::max(1.0f, setup_.workAreaSize.x)) * 2.0f - 1.0f, -1.0f, 1.0f);
    pendingSounds_.push_back({kind, std::clamp(gain, 0.05f, 1.0f), pan, std::clamp(pitch, 0.78f, 1.32f)});
}

void SlingshotMode::spawnDebris(Vec2 centerScene, Color baseColor, int count, float speedScale) {
    if (debris_.size() > 180) return;
    std::uint32_t rng = static_cast<std::uint32_t>(centerScene.x * 31.0f + centerScene.y * 17.0f) ^
                        static_cast<std::uint32_t>(debris_.size() * 7919u + 12345u);
    for (int i = 0; i < count; ++i) {
        const float ang = randRange(rng, 0.0f, 2.0f * kPi);
        const float spd = randRange(rng, 45.0f, 185.0f) * speedScale;
        Debris d;
        d.pos = centerScene;
        d.vel = {std::cos(ang) * spd, std::sin(ang) * spd - 55.0f};
        d.color = baseColor;
        d.radius = randRange(rng, 2.2f, 4.6f);
        d.maxLife = randRange(rng, 0.30f, 0.58f);
        d.life = d.maxLife;
        d.isShard = false;
        debris_.push_back(d);
    }
}

void SlingshotMode::spawnBlockShards(Vec2 centerScene, SlingshotBlockKind mat, int count, float speedScale) {
    if (debris_.size() > 180) return;
    std::uint32_t rng = static_cast<std::uint32_t>(centerScene.x * 53.0f + centerScene.y * 29.0f) ^
                        static_cast<std::uint32_t>(debris_.size() * 11939u + 0x517cc1b7u);
    for (int i = 0; i < count; ++i) {
        const float ang = randRange(rng, 0.0f, 2.0f * kPi);
        const float spd = randRange(rng, 55.0f, 235.0f) * speedScale;
        Debris d;
        d.pos = centerScene + Vec2{randRange(rng, -10.0f, 10.0f), randRange(rng, -10.0f, 10.0f)};
        d.vel = {std::cos(ang) * spd, std::sin(ang) * spd - 85.0f};
        d.angle = randRange(rng, 0.0f, 2.0f * kPi);
        d.angularVel = randRange(rng, -13.0f, 13.0f);
        d.kind = mat;
        d.isShard = true;
        if (mat == SlingshotBlockKind::Glass) {
            d.color = {185, 242, 255, 210};
            d.accent = {245, 254, 255, 240};
            d.shardSize = {randRange(rng, 7.0f, 15.0f), randRange(rng, 4.5f, 9.5f)};
            d.maxLife = randRange(rng, 0.42f, 0.78f);
        } else if (mat == SlingshotBlockKind::Wood) {
            d.color = (i % 2 == 0) ? Color{198, 136, 70, 248} : Color{234, 192, 128, 248};
            d.accent = {98, 56, 24, 240};
            d.shardSize = {randRange(rng, 11.0f, 22.0f), randRange(rng, 2.8f, 5.2f)};
            d.maxLife = randRange(rng, 0.48f, 0.86f);
        } else {
            d.color = (i % 2 == 0) ? Color{146, 154, 166, 250} : Color{112, 118, 130, 250};
            d.accent = {64, 70, 80, 245};
            d.shardSize = {randRange(rng, 7.5f, 14.0f), randRange(rng, 6.0f, 11.5f)};
            d.maxLife = randRange(rng, 0.45f, 0.82f);
        }
        d.life = d.maxLife;
        debris_.push_back(d);
    }
}

void SlingshotMode::defeatDefender(std::size_t defIdx, Vec2 hitScene) {
    auto& d = defenders_[defIdx];
    if (d.defeated) return;
    d.defeated = true;
    d.trophyAnim = 0.0f;
    world_->setEnabled(d.body, false);
    score_ += 1000;
    highScore_ = std::max(highScore_, score_);
    spawnDebris(hitScene, {255, 215, 70, 245}, 12, 1.15f);
    rings_.push_back({hitScene, d.radiusScene * 0.7f, d.radiusScene * 3.1f, 1.0f, {255, 225, 85, 240}});
    emitSound(SoundKind::TargetPop, 0.88f, hitScene, 1.05f);
}

void SlingshotMode::setVisualOptions(bool showTrajectory, bool showGroundChrome) {
    config_.showTrajectory = showTrajectory;
    config_.showGroundChrome = showGroundChrome;
}

void SlingshotMode::setEditing(bool editing) {
    if (editing_ == editing) return;
    if (editing) {
        editing_ = true;
        draggingPieceIdx_ = -1;
        if (config_.preset != SlingshotPreset::CustomBuilder) {
            config_.preset = SlingshotPreset::CustomBuilder;
        }
        if (editorPieces_.empty()) {
            editorPieces_ = config_.customPieces.empty() ? defaultSlingshotCustomPieces() : config_.customPieces;
        }
        config_.customPieces = editorPieces_;
        persistenceDirty_ = true;
        phase_ = Phase::ReadyToAim;
        pullPosScene_ = slingAnchor_;
        return;
    }
    // Ensure at least one DefenderSlot exists before entering live battle.
    bool hasDef = false;
    for (const auto& p : editorPieces_) {
        if (p.kind == SlingshotCustomPieceKind::DefenderSlot) {
            hasDef = true;
            break;
        }
    }
    if (!hasDef) {
        editorPieces_.push_back({SlingshotCustomPieceKind::DefenderSlot, 0.0f, 0.35f});
    }
    SlingshotConfig nextCfg = config_;
    nextCfg.preset = SlingshotPreset::CustomBuilder;
    nextCfg.customPieces = editorPieces_;
    nextCfg.startInEditor = false;
    const SlingshotProgress savedProg = progress_;
    const int best = highScore_;
    *this = SlingshotMode(setup_, nextCfg, 1, 0, best, savedProg);
    persistenceDirty_ = true;
}

void SlingshotMode::recordStageVictory() {
    const int unused = remainingAmmo();
    score_ += unused * 1000;
    highScore_ = std::max(highScore_, score_);
    lastClearStars_ = (unused >= 2 || shotsFired_ <= 1) ? 3 : (unused == 1 ? 2 : 1);
    progress_.recordClear(resolvedPreset(), stageIndex_, lastClearStars_, score_);
    highScore_ = std::max(highScore_, progress_.highScore);
    persistenceDirty_ = true;
    stageCleared_ = true;
    phase_ = Phase::StageClear;
    emitSound(SoundKind::HockeyGoal, 0.90f, {setup_.workAreaSize.x * 0.5f, setup_.workAreaSize.y * 0.4f},
              1.0f);
}

void SlingshotMode::advanceStage() {
    SlingshotConfig cfg = config_;
    cfg.startInEditor = false;
    const int nextStage = stageCleared_ ? (stageIndex_ + 1) : stageIndex_;
    const int carry = stageCleared_ ? score_ : 0;
    const int best = highScore_;
    const SlingshotProgress prog = progress_;
    const bool wasDirty = persistenceDirty_;
    *this = SlingshotMode(setup_, cfg, nextStage, carry, best, prog);
    persistenceDirty_ = wasDirty;
}

void SlingshotMode::handleInput(const InputEvent& event) {
    if (event.type == InputType::PointerCancel) {
        draggingPieceIdx_ = -1;
        if (phase_ == Phase::Pulling) {
            phase_ = Phase::ReadyToAim;
            pullPosScene_ = slingAnchor_;
        }
        return;
    }

    if (editing_) {
        if (event.type == InputType::PointerDown) {
            if (rectContains(trajToggleRect_, event.scenePos)) {
                config_.showTrajectory = !config_.showTrajectory;
                return;
            }
            if (rectContains(editorToggleRect_, event.scenePos)) {
                setEditing(false);
                return;
            }
            for (int t = 0; t < 7; ++t) {
                if (rectContains(editorToolPillRect(t), event.scenePos)) {
                    if (t == 6) {
                        editorPieces_.clear();
                        config_.customPieces.clear();
                        persistenceDirty_ = true;
                    } else {
                        editorTool_ = t;
                    }
                    return;
                }
            }
            const Rect zone = customBuildZoneScene();
            if (!rectContains(zone, event.scenePos)) return;

            int hitIdx = -1;
            for (int i = static_cast<int>(editorPieces_.size()) - 1; i >= 0; --i) {
                const Vec2 c = customPieceSceneCenter(editorPieces_[static_cast<std::size_t>(i)]);
                const Vec2 sz = customPieceSceneSize(editorPieces_[static_cast<std::size_t>(i)].kind);
                if (std::abs(event.scenePos.x - c.x) <= sz.x * 0.55f + 4.0f &&
                    std::abs(event.scenePos.y - c.y) <= sz.y * 0.55f + 4.0f) {
                    hitIdx = i;
                    break;
                }
            }

            if (editorTool_ == 5 || event.alternate) {
                if (hitIdx >= 0) {
                    editorPieces_.erase(editorPieces_.begin() + hitIdx);
                    config_.customPieces = editorPieces_;
                    persistenceDirty_ = true;
                    emitSound(SoundKind::WoodImpact, 0.35f, event.scenePos, 1.15f);
                }
                return;
            }
            if (hitIdx >= 0) {
                draggingPieceIdx_ = hitIdx;
                return;
            }
            if (editorPieces_.size() < 48u) {
                const auto kind = static_cast<SlingshotCustomPieceKind>(std::clamp(editorTool_, 0, 4));
                editorPieces_.push_back(sceneToCustomPiece(kind, event.scenePos));
                draggingPieceIdx_ = static_cast<int>(editorPieces_.size()) - 1;
                config_.customPieces = editorPieces_;
                persistenceDirty_ = true;
                emitSound(SoundKind::WoodImpact, 0.45f, event.scenePos, 1.0f);
            }
            return;
        }
        if (event.type == InputType::PointerMove && draggingPieceIdx_ >= 0 &&
            draggingPieceIdx_ < static_cast<int>(editorPieces_.size())) {
            auto& piece = editorPieces_[static_cast<std::size_t>(draggingPieceIdx_)];
            piece = sceneToCustomPiece(piece.kind, event.scenePos);
            config_.customPieces = editorPieces_;
            persistenceDirty_ = true;
            return;
        }
        if (event.type == InputType::PointerUp) {
            draggingPieceIdx_ = -1;
            return;
        }
        return;
    }

    if (event.type == InputType::PointerDown) {
        if (rectContains(trajToggleRect_, event.scenePos)) {
            config_.showTrajectory = !config_.showTrajectory;
            return;
        }
        if (rectContains(editorToggleRect_, event.scenePos)) {
            setEditing(true);
            return;
        }
        if (rectContains(nextStageRect_, event.scenePos)) {
            stageCleared_ = true;  // explicit skip/next stage button always advances stage
            advanceStage();
            return;
        }
        if (phase_ == Phase::StageClear || phase_ == Phase::StageFail) {
            advanceStage();
            return;
        }
        if (phase_ == Phase::ReadyToAim && currentAmmoIdx_ < ammo_.size()) {
            const float d = (event.scenePos - slingAnchor_).length();
            if (d <= maxPull_ * 1.45f) {
                phase_ = Phase::Pulling;
                Vec2 delta = event.scenePos - slingAnchor_;
                if (delta.length() > maxPull_) delta = delta * (maxPull_ / delta.length());
                pullPosScene_ = slingAnchor_ + delta;
                lastStretchSoundDist_ = delta.length();
                emitSound(SoundKind::SlingStretch, 0.42f, slingAnchor_, 0.96f);
                return;
            }
        }
        if (phase_ == Phase::InFlight && !dashUsedThisShot_ && currentAmmoIdx_ < ammo_.size()) {
            const BodyIndex b = ammo_[currentAmmoIdx_].body;
            const BodyState st = world_->state(b);
            if (st.enabled && st.linearVelocity.length() > 1.2f) {
                dashUsedThisShot_ = true;
                Vec2 boosted = st.linearVelocity * 1.48f;
                boosted.y -= 1.5f;  // crisp downward-forward siege strike!
                const float len = boosted.length();
                if (len > 44.0f) boosted = boosted * (44.0f / len);
                world_->setVelocity(b, boosted, st.angularVelocity * 1.5f);
                const Vec2 sc = coords_.toScene(st.position);
                rings_.push_back({sc, projRadius_ * 0.8f, projRadius_ * 3.4f, 1.0f, {120, 225, 255, 240}});
                spawnDebris(sc, {140, 230, 255, 235}, 8, 0.9f);
                emitSound(SoundKind::SlingRelease, 0.85f, sc, 1.24f);
            }
        }
        return;
    }

    if (event.type == InputType::PointerMove && phase_ == Phase::Pulling) {
        Vec2 delta = event.scenePos - slingAnchor_;
        const float len = delta.length();
        if (len > maxPull_) delta = delta * (maxPull_ / len);
        pullPosScene_ = slingAnchor_ + delta;
        if (std::abs(delta.length() - lastStretchSoundDist_) >= maxPull_ * 0.28f) {
            lastStretchSoundDist_ = delta.length();
            const float ratio = std::clamp(delta.length() / maxPull_, 0.0f, 1.0f);
            emitSound(SoundKind::SlingStretch, 0.30f + 0.35f * ratio, pullPosScene_, 0.90f + 0.25f * ratio);
        }
        return;
    }

    if (event.type == InputType::PointerUp && phase_ == Phase::Pulling) {
        const Vec2 pullVec = pullPosScene_ - slingAnchor_;
        if (pullVec.length() < 12.0f || currentAmmoIdx_ >= ammo_.size()) {
            phase_ = Phase::ReadyToAim;
            pullPosScene_ = slingAnchor_;
            return;
        }
        auto& shot = ammo_[currentAmmoIdx_];
        shot.launched = true;
        const Vec2 vPhys = launchVelocityPhysics(pullVec);
        world_->setEnabled(shot.body, true);
        world_->setTransform(shot.body, coords_.toPhysics(pullPosScene_), 0.0f);
        world_->setVelocity(shot.body, vPhys, -3.5f);
        world_->wake(shot.body);
        phase_ = Phase::InFlight;
        flightElapsed_ = 0.0;
        settleIdleElapsed_ = 0.0;
        ++shotsFired_;
        const float power = std::clamp(pullVec.length() / maxPull_, 0.15f, 1.0f);
        emitSound(SoundKind::SlingRelease, 0.55f + 0.42f * power, slingAnchor_, 0.92f + 0.20f * power);
    }
}

void SlingshotMode::stepPhysics(float dt) {
    if (editing_) return;
    world_->step(dt);
}

void SlingshotMode::updateRules(float dt) {
    // Animate debris & shock rings
    for (auto& d : debris_) {
        d.vel.y += 420.0f * dt;
        d.pos += d.vel * dt;
        d.angle += d.angularVel * dt;
        d.life -= dt;
    }
    debris_.erase(
        std::remove_if(debris_.begin(), debris_.end(), [](const Debris& d) { return d.life <= 0.0f; }),
        debris_.end());

    for (auto& r : rings_) {
        r.radius += (r.maxRadius - r.radius) * std::min(1.0f, dt * 11.0f);
        r.alpha -= dt * 2.8f;
    }
    rings_.erase(
        std::remove_if(rings_.begin(), rings_.end(), [](const ShockRing& r) { return r.alpha <= 0.0f; }),
        rings_.end());

    for (auto& def : defenders_) {
        if (def.defeated && def.trophyAnim < 1.0f) {
            def.trophyAnim = std::min(1.0f, def.trophyAnim + dt * 3.5f);
        }
    }

    // Only apply collision damage once at least one shot has been launched!
    if (shotsFired_ > 0) {
        for (const CollisionHit& hit : world_->lastHits()) {
            const Vec2 hitScene = coords_.toScene(hit.point);
            auto bodyMassFactor = [&](std::optional<BodyIndex> other) -> float {
                if (!other) return 1.1f;  // hitting hard ground/wall
                const float m = world_->mass(*other);
                return std::clamp(0.55f + m * 0.45f, 0.6f, 2.5f);
            };

            auto checkBody = [&](std::optional<BodyIndex> self, std::optional<BodyIndex> other) {
                if (!self) return;
                // Check blocks
                for (auto& blk : blocks_) {
                    if (!blk.destroyed && blk.body == *self) {
                        const float dmg = hit.approachSpeed * bodyMassFactor(other);
                        if (dmg < 0.85f) return;
                        const BodyState bst = world_->state(blk.body);
                        const Vec2 bc = coords_.toScene(bst.position);
                        const float brot = SceneToPhysics::angleToScene(bst.angle);
                        const float cr = std::cos(brot);
                        const float sr = std::sin(brot);
                        const Vec2 rel = hitScene - bc;
                        const Vec2 localHit = {rel.x * cr + rel.y * sr, -rel.x * sr + rel.y * cr};
                        blk.lastHitLocal = {
                            std::clamp(localHit.x, -blk.sizeScene.x * 0.42f, blk.sizeScene.x * 0.42f),
                            std::clamp(localHit.y, -blk.sizeScene.y * 0.42f, blk.sizeScene.y * 0.42f)};
                        blk.hp -= dmg;
                        const float gain = std::clamp(dmg / 12.0f, 0.12f, 0.92f);
                        if (blk.hp <= 0.0f) {
                            blk.destroyed = true;
                            world_->setEnabled(blk.body, false);
                            if (blk.material == SlingshotBlockKind::Glass) {
                                score_ += 120;
                                spawnBlockShards(bc, SlingshotBlockKind::Glass, 13, 1.05f);
                                emitSound(SoundKind::GlassShatter, std::max(0.45f, gain), hitScene, 1.08f);
                            } else if (blk.material == SlingshotBlockKind::Wood) {
                                score_ += 220;
                                spawnBlockShards(bc, SlingshotBlockKind::Wood, 11, 0.96f);
                                emitSound(SoundKind::WoodImpact, std::max(0.45f, gain), hitScene, 0.92f);
                            } else {
                                score_ += 380;
                                spawnBlockShards(bc, SlingshotBlockKind::Stone, 12, 0.88f);
                                emitSound(SoundKind::StoneImpact, std::max(0.50f, gain), hitScene, 0.85f);
                            }
                            highScore_ = std::max(highScore_, score_);
                        } else if (dmg >= 1.1f) {
                            score_ += 40;
                            highScore_ = std::max(highScore_, score_);
                            spawnBlockShards(hitScene, blk.material, 3, 0.55f);
                            const SoundKind sk =
                                blk.material == SlingshotBlockKind::Glass
                                    ? SoundKind::GlassShatter
                                    : (blk.material == SlingshotBlockKind::Wood ? SoundKind::WoodImpact
                                                                                : SoundKind::StoneImpact);
                            emitSound(sk, gain * 0.65f, hitScene, 1.0f);
                        }
                        return;
                    }
                }
                // Check defenders
                for (std::size_t di = 0; di < defenders_.size(); ++di) {
                    auto& def = defenders_[di];
                    if (!def.defeated && def.body == *self) {
                        bool hitByProjectile = false;
                        if (other) {
                            for (const auto& slot : ammo_) {
                                if (slot.launched && slot.body == *other) {
                                    hitByProjectile = true;
                                    break;
                                }
                            }
                        }
                        if (hitByProjectile) {
                            defeatDefender(di, coords_.toScene(world_->state(def.body).position));
                            return;
                        }
                        const float dmg = hit.approachSpeed * bodyMassFactor(other);
                        if (dmg < 0.28f) return;
                        def.hp -= dmg;
                        if (def.hp <= 0.0f) {
                            defeatDefender(di, coords_.toScene(world_->state(def.body).position));
                        } else if (dmg >= 0.65f) {
                            emitSound(SoundKind::BallCollide, std::clamp(dmg / 8.0f, 0.15f, 0.70f), hitScene,
                                      1.1f);
                        }
                        return;
                    }
                }
            };

            checkBody(hit.bodyA, hit.bodyB);
            checkBody(hit.bodyB, hit.bodyA);
        }

        // Direct projectile touch/graze check + knocked-off-perch / fall check so touching a defender
        // ALWAYS defeats it reliably even on low-velocity rolling or multi-substep grazing contacts!
        for (std::size_t di = 0; di < defenders_.size(); ++di) {
            auto& def = defenders_[di];
            if (def.defeated) continue;
            const BodyState st = world_->state(def.body);
            if (!st.enabled) continue;
            const Vec2 sc = coords_.toScene(st.position);

            bool touchedByAmmo = false;
            const float touchDist = projRadius_ + def.radiusScene + 4.5f;
            const float touchDist2 = touchDist * touchDist;
            for (const auto& slot : ammo_) {
                if (!slot.launched || slot.retired) continue;
                const BodyState ast = world_->state(slot.body);
                if (!ast.enabled) continue;
                const Vec2 asc = coords_.toScene(ast.position);
                if ((asc - sc).lengthSquared() <= touchDist2) {
                    touchedByAmmo = true;
                    break;
                }
            }

            const float dispFromPerch = (sc - def.initialScenePos).length();
            if (touchedByAmmo || dispFromPerch > def.radiusScene * 1.05f ||
                sc.x <= def.radiusScene * 1.5f || sc.x >= setup_.workAreaSize.x - def.radiusScene * 1.2f ||
                st.linearVelocity.length() > 2.4f) {
                defeatDefender(di, sc);
            }
        }
    }

    // Advance flight / turn state
    if (phase_ == Phase::InFlight || phase_ == Phase::Settling) {
        flightElapsed_ += dt;
        if (remainingDefenders() == 0) {
            recordStageVictory();
            return;
        }
        bool projectileStopped = true;
        if (currentAmmoIdx_ < ammo_.size()) {
            const BodyState pst = world_->state(ammo_[currentAmmoIdx_].body);
            if (pst.enabled && pst.awake && pst.linearVelocity.length() > 0.32f) {
                projectileStopped = false;
            }
        }
        if ((projectileStopped && flightElapsed_ >= 0.75) || flightElapsed_ >= 4.2) {
            phase_ = Phase::Settling;
            if (world_->allAsleep() || flightElapsed_ >= 4.8) {
                settleIdleElapsed_ += dt;
            } else {
                settleIdleElapsed_ = 0.0;
            }
            if (remainingDefenders() == 0) {
                recordStageVictory();
            } else if (settleIdleElapsed_ >= 0.30 || flightElapsed_ >= 5.2) {
                // Retire previous projectile to keep arena clean if desired, or leave it resting!
                ++currentAmmoIdx_;
                if (currentAmmoIdx_ < ammo_.size()) {
                    loadNextAmmo();
                } else {
                    stageFailed_ = true;
                    phase_ = Phase::StageFail;
                }
            }
        }
    } else if (shotsFired_ > 0 && remainingDefenders() == 0 && !stageCleared_) {
        recordStageVictory();
    }
}

int SlingshotMode::remainingAmmo() const {
    int rem = 0;
    for (const auto& a : ammo_) {
        if (!a.launched) ++rem;
    }
    return rem;
}

int SlingshotMode::remainingDefenders() const {
    int rem = 0;
    for (const auto& d : defenders_) {
        if (!d.defeated) ++rem;
    }
    return rem;
}

bool SlingshotMode::isIdle() const {
    if (!pendingSounds_.empty() || !debris_.empty() || !rings_.empty()) return false;
    for (const auto& d : defenders_) {
        if (d.defeated && d.trophyAnim < 1.0f) return false;
    }
    if (phase_ == Phase::InFlight || phase_ == Phase::Settling) return false;
    return world_->allAsleep();
}

ModeStats SlingshotMode::stats() const {
    ModeStats s;
    s.bodies = static_cast<int>(world_->bodyCount());
    s.awakeBodies = world_->awakeBodyCount();
    s.pocketed = static_cast<int>(defenders_.size()) - remainingDefenders();
    s.targets = static_cast<int>(defenders_.size());
    s.shots = shotsFired_;
    s.finished = stageCleared_ || stageFailed_;
    s.grabbing = (phase_ == Phase::Pulling);
    return s;
}

void SlingshotMode::drainSounds(std::vector<SoundEvent>& out) {
    out.insert(out.end(), pendingSounds_.begin(), pendingSounds_.end());
    pendingSounds_.clear();
}

Vec2 SlingshotMode::iconCurrentSceneCenter(std::size_t iconIndex) const {
    if (iconIndex >= setup_.icons.size()) return {};
    for (std::size_t i = 0; i < ammo_.size(); ++i) {
        const auto& a = ammo_[i];
        if (a.iconIndex && *a.iconIndex == iconIndex) {
            if (a.launched) return coords_.toScene(world_->state(a.body).position);
            if (i == currentAmmoIdx_) return phase_ == Phase::Pulling ? pullPosScene_ : slingAnchor_;
            return a.waitPosScene;
        }
    }
    for (const auto& d : defenders_) {
        if (d.iconIndex && *d.iconIndex == iconIndex) {
            if (d.defeated) return d.trophySlotScene;
            return coords_.toScene(world_->state(d.body).position);
        }
    }
    for (std::size_t i = 0; i < parkedIcons_.size(); ++i) {
        if (parkedIcons_[i] == iconIndex) return parkedPositions_[i];
    }
    return setup_.icons[iconIndex].center;
}

bool SlingshotMode::interactiveContains(Vec2 p) const {
    if (rectContains(trajToggleRect_, p) || rectContains(nextStageRect_, p) ||
        rectContains(editorToggleRect_, p)) {
        return true;
    }
    if (editing_) {
        for (int t = 0; t < 7; ++t) {
            if (rectContains(editorToolPillRect(t), p)) return true;
        }
        if (rectContains(customBuildZoneScene(), p)) return true;
    }
    if (phase_ == Phase::StageClear || phase_ == Phase::StageFail) return true;
    if (phase_ == Phase::InFlight && !dashUsedThisShot_) return true;
    if ((p - slingAnchor_).length() <= maxPull_ * 1.5f) return true;
    for (std::size_t i = 0; i < setup_.icons.size(); ++i) {
        if ((p - iconCurrentSceneCenter(i)).length() <= projRadius_ * 1.35f) return true;
    }
    return false;
}

void SlingshotMode::buildDrawList(DrawList& out) const {
    const float w = setup_.workAreaSize.x;

    auto addPoly = [&](std::initializer_list<Vec2> pts, Color fill, Color stroke = {0, 0, 0, 0},
                       float sw = 0.0f) {
        PolygonItem p{};
        std::uint8_t idx = 0;
        for (const Vec2& v : pts) {
            if (idx >= 8u) break;
            p.points[idx++] = v;
        }
        p.count = idx;
        p.fill = fill;
        p.stroke = stroke;
        p.strokeWidth = sw;
        if (idx >= 3u) out.add(p);
    };

    auto toWorld = [](Vec2 c, float cr, float sr, float lx, float ly) -> Vec2 {
        return {c.x + lx * cr - ly * sr, c.y + lx * sr + ly * cr};
    };

    auto drawTaperedBand = [&](Vec2 a, float hwA, Vec2 b, float hwB, Color fill, Color edge) {
        const Vec2 d = b - a;
        const float len = d.length();
        if (len < 1.5f) return;
        const Vec2 n = {-d.y / len, d.x / len};
        addPoly({a + n * hwA, b + n * hwB, b - n * hwB, a - n * hwA}, fill, edge, 1.0f);
        // Subtle specular latex highlight down the upper edge of the stretched band
        addPoly({a + n * (hwA * 0.62f), b + n * (hwB * 0.62f), b + n * (hwB * 0.12f), a + n * (hwA * 0.12f)},
                {255, 188, 156, 76});
    };

    // 1. Ground baseline, layered turf & sculpted stone launch mound under slingshot
    if (config_.showGroundChrome) {
        out.add(BoxItem{{w * 0.5f, groundY_ + 8.0f},
                        {w - 20.0f, 16.0f},
                        0.0f,
                        6.0f,
                        {44, 62, 52, 182},
                        {88, 138, 96, 175},
                        1.4f});
        out.add(BoxItem{{w * 0.5f, groundY_ + 2.5f},
                        {w - 22.0f, 5.0f},
                        0.0f,
                        2.5f,
                        {102, 182, 104, 205},
                        {0, 0, 0, 0},
                        0.0f});
    }

    // Grounded stone launch plinth under the Y-slingshot
    const float slingH = groundY_ - (forkBackTip_.y + forkFrontTip_.y) * 0.5f;
    const float bx = slingBase_.x;
    const float gy = groundY_;
    addPoly(
        {{bx - 34.0f, gy + 3.0f}, {bx - 24.0f, gy - 8.5f}, {bx + 24.0f, gy - 8.5f}, {bx + 34.0f, gy + 3.0f}},
        {108, 116, 130, 240}, {56, 62, 72, 245}, 1.5f);
    addPoly({{bx - 24.0f, gy - 8.5f},
             {bx - 20.0f, gy - 12.0f},
             {bx + 20.0f, gy - 12.0f},
             {bx + 24.0f, gy - 8.5f}},
            {156, 166, 180, 245}, {76, 84, 96, 220}, 1.0f);

    // 2. Sculpted 3D Y-Slingshot — Rear-Right Horn & Rear Tapered Latex Band
    const float crotchY = gy - slingH * 0.50f;
    const Vec2 backHornBaseL = {bx - 2.0f, crotchY + 4.0f};
    const Vec2 backHornBaseR = {bx + 9.5f, crotchY + 6.0f};
    const Vec2 backMidOuter = {bx + slingH * 0.16f, crotchY - slingH * 0.22f};
    const Vec2 backTipOuter = {forkBackTip_.x + 5.8f, forkBackTip_.y + 1.5f};
    const Vec2 backTipTop = {forkBackTip_.x + 0.5f, forkBackTip_.y - 5.5f};
    const Vec2 backTipInner = {forkBackTip_.x - 5.6f, forkBackTip_.y + 2.0f};
    const Vec2 backMidInner = {bx + slingH * 0.06f, crotchY - slingH * 0.20f};
    addPoly(
        {backHornBaseL, backHornBaseR, backMidOuter, backTipOuter, backTipTop, backTipInner, backMidInner},
        {118, 70, 34, 252}, {62, 34, 15, 252}, 1.8f);
    // Leather tie-collar around rear horn tip
    const float backHornAng = std::atan2(forkBackTip_.y - crotchY, forkBackTip_.x - bx) + 0.5f * kPi;
    out.add(BoxItem{forkBackTip_ + Vec2{-0.8f, 3.5f},
                    {12.8f, 5.8f},
                    backHornAng,
                    2.2f,
                    {72, 40, 22, 252},
                    {152, 104, 62, 220},
                    1.1f});

    const bool hasReadyProjectile =
        (phase_ == Phase::ReadyToAim || phase_ == Phase::Pulling) && currentAmmoIdx_ < ammo_.size();
    const Vec2 bandPos = (phase_ == Phase::Pulling) ? pullPosScene_ : slingAnchor_;
    const Vec2 pullVec = bandPos - slingAnchor_;
    const float pullDist = pullVec.length();
    const float stretch = std::clamp(pullDist / std::max(1.0f, maxPull_), 0.0f, 1.0f);
    const Vec2 pouchBackDir = pullDist > 2.5f ? (pullVec * (1.0f / pullDist)) : Vec2{-0.96f, 0.28f};
    const Vec2 pouchPerp = {-pouchBackDir.y, pouchBackDir.x};

    const Color bandFill = {
        static_cast<std::uint8_t>(168 + static_cast<int>(64.0f * stretch)),
        static_cast<std::uint8_t>(54 + static_cast<int>(30.0f * stretch)),
        38,
        246,
    };
    const Color bandEdge = {88, 24, 18, 238};
    const float tipBandHW = 4.4f - 1.5f * stretch;
    const float pouchBandHW = 4.8f - 1.2f * stretch;

    const Vec2 pouchRearAttach =
        bandPos + pouchBackDir * (projRadius_ * 0.88f) - pouchPerp * (projRadius_ * 0.22f);
    const Vec2 pouchFrontAttach =
        bandPos + pouchBackDir * (projRadius_ * 0.88f) + pouchPerp * (projRadius_ * 0.22f);

    if (hasReadyProjectile) {
        drawTaperedBand(forkBackTip_ + Vec2{-1.0f, 3.2f}, tipBandHW, pouchRearAttach, pouchBandHW, bandFill,
                        bandEdge);
    } else {
        // Slack resting band between the two horns after firing
        const Vec2 slackMid = {(forkBackTip_.x + forkFrontTip_.x) * 0.5f,
                               std::max(forkBackTip_.y, forkFrontTip_.y) + 18.0f};
        drawTaperedBand(forkBackTip_ + Vec2{-1.0f, 3.2f}, 3.6f, slackMid, 3.4f, {154, 52, 38, 215}, bandEdge);
    }

    // 3. Parabolic trajectory preview pearls when pulling!
    if (phase_ == Phase::Pulling && config_.showTrajectory && pullDist >= 12.0f) {
        out.add(
            CircleItem{slingAnchor_, projRadius_ * 0.42f, {255, 235, 165, 85}, {255, 220, 120, 165}, 1.2f});
        const Vec2 v0Phys = launchVelocityPhysics(pullVec);
        const Vec2 p0Phys = coords_.toPhysics(pullPosScene_);
        for (int step = 1; step <= 15; ++step) {
            const float t = static_cast<float>(step) * 0.052f;
            const Vec2 ptPhys = {p0Phys.x + v0Phys.x * t, p0Phys.y + v0Phys.y * t - 0.5f * 9.8f * t * t};
            const Vec2 ptScene = coords_.toScene(ptPhys);
            if (ptScene.y > groundY_ || ptScene.x > w) break;
            const auto alpha = static_cast<std::uint8_t>(std::max(38, 236 - step * 13));
            const float rad = std::max(2.1f, 4.9f - static_cast<float>(step) * 0.17f);
            out.add(CircleItem{ptScene,
                               rad + 1.4f,
                               {120, 220, 255, static_cast<std::uint8_t>(alpha / 3)},
                               {0, 0, 0, 0},
                               0.0f});
            out.add(CircleItem{ptScene, rad, {255, 252, 240, alpha}, {28, 40, 58, alpha}, 1.0f});
        }
    }

    // 4. Fortress structural blocks (3D Wood Timbers, Translucent Crystal Glass, Chiseled Stone)
    if (editing_) {
        const Rect zone = customBuildZoneScene();
        out.add(BoxItem{{zone.x + zone.width * 0.5f, zone.y + zone.height * 0.5f},
                        {zone.width, zone.height},
                        0.0f,
                        8.0f,
                        {14, 28, 46, 92},
                        {110, 210, 255, 195},
                        1.6f});
        for (float gx = zone.x + fortUnit_; gx < zone.x + zone.width - 8.0f; gx += fortUnit_) {
            out.add(LineItem{{gx, zone.y + 4.0f}, {gx, groundY_}, {120, 195, 240, 48}, 1.0f, true});
        }
        for (float gyLine = groundY_ - fortUnit_; gyLine > zone.y + 8.0f; gyLine -= fortUnit_) {
            out.add(LineItem{{zone.x + 4.0f, gyLine},
                             {zone.x + zone.width - 4.0f, gyLine},
                             {120, 195, 240, 48},
                             1.0f,
                             true});
        }
        std::size_t defTexIdx = 0;
        for (const auto& pc : editorPieces_) {
            const Vec2 c = customPieceSceneCenter(pc);
            const Vec2 sz = customPieceSceneSize(pc.kind);
            if (pc.kind == SlingshotCustomPieceKind::DefenderSlot) {
                const float r = sz.x * 0.5f;
                out.add(CircleItem{c, r + 2.5f, {58, 26, 32, 220}, {255, 128, 108, 248}, 2.4f});
                if (!setup_.icons.empty()) {
                    SpriteItem sp;
                    sp.texture = setup_.icons[(defTexIdx++) % setup_.icons.size()].texture;
                    sp.center = c;
                    sp.size = {r * 1.72f, r * 1.72f};
                    sp.clipCircle = true;
                    out.add(sp);
                }
            } else if (pc.kind == SlingshotCustomPieceKind::GlassBeam) {
                out.add(BoxItem{c, sz, 0.0f, 3.0f, {158, 234, 255, 145}, {225, 250, 255, 235}, 1.6f});
            } else if (pc.kind == SlingshotCustomPieceKind::StoneBlock) {
                out.add(BoxItem{c, sz, 0.0f, 2.8f, {134, 142, 154, 245}, {58, 64, 74, 250}, 1.8f});
            } else {
                out.add(BoxItem{c, sz, 0.0f, 3.0f, {194, 132, 68, 245}, {92, 52, 22, 250}, 1.8f});
            }
        }
    }
    for (const auto& blk : blocks_) {
        if (editing_ || blk.destroyed) continue;
        const BodyState st = world_->state(blk.body);
        const Vec2 c = coords_.toScene(st.position);
        const float rot = SceneToPhysics::angleToScene(st.angle);
        const float cr = std::cos(rot);
        const float sr = std::sin(rot);
        const float hw = blk.sizeScene.x * 0.5f;
        const float hh = blk.sizeScene.y * 0.5f;
        const float dmgRatio = std::clamp(1.0f - blk.hp / std::max(0.01f, blk.maxHp), 0.0f, 1.0f);

        auto W = [&](float lx, float ly) -> Vec2 {
            return toWorld(c, cr, sr, std::clamp(lx, -hw + 1.2f, hw - 1.2f),
                           std::clamp(ly, -hh + 1.2f, hh - 1.2f));
        };

        if (blk.material == SlingshotBlockKind::Glass) {
            // Translucent crystal ice-glass pane letting desktop wallpaper glow through
            out.add(BoxItem{c, blk.sizeScene, rot, 3.2f, {158, 234, 255, 108}, {225, 250, 255, 228}, 1.6f});
            if (hw > 5.0f && hh > 5.0f) {
                out.add(BoxItem{c,
                                {blk.sizeScene.x - 5.0f, blk.sizeScene.y - 5.0f},
                                rot,
                                2.0f,
                                {255, 255, 255, 18},
                                {255, 255, 255, 95},
                                0.9f});
            }
            // Diagonal specular reflection ribbons across the glass face
            addPoly({W(-hw * 0.62f, -hh + 1.8f), W(-hw * 0.24f, -hh + 1.8f), W(hw * 0.42f, hh - 1.8f),
                     W(hw * 0.04f, hh - 1.8f)},
                    {255, 255, 255, 56});
            addPoly({W(-hw * 0.10f, -hh + 1.8f), W(hw * 0.06f, -hh + 1.8f), W(hw * 0.72f, hh - 1.8f),
                     W(hw * 0.56f, hh - 1.8f)},
                    {255, 255, 255, 34});

            // Realistic multi-branch crystalline spiderweb fractures radiating from impact point
            if (dmgRatio > 0.08f) {
                const Vec2 imp = blk.lastHitLocal;
                std::uint32_t rng = blk.visualSeed ^ 0x9e3779b9u;
                // Frosted impact crush star
                const float chipR = std::min(hw, hh) * (0.16f + 0.12f * dmgRatio);
                addPoly({W(imp.x - chipR, imp.y - chipR * 0.3f), W(imp.x + chipR * 0.2f, imp.y - chipR),
                         W(imp.x + chipR, imp.y + chipR * 0.35f), W(imp.x - chipR * 0.3f, imp.y + chipR)},
                        {238, 252, 255, 168}, {255, 255, 255, 220}, 0.9f);

                const int rays = 4 + static_cast<int>(dmgRatio * 4.2f);
                const float maxReach = std::hypot(hw, hh) * (0.48f + 0.48f * dmgRatio);
                std::array<Vec2, 8> midWebPts{};
                for (int r = 0; r < rays; ++r) {
                    const float baseA = (static_cast<float>(r) / static_cast<float>(rays)) * 2.0f * kPi +
                                        randRange(rng, -0.22f, 0.22f);
                    Vec2 prev = imp;
                    for (int seg = 1; seg <= 3; ++seg) {
                        const float kink = baseA + randRange(rng, -0.32f, 0.32f);
                        const float stepLen = maxReach * (0.28f + 0.08f * static_cast<float>(seg));
                        Vec2 next = {std::clamp(prev.x + std::cos(kink) * stepLen, -hw + 1.5f, hw - 1.5f),
                                     std::clamp(prev.y + std::sin(kink) * stepLen, -hh + 1.5f, hh - 1.5f)};
                        if (seg == 1 && r < 8) midWebPts[static_cast<std::size_t>(r)] = next;
                        const Vec2 pA = W(prev.x, prev.y);
                        const Vec2 pB = W(next.x, next.y);
                        const float wLine = std::max(0.8f, 1.65f - 0.32f * static_cast<float>(seg));
                        out.add(LineItem{pA, pB, {246, 254, 255, 224}, wLine, false});
                        out.add(LineItem{
                            pA + Vec2{0.6f, 0.6f}, pB + Vec2{0.6f, 0.6f}, {32, 92, 128, 175}, 0.8f, false});
                        if ((next - prev).length() < 2.0f) break;
                        prev = next;
                    }
                }
                // Concentric spiderweb cross-links between adjacent fracture rays
                if (dmgRatio > 0.32f) {
                    const int webN = std::min(rays, 8);
                    for (int r = 0; r < webN; ++r) {
                        const Vec2 a = midWebPts[static_cast<std::size_t>(r)];
                        const Vec2 b = midWebPts[static_cast<std::size_t>((r + 1) % webN)];
                        out.add(LineItem{W(a.x, a.y), W(b.x, b.y), {235, 252, 255, 178}, 0.95f, false});
                    }
                }
            }
        } else if (blk.material == SlingshotBlockKind::Wood) {
            // 3D beveled warm oak timber + natural longitudinal wood grain + carpenter rivets
            out.add(BoxItem{c, blk.sizeScene, rot, 3.0f, {194, 132, 68, 250}, {92, 52, 22, 252}, 1.8f});
            const bool horiz = (hw >= hh);
            const float bevel = std::min(3.4f, std::min(hw, hh) * 0.28f);
            if (horiz) {
                addPoly({W(-hw + 1.5f, -hh + 1.5f), W(hw - 1.5f, -hh + 1.5f),
                         W(hw - 3.0f, -hh + 1.5f + bevel), W(-hw + 3.0f, -hh + 1.5f + bevel)},
                        {228, 172, 102, 148});
                addPoly({W(-hw + 3.0f, hh - 1.5f - bevel), W(hw - 3.0f, hh - 1.5f - bevel),
                         W(hw - 1.5f, hh - 1.5f), W(-hw + 1.5f, hh - 1.5f)},
                        {136, 80, 34, 156});
            } else {
                addPoly({W(-hw + 1.5f, -hh + 1.5f), W(-hw + 1.5f + bevel, -hh + 3.0f),
                         W(-hw + 1.5f + bevel, hh - 3.0f), W(-hw + 1.5f, hh - 1.5f)},
                        {228, 172, 102, 148});
                addPoly({W(hw - 1.5f - bevel, -hh + 3.0f), W(hw - 1.5f, -hh + 1.5f), W(hw - 1.5f, hh - 1.5f),
                         W(hw - 1.5f - bevel, hh - 3.0f)},
                        {136, 80, 34, 156});
            }

            // Subtle longitudinal wood-grain lines & iron end rivets
            std::uint32_t grng = blk.visualSeed ^ 0x85ebca6bu;
            const float longHalf = horiz ? hw : hh;
            const float shortHalf = horiz ? hh : hw;
            auto G = [&](float u, float v) -> Vec2 { return horiz ? W(u, v) : W(v, u); };
            for (int g = 0; g < 2; ++g) {
                const float vOff = (g == 0 ? -0.30f : 0.28f) * shortHalf + randRange(grng, -1.2f, 1.2f);
                const float u0 = -longHalf * randRange(grng, 0.58f, 0.86f);
                const float u1 = longHalf * randRange(grng, 0.58f, 0.86f);
                out.add(LineItem{G(u0, vOff),
                                 G(u1, vOff + randRange(grng, -1.0f, 1.0f)),
                                 {146, 88, 38, 105},
                                 1.1f,
                                 false});
            }
            if (longHalf > 18.0f && shortHalf > 4.5f) {
                out.add(
                    CircleItem{G(-longHalf + 6.5f, 0.0f), 1.9f, {74, 62, 54, 225}, {42, 32, 26, 220}, 0.7f});
                out.add(
                    CircleItem{G(longHalf - 6.5f, 0.0f), 1.9f, {74, 62, 54, 225}, {42, 32, 26, 220}, 0.7f});
            }

            // Realistic grain-following fibrous wood splits & chipped edge V-gouges (never a single fake
            // line!)
            if (dmgRatio > 0.08f) {
                const float impU = horiz ? blk.lastHitLocal.x : blk.lastHitLocal.y;
                const float impV = horiz ? blk.lastHitLocal.y : blk.lastHitLocal.x;
                const float edgeSign = (impV >= 0.0f) ? 1.0f : -1.0f;

                // 1) Chipped V-notch splinter gouge bitten out of the impacted timber edge
                const float notchW = std::clamp(longHalf * (0.16f + 0.14f * dmgRatio), 4.5f, 14.0f);
                const float notchD = std::clamp(shortHalf * (0.34f + 0.30f * dmgRatio), 2.6f, 8.5f);
                const float nu = std::clamp(impU, -longHalf + notchW + 2.0f, longHalf - notchW - 2.0f);
                addPoly({G(nu - notchW, edgeSign * (shortHalf - 1.3f)),
                         G(nu + notchW * 0.85f, edgeSign * (shortHalf - 1.3f)),
                         G(nu + notchW * 0.15f, edgeSign * (shortHalf - notchD * 0.55f)),
                         G(nu - notchW * 0.25f, edgeSign * (shortHalf - notchD))},
                        {54, 28, 12, 238}, {238, 196, 132, 220}, 1.0f);

                // 2) Multi-segment jagged longitudinal split running ALONG the wood grain with splay fork
                const int fissures = (dmgRatio > 0.48f) ? 2 : 1;
                for (int f = 0; f < fissures; ++f) {
                    const float baseV =
                        (f == 0) ? std::clamp(impV * 0.45f, -shortHalf * 0.48f, shortHalf * 0.48f)
                                 : std::clamp(-impV * 0.38f, -shortHalf * 0.52f, shortHalf * 0.52f);
                    const float span = longHalf * (0.42f + 0.46f * dmgRatio);
                    const float startU = std::clamp(nu - span * 0.55f, -longHalf + 2.5f, longHalf - 2.5f);
                    const float endU = std::clamp(nu + span * 0.65f, -longHalf + 2.5f, longHalf - 2.5f);
                    Vec2 prev = {startU, baseV + randRange(grng, -1.4f, 1.4f)};
                    for (int s = 1; s <= 4; ++s) {
                        const float t = static_cast<float>(s) / 4.0f;
                        const float cu = startU + (endU - startU) * t;
                        const float cv =
                            std::clamp(baseV + ((s % 2 == 0) ? 1.8f : -1.6f) * (0.6f + 0.6f * dmgRatio),
                                       -shortHalf + 2.0f, shortHalf - 2.0f);
                        const Vec2 p0 = G(prev.x, prev.y);
                        const Vec2 p1 = G(cu, cv);
                        const Vec2 lip0 =
                            G(prev.x, std::clamp(prev.y + 1.2f, -shortHalf + 1.5f, shortHalf - 1.5f));
                        const Vec2 lip1 = G(cu, std::clamp(cv + 1.2f, -shortHalf + 1.5f, shortHalf - 1.5f));
                        out.add(LineItem{lip0, lip1, {240, 200, 138, 205}, 1.2f, false});
                        out.add(LineItem{p0, p1, {48, 24, 10, 238}, (s == 2 || s == 3) ? 2.0f : 1.3f, false});
                        // Splayed wood-fiber fork off the middle of the split
                        if (s == 2 && dmgRatio > 0.30f) {
                            const float forkU =
                                std::clamp(cu + longHalf * 0.22f, -longHalf + 2.5f, longHalf - 2.5f);
                            const float forkV =
                                std::clamp(cv + (f == 0 ? 3.2f : -3.2f), -shortHalf + 2.0f, shortHalf - 2.0f);
                            out.add(LineItem{p1, G(forkU, forkV), {56, 30, 12, 215}, 1.2f, false});
                        }
                        prev = {cu, cv};
                    }
                }
            }
        } else {
            // Chiseled 4-facet ashlar granite stone block
            out.add(BoxItem{c, blk.sizeScene, rot, 2.8f, {134, 142, 154, 252}, {58, 64, 74, 252}, 1.8f});
            const float bev = std::min(4.2f, std::min(hw, hh) * 0.30f);
            addPoly({W(-hw + 1.4f, -hh + 1.4f), W(hw - 1.4f, -hh + 1.4f),
                     W(hw - 1.4f - bev, -hh + 1.4f + bev), W(-hw + 1.4f + bev, -hh + 1.4f + bev)},
                    {178, 186, 198, 165});
            addPoly({W(-hw + 1.4f + bev, hh - 1.4f - bev), W(hw - 1.4f - bev, hh - 1.4f - bev),
                     W(hw - 1.4f, hh - 1.4f), W(-hw + 1.4f, hh - 1.4f)},
                    {86, 92, 104, 180});

            if (dmgRatio > 0.08f) {
                std::uint32_t srng = blk.visualSeed ^ 0xc2b2ae35u;
                const Vec2 imp = blk.lastHitLocal;
                // Stepped zig-zag masonry fissure + limestone dust rim
                Vec2 cur = {std::clamp(imp.x, -hw * 0.5f, hw * 0.5f),
                            (imp.y <= 0.0f ? -hh + 1.5f : hh - 1.5f)};
                const float targetY = (imp.y <= 0.0f ? 1.0f : -1.0f) * hh * (0.25f + 0.65f * dmgRatio);
                for (int s = 1; s <= 4; ++s) {
                    const float t = static_cast<float>(s) / 4.0f;
                    const Vec2 nxt = {
                        std::clamp(imp.x + ((s % 2 == 0) ? 4.2f : -4.2f) + randRange(srng, -1.8f, 1.8f),
                                   -hw + 2.0f, hw - 2.0f),
                        std::clamp(cur.y + (targetY - cur.y) * t, -hh + 1.6f, hh - 1.6f)};
                    const Vec2 w0 = W(cur.x, cur.y);
                    const Vec2 w1 = W(nxt.x, nxt.y);
                    out.add(LineItem{
                        w0 + Vec2{1.0f, 0.8f}, w1 + Vec2{1.0f, 0.8f}, {204, 212, 222, 190}, 1.1f, false});
                    out.add(LineItem{w0, w1, {36, 40, 46, 244}, 2.1f - 0.22f * static_cast<float>(s), false});
                    cur = nxt;
                }
            }
        }
    }

    // 5. Fortress defender icons & trophy shelf
    for (const auto& def : defenders_) {
        if (editing_) break;
        const Vec2 c = def.defeated ? def.trophySlotScene : coords_.toScene(world_->state(def.body).position);
        const float rot = def.defeated ? 0.0f : SceneToPhysics::angleToScene(world_->state(def.body).angle);
        const float r = def.defeated ? (def.radiusScene * 0.78f * def.trophyAnim) : def.radiusScene;
        const Color ringFill = def.defeated ? Color{48, 68, 52, 195} : Color{58, 26, 32, 215};
        const Color ringStroke = def.defeated ? Color{120, 235, 145, 235} : Color{255, 108, 96, 245};
        out.add(CircleItem{c, r + 2.5f, ringFill, ringStroke, 2.5f});
        if (def.texture != kNoTexture) {
            SpriteItem sp;
            sp.texture = def.texture;
            sp.center = c;
            sp.size = {r * 1.72f, r * 1.72f};
            sp.rotation = rot;
            sp.clipCircle = true;
            sp.opacity = def.defeated ? 0.85f : 1.0f;
            out.add(sp);
        } else {
            out.add(CircleItem{c, r * 0.46f, ringStroke, {255, 245, 215, 220}, 1.5f});
        }
    }

    // 6. Queued, active, and launched ammo icons
    if (phase_ == Phase::ReadyToAim && hasReadyProjectile) {
        out.add(
            CircleItem{slingAnchor_, projRadius_ + 7.5f, {255, 225, 130, 26}, {255, 224, 128, 128}, 1.4f});
    }
    for (std::size_t i = 0; i < ammo_.size(); ++i) {
        const auto& a = ammo_[i];
        Vec2 c = a.waitPosScene;
        float rot = 0.0f;
        Color ringStroke{255, 214, 92, 245};
        if (a.launched) {
            const BodyState st = world_->state(a.body);
            if (!st.enabled) continue;
            c = coords_.toScene(st.position);
            rot = SceneToPhysics::angleToScene(st.angle);
            ringStroke = (i == currentAmmoIdx_ && phase_ == Phase::InFlight && !dashUsedThisShot_)
                             ? Color{110, 230, 255, 255}
                             : Color{255, 195, 85, 225};
        } else if (i == currentAmmoIdx_) {
            c = bandPos;
        }
        out.add(CircleItem{c, projRadius_ + 2.2f, {30, 38, 54, 224}, ringStroke, 2.4f});
        if (a.texture != kNoTexture) {
            SpriteItem sp;
            sp.texture = a.texture;
            sp.center = c;
            sp.size = {projRadius_ * 1.72f, projRadius_ * 1.72f};
            sp.rotation = rot;
            sp.clipCircle = true;
            out.add(sp);
        } else {
            out.add(CircleItem{c, projRadius_ * 0.48f, ringStroke, {255, 250, 230, 230}, 1.4f});
        }
    }

    // 7. Curved 3D Stitched Leather Pouch + Front Tapered Latex Band + Sculpted Front Y-Horn & Handle
    if (hasReadyProjectile) {
        // Crescent leather saddle pouch wrapping cleanly around the rear hemisphere of the icon
        const float pr = projRadius_;
        const Vec2 pOuterBack = bandPos + pouchBackDir * (pr + 6.5f);
        const Vec2 pOuterTop = bandPos + pouchBackDir * (pr * 0.55f) - pouchPerp * (pr * 0.76f);
        const Vec2 pInnerTop = bandPos + pouchBackDir * (pr * 0.22f) - pouchPerp * (pr * 0.58f);
        const Vec2 pInnerMid = bandPos + pouchBackDir * (pr * 0.80f);
        const Vec2 pInnerBot = bandPos + pouchBackDir * (pr * 0.22f) + pouchPerp * (pr * 0.58f);
        const Vec2 pOuterBot = bandPos + pouchBackDir * (pr * 0.55f) + pouchPerp * (pr * 0.76f);
        addPoly({pOuterTop, pOuterBack, pOuterBot, pInnerBot, pInnerMid, pInnerTop}, {84, 46, 24, 252},
                {44, 22, 10, 252}, 1.5f);
        // Tan leather stitch highlight along outer saddle ridge
        out.add(LineItem{bandPos + pouchBackDir * (pr + 3.2f) - pouchPerp * (pr * 0.42f),
                         bandPos + pouchBackDir * (pr + 3.2f) + pouchPerp * (pr * 0.42f),
                         {192, 138, 86, 215},
                         1.4f,
                         true});

        drawTaperedBand(forkFrontTip_ + Vec2{1.0f, 3.2f}, tipBandHW, pouchFrontAttach, pouchBandHW, bandFill,
                        bandEdge);
    } else {
        const Vec2 slackMid = {(forkBackTip_.x + forkFrontTip_.x) * 0.5f,
                               std::max(forkBackTip_.y, forkFrontTip_.y) + 18.0f};
        drawTaperedBand(forkFrontTip_ + Vec2{1.0f, 3.2f}, 3.6f, slackMid, 3.4f, {172, 58, 42, 225}, bandEdge);
    }

    // Sculpted Front-Left Y-Horn & Ergonomic Carved Hardwood Handle (drawn in front for true 3D depth!)
    addPoly({{bx - 8.2f, gy - 9.0f},
             {bx + 8.2f, gy - 9.0f},
             {bx + 6.8f, crotchY + 4.0f},
             {bx - 1.5f, crotchY - 3.0f},
             {forkFrontTip_.x + 6.0f, forkFrontTip_.y + 2.5f},
             {forkFrontTip_.x - 0.5f, forkFrontTip_.y - 5.2f},
             {forkFrontTip_.x - 6.2f, forkFrontTip_.y + 2.0f},
             {bx - slingH * 0.14f, crotchY - slingH * 0.14f}},
            {168, 106, 50, 254}, {74, 42, 18, 254}, 1.9f);
    // Warm sunlit oak bevel highlight along front handle & left horn
    addPoly({{bx - 5.2f, gy - 11.0f},
             {bx - 0.8f, gy - 11.0f},
             {bx - 1.8f, crotchY - 1.0f},
             {forkFrontTip_.x + 1.8f, forkFrontTip_.y + 2.0f},
             {forkFrontTip_.x - 2.6f, forkFrontTip_.y + 2.0f},
             {bx - 5.8f, crotchY + 2.0f}},
            {214, 152, 86, 158});
    // Rounded brass/mahogany base pommel & 3 wrapped leather handle grip bands
    out.add(
        BoxItem{{bx, gy - 10.0f}, {19.5f, 6.2f}, 0.0f, 3.0f, {118, 68, 32, 252}, {62, 34, 14, 250}, 1.4f});
    for (int g = 0; g < 3; ++g) {
        const float gyBand = gy - 20.0f - static_cast<float>(g) * (slingH * 0.085f);
        if (gyBand > crotchY + 8.0f) {
            out.add(BoxItem{
                {bx, gyBand}, {16.2f, 5.2f}, -0.06f, 2.2f, {86, 48, 26, 248}, {176, 124, 74, 195}, 1.0f});
        }
    }
    // Leather tie-collar around front horn tip
    const float frontHornAng = std::atan2(forkFrontTip_.y - crotchY, forkFrontTip_.x - bx) + 0.5f * kPi;
    out.add(BoxItem{forkFrontTip_ + Vec2{0.8f, 3.5f},
                    {13.4f, 6.0f},
                    frontHornAng,
                    2.2f,
                    {78, 44, 24, 254},
                    {168, 118, 72, 230},
                    1.1f});

    // 8. Parked reserve icons (if > 8 defenders)
    for (std::size_t i = 0; i < parkedIcons_.size(); ++i) {
        if (setup_.icons[parkedIcons_[i]].texture == kNoTexture) continue;
        SpriteItem sp;
        sp.texture = setup_.icons[parkedIcons_[i]].texture;
        sp.center = parkedPositions_[i];
        sp.size = {28.0f, 28.0f};
        sp.opacity = 0.88f;
        out.add(sp);
    }

    // 9. Spinning angular material shards & shockwave rings
    for (const auto& d : debris_) {
        const float fade = std::clamp(d.life / std::max(0.01f, d.maxLife), 0.0f, 1.0f);
        Color c = d.color;
        c.a = static_cast<std::uint8_t>(static_cast<float>(d.color.a) * fade);
        if (!d.isShard) {
            out.add(CircleItem{d.pos, d.radius, c, {0, 0, 0, 0}, 0.0f});
            continue;
        }
        Color edge = d.accent;
        edge.a = static_cast<std::uint8_t>(static_cast<float>(d.accent.a) * fade);
        const float dc = std::cos(d.angle);
        const float ds = std::sin(d.angle);
        const float sx = d.shardSize.x * 0.5f;
        const float sy = d.shardSize.y * 0.5f;
        if (d.kind == SlingshotBlockKind::Glass) {
            // Sharp glinting triangular/diamond crystal shard
            addPoly({toWorld(d.pos, dc, ds, -sx, -sy * 0.4f), toWorld(d.pos, dc, ds, sx * 0.35f, -sy),
                     toWorld(d.pos, dc, ds, sx, sy * 0.55f), toWorld(d.pos, dc, ds, -sx * 0.3f, sy * 0.8f)},
                    c, edge, 0.9f);
        } else if (d.kind == SlingshotBlockKind::Wood) {
            // Tapered jagged wood splinter
            addPoly({toWorld(d.pos, dc, ds, -sx, -sy * 0.5f), toWorld(d.pos, dc, ds, sx, -sy * 0.15f),
                     toWorld(d.pos, dc, ds, sx * 0.72f, sy), toWorld(d.pos, dc, ds, -sx * 0.85f, sy * 0.75f)},
                    c, edge, 0.9f);
        } else {
            // Angular stone spall chunk
            addPoly({toWorld(d.pos, dc, ds, -sx * 0.8f, -sy * 0.7f),
                     toWorld(d.pos, dc, ds, sx * 0.6f, -sy * 0.9f), toWorld(d.pos, dc, ds, sx, sy * 0.3f),
                     toWorld(d.pos, dc, ds, sx * 0.1f, sy), toWorld(d.pos, dc, ds, -sx * 0.9f, sy * 0.5f)},
                    c, edge, 1.0f);
        }
    }
    for (const auto& r : rings_) {
        Color stroke = r.color;
        stroke.a = static_cast<std::uint8_t>(std::clamp(r.alpha, 0.0f, 1.0f) * 235.0f);
        out.add(CircleItem{r.center, r.radius, {0, 0, 0, 0}, stroke, 3.0f});
    }

    // 10. Top-left mini HUD pills & non-overlapping score banner (below the top floating capsule bar)
    const Color pillBg{18, 24, 36, 208};
    out.add(BoxItem{
        {trajToggleRect_.x + trajToggleRect_.width * 0.5f, trajToggleRect_.y + trajToggleRect_.height * 0.5f},
        {trajToggleRect_.width, trajToggleRect_.height},
        0.0f,
        13.0f,
        pillBg,
        config_.showTrajectory ? Color{110, 220, 255, 220} : Color{130, 145, 165, 180},
        1.5f});
    out.addText({trajToggleRect_.x + trajToggleRect_.width * 0.5f, trajToggleRect_.y + 6.0f},
                config_.showTrajectory ? "轨迹线: 开" : "轨迹线: 关",
                config_.showTrajectory ? Color{155, 235, 255, 255} : Color{200, 210, 225, 230}, 12.5f, true);

    out.add(BoxItem{
        {nextStageRect_.x + nextStageRect_.width * 0.5f, nextStageRect_.y + nextStageRect_.height * 0.5f},
        {nextStageRect_.width, nextStageRect_.height},
        0.0f,
        13.0f,
        pillBg,
        {255, 205, 95, 220},
        1.5f});
    out.addText({nextStageRect_.x + nextStageRect_.width * 0.5f, nextStageRect_.y + 6.0f}, "下一关 ▸",
                {255, 225, 130, 255}, 12.5f, true);

    out.add(BoxItem{{editorToggleRect_.x + editorToggleRect_.width * 0.5f,
                     editorToggleRect_.y + editorToggleRect_.height * 0.5f},
                    {editorToggleRect_.width, editorToggleRect_.height},
                    0.0f,
                    13.0f,
                    editing_ ? Color{32, 88, 56, 232} : pillBg,
                    editing_ ? Color{130, 255, 175, 245} : Color{185, 165, 255, 215},
                    1.5f});
    out.addText({editorToggleRect_.x + editorToggleRect_.width * 0.5f, editorToggleRect_.y + 6.0f},
                editing_ ? "完成开战 ✓" : "编辑关卡",
                editing_ ? Color{185, 255, 205, 255} : Color{215, 200, 255, 250}, 12.5f, true);

    if (editing_) {
        static constexpr std::array<const char*, 7> kToolLabels = {"木柱", "木梁", "巨石", "冰晶",
                                                                   "守军", "橡皮", "清空"};
        for (int t = 0; t < 7; ++t) {
            const Rect r = editorToolPillRect(t);
            const bool active = (t < 6 && editorTool_ == t);
            const Color stroke = (t == 6) ? Color{255, 140, 125, 225}
                                          : (active ? Color{110, 225, 255, 250} : Color{125, 142, 168, 175});
            const Color fill = active ? Color{36, 98, 156, 236} : pillBg;
            out.add(BoxItem{{r.x + r.width * 0.5f, r.y + r.height * 0.5f},
                            {r.width, r.height},
                            0.0f,
                            11.0f,
                            fill,
                            stroke,
                            1.4f});
            out.addText({r.x + r.width * 0.5f, r.y + 6.0f}, kToolLabels[static_cast<std::size_t>(t)],
                        (t == 6) ? Color{255, 190, 180, 255} : Color{240, 246, 255, 250}, 12.0f, true);
        }
    }

    const float bannerY = 60.0f;
    char banner[160]{};
    if (editing_) {
        std::snprintf(banner, sizeof(banner),
                      "关卡工坊 (%zu/48 件) · 点击右侧蓝图摆放/拖动积木与守军，Option+点击删除，点「完成开战 ✓」保存开打",
                      editorPieces_.size());
        out.addText({w * 0.5f, bannerY}, banner, {165, 238, 255, 255}, 13.5f, true);
    } else if (stageCleared_) {
        std::snprintf(banner, sizeof(banner),
                      "攻城大捷 %s！第 %d 关清台 · 得分 %d (最高 %d · 总星 %d★) · 点击进入下一关",
                      starBadge(lastClearStars_), stageIndex_, score_, highScore_, progress_.totalStars());
        out.addText({w * 0.5f, bannerY}, banner, {135, 255, 165, 255}, 15.0f, true);
    } else if (stageFailed_) {
        std::snprintf(banner, sizeof(banner), "弹药耗尽！剩余守军 %d · 得分 %d · 点击屏幕重新挑战本关",
                      remainingDefenders(), score_);
        out.addText({w * 0.5f, bannerY}, banner, {255, 165, 130, 255}, 15.0f, true);
    } else if (phase_ == Phase::InFlight && !dashUsedThisShot_) {
        std::snprintf(banner, sizeof(banner), "关卡 %d (%s) · 得分 %d · 空中点击屏幕可触发二段冲刺！",
                      stageIndex_, presetLabel(resolvedPreset()), score_);
        out.addText({w * 0.5f, bannerY}, banner, {145, 235, 255, 255}, 14.0f, true);
    } else {
        std::snprintf(banner, sizeof(banner),
                      "弹弓攻城 · 第 %d 关 (%s %s)  |  得分 %d (最高 %d · %d★)  |  弹药 %d  |  守军 %d",
                      stageIndex_, presetLabel(resolvedPreset()),
                      starBadge(progress_.starsForStage(stageIndex_)), score_, highScore_,
                      progress_.totalStars(), remainingAmmo(), remainingDefenders());
        out.addText({w * 0.5f, bannerY}, banner, {242, 246, 255, 245}, 13.5f, true);
    }
}

}  // namespace dpp
