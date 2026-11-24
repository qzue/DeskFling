#include "core/modes/billiards_mode.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <random>

namespace dpp {
namespace {

PhysicsConfig tablePhysics() {
    PhysicsConfig c;
    c.gravity = {0.0f, 0.0f};  // top-down table
    c.enableSleep = true;
    c.maximumLinearSpeed = 40.0f;
    c.enableHitEvents = true;
    c.hitEventThreshold = 0.16f;
    return c;
}

PhysicsConfig extraFloorPhysics() {
    PhysicsConfig c;
    c.gravity = {0.0f, -9.8f};  // normal downward desktop gravity
    c.enableSleep = true;
    c.maximumLinearSpeed = 30.0f;
    c.enableHitEvents = false;
    return c;
}

Vec2 normalizedOr(Vec2 v, Vec2 fallback) {
    const float len = v.length();
    return len > 1e-4f ? v / len : fallback;
}

float pointToSegmentDistSq(Vec2 p, Vec2 a, Vec2 b) {
    const Vec2 ab = b - a;
    const float ab2 = ab.lengthSquared();
    if (ab2 <= 1e-8f) return (p - a).lengthSquared();
    const float t = std::clamp(((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / ab2, 0.0f, 1.0f);
    return (p - (a + ab * t)).lengthSquared();
}

float smoothStep01(float x) {
    const float t = std::clamp(x, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

const char* aiDifficultyName(BilliardsAiDifficulty d) {
    switch (d) {
        case BilliardsAiDifficulty::Easy:
            return "简单";
        case BilliardsAiDifficulty::Medium:
            return "标准";
        case BilliardsAiDifficulty::Master:
            return "大师";
        case BilliardsAiDifficulty::Goat:
            return "球王";
    }
    return "标准";
}

const char* groupShortName(BallGroup g) {
    switch (g) {
        case BallGroup::Solid:
            return "全色";
        case BallGroup::Stripe:
            return "花色";
        case BallGroup::Eight:
            return "黑八";
        default:
            return "开放台";
    }
}

const char* snookerTargetName(int t) {
    switch (t) {
        case 1:
            return "红球(1)";
        case 2:
            return "黄球(2)";
        case 3:
            return "绿球(3)";
        case 4:
            return "棕球(4)";
        case 5:
            return "蓝球(5)";
        case 6:
            return "粉球(6)";
        case 7:
            return "黑球(7)";
        case 8:
            return "任意彩球";
        default:
            return "红球";
    }
}

std::size_t effectiveRackBallCount(std::size_t iconCount, const BilliardsConfig& config) {
    if (config.style == BilliardsStyle::Classic) return iconCount;
    if (config.rule == BilliardsRule::EightBall) return 15;
    if (config.rule == BilliardsRule::Snooker) return 21;
    if (config.iconSelection == IconSelectionPolicy::Custom) {
        std::size_t valid = 0;
        for (std::size_t idx : config.customIconIndices) {
            if (idx < iconCount) ++valid;
        }
        std::size_t chosen = valid > 0 ? valid : 15;
        if (config.freeModeMaxRackBalls > 0 && chosen > config.freeModeMaxRackBalls) {
            chosen = config.freeModeMaxRackBalls;
        }
        return chosen;
    }
    if (iconCount == 0) {
        return config.freeModeMaxRackBalls > 0 ? std::min<std::size_t>(15u, config.freeModeMaxRackBalls) : 15u;
    }
    if (config.freeModeMaxRackBalls > 0 && iconCount > config.freeModeMaxRackBalls) {
        return config.freeModeMaxRackBalls;
    }
    return iconCount;
}

}  // namespace

Vec2 BilliardsMode::computeTableSizeMeters(Vec2 workAreaSize, const BilliardsConfig& config) {
    const SceneToPhysics coords(workAreaSize);
    const Vec2 ws = coords.worldSizeMeters();
    const float availW = ws.x * (1.0f - 2.0f * config.tableInsetX);
    const float availH = ws.y * (1.0f - 2.0f * config.tableInsetY);
    if (config.style != BilliardsStyle::Desktop) {
        return {availW, availH};
    }
    // International Heyball/8-Ball: 2540x1270mm (2.0000:1);
    // International WPBSA 12-foot Snooker: 3569x1778mm (2.00731:1).
    const float aspect = (config.rule == BilliardsRule::Snooker) ? BilliardsConfig::kSnookerAspect
                                                                 : BilliardsConfig::kEightBallAspect;
    const float w = std::min(availH, availW / aspect);
    return {w * aspect, w};
}

float BilliardsMode::computeBallRadiusMeters(Vec2 workAreaSize, std::size_t iconCount,
                                             const BilliardsConfig& config) {
    const Vec2 tableSize = computeTableSizeMeters(workAreaSize, config);
    const std::size_t effCount = effectiveRackBallCount(iconCount, config);
    const int n = static_cast<int>(effCount);
    int columns = 1;
    while (columns * (columns + 1) / 2 < n) ++columns;

    if (config.style != BilliardsStyle::Classic) {
        if (config.rule == BilliardsRule::Snooker) {
            // Exact WPBSA 12-foot Snooker ball-to-width ratio: 52.5 mm / 1778 mm = 0.029528.
            const float snookerRadius = tableSize.y * (BilliardsConfig::kSnookerBallToWidth * 0.5f);
            return std::max(snookerRadius, 0.02f);
        }
        // Exact International Heyball / 8-Ball 9-foot ratio: 57.15 mm / 1270 mm = 0.0450.
        const float standardRadius = tableSize.y * (BilliardsConfig::kEightBallToWidth * 0.5f);
        const float rackRadiusX =
            (tableSize.x * 0.42f) / (static_cast<float>(std::max(1, columns - 1)) * 1.76f + 2.5f);
        const float rackRadiusY = (tableSize.y * 0.86f) / (static_cast<float>(columns) * 2.04f);
        return std::max(std::min({standardRadius, rackRadiusX, rackRadiusY}), 0.02f);
    }

    float r = std::clamp(tableSize.y * 0.04f, 0.10f, 0.32f);
    r = std::min(r, tableSize.x * 0.40f / (static_cast<float>(columns) * 1.8f + 0.5f));
    r = std::min(r, tableSize.y * 0.90f / (static_cast<float>(columns) * 2.05f));
    return std::max(r, 0.02f);
}

bool BilliardsMode::canFitIcons(Vec2 workAreaSize, std::size_t iconCount, const BilliardsConfig& config) {
    if (!workAreaSize.isFinite() || workAreaSize.x < 320.0f || workAreaSize.y < 220.0f) {
        return false;
    }
    const std::size_t effCount = effectiveRackBallCount(iconCount, config);
    if (effCount == 0 || effCount > 91) {
        return false;
    }
    const SceneToPhysics coords(workAreaSize);
    const float actualR = computeBallRadiusMeters(workAreaSize, iconCount, config);
    const float diameterPt = 2.0f * coords.lengthToScene(actualR);
    if (diameterPt < BilliardsConfig::kMinBallDiameterPoints) return false;
    if (config.rule == BilliardsRule::Free) {
        BilliardsConfig ref = config;
        ref.iconSelection = IconSelectionPolicy::FirstN;
        ref.freeModeMaxRackBalls = 0;
        const float standardR = computeBallRadiusMeters(workAreaSize, 15, ref);
        return actualR >= standardR * 0.72f;
    }
    return true;
}

std::size_t BilliardsMode::maxFittingRackBalls(Vec2 workAreaSize, const BilliardsConfig& config) {
    if (!workAreaSize.isFinite() || workAreaSize.x < 320.0f || workAreaSize.y < 220.0f) {
        return 0;
    }
    BilliardsConfig probe = config;
    probe.rule = BilliardsRule::Free;
    probe.iconSelection = IconSelectionPolicy::FirstN;
    probe.freeModeMaxRackBalls = 0;
    for (std::size_t candidate : {std::size_t{21}, std::size_t{15}, std::size_t{10}, std::size_t{6},
                                  std::size_t{3}, std::size_t{1}}) {
        if (canFitIcons(workAreaSize, candidate, probe)) return candidate;
    }
    return 0;
}

float BilliardsMode::railOuterMeters() const {
    if (config_.style != BilliardsStyle::Desktop) return radius_ * 1.45f;
    return (config_.rule == BilliardsRule::Snooker ? 3.85f : 3.45f) * radius_;
}

void BilliardsMode::populateDesktopTableCushions(PhysicsWorld& targetWorld) const {
    const float left = tableMin_.x, bottom = tableMin_.y;
    const float right = left + tableSize_.x, top = bottom + tableSize_.y;
    const float midX = left + tableSize_.x * 0.5f;
    const float r = radius_;
    const bool snk = (config_.rule == BilliardsRule::Snooker);

    const float cDepth = (snk ? 1.28f : 1.20f) * r;
    const float jr = 0.12f * r;  // Rounded cushion jaw radius (authentic Heyball & Snooker rounded jaws)
    const float cBack = (snk ? 0.66f : 0.62f) * r;
    const float sBack = (snk ? 1.06f : 1.02f) * r;

    // Top-Left & Top-Right long cushions (nose at y = top, back at y = top + cDepth).
    const std::array<Vec2, 4> topLeft = {{
        {left + cornerMouth_ + jr, top + jr},
        {midX - sideMouth_ - jr, top + jr},
        {midX - sBack - jr, top + cDepth - jr},
        {left + cBack + jr, top + cDepth - jr},
    }};
    const std::array<Vec2, 4> topRight = {{
        {midX + sideMouth_ + jr, top + jr},
        {right - cornerMouth_ - jr, top + jr},
        {right - cBack - jr, top + cDepth - jr},
        {midX + sBack + jr, top + cDepth - jr},
    }};
    // Bottom-Left & Bottom-Right long cushions (nose at y = bottom, back at y = bottom - cDepth).
    const std::array<Vec2, 4> botLeft = {{
        {left + cornerMouth_ + jr, bottom - jr},
        {midX - sideMouth_ - jr, bottom - jr},
        {midX - sBack - jr, bottom - cDepth + jr},
        {left + cBack + jr, bottom - cDepth + jr},
    }};
    const std::array<Vec2, 4> botRight = {{
        {midX + sideMouth_ + jr, bottom - jr},
        {right - cornerMouth_ - jr, bottom - jr},
        {right - cBack - jr, bottom - cDepth + jr},
        {midX + sBack + jr, bottom - cDepth + jr},
    }};
    // Left & Right short cushions.
    const std::array<Vec2, 4> leftRail = {{
        {left - jr, bottom + cornerMouth_ + jr},
        {left - jr, top - cornerMouth_ - jr},
        {left - cDepth + jr, top - cBack - jr},
        {left - cDepth + jr, bottom + cBack + jr},
    }};
    const std::array<Vec2, 4> rightRail = {{
        {right + jr, bottom + cornerMouth_ + jr},
        {right + jr, top - cornerMouth_ - jr},
        {right + cDepth - jr, top - cBack - jr},
        {right + cDepth - jr, bottom + cBack + jr},
    }};

    for (const auto& poly : {topLeft, topRight, botLeft, botRight, leftRail, rightRail}) {
        targetWorld.addStaticPolygon(poly, jr, /*friction=*/0.10f, /*restitution=*/0.85f);
    }

    // Outer safety frame behind all 6 recessed pocket wells so fast shots never escape the table.
    const float outerM = railOuterMeters();
    targetWorld.createBoundary({left - outerM, bottom - outerM},
                               {tableSize_.x + 2.0f * outerM, tableSize_.y + 2.0f * outerM}, true, 1.0f,
                               0.25f, 0.35f);
}

void BilliardsMode::buildDesktopTableCushions() { populateDesktopTableCushions(world_); }

BilliardsMode::BilliardsMode(const SceneSetup& setup, const BilliardsConfig& config, int initialBestShots,
                             int startingPlayer, std::array<int, 2> initialSeriesWins)
    : sceneSize_(setup.workAreaSize),
      coords_(setup.workAreaSize),
      config_(config),
      world_(tablePhysics()),
      bestShots_(std::max(0, initialBestShots)),
      startingPlayer_((config.matchMode != BilliardsMatchMode::Solo && startingPlayer == 1) ? 1 : 0),
      currentPlayer_((config.matchMode != BilliardsMatchMode::Solo && startingPlayer == 1) ? 1 : 0),
      seriesWins_(initialSeriesWins) {
    config_.tableTheme = config_.effectiveTableTheme();
    config_.showTableChrome = (config_.tableTheme == BilliardsTableTheme::Full);
    const Vec2 ws = coords_.worldSizeMeters();
    tableSize_ = computeTableSizeMeters(setup.workAreaSize, config_);
    tableMin_ = {(ws.x - tableSize_.x) * 0.5f, (ws.y - tableSize_.y) * 0.5f};
    radius_ = computeBallRadiusMeters(setup.workAreaSize, setup.icons.size(), config_);

    const float left = tableMin_.x, bottom = tableMin_.y;
    const float right = left + tableSize_.x, top = bottom + tableSize_.y;
    const float midX = left + tableSize_.x * 0.5f, midY = bottom + tableSize_.y * 0.5f;

    if (config_.style == BilliardsStyle::Desktop) {
        const bool snk = (config_.rule == BilliardsRule::Snooker);
        // Tight precision Heyball & WPBSA Snooker jaw mouths hugging the recessed pocket wells:
        cornerMouth_ = (snk ? 1.76f : 1.70f) * radius_;
        sideMouth_ = (snk ? 1.48f : 1.44f) * radius_;
        pocketHoleRadius_ = (snk ? 1.46f : 1.44f) * radius_;
        sidePocketHoleRadius_ = (snk ? 1.48f : 1.46f) * radius_;

        // Recessed well centers sit INSIDE the rail & corner jaws, never protruding past the wood apron.
        const float cRecess = (snk ? 0.78f : 0.76f) * radius_;
        const float sRecess = (snk ? 1.16f : 1.14f) * radius_;
        pockets_ = {
            {left - cRecess, top + cRecess},    {midX, top + sRecess},    {right + cRecess, top + cRecess},
            {left - cRecess, bottom - cRecess}, {midX, bottom - sRecess}, {right + cRecess, bottom - cRecess},
        };
        pocketRadius_ = 1.74f * radius_;
        sidePocketRadius_ = 1.56f * radius_;
        buildDesktopTableCushions();
    } else {
        world_.createBoundary(tableMin_, tableSize_, true, 1.0f, /*friction=*/0.1f, /*restitution=*/0.85f);
        pocketRadius_ = std::min(std::max(radius_ * 1.6f, tableSize_.y * 0.045f), 0.5f);
        sidePocketRadius_ = pocketRadius_;
        pocketHoleRadius_ = pocketRadius_;
        sidePocketHoleRadius_ = sidePocketRadius_;
        pockets_ = {{left, top}, {midX, top}, {right, top}, {left, bottom}, {midX, bottom}, {right, bottom}};
    }

    const bool desktop = config_.style == BilliardsStyle::Desktop;
    const float linDamp = desktop ? 0.95f : 0.5f;
    BodyMaterial ballMat{1.0f, 0.05f, 0.92f, linDamp, 1.0f, desktop};
    BodyMaterial whiteMat = ballMat;
    whiteMat.bullet = true;

    // 1. Select which desktop icons go onto the table according to IconSelectionPolicy & BilliardsRule.
    const std::size_t totalIcons = setup.icons.size();
    iconToBall_.assign(totalIcons, -1);
    iconToExtra_.assign(totalIcons, -1);

    std::vector<std::size_t> pool;
    if (!desktop || config_.iconSelection == IconSelectionPolicy::FirstN) {
        pool.resize(totalIcons);
        std::iota(pool.begin(), pool.end(), std::size_t{0});
    } else if (config_.iconSelection == IconSelectionPolicy::Random) {
        pool.resize(totalIcons);
        std::iota(pool.begin(), pool.end(), std::size_t{0});
        std::mt19937 rng(config_.randomSeed);
        std::shuffle(pool.begin(), pool.end(), rng);
    } else {
        std::vector<bool> seen(totalIcons, false);
        for (std::size_t idx : config_.customIconIndices) {
            if (idx < totalIcons && !seen[idx]) {
                seen[idx] = true;
                pool.push_back(idx);
            }
        }
    }

    std::size_t targetBalls = pool.size();
    if (desktop) {
        if (config_.rule == BilliardsRule::EightBall) {
            targetBalls = 15;
        } else if (config_.rule == BilliardsRule::Snooker) {
            targetBalls = 21;
        } else if (config_.iconSelection == IconSelectionPolicy::Custom && pool.empty()) {
            targetBalls = 15;
        }
        if (config_.rule == BilliardsRule::Free && config_.freeModeMaxRackBalls > 0 &&
            targetBalls > config_.freeModeMaxRackBalls) {
            targetBalls = config_.freeModeMaxRackBalls;
        }
    }
    if (pool.size() > targetBalls) pool.resize(targetBalls);

    // 2. Spawn cue ball and rack object balls according to exact international rules & spots.
    if (desktop && config_.rule == BilliardsRule::Snooker) {
        // WPBSA 12-foot Snooker: Baulk at 737/3569 L, D radius 292/1778 W, Pink at 0.75 L, Black at 324mm
        // from top.
        const float baulkX = left + tableSize_.x * BilliardsConfig::kSnookerBaulkRatio;
        const float dR = tableSize_.y * BilliardsConfig::kSnookerDRatio;
        whiteSpawn_ = {baulkX - dR * 0.50f, midY - dR * 0.30f};
        white_ = world_.addCircle(whiteSpawn_, radius_, whiteMat);

        const Vec2 yellowSpot{baulkX, midY - dR};
        const Vec2 greenSpot{baulkX, midY + dR};
        const Vec2 brownSpot{baulkX, midY};
        const Vec2 blueSpot{left + tableSize_.x * 0.50f, midY};
        const Vec2 pinkSpot{left + tableSize_.x * 0.75f, midY};
        const Vec2 blackSpot{left + tableSize_.x * BilliardsConfig::kSnookerBlackRatio, midY};

        balls_.reserve(21);
        std::size_t iconCursor = 0;
        auto nextTex = [&](int& outIconIdx) -> TextureId {
            if (iconCursor < pool.size()) {
                const std::size_t ii = pool[iconCursor++];
                outIconIdx = static_cast<int>(ii);
                return setup.icons[ii].texture;
            }
            outIconIdx = -1;
            return kNoTexture;
        };

        const float pitchX = 2.0f * radius_ * 0.8660254f * 1.02f;
        const float pitchY = 2.0f * radius_ * 1.02f;
        const float redApexX = pinkSpot.x + 2.12f * radius_;
        for (int col = 0; col < 5; ++col) {
            for (int row = 0; row <= col; ++row) {
                const Vec2 p{redApexX + static_cast<float>(col) * pitchX,
                             midY + (static_cast<float>(row) - static_cast<float>(col) * 0.5f) * pitchY};
                int iconIdx = -1;
                const TextureId tex = nextTex(iconIdx);
                const BodyIndex bi = world_.addCircle(p, radius_, ballMat);
                const int bSlot = static_cast<int>(balls_.size());
                if (iconIdx >= 0) iconToBall_[static_cast<std::size_t>(iconIdx)] = bSlot;
                balls_.push_back(Ball{bi, tex, iconIdx, 1, BallGroup::SnookerRed, p});
            }
        }

        const std::array<std::pair<int, Vec2>, 6> colors = {{
            {2, yellowSpot},
            {3, greenSpot},
            {4, brownSpot},
            {5, blueSpot},
            {6, pinkSpot},
            {7, blackSpot},
        }};
        for (const auto& [pts, spot] : colors) {
            int iconIdx = -1;
            const TextureId tex = nextTex(iconIdx);
            const BodyIndex bi = world_.addCircle(spot, radius_, ballMat);
            const int bSlot = static_cast<int>(balls_.size());
            if (iconIdx >= 0) iconToBall_[static_cast<std::size_t>(iconIdx)] = bSlot;
            balls_.push_back(Ball{bi, tex, iconIdx, pts, BallGroup::SnookerColor, spot});
        }
        snookerTarget_ = 1;
    } else if (desktop && config_.rule == BilliardsRule::EightBall) {
        whiteSpawn_ = {left + tableSize_.x * 0.25f, midY};
        white_ = world_.addCircle(whiteSpawn_, radius_, whiteMat);

        // WPA / Heyball 15-ball rack: #8 at (col=2,row=1), one solid at (4,0), one stripe at (4,4).
        std::vector<int> solids = {1, 2, 3, 4, 5, 6, 7};
        std::vector<int> stripes = {9, 10, 11, 12, 13, 14, 15};
        std::mt19937 rng(config_.randomSeed ^ 0x8ba11u);
        std::shuffle(solids.begin(), solids.end(), rng);
        std::shuffle(stripes.begin(), stripes.end(), rng);

        const int cornerSolid = solids.back();
        solids.pop_back();
        const int cornerStripe = stripes.back();
        stripes.pop_back();
        std::vector<int> rest;
        rest.insert(rest.end(), solids.begin(), solids.end());
        rest.insert(rest.end(), stripes.begin(), stripes.end());
        std::shuffle(rest.begin(), rest.end(), rng);

        const float pitchX = 2.0f * radius_ * 0.8660254f * 1.018f;
        const float pitchY = 2.0f * radius_ * 1.018f;
        const float apexX = left + tableSize_.x * 0.75f;
        const Vec2 footSpot{apexX, midY};

        balls_.reserve(15);
        std::size_t restCursor = 0;
        std::size_t iconCursor = 0;
        for (int col = 0; col < 5; ++col) {
            for (int row = 0; row <= col; ++row) {
                int num = 1;
                if (col == 2 && row == 1) {
                    num = 8;
                } else if (col == 4 && row == 0) {
                    num = cornerSolid;
                } else if (col == 4 && row == 4) {
                    num = cornerStripe;
                } else {
                    num = rest[restCursor++];
                }
                const BallGroup grp =
                    num == 8 ? BallGroup::Eight : (num < 8 ? BallGroup::Solid : BallGroup::Stripe);
                const Vec2 p{apexX + static_cast<float>(col) * pitchX,
                             midY + (static_cast<float>(row) - static_cast<float>(col) * 0.5f) * pitchY};
                int iconIdx = -1;
                TextureId tex = kNoTexture;
                if (iconCursor < pool.size()) {
                    const std::size_t ii = pool[iconCursor++];
                    iconIdx = static_cast<int>(ii);
                    tex = setup.icons[ii].texture;
                }
                const BodyIndex bi = world_.addCircle(p, radius_, ballMat);
                const int bSlot = static_cast<int>(balls_.size());
                if (iconIdx >= 0) iconToBall_[static_cast<std::size_t>(iconIdx)] = bSlot;
                balls_.push_back(Ball{bi, tex, iconIdx, num, grp, num == 8 ? footSpot : p});
            }
        }
    } else {
        // Free mode (Desktop or Classic).
        whiteSpawn_ = {left + tableSize_.x * (desktop ? 0.25f : 0.22f), midY};
        white_ = world_.addCircle(whiteSpawn_, radius_, whiteMat);

        const int n = static_cast<int>(targetBalls);
        int columns = 1;
        while (columns * (columns + 1) / 2 < n) ++columns;

        const float pitchX = 2.0f * radius_ * (desktop ? 0.8660254f * 1.018f : 0.88f * 1.02f);
        const float pitchY = 2.0f * radius_ * (desktop ? 1.018f : 1.02f);
        float apexX = left + tableSize_.x * (desktop ? 0.75f : 0.62f);
        if (desktop && columns > 1) {
            const float maxApex = right - static_cast<float>(columns - 1) * pitchX - 2.2f * radius_;
            apexX = std::min(apexX, maxApex);
        }
        balls_.reserve(targetBalls);
        int placed = 0;
        for (int col = 0; col < columns && placed < n; ++col) {
            const int depth = std::min(col + 1, n - placed);
            for (int i = 0; i < depth; ++i) {
                const Vec2 p{apexX + static_cast<float>(col) * pitchX,
                             midY + (static_cast<float>(i) - static_cast<float>(depth - 1) * 0.5f) * pitchY};
                const BodyIndex bi = world_.addCircle(p, radius_, ballMat);
                int iconIdx = -1;
                TextureId tex = kNoTexture;
                if (static_cast<std::size_t>(placed) < pool.size()) {
                    const std::size_t ii = pool[static_cast<std::size_t>(placed)];
                    iconIdx = static_cast<int>(ii);
                    tex = setup.icons[ii].texture;
                }
                const int bSlot = static_cast<int>(balls_.size());
                if (iconIdx >= 0) iconToBall_[static_cast<std::size_t>(iconIdx)] = bSlot;
                const int ballNum = (placed % 15) + 1;
                balls_.push_back(Ball{bi, tex, iconIdx, ballNum, BallGroup::FreeBall, p});
                ++placed;
            }
        }
    }

    // 3. Handle unselected desktop icons according to ExtraIconsPolicy (Hide vs BottomGravity).
    if (desktop) {
        for (std::size_t i = 0; i < totalIcons; ++i) {
            if (iconToBall_[i] < 0) {
                iconToExtra_[i] = static_cast<int>(extraIcons_.size());
                extraIcons_.push_back(
                    ExtraIcon{i, setup.icons[i].texture, setup.icons[i].iconSize, setup.icons[i].center, 0});
            }
        }
        if (config_.extraIcons == ExtraIconsPolicy::BottomGravity && !extraIcons_.empty()) {
            extraWorld_ = std::make_unique<PhysicsWorld>(extraFloorPhysics());
            extraWorld_->createBoundary({0.0f, 0.0f}, ws, true, 1.0f, 0.45f, 0.25f);
            const BodyMaterial iconMat{1.0f, 0.35f, 0.30f, 0.12f, 0.25f, false};
            for (ExtraIcon& ex : extraIcons_) {
                const Vec2 halfPhys{std::max(0.05f, coords_.lengthToPhysics(ex.visualSize.x * 0.45f)),
                                    std::max(0.05f, coords_.lengthToPhysics(ex.visualSize.y * 0.45f))};
                const float cornerR = std::min(halfPhys.x, halfPhys.y) * 0.22f;
                ex.body = extraWorld_->addBox(coords_.toPhysics(ex.originalCenter), halfPhys, 0.0f, iconMat,
                                              cornerR);
            }
        }
    }

    if (balls_.empty()) state_ = BilliardsState::Finished;
    aimScene_ = coords_.toScene(whiteSpawn_ + Vec2{1.0f, 0.0f});
    placeScene_ = coords_.toScene(whiteSpawn_);
    aimDir_ = {1.0f, 0.0f};
    lockedAimDir_ = aimDir_;
    aiRngState_ = (config_.randomSeed ^ 0x9E3779B9u) | 1u;
}

void BilliardsMode::setAiDifficulty(BilliardsAiDifficulty diff) {
    if (config_.aiDifficulty == diff) return;
    config_.aiDifficulty = diff;
    if (isAiTurn() && state_ == BilliardsState::Ready && aiPhase_ == AiPhase::Aiming) {
        aiPhase_ = AiPhase::Idle;
        aiPhaseTime_ = 0.0f;
    }
}

bool BilliardsMode::isAiTurn() const {
    return config_.style == BilliardsStyle::Desktop && config_.matchMode == BilliardsMatchMode::VsAi &&
           lanLocalPlayer_ < 0 && currentPlayer_ == 1 && state_ != BilliardsState::Finished;
}

const char* BilliardsMode::playerLabel(int player) const {
    if (config_.matchMode == BilliardsMatchMode::VsAi) {
        return player == 1 ? "人机" : "玩家";
    }
    return player == 1 ? "P2" : "P1";
}

Vec2 BilliardsMode::toTableNorm(Vec2 physPos) const {
    return {(physPos.x - tableMin_.x) / std::max(1e-4f, tableSize_.x),
            (physPos.y - tableMin_.y) / std::max(1e-4f, tableSize_.y)};
}

Vec2 BilliardsMode::fromTableNorm(Vec2 normPos) const {
    return {tableMin_.x + normPos.x * tableSize_.x, tableMin_.y + normPos.y * tableSize_.y};
}

float BilliardsMode::pocketCatchRadius(std::size_t pocketIndex) const {
    return (pocketIndex == 1 || pocketIndex == 4) ? sidePocketRadius_ : pocketRadius_;
}

void BilliardsMode::setTipOffset(Vec2 offset) {
    if (!offset.isFinite()) return;
    const float len = offset.length();
    if (len > 1.0f) {
        tipOffset_ = offset / len;
    } else if (len < 0.08f) {
        tipOffset_ = {0.0f, 0.0f};
    } else {
        tipOffset_ = offset;
    }
}

BilliardsMode::SpinWidgetGeometry BilliardsMode::spinWidgetGeometry() const {
    SpinWidgetGeometry g;
    const Vec2 tl = coords_.toScene({tableMin_.x, tableMin_.y + tableSize_.y});
    const float railOuter = railOuterWidthScene();
    const float outerTop = tl.y - railOuter;
    g.radius = std::clamp((outerTop - 10.0f) * 0.42f, 14.0f, 25.0f);
    g.center = {
        std::clamp(tl.x + g.radius + 8.0f, g.radius + 8.0f, sceneSize_.x * 0.35f),
        std::clamp(outerTop - g.radius - 6.0f, g.radius + 4.0f,
                   std::max(g.radius + 4.0f, outerTop - g.radius - 2.0f)),
    };
    g.resetBox = {g.center.x + g.radius + 6.0f, g.center.y - 14.0f, 185.0f, 28.0f};
    return g;
}

Vec2 BilliardsMode::spinWidgetCenterScene() const { return spinWidgetGeometry().center; }

float BilliardsMode::spinWidgetRadiusScene() const { return spinWidgetGeometry().radius; }

bool BilliardsMode::spinWidgetContains(Vec2 scenePos) const {
    if (config_.style != BilliardsStyle::Desktop || isAiTurn() || !scenePos.isFinite()) return false;
    const SpinWidgetGeometry g = spinWidgetGeometry();
    const float hitR = g.radius + 4.0f;
    return (scenePos - g.center).lengthSquared() <= hitR * hitR;
}

bool BilliardsMode::spinResetContains(Vec2 scenePos) const {
    if (config_.style != BilliardsStyle::Desktop || isAiTurn() || !scenePos.isFinite()) return false;
    if (tipOffset_.lengthSquared() <= 1e-4f) return false;
    const Rect b = spinWidgetGeometry().resetBox;
    return scenePos.x >= b.x && scenePos.x <= b.x + b.width && scenePos.y >= b.y &&
           scenePos.y <= b.y + b.height;
}

Rect BilliardsMode::aimToggleRect() const {
    const Vec2 br = coords_.toScene({tableMin_.x + tableSize_.x, tableMin_.y});
    const SpinWidgetGeometry sw = spinWidgetGeometry();
    const float w = 98.0f, h = 26.0f;
    const float x = std::clamp(br.x - w - 4.0f, sceneSize_.x * 0.65f,
                               std::max(sceneSize_.x * 0.65f, sceneSize_.x - w - 8.0f));
    const float y = std::max(4.0f, sw.center.y - h * 0.5f);
    return {x, y, w, h};
}

Vec2 BilliardsMode::aimToggleCenterScene() const {
    const Rect b = aimToggleRect();
    return {b.x + b.width * 0.5f, b.y + b.height * 0.5f};
}

bool BilliardsMode::aimToggleContains(Vec2 scenePos) const {
    if (config_.style != BilliardsStyle::Desktop || state_ == BilliardsState::Finished ||
        !scenePos.isFinite())
        return false;
    const Rect b = aimToggleRect();
    return scenePos.x >= b.x && scenePos.x <= b.x + b.width && scenePos.y >= b.y &&
           scenePos.y <= b.y + b.height;
}

Rect BilliardsMode::aiIntentToggleRect() const {
    const Rect ab = aimToggleRect();
    const float w = 98.0f;
    return {std::max(8.0f, ab.x - w - 8.0f), ab.y, w, ab.height};
}

Vec2 BilliardsMode::aiIntentToggleCenterScene() const {
    const Rect b = aiIntentToggleRect();
    return {b.x + b.width * 0.5f, b.y + b.height * 0.5f};
}

bool BilliardsMode::aiIntentToggleContains(Vec2 scenePos) const {
    if (config_.style != BilliardsStyle::Desktop || config_.matchMode != BilliardsMatchMode::VsAi ||
        state_ == BilliardsState::Finished || !scenePos.isFinite()) {
        return false;
    }
    const Rect b = aiIntentToggleRect();
    return scenePos.x >= b.x && scenePos.x <= b.x + b.width && scenePos.y >= b.y &&
           scenePos.y <= b.y + b.height;
}

Rect BilliardsMode::rematchButtonRect() const {
    const Vec2 br = coords_.toScene({tableMin_.x + tableSize_.x, tableMin_.y});
    const SpinWidgetGeometry sw = spinWidgetGeometry();
    const float w = 108.0f, h = 26.0f;
    const float x = std::clamp(br.x - w - 4.0f, sceneSize_.x * 0.65f,
                               std::max(sceneSize_.x * 0.65f, sceneSize_.x - w - 8.0f));
    const float y = std::max(4.0f, sw.center.y - h * 0.5f);
    return {x, y, w, h};
}

Vec2 BilliardsMode::rematchButtonCenterScene() const {
    const Rect b = rematchButtonRect();
    return {b.x + b.width * 0.5f, b.y + b.height * 0.5f};
}

bool BilliardsMode::rematchButtonContains(Vec2 scenePos) const {
    if (config_.style != BilliardsStyle::Desktop || state_ != BilliardsState::Finished ||
        lanLocalPlayer_ == 1 || !scenePos.isFinite()) {
        return false;
    }
    const Rect b = rematchButtonRect();
    return scenePos.x >= b.x && scenePos.x <= b.x + b.width && scenePos.y >= b.y &&
           scenePos.y <= b.y + b.height;
}

void BilliardsMode::recordSeriesFrameWinIfNeeded() {
    if (seriesCountedLocal_) return;
    if (config_.matchMode != BilliardsMatchMode::Solo && (winner_ == 0 || winner_ == 1)) {
        ++seriesWins_[static_cast<std::size_t>(winner_)];
    }
    seriesCountedLocal_ = true;
}

void BilliardsMode::updateSpinFromPointer(Vec2 scenePos) {
    if (!scenePos.isFinite()) return;
    const SpinWidgetGeometry g = spinWidgetGeometry();
    const float maxTip = std::max(6.0f, g.radius * 0.78f);
    const Vec2 raw{(scenePos.x - g.center.x) / maxTip, (g.center.y - scenePos.y) / maxTip};
    setTipOffset(raw);
}

bool BilliardsMode::interactiveContains(Vec2 scenePos) const {
    return tableContains(scenePos) || spinWidgetContains(scenePos) || spinResetContains(scenePos) ||
           aimToggleContains(scenePos) || aiIntentToggleContains(scenePos) || rematchButtonContains(scenePos);
}

bool BilliardsMode::tableContains(Vec2 scenePos) const {
    if (!scenePos.isFinite()) return false;
    const Vec2 tl = coords_.toScene({tableMin_.x, tableMin_.y + tableSize_.y});
    const Vec2 br = coords_.toScene({tableMin_.x + tableSize_.x, tableMin_.y});
    const float railBand = config_.style == BilliardsStyle::Desktop ? (railOuterWidthScene() + 4.0f) : 14.0f;
    float minX = tl.x - railBand;
    float maxX = br.x + railBand;
    if (config_.style == BilliardsStyle::Desktop &&
        config_.effectiveTableTheme() == BilliardsTableTheme::Full) {
        const TubeGeometry leftTube = tubeGeometry(0);
        const TubeGeometry rightTube = tubeGeometry(1);
        minX = std::max(minX, leftTube.cx + leftTube.capR + 2.0f);
        maxX = std::min(maxX, rightTube.cx - rightTube.capR - 2.0f);
    }
    return scenePos.x >= minX && scenePos.x <= maxX && scenePos.y >= tl.y - railBand &&
           scenePos.y <= br.y + railBand;
}

bool BilliardsMode::canPlaceWhite(Vec2 scenePos) const {
    if (!scenePos.isFinite()) return false;
    const Vec2 p = coords_.toPhysics(scenePos);
    const float wallClear = radius_ * 1.04f;
    if (p.x < tableMin_.x + wallClear || p.x > tableMin_.x + tableSize_.x - wallClear ||
        p.y < tableMin_.y + wallClear || p.y > tableMin_.y + tableSize_.y - wallClear) {
        return false;
    }
    for (std::size_t i = 0; i < pockets_.size(); ++i) {
        const float pocketClear = pocketCatchRadius(i) + radius_ * 0.35f;
        if ((p - pockets_[i]).lengthSquared() < pocketClear * pocketClear) return false;
    }
    const float ballClear = 2.0f * radius_ + 0.006f;
    const float ballClear2 = ballClear * ballClear;
    for (const Ball& b : balls_) {
        if (!b.pocketed && (world_.state(b.body).position - p).lengthSquared() < ballClear2) return false;
    }
    return !world_.overlapsCircle(p, radius_ * 1.01f, white_);
}

std::optional<BilliardsRemoteAction> BilliardsMode::takeLocalLanAction() {
    auto out = pendingLanAction_;
    pendingLanAction_.reset();
    return out;
}

void BilliardsMode::applyRemoteLanAction(const BilliardsRemoteAction& action) {
    if (lanLocalPlayer_ == 0 && currentPlayer_ != 1) return;
    switch (action.kind) {
        case BilliardsRemoteAction::Kind::Aim: {
            if (state_ == BilliardsState::PlacingWhite) {
                placeScene_ = coords_.toScene(fromTableNorm(action.placeNormPos));
                aimScene_ = placeScene_;
            } else if (state_ == BilliardsState::Ready || state_ == BilliardsState::Charging) {
                aimDir_ = normalizedOr(action.aimDir, aimDir_);
                lockedAimDir_ = aimDir_;
                setTipOffset(action.tipOffset);
                if (action.pullRatio > 0.002f) {
                    state_ = BilliardsState::Charging;
                    pullDistancePt_ = std::clamp(action.pullRatio, 0.0f, 1.0f) * config_.maxPullPoints;
                } else {
                    pullDistancePt_ = 0.0f;
                    state_ = BilliardsState::Ready;
                }
            }
            break;
        }
        case BilliardsRemoteAction::Kind::PlaceWhite: {
            if (state_ == BilliardsState::PlacingWhite) {
                const Vec2 spot = coords_.toScene(fromTableNorm(action.placeNormPos));
                if (canPlaceWhite(spot)) {
                    placeScene_ = spot;
                    world_.setTransform(white_, coords_.toPhysics(spot), 0.0f);
                    world_.setVelocity(white_, {0.0f, 0.0f}, 0.0f);
                    world_.setEnabled(white_, true);
                    world_.wake(white_);
                    whitePocketed_ = false;
                    ++whitePlacements_;
                    state_ = BilliardsState::Ready;
                }
            }
            break;
        }
        case BilliardsRemoteAction::Kind::Shoot: {
            if (state_ == BilliardsState::Ready || state_ == BilliardsState::Charging) {
                aimDir_ = normalizedOr(action.aimDir, aimDir_);
                lockedAimDir_ = aimDir_;
                setTipOffset(action.tipOffset);
                pullDistancePt_ = 0.0f;
                shootDirection(lockedAimDir_, std::clamp(action.power, 0.05f, 1.0f));
            }
            break;
        }
    }
}

BilliardsLanSnapshot BilliardsMode::exportLanSnapshot() const {
    BilliardsLanSnapshot s;
    s.state = state_;
    s.rule = config_.rule;
    s.currentPlayer = currentPlayer_;
    s.winner = winner_;
    s.scores = scores_;
    s.playerGroups = playerGroups_;
    s.snookerTarget = snookerTarget_;
    s.shots = shots_;
    s.pocketedCount = pocketedCount_;
    s.whitePocketed = whitePocketed_;
    s.whiteNormPos = toTableNorm(world_.state(white_).position);
    s.placeNormPos = toTableNorm(coords_.toPhysics(placeScene_));
    s.aimDir = state_ == BilliardsState::Charging ? lockedAimDir_ : aimDir_;
    s.tipOffset = tipOffset_;
    s.pullRatio = state_ == BilliardsState::Charging
                      ? std::clamp(pullDistancePt_ / std::max(1.0f, config_.maxPullPoints), 0.0f, 1.0f)
                      : 0.0f;
    s.statusNote = turnNote_;
    s.balls.reserve(balls_.size());
    for (const Ball& b : balls_) {
        BilliardsLanBall lb;
        lb.normPos = toTableNorm(world_.state(b.body).position);
        lb.number = b.number;
        lb.group = b.group;
        lb.pocketed = b.pocketed;
        lb.tube = b.tube;
        lb.tubeSlot = b.tubeSlot;
        lb.dropProgress = b.dropProgress;
        s.balls.push_back(lb);
    }
    return s;
}

void BilliardsMode::applyLanSnapshot(const BilliardsLanSnapshot& snap) {
    const bool wasGuestTurn = (lanLocalPlayer_ == 1 && currentPlayer_ == 1);
    const bool sameEditablePhase =
        (snap.state == BilliardsState::PlacingWhite && state_ == BilliardsState::PlacingWhite) ||
        ((snap.state == BilliardsState::Ready || snap.state == BilliardsState::Charging) &&
         (state_ == BilliardsState::Ready || state_ == BilliardsState::Charging));
    const bool localGuestEditing = (wasGuestTurn && snap.currentPlayer == 1 && sameEditablePhase);

    currentPlayer_ = snap.currentPlayer;
    winner_ = snap.winner;
    scores_ = snap.scores;
    playerGroups_ = snap.playerGroups;
    snookerTarget_ = snap.snookerTarget;
    shots_ = snap.shots;
    pocketedCount_ = snap.pocketedCount;
    whitePocketed_ = snap.whitePocketed;
    turnNote_ = snap.statusNote;

    const Vec2 whitePhys = fromTableNorm(snap.whiteNormPos);
    world_.setTransform(white_, whitePhys, 0.0f);
    world_.setVelocity(white_, {0.0f, 0.0f}, 0.0f);
    world_.setEnabled(white_, !snap.whitePocketed);

    if (!localGuestEditing) {
        state_ = snap.state;
        aimDir_ = normalizedOr(snap.aimDir, aimDir_);
        lockedAimDir_ = aimDir_;
        tipOffset_ = snap.tipOffset;
        pullDistancePt_ = std::clamp(snap.pullRatio, 0.0f, 1.0f) * config_.maxPullPoints;
        placeScene_ = coords_.toScene(fromTableNorm(snap.placeNormPos));
    } else if (state_ == BilliardsState::Rolling && snap.state != BilliardsState::Rolling) {
        state_ = snap.state;
        pullDistancePt_ = 0.0f;
        if (state_ == BilliardsState::PlacingWhite) {
            placeScene_ = coords_.toScene(fromTableNorm(snap.placeNormPos));
        }
    }

    const BodyMaterial ballMat{1.0f, 0.05f, 0.92f, 0.95f, 1.0f, true};
    while (balls_.size() < snap.balls.size()) {
        const auto idx = balls_.size();
        const Vec2 p = fromTableNorm(snap.balls[idx].normPos);
        const BodyIndex bi = world_.addCircle(p, radius_, ballMat);
        balls_.push_back(Ball{bi, kNoTexture, -1, snap.balls[idx].number, snap.balls[idx].group, p});
    }
    for (std::size_t i = snap.balls.size(); i < balls_.size(); ++i) {
        world_.setEnabled(balls_[i].body, false);
        balls_[i].pocketed = true;
    }

    tubeOrder_[0].clear();
    tubeOrder_[1].clear();
    std::array<std::vector<std::pair<int, std::size_t>>, 2> sortedTubes{};
    for (std::size_t i = 0; i < snap.balls.size(); ++i) {
        const BilliardsLanBall& sb = snap.balls[i];
        Ball& b = balls_[i];
        b.number = sb.number;
        b.group = sb.group;
        b.pocketed = sb.pocketed;
        b.tube = (sb.tube == 1) ? 1 : 0;
        b.tubeSlot = sb.tubeSlot;
        b.dropProgress = sb.dropProgress;
        world_.setTransform(b.body, fromTableNorm(sb.normPos), 0.0f);
        world_.setVelocity(b.body, {0.0f, 0.0f}, 0.0f);
        world_.setEnabled(b.body, !sb.pocketed);
        if (sb.pocketed) {
            sortedTubes[static_cast<std::size_t>(b.tube)].push_back({b.tubeSlot, i});
        }
    }
    for (std::size_t side = 0; side < 2; ++side) {
        auto& vec = sortedTubes[side];
        std::sort(vec.begin(), vec.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& kv : vec) tubeOrder_[side].push_back(kv.second);
    }
}

void BilliardsMode::handleInput(const InputEvent& e) {
    if (e.type == InputType::PointerCancel) {
        adjustingSpin_ = false;
        if (!isAiTurn() && state_ == BilliardsState::Charging) {
            pullDistancePt_ = 0.0f;
            state_ = BilliardsState::Ready;
            if (lanLocalPlayer_ == 1 && currentPlayer_ == 1) {
                BilliardsRemoteAction act;
                act.kind = BilliardsRemoteAction::Kind::Aim;
                act.aimDir = aimDir_;
                act.tipOffset = tipOffset_;
                act.pullRatio = 0.0f;
                pendingLanAction_ = act;
            }
        }
        return;
    }
    if (!e.scenePos.isFinite() || !std::isfinite(e.time)) return;

    if (config_.style == BilliardsStyle::Classic) {
        switch (e.type) {
            case InputType::PointerMove:
                aimScene_ = e.scenePos;
                aimDir_ = normalizedOr(aimScene_ - whiteSceneCenter(), aimDir_);
                break;
            case InputType::PointerDown:
                aimScene_ = e.scenePos;
                aimDir_ = normalizedOr(aimScene_ - whiteSceneCenter(), aimDir_);
                if (state_ == BilliardsState::Ready) {
                    state_ = BilliardsState::Charging;
                    chargeStart_ = e.time;
                    chargeElapsed_ = 0.0;
                }
                break;
            case InputType::PointerUp:
                if (state_ == BilliardsState::Charging) {
                    const double held = std::max(0.0, e.time - chargeStart_);
                    const float charge = std::clamp(static_cast<float>(held / config_.fullChargeSeconds),
                                                    config_.minCharge, 1.0f);
                    shootDirection(normalizedOr(aimScene_ - whiteSceneCenter(), Vec2{1.0f, 0.0f}), charge);
                }
                break;
            default:
                break;
        }
        return;
    }

    if (state_ == BilliardsState::Finished) {
        if (e.type == InputType::PointerDown && rematchButtonContains(e.scenePos)) {
            rematchRequested_ = true;
        }
        return;
    }

    if (isAiTurn()) {
        if (e.type == InputType::PointerDown) {
            if ((state_ == BilliardsState::Ready || state_ == BilliardsState::Charging) &&
                aimToggleContains(e.scenePos)) {
                config_.showAimGuide = !config_.showAimGuide;
            } else if (aiIntentToggleContains(e.scenePos)) {
                config_.showAiIntent = !config_.showAiIntent;
            }
        }
        return;
    }

    if (lanLocalPlayer_ >= 0 && currentPlayer_ != lanLocalPlayer_) {
        return;
    }

    const bool isGuestTurn = (lanLocalPlayer_ == 1 && currentPlayer_ == 1);
    auto queueGuestAim = [&]() {
        if (!isGuestTurn) return;
        BilliardsRemoteAction act;
        act.kind = BilliardsRemoteAction::Kind::Aim;
        act.aimDir = state_ == BilliardsState::Charging ? lockedAimDir_ : aimDir_;
        act.tipOffset = tipOffset_;
        act.placeNormPos = toTableNorm(coords_.toPhysics(placeScene_));
        act.pullRatio = state_ == BilliardsState::Charging
                            ? std::clamp(pullDistancePt_ / std::max(1.0f, config_.maxPullPoints), 0.0f, 1.0f)
                            : 0.0f;
        pendingLanAction_ = act;
    };

    switch (e.type) {
        case InputType::PointerMove:
            if (adjustingSpin_) {
                updateSpinFromPointer(e.scenePos);
                queueGuestAim();
            } else if (state_ == BilliardsState::PlacingWhite) {
                placeScene_ = e.scenePos;
                aimScene_ = e.scenePos;
                queueGuestAim();
            } else if (state_ == BilliardsState::Ready) {
                aimScene_ = e.scenePos;
                aimDir_ = normalizedOr(aimScene_ - whiteSceneCenter(), aimDir_);
                queueGuestAim();
            } else if (state_ == BilliardsState::Charging) {
                const Vec2 drag = e.scenePos - dragStartScene_;
                const float proj = -(drag.x * lockedAimDir_.x + drag.y * lockedAimDir_.y);
                pullDistancePt_ = std::clamp(proj, 0.0f, config_.maxPullPoints);
                queueGuestAim();
            }
            break;
        case InputType::PointerDown:
            if (aiIntentToggleContains(e.scenePos)) {
                config_.showAiIntent = !config_.showAiIntent;
                adjustingSpin_ = false;
            } else if (state_ == BilliardsState::PlacingWhite) {
                placeScene_ = e.scenePos;
                if (canPlaceWhite(placeScene_)) {
                    const Vec2 physSpot = coords_.toPhysics(placeScene_);
                    world_.setTransform(white_, physSpot, 0.0f);
                    world_.setVelocity(white_, {0.0f, 0.0f}, 0.0f);
                    world_.setEnabled(white_, true);
                    world_.wake(white_);
                    whitePocketed_ = false;
                    ++whitePlacements_;
                    state_ = BilliardsState::Ready;
                    if (isGuestTurn) {
                        BilliardsRemoteAction act;
                        act.kind = BilliardsRemoteAction::Kind::PlaceWhite;
                        act.placeNormPos = toTableNorm(physSpot);
                        pendingLanAction_ = act;
                    }
                }
            } else if (state_ == BilliardsState::Ready && aimToggleContains(e.scenePos)) {
                config_.showAimGuide = !config_.showAimGuide;
                adjustingSpin_ = false;
            } else if (state_ == BilliardsState::Ready && spinResetContains(e.scenePos)) {
                tipOffset_ = {0.0f, 0.0f};
                adjustingSpin_ = false;
                queueGuestAim();
            } else if (state_ == BilliardsState::Ready && spinWidgetContains(e.scenePos)) {
                if (e.alternate) {
                    tipOffset_ = {0.0f, 0.0f};
                    adjustingSpin_ = false;
                } else {
                    adjustingSpin_ = true;
                    updateSpinFromPointer(e.scenePos);
                }
                queueGuestAim();
            } else if (state_ == BilliardsState::Ready && tableContains(e.scenePos)) {
                const Vec2 w = whiteSceneCenter();
                if ((e.scenePos - w).lengthSquared() > 4.0f) {
                    aimScene_ = e.scenePos;
                    aimDir_ = normalizedOr(aimScene_ - w, aimDir_);
                }
                lockedAimDir_ = aimDir_;
                dragStartScene_ = e.scenePos;
                pullDistancePt_ = 0.0f;
                state_ = BilliardsState::Charging;
                queueGuestAim();
            }
            break;
        case InputType::PointerUp:
            if (adjustingSpin_) {
                updateSpinFromPointer(e.scenePos);
                adjustingSpin_ = false;
                queueGuestAim();
            } else if (state_ == BilliardsState::Charging) {
                const Vec2 drag = e.scenePos - dragStartScene_;
                const float proj = -(drag.x * lockedAimDir_.x + drag.y * lockedAimDir_.y);
                pullDistancePt_ = std::clamp(proj, 0.0f, config_.maxPullPoints);
                if (pullDistancePt_ < config_.cancelPullPoints) {
                    pullDistancePt_ = 0.0f;
                    state_ = BilliardsState::Ready;
                    queueGuestAim();
                } else {
                    const float power = std::clamp(pullDistancePt_ / config_.maxPullPoints, 0.0f, 1.0f);
                    pullDistancePt_ = 0.0f;
                    if (isGuestTurn) {
                        BilliardsRemoteAction act;
                        act.kind = BilliardsRemoteAction::Kind::Shoot;
                        act.aimDir = lockedAimDir_;
                        act.power = power;
                        act.tipOffset = tipOffset_;
                        pendingLanAction_ = act;
                        tipOffset_ = {0.0f, 0.0f};
                        state_ = BilliardsState::Rolling;
                    } else {
                        shootDirection(lockedAimDir_, power);
                    }
                }
            }
            break;
        default:
            break;
    }
}

float BilliardsMode::scenePan(float sceneX) const {
    if (!(sceneSize_.x > 1.0f) || !std::isfinite(sceneX)) return 0.0f;
    return std::clamp((sceneX / sceneSize_.x) * 2.0f - 1.0f, -0.90f, 0.90f);
}

void BilliardsMode::pushSound(SoundKind kind, float gain, float sceneX, float pitch) {
    const SoundEvent ev{
        kind,
        std::clamp(gain, 0.05f, 1.0f),
        scenePan(sceneX),
        std::clamp(pitch, 0.90f, 1.10f),
    };
    constexpr std::size_t kMaxPendingSounds = 24;
    if (pendingSounds_.size() < kMaxPendingSounds) {
        pendingSounds_.push_back(ev);
        return;
    }
    auto quietest =
        std::min_element(pendingSounds_.begin(), pendingSounds_.end(),
                         [](const SoundEvent& a, const SoundEvent& b) { return a.gain < b.gain; });
    if (quietest != pendingSounds_.end() && ev.gain > quietest->gain) *quietest = ev;
}

void BilliardsMode::drainSounds(std::vector<SoundEvent>& out) {
    for (const SoundEvent& ev : pendingSounds_) out.push_back(ev);
    pendingSounds_.clear();
}

void BilliardsMode::shootDirection(Vec2 dirScene, float power) {
    const Vec2 unitScene = normalizedOr(dirScene, Vec2{1.0f, 0.0f});
    const Vec2 dirPhys = coords_.velocityToPhysics(unitScene);
    float topSpeed = config_.maxShotSpeed;
    if (config_.style == BilliardsStyle::Desktop) {
        topSpeed = std::clamp(tableSize_.x * 2.0f, 16.0f, 36.0f);
    }
    const Vec2 dirUnit = normalizedOr(dirPhys, Vec2{1.0f, 0.0f});
    const Vec2 v = dirUnit * (topSpeed * power);
    world_.setVelocity(white_, v, 0.0f);
    world_.wake(white_);
    cueHitBall_ = false;
    firstHitBall_ = -1;
    whitePottedThisShot_ = false;
    pottedThisShot_.clear();
    turnNote_[0] = '\0';

    if (config_.style == BilliardsStyle::Desktop) {
        const float speed = v.length();
        const float rollFactor = tipOffset_.y >= 0.0f ? (tipOffset_.y * 1.35f) : (tipOffset_.y * 1.45f);
        cueRollVel_ = dirUnit * (speed * rollFactor);
        cueSideSpin_ = speed * (tipOffset_.x * 0.90f);
        tipOffset_ = {0.0f, 0.0f};
    } else {
        cueRollVel_ = {0.0f, 0.0f};
        cueSideSpin_ = 0.0f;
    }
    state_ = BilliardsState::Rolling;
    rollingTime_ = 0.0;
    ++shots_;
    const float strikeGain =
        std::clamp(0.18f + 0.82f * std::sqrt(std::clamp(power, 0.0f, 1.0f)), 0.12f, 1.0f);
    pushSound(SoundKind::CueStrike, strikeGain, whiteSceneCenter().x, 1.03f - 0.06f * power);
}

void BilliardsMode::stepPhysics(float dt) {
    if (extraWorld_ && !extraWorld_->allAsleep()) extraWorld_->step(dt);
    if (lanLocalPlayer_ == 1) return;

    world_.step(dt);
    if (!std::isfinite(dt) || dt <= 0.0f || dt > 0.25f) return;
    simTime_ += static_cast<double>(dt);

    pairCooldowns_.erase(std::remove_if(pairCooldowns_.begin(), pairCooldowns_.end(),
                                        [this](const PairCooldown& c) { return c.until <= simTime_; }),
                         pairCooldowns_.end());
    const float refSpeed = config_.style == BilliardsStyle::Desktop
                               ? std::clamp(tableSize_.x * 1.45f, 12.0f, 28.0f)
                               : std::max(8.0f, config_.maxShotSpeed * 0.85f);
    for (const CollisionHit& h : world_.lastHits()) {
        std::uint32_t idA = h.bodyA ? (*h.bodyA + 1u) : 0u;
        std::uint32_t idB = h.bodyB ? (*h.bodyB + 1u) : 0u;
        if (idA == 0u && idB == 0u) continue;
        if (idA > idB) std::swap(idA, idB);

        if (idA == 1u && idB >= 2u) {
            cueHitBall_ = true;
            const std::size_t ballIdx = static_cast<std::size_t>(idB - 2u);
            if (firstHitBall_ < 0 && ballIdx < balls_.size()) {
                firstHitBall_ = static_cast<int>(ballIdx);
            }
        }

        const std::uint32_t pairKey = (idA << 16) | (idB & 0xffffu);
        bool cooling = false;
        for (const PairCooldown& c : pairCooldowns_) {
            if (c.key == pairKey) {
                cooling = true;
                break;
            }
        }
        if (cooling) continue;
        if (pairCooldowns_.size() < 128) pairCooldowns_.push_back(PairCooldown{pairKey, simTime_ + 0.028});

        const float norm = std::clamp(h.approachSpeed / refSpeed, 0.0f, 1.0f);
        const float gain = std::clamp(std::pow(norm, 0.62f), 0.06f, 1.0f);
        const float sx = coords_.toScene(h.point).x;
        if (idA == 0u) {
            if (!inPocket(h.point)) {
                pushSound(SoundKind::RailBounce, gain * 0.88f, sx, 0.94f + 0.10f * norm);
                if (config_.style == BilliardsStyle::Desktop && idB == 1u && !whitePocketed_ &&
                    std::abs(cueSideSpin_) > 0.05f) {
                    const float dLeft = std::abs(h.point.x - tableMin_.x);
                    const float dRight = std::abs(h.point.x - (tableMin_.x + tableSize_.x));
                    const float dBottom = std::abs(h.point.y - tableMin_.y);
                    const float dTop = std::abs(h.point.y - (tableMin_.y + tableSize_.y));
                    Vec2 nIn{0.0f, -1.0f};
                    float bestD = dTop;
                    if (dBottom < bestD) {
                        bestD = dBottom;
                        nIn = {0.0f, 1.0f};
                    }
                    if (dLeft < bestD) {
                        bestD = dLeft;
                        nIn = {1.0f, 0.0f};
                    }
                    if (dRight < bestD) {
                        nIn = {-1.0f, 0.0f};
                    }
                    const Vec2 tRight{-nIn.y, nIn.x};
                    const BodyState ws = world_.state(white_);
                    const float curSpd = ws.linearVelocity.length();
                    const float kick = std::clamp(cueSideSpin_ * 0.48f, -15.0f, 15.0f);
                    Vec2 nextV = ws.linearVelocity + tRight * kick;
                    const float capSpd = std::min(36.0f, std::max(curSpd * 1.18f, 4.0f));
                    if (nextV.length() > capSpd) nextV = normalizedOr(nextV, tRight) * capSpd;
                    world_.setVelocity(white_, nextV, ws.angularVelocity);
                    cueSideSpin_ *= 0.45f;
                    cueRollVel_ = normalizedOr(nextV, {1.0f, 0.0f}) * (cueRollVel_.length() * 0.65f);
                }
            }
        } else {
            const float det = static_cast<float>((idA * 7u + idB * 13u) % 9u) * 0.01f - 0.04f;
            pushSound(SoundKind::BallCollide, gain, sx, 1.0f + det + 0.03f * norm);
        }
    }

    if (config_.style == BilliardsStyle::Desktop) {
        if (!whitePocketed_ && state_ == BilliardsState::Rolling) {
            const BodyState ws = world_.state(white_);
            if (ws.enabled) {
                if (!cueHitBall_) {
                    const float decay = std::exp(-0.50f * dt);
                    cueRollVel_ = cueRollVel_ * decay + ws.linearVelocity * (0.14f * dt);
                } else if ((cueRollVel_ - ws.linearVelocity).lengthSquared() > 1e-4f) {
                    const Vec2 slip = cueRollVel_ - ws.linearVelocity;
                    const float alpha = std::min(1.0f, 8.0f * dt);
                    Vec2 nextV = ws.linearVelocity + slip * (0.45f * alpha);
                    if (nextV.length() > 36.0f) nextV = normalizedOr(nextV, {1.0f, 0.0f}) * 36.0f;
                    cueRollVel_ = cueRollVel_ - slip * (0.55f * alpha);
                    world_.setVelocity(white_, nextV, ws.angularVelocity);
                    world_.wake(white_);
                }
                cueSideSpin_ *= std::exp(-0.35f * dt);
            }
        }
        const float drag = std::clamp(tableSize_.x * 0.025f, 0.20f, 0.45f);
        auto settleBody = [&](BodyIndex idx) {
            const BodyState st = world_.state(idx);
            if (!st.enabled || !st.awake) return;
            const float spd = st.linearVelocity.length();
            if (spd > 1e-4f && spd < 1.8f) {
                const float nextSpd = std::max(0.0f, spd - drag * dt);
                world_.setVelocity(idx, st.linearVelocity * (nextSpd / spd), st.angularVelocity);
            }
        };
        if (!whitePocketed_) settleBody(white_);
        for (const Ball& b : balls_) {
            if (!b.pocketed) settleBody(b.body);
        }
    }
}

int BilliardsMode::matchingPocket(Vec2 p) const {
    for (std::size_t i = 0; i < pockets_.size(); ++i) {
        const float pr = pocketCatchRadius(i);
        if ((p - pockets_[i]).lengthSquared() < pr * pr) return static_cast<int>(i);
    }
    return -1;
}

bool BilliardsMode::inPocket(Vec2 p) const { return matchingPocket(p) >= 0; }

bool BilliardsMode::everythingStopped() const {
    const float s2 = config_.restSpeed * config_.restSpeed;
    if (!whitePocketed_) {
        if (world_.state(white_).linearVelocity.lengthSquared() > s2) return false;
        if (cueHitBall_ && cueRollVel_.lengthSquared() > 0.12f) return false;
    }
    for (const Ball& b : balls_)
        if (!b.pocketed && world_.state(b.body).linearVelocity.lengthSquared() > s2) return false;
    return true;
}

bool BilliardsMode::respawnWhite() {
    const float clearance = radius_ * 2.05f;
    Vec2 spot = whiteSpawn_;
    bool found = !world_.overlapsCircle(spot, clearance, white_) && !inPocket(spot);
    const float step = radius_ * 2.2f;
    for (int ring = 1; !found && ring <= 14; ++ring) {
        for (int dy = -ring; !found && dy <= ring; ++dy) {
            for (int dx = -ring; !found && dx <= ring; ++dx) {
                const Vec2 c =
                    whiteSpawn_ + Vec2{static_cast<float>(dx) * step, static_cast<float>(dy) * step};
                if (c.x < tableMin_.x + radius_ * 1.5f || c.x > tableMin_.x + tableSize_.x - radius_ * 1.5f ||
                    c.y < tableMin_.y + radius_ * 1.5f || c.y > tableMin_.y + tableSize_.y - radius_ * 1.5f)
                    continue;
                if (!world_.overlapsCircle(c, clearance, white_) && !inPocket(c)) {
                    spot = c;
                    found = true;
                }
            }
        }
    }
    if (!found) return false;
    world_.setTransform(white_, spot, 0.0f);
    world_.setVelocity(white_, {0, 0}, 0.0f);
    world_.setEnabled(white_, true);
    world_.wake(white_);
    whitePocketed_ = false;
    return true;
}

int BilliardsMode::remainingReds() const {
    int count = 0;
    for (const Ball& b : balls_) {
        if (!b.pocketed && world_.state(b.body).enabled && b.group == BallGroup::SnookerRed) ++count;
    }
    return count;
}

int BilliardsMode::lowestRemainingColorValue() const {
    int best = 0;
    for (const Ball& b : balls_) {
        if (!b.pocketed && world_.state(b.body).enabled && b.group == BallGroup::SnookerColor) {
            if (best == 0 || b.number < best) best = b.number;
        }
    }
    return best;
}

int BilliardsMode::remainingGroupBalls(BallGroup g) const {
    if (g != BallGroup::Solid && g != BallGroup::Stripe) return 0;
    int count = 0;
    for (const Ball& b : balls_) {
        if (!b.pocketed && world_.state(b.body).enabled && b.group == g) ++count;
    }
    return count;
}

void BilliardsMode::respotSnookerColor(std::size_t ballIndex) {
    if (ballIndex >= balls_.size()) return;
    Ball& b = balls_[ballIndex];
    if (b.pocketed) {
        auto& ord = tubeOrder_[static_cast<std::size_t>(b.tube == 1 ? 1 : 0)];
        ord.erase(std::remove(ord.begin(), ord.end(), ballIndex), ord.end());
        for (std::size_t s = 0; s < ord.size(); ++s) {
            balls_[ord[s]].tubeSlot = static_cast<int>(s);
        }
        b.pocketed = false;
        b.dropProgress = 1.0f;
        pocketedCount_ = std::max(0, pocketedCount_ - 1);
    }

    const float clearR = radius_ * 2.02f;
    auto spotFree = [&](Vec2 s) {
        if (s.x < tableMin_.x + radius_ * 1.2f || s.x > tableMin_.x + tableSize_.x - radius_ * 1.2f ||
            s.y < tableMin_.y + radius_ * 1.2f || s.y > tableMin_.y + tableSize_.y - radius_ * 1.2f) {
            return false;
        }
        if (inPocket(s)) return false;
        for (std::size_t j = 0; j < balls_.size(); ++j) {
            if (j == ballIndex || balls_[j].pocketed) continue;
            if ((world_.state(balls_[j].body).position - s).lengthSquared() < clearR * clearR) return false;
        }
        if (!whitePocketed_ && (world_.state(white_).position - s).lengthSquared() < clearR * clearR) {
            return false;
        }
        return !world_.overlapsCircle(s, clearR, b.body);
    };

    Vec2 chosen = b.homeSpot;
    bool placed = spotFree(chosen);
    if (!placed) {
        for (int val = 7; !placed && val >= 2; --val) {
            for (const Ball& other : balls_) {
                if (other.group == BallGroup::SnookerColor && other.number == val) {
                    if (spotFree(other.homeSpot)) {
                        chosen = other.homeSpot;
                        placed = true;
                    }
                    break;
                }
            }
        }
    }
    if (!placed) {
        const float step = radius_ * 0.55f;
        for (int k = 1; !placed && k <= 60; ++k) {
            const Vec2 towardTop{b.homeSpot.x + static_cast<float>(k) * step, b.homeSpot.y};
            if (spotFree(towardTop)) {
                chosen = towardTop;
                placed = true;
                break;
            }
            const Vec2 towardBaulk{b.homeSpot.x - static_cast<float>(k) * step, b.homeSpot.y};
            if (spotFree(towardBaulk)) {
                chosen = towardBaulk;
                placed = true;
                break;
            }
        }
    }

    world_.setTransform(b.body, chosen, 0.0f);
    world_.setVelocity(b.body, {0.0f, 0.0f}, 0.0f);
    world_.setEnabled(b.body, true);
    world_.wake(b.body);
}

void BilliardsMode::rebalanceEightBallPlayerTubes() {
    if (config_.matchMode == BilliardsMatchMode::Solo || playerGroups_[0] == BallGroup::FreeBall) return;
    std::vector<std::size_t> allPocketed;
    allPocketed.reserve(tubeOrder_[0].size() + tubeOrder_[1].size());
    for (std::size_t idx : tubeOrder_[0]) allPocketed.push_back(idx);
    for (std::size_t idx : tubeOrder_[1]) allPocketed.push_back(idx);
    tubeOrder_[0].clear();
    tubeOrder_[1].clear();
    for (std::size_t bi : allPocketed) {
        Ball& b = balls_[bi];
        const int side = (b.group == playerGroups_[1]) ? 1 : 0;
        b.tube = side;
        b.tubeSlot = static_cast<int>(tubeOrder_[static_cast<std::size_t>(side)].size());
        tubeOrder_[static_cast<std::size_t>(side)].push_back(bi);
    }
}

void BilliardsMode::evaluateTurnRules() {
    const int p = currentPlayer_ == 1 ? 1 : 0;
    const int opp = 1 - p;
    const bool twoPlayer = config_.matchMode != BilliardsMatchMode::Solo;

    if (config_.style == BilliardsStyle::Classic || config_.rule == BilliardsRule::Free) {
        const int gained = static_cast<int>(pottedThisShot_.size());
        if (!whitePottedThisShot_ && gained > 0) {
            scores_[static_cast<std::size_t>(p)] += gained;
        }
        if (pocketedCount_ == static_cast<int>(balls_.size())) {
            state_ = BilliardsState::Finished;
            winner_ = twoPlayer ? (scores_[0] >= scores_[1] ? 0 : 1) : 0;
            recordSeriesFrameWinIfNeeded();
            if (!twoPlayer && shots_ > 0 && (bestShots_ == 0 || shots_ < bestShots_)) bestShots_ = shots_;
            return;
        }
        if (twoPlayer && (whitePottedThisShot_ || gained == 0)) {
            currentPlayer_ = opp;
        }
        if (whitePocketed_) {
            if (config_.style == BilliardsStyle::Desktop) {
                if (!placeScene_.isFinite()) placeScene_ = coords_.toScene(whiteSpawn_);
                state_ = BilliardsState::PlacingWhite;
                return;
            }
            if (!respawnWhite()) return;
        }
        state_ = BilliardsState::Ready;
        return;
    }

    if (config_.rule == BilliardsRule::EightBall) {
        int eightIdx = -1;
        for (std::size_t bi : pottedThisShot_) {
            if (balls_[bi].group == BallGroup::Eight) eightIdx = static_cast<int>(bi);
        }
        if (eightIdx >= 0 && shots_ == 1) {
            respotSnookerColor(static_cast<std::size_t>(eightIdx));
            pottedThisShot_.erase(std::remove(pottedThisShot_.begin(), pottedThisShot_.end(),
                                              static_cast<std::size_t>(eightIdx)),
                                  pottedThisShot_.end());
            eightIdx = -1;
            std::snprintf(turnNote_.data(), turnNote_.size(), "开球黑八落袋 · 已置回置球点");
        }

        const BallGroup myGroup = playerGroups_[static_cast<std::size_t>(p)];
        int solidsPotted = 0, stripesPotted = 0;
        for (std::size_t bi : pottedThisShot_) {
            if (balls_[bi].group == BallGroup::Solid) ++solidsPotted;
            if (balls_[bi].group == BallGroup::Stripe) ++stripesPotted;
        }
        const int ownPotted =
            myGroup == BallGroup::Solid ? solidsPotted : (myGroup == BallGroup::Stripe ? stripesPotted : 0);
        const int ownRemainingBefore =
            myGroup == BallGroup::FreeBall ? 7 : (remainingGroupBalls(myGroup) + ownPotted);

        bool foul = false;
        const char* foulReason = "";
        if (firstHitBall_ < 0) {
            foul = true;
            foulReason = "未击中目标球";
        } else {
            const BallGroup hitGroup = balls_[static_cast<std::size_t>(firstHitBall_)].group;
            if (myGroup == BallGroup::FreeBall) {
                if (hitGroup == BallGroup::Eight) {
                    foul = true;
                    foulReason = "开放台误击黑八";
                }
            } else if (ownRemainingBefore == 0) {
                if (hitGroup != BallGroup::Eight) {
                    foul = true;
                    foulReason = "须先击打黑八";
                }
            } else if (hitGroup != myGroup) {
                foul = true;
                foulReason = "误击非本方花色";
            }
        }
        if (whitePottedThisShot_) {
            foul = true;
            foulReason = "母球落袋犯规";
        }

        if (eightIdx >= 0) {
            if (!foul && myGroup != BallGroup::FreeBall && ownRemainingBefore == 0) {
                winner_ = p;
                state_ = BilliardsState::Finished;
                if (!twoPlayer && shots_ > 0 && (bestShots_ == 0 || shots_ < bestShots_)) bestShots_ = shots_;
                if (twoPlayer) {
                    std::snprintf(turnNote_.data(), turnNote_.size(), "黑八合法进袋 · %s获胜！",
                                  config_.matchMode == BilliardsMatchMode::VsAi ? playerLabel(p)
                                                                                : (p == 0 ? "P1 " : "P2 "));
                } else {
                    std::snprintf(turnNote_.data(), turnNote_.size(), "黑八合法进袋 · 获胜！");
                }
            } else {
                winner_ = twoPlayer ? opp : -2;
                state_ = BilliardsState::Finished;
                if (twoPlayer) {
                    std::snprintf(turnNote_.data(), turnNote_.size(), "黑八犯规进袋 · %s获胜",
                                  config_.matchMode == BilliardsMatchMode::VsAi ? playerLabel(opp)
                                                                                : (opp == 0 ? "P1 " : "P2 "));
                } else {
                    std::snprintf(turnNote_.data(), turnNote_.size(), "黑八犯规进袋 · 挑战失败获胜");
                }
            }
            recordSeriesFrameWinIfNeeded();
            return;
        }

        if (foul) {
            if (twoPlayer) {
                currentPlayer_ = opp;
                std::snprintf(turnNote_.data(), turnNote_.size(), "%s · %s自由球", foulReason,
                              playerLabel(opp));
                if (!whitePocketed_) {
                    placeScene_ = whiteSceneCenter();
                    world_.setEnabled(white_, false);
                    whitePocketed_ = true;
                }
                state_ = BilliardsState::PlacingWhite;
                return;
            }
            std::snprintf(turnNote_.data(), turnNote_.size(), "犯规：%s", foulReason);
            if (whitePocketed_) {
                state_ = BilliardsState::PlacingWhite;
                return;
            }
            state_ = BilliardsState::Ready;
            return;
        }

        if (shots_ > 1 && myGroup == BallGroup::FreeBall && (solidsPotted > 0 || stripesPotted > 0)) {
            const BallGroup chosen = solidsPotted >= stripesPotted ? BallGroup::Solid : BallGroup::Stripe;
            playerGroups_[static_cast<std::size_t>(p)] = chosen;
            playerGroups_[static_cast<std::size_t>(opp)] =
                chosen == BallGroup::Solid ? BallGroup::Stripe : BallGroup::Solid;
            if (twoPlayer) rebalanceEightBallPlayerTubes();
            std::snprintf(turnNote_.data(), turnNote_.size(), "%s定组：%s",
                          twoPlayer ? playerLabel(p) : "本局", groupShortName(chosen));
        }

        const BallGroup curGroup = playerGroups_[static_cast<std::size_t>(p)];
        const bool pottedOwn = curGroup == BallGroup::FreeBall
                                   ? (solidsPotted + stripesPotted > 0)
                                   : ((curGroup == BallGroup::Solid ? solidsPotted : stripesPotted) > 0);
        if (!pottedOwn && twoPlayer) {
            currentPlayer_ = opp;
        }
        if (playerGroups_[0] != BallGroup::FreeBall) {
            scores_[0] = 7 - remainingGroupBalls(playerGroups_[0]);
            scores_[1] = 7 - remainingGroupBalls(playerGroups_[1]);
        }
        state_ = BilliardsState::Ready;
        return;
    }

    // Snooker rules.
    const int target = snookerTarget_;
    bool foul = false;
    int foulPenalty = 4;
    if (whitePottedThisShot_) foul = true;

    if (firstHitBall_ < 0) {
        foul = true;
        const int req = target == 1 ? 1 : (target == 8 ? 4 : target);
        foulPenalty = std::max(foulPenalty, req);
    } else {
        const int hitVal = balls_[static_cast<std::size_t>(firstHitBall_)].number;
        foulPenalty = std::max(foulPenalty, hitVal);
        if (target == 1 && hitVal != 1) foul = true;
        if (target == 8 && hitVal < 2) foul = true;
        if (target >= 2 && target <= 7 && hitVal != target) foul = true;
    }

    int redsPotted = 0, colorsPotted = 0, colorPts = 0;
    for (std::size_t bi : pottedThisShot_) {
        const int val = balls_[bi].number;
        foulPenalty = std::max(foulPenalty, val);
        if (val == 1) {
            ++redsPotted;
            if (target != 1) foul = true;
        } else {
            ++colorsPotted;
            colorPts += val;
            if (target == 1) foul = true;
            if (target == 8 &&
                (firstHitBall_ < 0 || val != balls_[static_cast<std::size_t>(firstHitBall_)].number)) {
                foul = true;
            }
            if (target >= 2 && target <= 7 && val != target) foul = true;
        }
    }
    if (colorsPotted > 1) foul = true;

    for (std::size_t bi : pottedThisShot_) {
        if (balls_[bi].group == BallGroup::SnookerColor && (foul || target == 8)) {
            respotSnookerColor(bi);
        }
    }

    const int redsLeft = remainingReds();
    if (foul) {
        currentBreak_ = 0;
        if (twoPlayer) {
            scores_[static_cast<std::size_t>(opp)] += foulPenalty;
            currentPlayer_ = opp;
            std::snprintf(turnNote_.data(), turnNote_.size(), "犯规罚 %d 分 · 轮到%s", foulPenalty,
                          playerLabel(opp));
        } else {
            std::snprintf(turnNote_.data(), turnNote_.size(), "犯规（标准罚 %d 分）", foulPenalty);
        }
        snookerTarget_ = redsLeft > 0 ? 1 : lowestRemainingColorValue();
    } else if (target == 1) {
        if (redsPotted > 0) {
            scores_[static_cast<std::size_t>(p)] += redsPotted;
            currentBreak_ += redsPotted;
            snookerTarget_ = 8;
        } else {
            currentBreak_ = 0;
            if (twoPlayer) currentPlayer_ = opp;
            snookerTarget_ = redsLeft > 0 ? 1 : lowestRemainingColorValue();
        }
    } else if (target == 8) {
        if (colorsPotted == 1) {
            scores_[static_cast<std::size_t>(p)] += colorPts;
            currentBreak_ += colorPts;
            snookerTarget_ = redsLeft > 0 ? 1 : lowestRemainingColorValue();
        } else {
            currentBreak_ = 0;
            if (twoPlayer) currentPlayer_ = opp;
            snookerTarget_ = redsLeft > 0 ? 1 : lowestRemainingColorValue();
        }
    } else {
        if (colorsPotted == 1) {
            scores_[static_cast<std::size_t>(p)] += colorPts;
            currentBreak_ += colorPts;
            const int nextColor = lowestRemainingColorValue();
            if (nextColor == 0 && redsLeft == 0) {
                state_ = BilliardsState::Finished;
                winner_ = twoPlayer ? (scores_[0] >= scores_[1] ? 0 : 1) : 0;
                recordSeriesFrameWinIfNeeded();
                if (!twoPlayer && shots_ > 0 && (bestShots_ == 0 || shots_ < bestShots_)) bestShots_ = shots_;
                std::snprintf(turnNote_.data(), turnNote_.size(), "斯诺克清台完成！");
                return;
            }
            snookerTarget_ = nextColor;
        } else {
            currentBreak_ = 0;
            if (twoPlayer) currentPlayer_ = opp;
            snookerTarget_ = lowestRemainingColorValue();
        }
    }

    if (redsLeft == 0 && lowestRemainingColorValue() == 0) {
        state_ = BilliardsState::Finished;
        winner_ = twoPlayer ? (scores_[0] >= scores_[1] ? 0 : 1) : 0;
        recordSeriesFrameWinIfNeeded();
        return;
    }

    if (whitePocketed_) {
        placeScene_ = coords_.toScene(whiteSpawn_);
        state_ = BilliardsState::PlacingWhite;
        return;
    }
    state_ = BilliardsState::Ready;
}

void BilliardsMode::updateRules(float dt) {
    if (!std::isfinite(dt) || dt <= 0 || dt > 0.25f) return;
    if (state_ == BilliardsState::Charging && config_.style == BilliardsStyle::Classic) chargeElapsed_ += dt;

    const float dropStep = config_.tubeDropSeconds > 1e-4f ? dt / config_.tubeDropSeconds : 1.0f;
    for (std::size_t i = 0; i < balls_.size(); ++i) {
        Ball& b = balls_[i];
        if (b.pocketed) {
            if (b.dropProgress < 1.0f) b.dropProgress = std::min(1.0f, b.dropProgress + dropStep);
            continue;
        }
        if (lanLocalPlayer_ == 1) continue;
        const BodyState bs = world_.state(b.body);
        const int pk = matchingPocket(bs.position);
        if (pk >= 0) {
            const float spd = bs.linearVelocity.length();
            const float dropGain = std::clamp(0.55f + 0.45f * std::min(1.0f, spd / 8.0f), 0.55f, 1.0f);
            pushSound(SoundKind::PocketDrop, dropGain,
                      coords_.toScene(pockets_[static_cast<std::size_t>(pk)]).x, 1.0f);
            world_.setEnabled(b.body, false);
            b.pocketed = true;
            ++pocketedCount_;
            pottedThisShot_.push_back(i);
            int side = 0;
            if (config_.style == BilliardsStyle::Desktop && config_.matchMode != BilliardsMatchMode::Solo) {
                // Two-player modes (Hotseat / LAN): Left tube (0) = P1, Right tube (1) = P2!
                if (config_.rule == BilliardsRule::EightBall && playerGroups_[0] != BallGroup::FreeBall) {
                    if (b.group == playerGroups_[0]) {
                        side = 0;
                    } else if (b.group == playerGroups_[1]) {
                        side = 1;
                    } else {
                        side = (currentPlayer_ == 1) ? 1 : 0;
                    }
                } else {
                    side = (currentPlayer_ == 1) ? 1 : 0;
                }
            } else if (pk == 2 || pk == 5) {
                side = 1;
            } else if (pk == 1 || pk == 4) {
                side = tubeOrder_[0].size() <= tubeOrder_[1].size() ? 0 : 1;
            }
            b.tube = side;
            b.tubeSlot = static_cast<int>(tubeOrder_[static_cast<std::size_t>(side)].size());
            b.dropProgress = config_.style == BilliardsStyle::Desktop ? 0.0f : 1.0f;
            tubeOrder_[static_cast<std::size_t>(side)].push_back(i);
        }
    }

    if (lanLocalPlayer_ == 1) return;

    if (!whitePocketed_) {
        const BodyState ws = world_.state(white_);
        const int wpk = matchingPocket(ws.position);
        if (wpk >= 0) {
            const float spd = ws.linearVelocity.length();
            const float dropGain = std::clamp(0.55f + 0.45f * std::min(1.0f, spd / 8.0f), 0.55f, 1.0f);
            pushSound(SoundKind::PocketDrop, dropGain,
                      coords_.toScene(pockets_[static_cast<std::size_t>(wpk)]).x, 0.92f);
            world_.setEnabled(white_, false);
            whitePocketed_ = true;
            whitePottedThisShot_ = true;
        }
    }

    if (state_ == BilliardsState::Rolling) {
        rollingTime_ += dt;
        if (everythingStopped() || rollingTime_ > config_.maxRollSeconds) {
            world_.setVelocity(white_, {0, 0}, 0.0f);
            cueRollVel_ = {0.0f, 0.0f};
            cueSideSpin_ = 0.0f;
            cueHitBall_ = false;
            for (const Ball& b : balls_)
                if (!b.pocketed) world_.setVelocity(b.body, {0, 0}, 0.0f);
            evaluateTurnRules();
            aiPhase_ = AiPhase::Idle;
            aiPhaseTime_ = 0.0f;
        }
    }

    if (isAiTurn()) {
        updateAiTurn(dt);
    } else {
        aiPhase_ = AiPhase::Idle;
        aiPhaseTime_ = 0.0f;
    }
}

BilliardsMode::TubeGeometry BilliardsMode::tubeGeometry(int side) const {
    TubeGeometry g;
    const Vec2 tl = coords_.toScene({tableMin_.x, tableMin_.y + tableSize_.y});
    const Vec2 br = coords_.toScene({tableMin_.x + tableSize_.x, tableMin_.y});
    const float r = coords_.lengthToScene(radius_);
    const float railOuter = railOuterWidthScene();
    g.capR = std::max(r * 1.22f, r + 3.0f);

    if (side == 0) {
        const float idealX = tl.x - railOuter - g.capR - 12.0f;
        const float maxLeft = std::max(g.capR + 4.0f, tl.x - railOuter - g.capR - 3.0f);
        g.cx = std::clamp(idealX, g.capR + 4.0f, maxLeft);
    } else {
        const float idealX = br.x + railOuter + g.capR + 12.0f;
        const float minRight = std::min(sceneSize_.x - g.capR - 4.0f, br.x + railOuter + g.capR + 3.0f);
        g.cx = std::clamp(idealX, minRight, sceneSize_.x - g.capR - 4.0f);
    }

    const float minY = g.capR + 4.0f;
    const float maxY = std::max(minY, sceneSize_.y - g.capR - 4.0f);
    float rawTop = std::clamp(tl.y + g.capR * 0.6f, minY, maxY);
    float rawBot = std::clamp(br.y - g.capR * 0.6f, minY, maxY);
    if (rawTop > rawBot) {
        rawTop = rawBot = sceneSize_.y * 0.5f;
    }
    g.topY = rawTop;
    g.botY = rawBot;

    const auto& order = tubeOrder_[static_cast<std::size_t>(side == 1 ? 1 : 0)];
    const std::size_t count = order.size();
    const float naturalStep = 2.0f * r + 2.0f;
    const float minStep = std::max(7.0f, r * 1.10f);
    const float availSpan = std::max(0.0f, g.botY - g.topY);
    g.overflowCenterY = g.topY;

    if (count == 0) return g;
    if (count == 1 || static_cast<float>(count - 1) * minStep <= availSpan) {
        g.firstVisible = 0;
        g.visibleCount = count;
        g.overflow = 0;
        g.step = count > 1 ? std::min(naturalStep, availSpan / static_cast<float>(count - 1)) : 0.0f;
    } else {
        const float headerH = std::min(20.0f, availSpan * 0.25f);
        const float usableSpan = std::max(0.0f, availSpan - headerH);
        std::size_t vis = static_cast<std::size_t>(std::floor(usableSpan / minStep)) + 1;
        vis = std::clamp<std::size_t>(vis, 1, count);
        g.visibleCount = vis;
        g.firstVisible = count - vis;
        g.overflow = count - vis;
        g.step = vis > 1 ? usableSpan / static_cast<float>(vis - 1) : 0.0f;
        g.overflowCenterY = std::min(g.botY, g.topY + headerH);
    }
    return g;
}

Vec2 BilliardsMode::tubeBallSceneCenter(std::size_t ballIndex, const TubeGeometry& geom) const {
    const Ball& b = balls_.at(ballIndex);
    const float r = coords_.lengthToScene(radius_);
    const auto slot = static_cast<std::size_t>(std::max(0, b.tubeSlot));
    if (slot < geom.firstVisible) {
        return {geom.cx, std::clamp(geom.overflowCenterY, r + 2.0f, sceneSize_.y - r - 2.0f)};
    }
    const std::size_t visSlot = slot - geom.firstVisible;
    const float targetY = geom.botY - static_cast<float>(visSlot) * geom.step;
    const float t = std::clamp(b.dropProgress, 0.0f, 1.0f);
    const float ease = 1.0f - (1.0f - t) * (1.0f - t);
    const float startY = geom.overflow > 0 ? geom.overflowCenterY : geom.topY;
    const float y = startY + (targetY - startY) * ease;
    return {geom.cx, std::clamp(y, r + 2.0f, sceneSize_.y - r - 2.0f)};
}

Vec2 BilliardsMode::iconCurrentSceneCenter(std::size_t i) const {
    if (i < iconToBall_.size()) {
        const int bIdx = iconToBall_[i];
        if (bIdx >= 0 && static_cast<std::size_t>(bIdx) < balls_.size()) {
            const auto bi = static_cast<std::size_t>(bIdx);
            const Ball& b = balls_[bi];
            if (!b.pocketed) return coords_.toScene(world_.state(b.body).position);
            return tubeBallSceneCenter(bi, tubeGeometry(b.tube));
        }
        const int eIdx = iconToExtra_[i];
        if (eIdx >= 0 && static_cast<std::size_t>(eIdx) < extraIcons_.size()) {
            const ExtraIcon& ex = extraIcons_[static_cast<std::size_t>(eIdx)];
            if (extraWorld_) return coords_.toScene(extraWorld_->state(ex.body).position);
            return ex.originalCenter;
        }
    }
    const Ball& b = balls_.at(i);
    if (!b.pocketed) return coords_.toScene(world_.state(b.body).position);
    return tubeBallSceneCenter(i, tubeGeometry(b.tube));
}

bool BilliardsMode::isTubeBallHit(Vec2 scenePos) const {
    if (config_.style != BilliardsStyle::Desktop ||
        config_.effectiveTableTheme() != BilliardsTableTheme::Full || !scenePos.isFinite()) {
        return false;
    }
    const float spriteHalf = coords_.lengthToScene(radius_) * (config_.showBallBorders ? 0.78f : 0.94f);
    const float r2 = spriteHalf * spriteHalf;
    for (int side = 0; side < 2; ++side) {
        const TubeGeometry tg = tubeGeometry(side);
        const auto& order = tubeOrder_[static_cast<std::size_t>(side)];
        for (std::size_t s = tg.firstVisible; s < tg.firstVisible + tg.visibleCount && s < order.size();
             ++s) {
            const std::size_t bi = order[s];
            if (balls_[bi].texture == kNoTexture) continue;
            const Vec2 c = tubeBallSceneCenter(bi, tg);
            if ((scenePos - c).lengthSquared() <= r2) return true;
        }
    }
    return false;
}

Vec2 BilliardsMode::whiteSceneCenter() const { return coords_.toScene(world_.state(white_).position); }

Vec2 BilliardsMode::objectBallScene(std::size_t i) const {
    return coords_.toScene(world_.state(balls_.at(i).body).position);
}

void BilliardsMode::debugPlaceBall(std::size_t i, Vec2 scenePos, Vec2 sceneVelocity) {
    const BodyIndex b = balls_.at(i).body;
    world_.setTransform(b, coords_.toPhysics(scenePos), 0.0f);
    world_.setVelocity(b, coords_.velocityToPhysics(sceneVelocity), 0.0f);
    world_.wake(b);
    if (sceneVelocity.lengthSquared() > 0.0f) state_ = BilliardsState::Rolling;
}

void BilliardsMode::debugPlaceWhite(Vec2 scenePos, Vec2 sceneVelocity) {
    world_.setTransform(white_, coords_.toPhysics(scenePos), 0.0f);
    world_.setVelocity(white_, coords_.velocityToPhysics(sceneVelocity), 0.0f);
    world_.wake(white_);
    if (sceneVelocity.lengthSquared() > 0.0f) state_ = BilliardsState::Rolling;
}

void BilliardsMode::debugSetTurnState(int player, BallGroup p0Group, BallGroup p1Group, int snookerTarget) {
    currentPlayer_ = (player == 1) ? 1 : 0;
    playerGroups_[0] = p0Group;
    playerGroups_[1] = p1Group;
    if (snookerTarget >= 1 && snookerTarget <= 8) snookerTarget_ = snookerTarget;
    aiPhase_ = AiPhase::Idle;
    aiPhaseTime_ = 0.0f;
}

bool BilliardsMode::isLegalTargetBall(std::size_t ballIndex, int player) const {
    if (ballIndex >= balls_.size() || balls_[ballIndex].pocketed ||
        !world_.state(balls_[ballIndex].body).enabled) {
        return false;
    }
    const Ball& b = balls_[ballIndex];
    if (config_.style == BilliardsStyle::Classic || config_.rule == BilliardsRule::Free) {
        return true;
    }
    if (config_.rule == BilliardsRule::EightBall) {
        const BallGroup grp = playerGroups_[static_cast<std::size_t>(player == 1 ? 1 : 0)];
        if (grp == BallGroup::FreeBall) {
            return b.group == BallGroup::Solid || b.group == BallGroup::Stripe;
        }
        if (remainingGroupBalls(grp) > 0) {
            return b.group == grp;
        }
        return b.group == BallGroup::Eight;
    }
    if (snookerTarget_ == 1) return b.group == BallGroup::SnookerRed;
    if (snookerTarget_ == 8) return b.group == BallGroup::SnookerColor;
    return b.group == BallGroup::SnookerColor && b.number == snookerTarget_;
}

std::pair<Vec2, Vec2> BilliardsMode::pocketJawCenters(std::size_t pocketIndex) const {
    const float left = tableMin_.x, bottom = tableMin_.y;
    const float right = left + tableSize_.x, top = bottom + tableSize_.y;
    const float midX = left + tableSize_.x * 0.5f;
    const float jr = 0.12f * radius_;
    switch (pocketIndex) {
        case 0:
            return {{left + cornerMouth_ + jr, top + jr}, {left - jr, top - cornerMouth_ - jr}};
        case 1:
            return {{midX - sideMouth_ - jr, top + jr}, {midX + sideMouth_ + jr, top + jr}};
        case 2:
            return {{right - cornerMouth_ - jr, top + jr}, {right + jr, top - cornerMouth_ - jr}};
        case 3:
            return {{left + cornerMouth_ + jr, bottom - jr}, {left - jr, bottom + cornerMouth_ + jr}};
        case 4:
            return {{midX - sideMouth_ - jr, bottom - jr}, {midX + sideMouth_ + jr, bottom - jr}};
        default:
            return {{right - cornerMouth_ - jr, bottom - jr}, {right + jr, bottom + cornerMouth_ + jr}};
    }
}

Vec2 BilliardsMode::pocketThroatMeters(std::size_t pocketIndex) const {
    if (config_.style != BilliardsStyle::Desktop) return pockets_.at(pocketIndex);
    const auto [jawA, jawB] = pocketJawCenters(pocketIndex);
    const Vec2 jawMid = (jawA + jawB) * 0.5f;
    return jawMid * 0.52f + pockets_.at(pocketIndex) * 0.48f;
}

bool BilliardsMode::pocketPathClearJaws(std::size_t pocketIndex, Vec2 fromPhys, Vec2 entryPhys) const {
    if (config_.style != BilliardsStyle::Desktop) return true;
    const Vec2 pocketWell = pockets_.at(pocketIndex);
    const Vec2 throat = pocketThroatMeters(pocketIndex);
    const Vec2 inDir = normalizedOr(pocketWell - throat, {0.0f, 1.0f});
    const Vec2 potDir = normalizedOr(entryPhys - fromPhys, inDir);
    if (potDir.x * inDir.x + potDir.y * inDir.y < 0.35f) return false;

    const auto [jawA, jawB] = pocketJawCenters(pocketIndex);
    const Vec2 deepPt = entryPhys + potDir * (1.45f * radius_);
    const float minJawClear = 1.04f * radius_;
    const float minJawClear2 = minJawClear * minJawClear;
    if (pointToSegmentDistSq(jawA, fromPhys, deepPt) < minJawClear2) return false;
    if (pointToSegmentDistSq(jawB, fromPhys, deepPt) < minJawClear2) return false;

    // Ensure ball path does not scrape along an adjacent cushion nose before reaching the jaws.
    const float left = tableMin_.x + radius_ * 0.96f;
    const float right = tableMin_.x + tableSize_.x - radius_ * 0.96f;
    const float bottom = tableMin_.y + radius_ * 0.96f;
    const float top = tableMin_.y + tableSize_.y - radius_ * 0.96f;
    const Vec2 midPath = (fromPhys + entryPhys) * 0.5f;
    if ((pocketIndex == 1 || pocketIndex == 4) && (midPath.x < left || midPath.x > right)) return false;
    if (midPath.y < bottom - radius_ || midPath.y > top + radius_) return false;
    return true;
}

bool BilliardsMode::segmentClearOfBalls(Vec2 a, Vec2 b, float clearance, int ignoreBallA,
                                        int ignoreBallB) const {
    const float clear2 = clearance * clearance;
    for (std::size_t i = 0; i < balls_.size(); ++i) {
        if (balls_[i].pocketed || !world_.state(balls_[i].body).enabled) continue;
        const int idx = static_cast<int>(i);
        if (idx == ignoreBallA || idx == ignoreBallB) continue;
        const Vec2 bp = world_.state(balls_[i].body).position;
        if (pointToSegmentDistSq(bp, a, b) < clear2) return false;
    }
    return true;
}

bool BilliardsMode::cushionBounceValid(int railSide, Vec2 bouncePhys) const {
    const float left = tableMin_.x, bottom = tableMin_.y;
    const float right = left + tableSize_.x, top = bottom + tableSize_.y;
    const float midX = left + tableSize_.x * 0.5f;
    const float cornerPad = cornerMouth_ + 1.25f * radius_;
    const float sidePad = sideMouth_ + 1.25f * radius_;
    if (railSide == 0 || railSide == 1) {
        return bouncePhys.y >= bottom + cornerPad && bouncePhys.y <= top - cornerPad;
    }
    const bool inLeftHalf = bouncePhys.x >= left + cornerPad && bouncePhys.x <= midX - sidePad;
    const bool inRightHalf = bouncePhys.x >= midX + sidePad && bouncePhys.x <= right - cornerPad;
    return inLeftHalf || inRightHalf;
}

bool BilliardsMode::pathRisksScratch(Vec2 startPhys, Vec2 dirUnit, float travelDist) const {
    if (travelDist <= radius_ * 0.5f) return false;
    const Vec2 endPhys = startPhys + dirUnit * std::min(travelDist, tableSize_.x * 1.15f);
    for (std::size_t pk = 0; pk < pockets_.size(); ++pk) {
        const float catchR = pocketCatchRadius(pk) * 1.22f;
        if (pointToSegmentDistSq(pockets_[pk], startPhys, endPhys) < catchR * catchR) {
            if (segmentClearOfBalls(startPhys, pockets_[pk], 1.85f * radius_)) return true;
        }
    }
    return false;
}

BilliardsMode::AiSimOutcome BilliardsMode::simulateShotRollout(Vec2 whiteStartPhys, Vec2 aimPhysDir,
                                                               float power, Vec2 spin, int maxSteps) const {
    AiSimOutcome out;
    out.nextAiGroup = playerGroups_[1];
    out.nextSnookerTarget = snookerTarget_;
    out.endBallPos.resize(balls_.size(), Vec2{0.0f, 0.0f});
    out.endBallActive.assign(balls_.size(), false);

    const bool desktop = (config_.style == BilliardsStyle::Desktop);
    const float topSpeed = desktop ? std::clamp(tableSize_.x * 2.0f, 16.0f, 36.0f) : config_.maxShotSpeed;
    const Vec2 unitScene = normalizedOr(coords_.velocityToScene(aimPhysDir), Vec2{1.0f, 0.0f});
    const Vec2 dirUnit = normalizedOr(coords_.velocityToPhysics(unitScene), Vec2{1.0f, 0.0f});

    PhysicsWorld sim(tablePhysics());
    if (desktop) {
        populateDesktopTableCushions(sim);
    } else {
        sim.createBoundary(tableMin_, tableSize_, true, 1.0f, 0.1f, 0.85f);
    }

    const float linDamp = desktop ? 0.95f : 0.5f;
    const BodyMaterial ballMat{1.0f, 0.05f, 0.92f, linDamp, 1.0f, desktop};
    BodyMaterial whiteMat = ballMat;
    whiteMat.bullet = true;

    const BodyIndex simWhite = sim.addCircle(whiteStartPhys, radius_, whiteMat);
    std::vector<std::pair<std::size_t, BodyIndex>> simBalls;
    simBalls.reserve(balls_.size());
    for (std::size_t bi = 0; bi < balls_.size(); ++bi) {
        if (balls_[bi].pocketed || !world_.isEnabled(balls_[bi].body)) continue;
        const Vec2 bPos = world_.position(balls_[bi].body);
        const BodyIndex sb = sim.addCircle(bPos, radius_, ballMat);
        simBalls.push_back({bi, sb});
        out.endBallPos[bi] = bPos;
        out.endBallActive[bi] = true;
    }

    const Vec2 vInit = dirUnit * (topSpeed * power);
    sim.setVelocity(simWhite, vInit, 0.0f);
    sim.wake(simWhite);

    const float initSpd = vInit.length();
    const float rollFac = spin.y >= 0.0f ? (spin.y * 1.35f) : (spin.y * 1.45f);
    Vec2 simRollVel = desktop ? (dirUnit * (initSpd * rollFac)) : Vec2{0.0f, 0.0f};
    float simSideSpin = desktop ? (initSpd * (spin.x * 0.90f)) : 0.0f;
    bool simCueHit = false;
    std::vector<PairCooldown> simCooldowns;
    simCooldowns.reserve(32);
    std::vector<std::size_t> simPotted;
    simPotted.reserve(4);

    constexpr float kStep = 1.0f / 60.0f;
    const float kRollDecay = std::exp(-0.50f * kStep);
    const float kSpinDecay = std::exp(-0.35f * kStep);
    const float drag = std::clamp(tableSize_.x * 0.025f, 0.20f, 0.45f);
    const float restS2 = config_.restSpeed * config_.restSpeed;
    double localTime = 0.0;

    for (int step = 0; step < maxSteps; ++step) {
        sim.step(kStep);
        localTime += static_cast<double>(kStep);

        simCooldowns.erase(
            std::remove_if(simCooldowns.begin(), simCooldowns.end(),
                           [localTime](const PairCooldown& c) { return c.until <= localTime; }),
            simCooldowns.end());

        for (const CollisionHit& h : sim.lastHits()) {
            std::uint32_t idA = h.bodyA ? (*h.bodyA + 1u) : 0u;
            std::uint32_t idB = h.bodyB ? (*h.bodyB + 1u) : 0u;
            if (idA == 0u && idB == 0u) continue;
            if (idA > idB) std::swap(idA, idB);

            if (idA == 1u && idB >= 2u) {
                simCueHit = true;
                const std::size_t slot = static_cast<std::size_t>(idB - 2u);
                if (out.firstHitBall < 0 && slot < simBalls.size()) {
                    out.firstHitBall = static_cast<int>(simBalls[slot].first);
                }
            }

            const std::uint32_t pairKey = (idA << 16) | (idB & 0xffffu);
            bool cooling = false;
            for (const PairCooldown& c : simCooldowns) {
                if (c.key == pairKey) {
                    cooling = true;
                    break;
                }
            }
            if (cooling) continue;
            if (simCooldowns.size() < 128) simCooldowns.push_back(PairCooldown{pairKey, localTime + 0.028});

            if (idA == 0u && !inPocket(h.point) && desktop && idB == 1u && !out.scratched &&
                std::abs(simSideSpin) > 0.05f) {
                const float dLeft = std::abs(h.point.x - tableMin_.x);
                const float dRight = std::abs(h.point.x - (tableMin_.x + tableSize_.x));
                const float dBottom = std::abs(h.point.y - tableMin_.y);
                const float dTop = std::abs(h.point.y - (tableMin_.y + tableSize_.y));
                Vec2 nIn{0.0f, -1.0f};
                float bestD = dTop;
                if (dBottom < bestD) {
                    bestD = dBottom;
                    nIn = {0.0f, 1.0f};
                }
                if (dLeft < bestD) {
                    bestD = dLeft;
                    nIn = {1.0f, 0.0f};
                }
                if (dRight < bestD) nIn = {-1.0f, 0.0f};
                const Vec2 tRight{-nIn.y, nIn.x};
                const BodyState ws = sim.state(simWhite);
                const float curSpd = ws.linearVelocity.length();
                const float kick = std::clamp(simSideSpin * 0.48f, -15.0f, 15.0f);
                Vec2 nextV = ws.linearVelocity + tRight * kick;
                const float capSpd = std::min(36.0f, std::max(curSpd * 1.18f, 4.0f));
                if (nextV.length() > capSpd) nextV = normalizedOr(nextV, tRight) * capSpd;
                sim.setVelocity(simWhite, nextV, ws.angularVelocity);
                simSideSpin *= 0.45f;
                simRollVel = normalizedOr(nextV, {1.0f, 0.0f}) * (simRollVel.length() * 0.65f);
            }
        }

        if (desktop) {
            if (!out.scratched && sim.isEnabled(simWhite)) {
                const Vec2 wLin = sim.linearVelocity(simWhite);
                if (!simCueHit) {
                    simRollVel = simRollVel * kRollDecay + wLin * (0.14f * kStep);
                } else if ((simRollVel - wLin).lengthSquared() > 1e-4f) {
                    const Vec2 slip = simRollVel - wLin;
                    const float alpha = std::min(1.0f, 8.0f * kStep);
                    Vec2 nextV = wLin + slip * (0.45f * alpha);
                    if (nextV.length() > 36.0f) nextV = normalizedOr(nextV, {1.0f, 0.0f}) * 36.0f;
                    simRollVel = simRollVel - slip * (0.55f * alpha);
                    sim.setVelocity(simWhite, nextV, sim.angularVelocity(simWhite));
                    sim.wake(simWhite);
                }
                simSideSpin *= kSpinDecay;
            }

            auto settleSimBody = [&](BodyIndex idx) {
                if (!sim.isAwake(idx)) return;
                const Vec2 lv = sim.linearVelocity(idx);
                const float spdSq = lv.lengthSquared();
                if (spdSq > 1e-8f && spdSq < (1.8f * 1.8f)) {
                    const float spd = std::sqrt(spdSq);
                    const float nextSpd = std::max(0.0f, spd - drag * kStep);
                    sim.setVelocity(idx, lv * (nextSpd / spd), sim.angularVelocity(idx));
                }
            };
            if (!out.scratched) settleSimBody(simWhite);
            for (const auto& [origIdx, sBody] : simBalls) {
                if (out.endBallActive[origIdx]) settleSimBody(sBody);
            }
        }

        // Pocket checks matching `updateRules` (only awake balls can enter a pocket).
        for (const auto& [origIdx, sBody] : simBalls) {
            if (!out.endBallActive[origIdx] || !sim.isAwake(sBody)) continue;
            if (matchingPocket(sim.position(sBody)) >= 0) {
                sim.setEnabled(sBody, false);
                out.endBallActive[origIdx] = false;
                simPotted.push_back(origIdx);
            }
        }
        if (!out.scratched && sim.isAwake(simWhite)) {
            if (matchingPocket(sim.position(simWhite)) >= 0) {
                sim.setEnabled(simWhite, false);
                out.scratched = true;
                break;  // Cue-ball scratch is an unconditional foul across all rules; exit early!
            }
        }

        bool allStopped = true;
        if (!out.scratched) {
            if (sim.isAwake(simWhite) && sim.linearVelocity(simWhite).lengthSquared() > restS2)
                allStopped = false;
            if (simCueHit && simRollVel.lengthSquared() > 0.12f) allStopped = false;
        }
        if (allStopped) {
            for (const auto& [origIdx, sBody] : simBalls) {
                if (out.endBallActive[origIdx] && sim.isAwake(sBody) &&
                    sim.linearVelocity(sBody).lengthSquared() > restS2) {
                    allStopped = false;
                    break;
                }
            }
        }
        if (allStopped) break;
    }

    out.whiteEndPhys = out.scratched ? whiteSpawn_ : sim.position(simWhite);
    for (const auto& [origIdx, sBody] : simBalls) {
        if (out.endBallActive[origIdx]) {
            out.endBallPos[origIdx] = sim.position(sBody);
        }
    }

    // Evaluate exact turn rules for AI (Player 1).
    if (!desktop || config_.rule == BilliardsRule::Free) {
        out.foul = out.scratched;
        out.ownPotted = static_cast<int>(simPotted.size());
        out.gainedScore = !out.scratched ? out.ownPotted : 0;
        int remAfter = 0;
        for (bool act : out.endBallActive) {
            if (act) ++remAfter;
        }
        if (remAfter == 0) {
            out.wonGame = (scores_[1] + out.gainedScore > scores_[0]);
        }
        out.continuesTurn = (!out.scratched && out.ownPotted > 0) || out.wonGame;
        return out;
    }

    if (config_.rule == BilliardsRule::EightBall) {
        int eightIdx = -1;
        for (std::size_t bi : simPotted) {
            if (balls_[bi].group == BallGroup::Eight) eightIdx = static_cast<int>(bi);
        }
        if (eightIdx >= 0 && (shots_ + 1) == 1) {
            const auto eIdx = static_cast<std::size_t>(eightIdx);
            out.endBallActive[eIdx] = true;
            out.endBallPos[eIdx] = balls_[eIdx].homeSpot;
            simPotted.erase(std::remove(simPotted.begin(), simPotted.end(), eIdx), simPotted.end());
            eightIdx = -1;
        }

        const BallGroup myGroup = playerGroups_[1];
        int solidsPotted = 0, stripesPotted = 0;
        for (std::size_t bi : simPotted) {
            if (balls_[bi].group == BallGroup::Solid) ++solidsPotted;
            if (balls_[bi].group == BallGroup::Stripe) ++stripesPotted;
        }
        int ownBefore = 7;
        if (myGroup == BallGroup::Solid || myGroup == BallGroup::Stripe) {
            ownBefore = 0;
            for (const auto& [origIdx, sBody] : simBalls) {
                (void)sBody;
                if (balls_[origIdx].group == myGroup) ++ownBefore;
            }
        }

        bool foul = out.scratched;
        if (out.firstHitBall < 0) {
            foul = true;
        } else {
            const BallGroup hitGroup = balls_[static_cast<std::size_t>(out.firstHitBall)].group;
            if (myGroup == BallGroup::FreeBall) {
                if (hitGroup == BallGroup::Eight) foul = true;
            } else if (ownBefore == 0) {
                if (hitGroup != BallGroup::Eight) foul = true;
            } else if (hitGroup != myGroup) {
                foul = true;
            }
        }
        out.foul = foul;

        if (eightIdx >= 0) {
            if (!foul && myGroup != BallGroup::FreeBall && ownBefore == 0) {
                out.wonGame = true;
                out.continuesTurn = true;
                out.ownPotted = 1;
                out.gainedScore = 10;
            } else {
                out.lostGame = true;
                out.foul = true;
            }
            return out;
        }
        if (foul) return out;

        BallGroup nextGrp = myGroup;
        if ((shots_ + 1) > 1 && nextGrp == BallGroup::FreeBall && (solidsPotted > 0 || stripesPotted > 0)) {
            nextGrp = solidsPotted >= stripesPotted ? BallGroup::Solid : BallGroup::Stripe;
        }
        out.nextAiGroup = nextGrp;
        if (nextGrp == BallGroup::FreeBall) {
            out.ownPotted = solidsPotted + stripesPotted;
        } else {
            out.ownPotted = (nextGrp == BallGroup::Solid) ? solidsPotted : stripesPotted;
            out.oppPotted = (solidsPotted + stripesPotted) - out.ownPotted;
        }
        out.gainedScore = out.ownPotted;
        out.continuesTurn = (out.ownPotted > 0);
        return out;
    }

    // Snooker rules.
    const int target = snookerTarget_;
    bool foul = out.scratched;
    if (out.firstHitBall < 0) {
        foul = true;
    } else {
        const int hitVal = balls_[static_cast<std::size_t>(out.firstHitBall)].number;
        if (target == 1 && hitVal != 1) foul = true;
        if (target == 8 && hitVal < 2) foul = true;
        if (target >= 2 && target <= 7 && hitVal != target) foul = true;
    }

    int redsPotted = 0, colorsPotted = 0, colorPts = 0;
    for (std::size_t bi : simPotted) {
        const int val = balls_[bi].number;
        if (val == 1) {
            ++redsPotted;
            if (target != 1) foul = true;
        } else {
            ++colorsPotted;
            colorPts += val;
            if (target == 1) foul = true;
            if (target == 8 &&
                (out.firstHitBall < 0 || val != balls_[static_cast<std::size_t>(out.firstHitBall)].number)) {
                foul = true;
            }
            if (target >= 2 && target <= 7 && val != target) foul = true;
        }
    }
    if (colorsPotted > 1) foul = true;

    // Re-spot potted Snooker colors on foul or free-color turn (`target == 8`).
    const float clearR2 = (radius_ * 2.02f) * (radius_ * 2.02f);
    for (std::size_t bi : simPotted) {
        if (balls_[bi].group == BallGroup::SnookerColor && (foul || target == 8)) {
            Vec2 chosen = balls_[bi].homeSpot;
            auto spotFreeSim = [&](Vec2 s) {
                if (!out.scratched && (out.whiteEndPhys - s).lengthSquared() < clearR2) return false;
                for (std::size_t j = 0; j < balls_.size(); ++j) {
                    if (j != bi && out.endBallActive[j] &&
                        (out.endBallPos[j] - s).lengthSquared() < clearR2) {
                        return false;
                    }
                }
                return true;
            };
            if (!spotFreeSim(chosen)) {
                for (int val = 7; val >= 2; --val) {
                    bool found = false;
                    for (const Ball& ob : balls_) {
                        if (ob.group == BallGroup::SnookerColor && ob.number == val &&
                            spotFreeSim(ob.homeSpot)) {
                            chosen = ob.homeSpot;
                            found = true;
                            break;
                        }
                    }
                    if (found) break;
                }
            }
            out.endBallActive[bi] = true;
            out.endBallPos[bi] = chosen;
        }
    }

    int redsAfter = 0;
    int lowestColorAfter = 0;
    for (std::size_t j = 0; j < balls_.size(); ++j) {
        if (!out.endBallActive[j]) continue;
        if (balls_[j].group == BallGroup::SnookerRed) {
            ++redsAfter;
        } else if (balls_[j].group == BallGroup::SnookerColor) {
            if (lowestColorAfter == 0 || balls_[j].number < lowestColorAfter) {
                lowestColorAfter = balls_[j].number;
            }
        }
    }

    out.foul = foul;
    if (foul) {
        out.nextSnookerTarget = redsAfter > 0 ? 1 : lowestColorAfter;
        return out;
    }
    if (target == 1) {
        if (redsPotted > 0) {
            out.continuesTurn = true;
            out.ownPotted = redsPotted;
            out.gainedScore = redsPotted;
            out.nextSnookerTarget = 8;
        } else {
            out.nextSnookerTarget = redsAfter > 0 ? 1 : lowestColorAfter;
        }
    } else if (target == 8) {
        if (colorsPotted == 1) {
            out.continuesTurn = true;
            out.ownPotted = 1;
            out.gainedScore = colorPts;
            out.nextSnookerTarget = redsAfter > 0 ? 1 : lowestColorAfter;
        } else {
            out.nextSnookerTarget = redsAfter > 0 ? 1 : lowestColorAfter;
        }
    } else {
        if (colorsPotted == 1) {
            out.continuesTurn = true;
            out.ownPotted = 1;
            out.gainedScore = colorPts;
            out.nextSnookerTarget = lowestColorAfter;
            if (lowestColorAfter == 0 && redsAfter == 0) {
                out.wonGame = (scores_[1] + colorPts >= scores_[0]);
            }
        } else {
            out.nextSnookerTarget = lowestColorAfter;
        }
    }
    return out;
}

int BilliardsMode::countTiedOwnBalls(const std::vector<Vec2>& ballPos, const std::vector<bool>& ballActive,
                                     BallGroup aiGrp, int snkTarget) const {
    auto segClear = [&](Vec2 a, Vec2 b, float clearance, int igA) {
        const float c2 = clearance * clearance;
        for (std::size_t i = 0; i < balls_.size(); ++i) {
            if (!ballActive[i] || static_cast<int>(i) == igA) continue;
            if (pointToSegmentDistSq(ballPos[i], a, b) < c2) return false;
        }
        return true;
    };

    int tied = 0;
    for (std::size_t j = 0; j < balls_.size(); ++j) {
        if (!ballActive[j]) continue;
        const Ball& nb = balls_[j];
        bool isOwn = false;
        if (config_.rule == BilliardsRule::Free || config_.style == BilliardsStyle::Classic) {
            isOwn = true;
        } else if (config_.rule == BilliardsRule::EightBall) {
            if (aiGrp == BallGroup::Solid || aiGrp == BallGroup::Stripe) {
                isOwn = (nb.group == aiGrp);
            } else {
                isOwn = (nb.group == BallGroup::Solid || nb.group == BallGroup::Stripe);
            }
        } else {
            isOwn = (snkTarget == 1 || snkTarget == 8) ? (nb.group == BallGroup::SnookerRed)
                                                       : (nb.group == BallGroup::SnookerColor);
        }
        if (!isOwn) continue;

        const Vec2 posN = ballPos[j];
        bool hasAnyPocketLine = false;
        for (std::size_t pk = 0; pk < pockets_.size(); ++pk) {
            const Vec2 throat = pocketThroatMeters(pk);
            const Vec2 entry = (posN - pockets_[pk]).lengthSquared() < (throat - pockets_[pk]).lengthSquared()
                                   ? pockets_[pk]
                                   : throat;
            if (!pocketPathClearJaws(pk, posN, entry)) continue;
            if (!segClear(posN, entry, 2.02f * radius_, static_cast<int>(j))) continue;
            const Vec2 potDir = normalizedOr(entry - posN, {1.0f, 0.0f});
            const Vec2 ghost = posN - potDir * (2.0f * radius_);
            if (ghost.x < tableMin_.x + radius_ * 1.03f ||
                ghost.x > tableMin_.x + tableSize_.x - radius_ * 1.03f ||
                ghost.y < tableMin_.y + radius_ * 1.03f ||
                ghost.y > tableMin_.y + tableSize_.y - radius_ * 1.03f) {
                continue;
            }
            if (!segClear(ghost, ghost, 1.98f * radius_, static_cast<int>(j))) continue;
            hasAnyPocketLine = true;
            break;
        }
        if (!hasAnyPocketLine) ++tied;
    }
    return tied;
}

bool BilliardsMode::hasVerifiedSecondPlyPot(const AiSimOutcome& afterFirstShot,
                                            float* outBestPly2Score) const {
    if (outBestPly2Score) *outBestPly2Score = -30.0f;
    if (afterFirstShot.lostGame || afterFirstShot.foul || !afterFirstShot.continuesTurn) return false;
    if (afterFirstShot.wonGame) {
        if (outBestPly2Score) *outBestPly2Score = 500.0f;
        return true;
    }

    const Vec2 w = afterFirstShot.whiteEndPhys;
    const bool desktop = (config_.style == BilliardsStyle::Desktop);
    const float topSpeed = desktop ? std::clamp(tableSize_.x * 2.0f, 16.0f, 36.0f) : config_.maxShotSpeed;

    auto segClearAfter = [&](Vec2 a, Vec2 b, float clearance, int igA) {
        const float c2 = clearance * clearance;
        for (std::size_t i = 0; i < balls_.size(); ++i) {
            if (!afterFirstShot.endBallActive[i] || static_cast<int>(i) == igA) continue;
            if (pointToSegmentDistSq(afterFirstShot.endBallPos[i], a, b) < c2) return false;
        }
        return true;
    };

    bool nextIsEight = false;
    if (config_.rule == BilliardsRule::EightBall &&
        (afterFirstShot.nextAiGroup == BallGroup::Solid || afterFirstShot.nextAiGroup == BallGroup::Stripe)) {
        int rem = 0;
        for (std::size_t j = 0; j < balls_.size(); ++j) {
            if (afterFirstShot.endBallActive[j] && balls_[j].group == afterFirstShot.nextAiGroup) ++rem;
        }
        nextIsEight = (rem == 0);
    }

    auto isLegalPly2 = [&](std::size_t bi) {
        if (!afterFirstShot.endBallActive[bi]) return false;
        const Ball& nb = balls_[bi];
        if (!desktop || config_.rule == BilliardsRule::Free) return true;
        if (config_.rule == BilliardsRule::EightBall) {
            if (nextIsEight) return nb.group == BallGroup::Eight;
            if (afterFirstShot.nextAiGroup != BallGroup::FreeBall)
                return nb.group == afterFirstShot.nextAiGroup;
            return nb.group == BallGroup::Solid || nb.group == BallGroup::Stripe;
        }
        if (afterFirstShot.nextSnookerTarget == 1) return nb.group == BallGroup::SnookerRed;
        if (afterFirstShot.nextSnookerTarget == 8) return nb.group == BallGroup::SnookerColor;
        return nb.group == BallGroup::SnookerColor && nb.number == afterFirstShot.nextSnookerTarget;
    };

    struct Ply2Cand {
        std::size_t targetIdx = 0;
        Vec2 aimPhysDir{1.0f, 0.0f};
        float power = 0.4f;
        float cueDist = 2.0f;
        float heur = -1e9f;
    };
    std::vector<Ply2Cand> cands;
    cands.reserve(16);

    for (std::size_t j = 0; j < balls_.size(); ++j) {
        if (!isLegalPly2(j)) continue;
        const Vec2 posN = afterFirstShot.endBallPos[j];
        for (std::size_t pk = 0; pk < pockets_.size(); ++pk) {
            const Vec2 throat = pocketThroatMeters(pk);
            const Vec2 entry = (posN - pockets_[pk]).lengthSquared() < (throat - pockets_[pk]).lengthSquared()
                                   ? pockets_[pk]
                                   : throat;
            if (!pocketPathClearJaws(pk, posN, entry)) continue;
            if (!segClearAfter(posN, entry, 2.02f * radius_, static_cast<int>(j))) continue;
            const Vec2 potDir = normalizedOr(entry - posN, {1.0f, 0.0f});
            const Vec2 roughGhost = posN - potDir * (2.0f * radius_);
            const Vec2 roughAim = normalizedOr(roughGhost - w, potDir);
            const float roughCos = std::clamp(roughAim.x * potDir.x + roughAim.y * potDir.y, -1.0f, 1.0f);
            const float alpha = std::acos(roughCos);
            const float muEff = std::min(0.05f, std::tan(std::min(alpha, 1.48f)) / (6.0f * 0.96f));
            const float cross = roughAim.x * potDir.y - roughAim.y * potDir.x;
            const float throwAng = 0.35f * std::atan(muEff) * (cross >= 0.0f ? 1.0f : -1.0f);
            const float tc = std::cos(throwAng), ts = std::sin(throwAng);
            const Vec2 contactNormal{potDir.x * tc - potDir.y * ts, potDir.x * ts + potDir.y * tc};

            const Vec2 ghost = posN - contactNormal * (2.0f * radius_);
            const float ply2WallPad = radius_ * 0.985f;
            if (ghost.x < tableMin_.x + ply2WallPad || ghost.x > tableMin_.x + tableSize_.x - ply2WallPad ||
                ghost.y < tableMin_.y + ply2WallPad || ghost.y > tableMin_.y + tableSize_.y - ply2WallPad)
                continue;
            if (!segClearAfter(ghost, ghost, 1.94f * radius_, static_cast<int>(j))) continue;
            const Vec2 toG = ghost - w;
            const float cueDist = toG.length();
            if (cueDist < 0.25f * radius_) continue;
            const Vec2 aimDir = toG / cueDist;
            const float cutCos = aimDir.x * contactNormal.x + aimDir.y * contactNormal.y;
            if (cutCos < 0.06f) continue;
            if (!segClearAfter(w, ghost, 1.95f * radius_, static_cast<int>(j))) continue;

            const float potDist = (entry - posN).length();
            const float v0Need =
                (2.20f + 1.08f * potDist) / std::max(0.10f, cutCos * 0.95f) + 1.02f * cueDist;
            if (v0Need > topSpeed * 1.18f) continue;

            Ply2Cand pc;
            pc.targetIdx = j;
            pc.aimPhysDir = aimDir;
            pc.power = std::clamp(v0Need / topSpeed, 0.14f, 0.94f);
            pc.cueDist = std::max(2.0f * radius_, cueDist);
            pc.heur = cutCos * 65.0f - cueDist * 4.0f - potDist * 6.5f +
                      (config_.rule == BilliardsRule::Snooker && afterFirstShot.nextSnookerTarget == 8
                           ? static_cast<float>(balls_[j].number) * 5.0f
                           : 0.0f);
            cands.push_back(pc);
        }
    }

    if (cands.empty()) return false;
    std::sort(cands.begin(), cands.end(),
              [](const Ply2Cand& a, const Ply2Cand& b) { return a.heur > b.heur; });

    // Simulate top 2 second-ply pots in real Box2D physics on `afterFirstShot`'s resting table!
    const BodyMaterial ballMat{1.0f, 0.05f, 0.92f, desktop ? 0.95f : 0.5f, 1.0f, desktop};
    BodyMaterial whiteMat = ballMat;
    whiteMat.bullet = true;
    const float drag = std::clamp(tableSize_.x * 0.025f, 0.20f, 0.45f);
    const float restS2 = config_.restSpeed * config_.restSpeed;
    constexpr std::array<float, 5> kPly2MicroR = {0.0f, -0.028f, 0.028f, -0.065f, 0.065f};

    bool anyConfirmed = false;
    float bestPly2 = -30.0f;
    const std::size_t limit = std::min<std::size_t>(cands.size(), 4u);

    for (std::size_t ci = 0; ci < limit; ++ci) {
        const Ply2Cand& pc = cands[ci];
        for (float gr : kPly2MicroR) {
            const float rad = std::atan2(gr * radius_, pc.cueDist);
            const float mc = std::cos(rad), ms = std::sin(rad);
            const Vec2 rawDir{pc.aimPhysDir.x * mc - pc.aimPhysDir.y * ms,
                              pc.aimPhysDir.x * ms + pc.aimPhysDir.y * mc};
            const Vec2 uScene = normalizedOr(coords_.velocityToScene(rawDir), {1.0f, 0.0f});
            const Vec2 dirUnit = normalizedOr(coords_.velocityToPhysics(uScene), {1.0f, 0.0f});

            PhysicsWorld sim(tablePhysics());
            if (desktop) {
                populateDesktopTableCushions(sim);
            } else {
                sim.createBoundary(tableMin_, tableSize_, true, 1.0f, 0.1f, 0.85f);
            }
            const BodyIndex simWhite = sim.addCircle(w, radius_, whiteMat);
            std::vector<std::pair<std::size_t, BodyIndex>> simBalls;
            simBalls.reserve(balls_.size());
            std::vector<bool> act3 = afterFirstShot.endBallActive;
            std::vector<Vec2> pos3 = afterFirstShot.endBallPos;
            for (std::size_t bi = 0; bi < balls_.size(); ++bi) {
                if (!act3[bi]) continue;
                simBalls.push_back({bi, sim.addCircle(pos3[bi], radius_, ballMat)});
            }

            sim.setVelocity(simWhite, dirUnit * (topSpeed * pc.power), 0.0f);
            sim.wake(simWhite);
            int firstHit = -1;
            bool scratched = false;
            std::vector<std::size_t> potted2;

            constexpr float kStep = 1.0f / 60.0f;
            for (int step = 0; step < 220; ++step) {
                sim.step(kStep);
                for (const CollisionHit& h : sim.lastHits()) {
                    std::uint32_t idA = h.bodyA ? (*h.bodyA + 1u) : 0u;
                    std::uint32_t idB = h.bodyB ? (*h.bodyB + 1u) : 0u;
                    if (idA > idB) std::swap(idA, idB);
                    if (idA == 1u && idB >= 2u && firstHit < 0) {
                        const std::size_t slot = static_cast<std::size_t>(idB - 2u);
                        if (slot < simBalls.size()) firstHit = static_cast<int>(simBalls[slot].first);
                    }
                }
                if (!scratched) {
                    const BodyState ws = sim.state(simWhite);
                    const float spd = ws.linearVelocity.length();
                    if (spd > 1e-4f && spd < 1.8f) {
                        sim.setVelocity(simWhite,
                                        ws.linearVelocity * (std::max(0.0f, spd - drag * kStep) / spd), 0.0f);
                    }
                }
                for (const auto& [origIdx, sBody] : simBalls) {
                    if (!act3[origIdx]) continue;
                    const BodyState bs = sim.state(sBody);
                    const float spd = bs.linearVelocity.length();
                    if (spd > 1e-4f && spd < 1.8f) {
                        sim.setVelocity(sBody, bs.linearVelocity * (std::max(0.0f, spd - drag * kStep) / spd),
                                        0.0f);
                    }
                    if (matchingPocket(bs.position) >= 0) {
                        sim.setEnabled(sBody, false);
                        act3[origIdx] = false;
                        potted2.push_back(origIdx);
                    }
                }
                if (!scratched && matchingPocket(sim.state(simWhite).position) >= 0) {
                    scratched = true;
                    break;
                }
                bool stopped = scratched || sim.state(simWhite).linearVelocity.lengthSquared() <= restS2;
                if (stopped) {
                    for (const auto& [origIdx, sBody] : simBalls) {
                        if (act3[origIdx] && sim.state(sBody).linearVelocity.lengthSquared() > restS2) {
                            stopped = false;
                            break;
                        }
                    }
                }
                if (stopped) break;
            }

            if (scratched || firstHit < 0 || !isLegalPly2(static_cast<std::size_t>(firstHit))) continue;
            bool legalPot2 = false;
            bool foulPot2 = false;
            for (std::size_t pbi : potted2) {
                if (config_.rule == BilliardsRule::EightBall) {
                    if (balls_[pbi].group == BallGroup::Eight && !nextIsEight) foulPot2 = true;
                    if (isLegalPly2(pbi)) legalPot2 = true;
                } else if (config_.rule == BilliardsRule::Snooker) {
                    if (!isLegalPly2(pbi) ||
                        (afterFirstShot.nextSnookerTarget == 8 &&
                         balls_[pbi].number != balls_[static_cast<std::size_t>(firstHit)].number)) {
                        foulPot2 = true;
                    } else {
                        legalPot2 = true;
                    }
                    if (afterFirstShot.nextSnookerTarget == 8 && potted2.size() > 1) foulPot2 = true;
                } else {
                    legalPot2 = true;
                }
            }
            if (foulPot2 || !legalPot2) continue;

            anyConfirmed = true;
            AiSimOutcome out2;
            out2.continuesTurn = true;
            out2.whiteEndPhys = sim.state(simWhite).position;
            out2.endBallActive = act3;
            out2.endBallPos = pos3;
            for (const auto& [origIdx, sBody] : simBalls) {
                if (act3[origIdx]) out2.endBallPos[origIdx] = sim.state(sBody).position;
            }
            out2.nextAiGroup = afterFirstShot.nextAiGroup;
            if (config_.rule == BilliardsRule::EightBall && out2.nextAiGroup == BallGroup::FreeBall) {
                out2.nextAiGroup = balls_[potted2.front()].group;
            }
            out2.nextSnookerTarget = afterFirstShot.nextSnookerTarget;
            if (config_.rule == BilliardsRule::Snooker) {
                if (afterFirstShot.nextSnookerTarget == 8 &&
                    balls_[potted2.front()].group == BallGroup::SnookerColor) {
                    out2.endBallActive[potted2.front()] = true;
                    out2.endBallPos[potted2.front()] = balls_[potted2.front()].homeSpot;
                }
                int redsLeft3 = 0, lowCol3 = 0;
                for (std::size_t k = 0; k < balls_.size(); ++k) {
                    if (!out2.endBallActive[k]) continue;
                    if (balls_[k].group == BallGroup::SnookerRed) ++redsLeft3;
                    if (balls_[k].group == BallGroup::SnookerColor &&
                        (lowCol3 == 0 || balls_[k].number < lowCol3)) {
                        lowCol3 = balls_[k].number;
                    }
                }
                out2.nextSnookerTarget =
                    (afterFirstShot.nextSnookerTarget == 1) ? 8 : (redsLeft3 > 0 ? 1 : lowCol3);
            }
            if (nextIsEight) out2.wonGame = true;
            bestPly2 = std::max(bestPly2, out2.wonGame ? 300.0f : evaluatePostRolloutPosition(out2, -1));
            break;
        }
    }

    if (outBestPly2Score) *outBestPly2Score = bestPly2;
    return anyConfirmed;
}

float BilliardsMode::evaluatePostRolloutPosition(const AiSimOutcome& out, int tiedBefore) const {
    if (out.lostGame) return -100000.0f;
    if (out.wonGame) return 100000.0f;
    if (out.foul) return -5000.0f;

    const Vec2 w = out.whiteEndPhys;
    const float left = tableMin_.x + 1.55f * radius_;
    const float right = tableMin_.x + tableSize_.x - 1.55f * radius_;
    const float bottom = tableMin_.y + 1.55f * radius_;
    const float top = tableMin_.y + tableSize_.y - 1.55f * radius_;

    float railPenalty = 0.0f;
    if (w.x < left || w.x > right) railPenalty -= 9.5f;
    if (w.y < bottom || w.y > top) railPenalty -= 9.5f;
    for (const Vec2& pk : pockets_) {
        if ((w - pk).lengthSquared() < (3.4f * radius_) * (3.4f * radius_)) {
            railPenalty -= 14.0f;  // Tucked awkwardly inside pocket jaws
        }
    }

    auto segClearAfter = [&](Vec2 a, Vec2 b, float clearance, int igA, int igB) {
        const float c2 = clearance * clearance;
        for (std::size_t i = 0; i < balls_.size(); ++i) {
            if (!out.endBallActive[i]) continue;
            const int idx = static_cast<int>(i);
            if (idx == igA || idx == igB) continue;
            if (pointToSegmentDistSq(out.endBallPos[i], a, b) < c2) return false;
        }
        return true;
    };

    bool nextIsEight = false;
    int groupRemAfter = 0;
    if (config_.rule == BilliardsRule::EightBall &&
        (out.nextAiGroup == BallGroup::Solid || out.nextAiGroup == BallGroup::Stripe)) {
        for (std::size_t j = 0; j < balls_.size(); ++j) {
            if (out.endBallActive[j] && balls_[j].group == out.nextAiGroup) ++groupRemAfter;
        }
        nextIsEight = (groupRemAfter == 0);
    }

    float bestNext = -18.0f;
    float secondNext = -18.0f;
    int makableBallCount = 0;

    for (std::size_t j = 0; j < balls_.size(); ++j) {
        if (!out.endBallActive[j]) continue;
        const Ball& nb = balls_[j];
        bool legalNext = false;
        float valBonus = 0.0f;

        if (config_.rule == BilliardsRule::Free || config_.style == BilliardsStyle::Classic) {
            legalNext = true;
        } else if (config_.rule == BilliardsRule::EightBall) {
            if (nextIsEight) {
                legalNext = (nb.group == BallGroup::Eight);
                valBonus = 42.0f;
            } else if (out.nextAiGroup != BallGroup::FreeBall) {
                legalNext = (nb.group == out.nextAiGroup);
            } else {
                legalNext = (nb.group == BallGroup::Solid || nb.group == BallGroup::Stripe);
            }
        } else {
            if (out.nextSnookerTarget == 1) {
                legalNext = (nb.group == BallGroup::SnookerRed);
            } else if (out.nextSnookerTarget == 8) {
                legalNext = (nb.group == BallGroup::SnookerColor);
                valBonus = static_cast<float>(nb.number) * 3.6f;
            } else {
                legalNext = (nb.group == BallGroup::SnookerColor && nb.number == out.nextSnookerTarget);
                valBonus = 18.0f;
            }
        }
        if (!legalNext) continue;

        const Vec2 posN = out.endBallPos[j];
        float bestForBall = -1e6f;
        for (std::size_t pk = 0; pk < pockets_.size(); ++pk) {
            const Vec2 throat = pocketThroatMeters(pk);
            const Vec2 entry = (posN - pockets_[pk]).lengthSquared() < (throat - pockets_[pk]).lengthSquared()
                                   ? pockets_[pk]
                                   : throat;
            if (!pocketPathClearJaws(pk, posN, entry)) continue;
            if (!segClearAfter(posN, entry, 2.02f * radius_, static_cast<int>(j), -1)) continue;
            const Vec2 potDir = normalizedOr(entry - posN, {1.0f, 0.0f});
            const Vec2 ghost = posN - potDir * (2.0f * radius_);
            if (ghost.x < tableMin_.x + radius_ * 1.03f ||
                ghost.x > tableMin_.x + tableSize_.x - radius_ * 1.03f ||
                ghost.y < tableMin_.y + radius_ * 1.03f ||
                ghost.y > tableMin_.y + tableSize_.y - radius_ * 1.03f) {
                continue;
            }
            const Vec2 toGhost = ghost - w;
            const float dist = toGhost.length();
            if (dist < 2.05f * radius_ || dist > tableSize_.x * 0.92f) continue;
            const Vec2 cueDir = toGhost / dist;
            const float cutCos = cueDir.x * potDir.x + cueDir.y * potDir.y;
            if (cutCos < 0.22f) continue;
            if (!segClearAfter(w, ghost, 2.03f * radius_, static_cast<int>(j), -1)) continue;

            const float potDist = (entry - posN).length();
            const float idealDist = tableSize_.y * 0.34f;
            const float distQuality = std::max(0.0f, 1.0f - std::abs(dist - idealDist) / tableSize_.x);
            const float sidePen = (pk == 1 || pk == 4) ? 2.5f : 0.0f;
            const float sc = cutCos * 28.0f + distQuality * 14.0f - potDist * 2.4f + valBonus - sidePen;
            bestForBall = std::max(bestForBall, sc);
        }

        if (bestForBall > -1e5f) {
            ++makableBallCount;
            if (bestForBall > bestNext) {
                secondNext = bestNext;
                bestNext = bestForBall;
            } else if (bestForBall > secondNext) {
                secondNext = bestForBall;
            }
        }
    }

    float clusterOpenBonus = static_cast<float>(std::min(makableBallCount, 6)) * 8.5f;
    if (secondNext > 0.0f) clusterOpenBonus += secondNext * 0.22f;
    if (nextIsEight && makableBallCount == 0) {
        // Strongly discourage potting the last group ball while hooking ourselves on the 8-ball!
        clusterOpenBonus -= 42.0f;
    }

    // On an open 8-Ball table where this shot locks AI's group, favor the group with fewer remaining & fewer
    // tied-up balls.
    if (config_.rule == BilliardsRule::EightBall && playerGroups_[1] == BallGroup::FreeBall &&
        (out.nextAiGroup == BallGroup::Solid || out.nextAiGroup == BallGroup::Stripe)) {
        const BallGroup oppGrp = out.nextAiGroup == BallGroup::Solid ? BallGroup::Stripe : BallGroup::Solid;
        int oppRem = 0;
        for (std::size_t j = 0; j < balls_.size(); ++j) {
            if (out.endBallActive[j] && balls_[j].group == oppGrp) ++oppRem;
        }
        clusterOpenBonus += static_cast<float>(oppRem - groupRemAfter) * 6.0f;
    }

    // Active K-Ball / Cluster Breakout bonus: reward freeing previously dead/tied own balls while retaining a
    // playable next shot!
    if (tiedBefore > 0 && makableBallCount > 0) {
        const int tiedAfter =
            countTiedOwnBalls(out.endBallPos, out.endBallActive, out.nextAiGroup, out.nextSnookerTarget);
        if (tiedAfter < tiedBefore) {
            clusterOpenBonus += static_cast<float>(tiedBefore - tiedAfter) * 52.0f;
        }
    }

    return bestNext + clusterOpenBonus + railPenalty;
}

float BilliardsMode::evaluateOpponentThreat(const AiSimOutcome& out) const {
    if (out.lostGame) return 100000.0f;
    if (out.foul) return 420.0f;  // Handing human ball-in-hand or penalty points is huge threat!

    const Vec2 w = out.whiteEndPhys;
    auto segClearAfter = [&](Vec2 a, Vec2 b, float clearance, int igA, int igB) {
        const float c2 = clearance * clearance;
        for (std::size_t i = 0; i < balls_.size(); ++i) {
            if (!out.endBallActive[i]) continue;
            const int idx = static_cast<int>(i);
            if (idx == igA || idx == igB) continue;
            if (pointToSegmentDistSq(out.endBallPos[i], a, b) < c2) return false;
        }
        return true;
    };

    const BallGroup p0Group =
        (config_.rule == BilliardsRule::EightBall && playerGroups_[0] == BallGroup::FreeBall &&
         (out.nextAiGroup == BallGroup::Solid || out.nextAiGroup == BallGroup::Stripe))
            ? (out.nextAiGroup == BallGroup::Solid ? BallGroup::Stripe : BallGroup::Solid)
            : playerGroups_[0];

    bool p0OnEight = false;
    if (config_.rule == BilliardsRule::EightBall &&
        (p0Group == BallGroup::Solid || p0Group == BallGroup::Stripe)) {
        int p0Rem = 0;
        for (std::size_t j = 0; j < balls_.size(); ++j) {
            if (out.endBallActive[j] && balls_[j].group == p0Group) ++p0Rem;
        }
        p0OnEight = (p0Rem == 0);
    }

    bool humanCanSeeAnyLegal = false;
    float maxHumanPot = 0.0f;
    int humanPotCount = 0;
    float minHumanDist = tableSize_.x * 2.0f;

    for (std::size_t j = 0; j < balls_.size(); ++j) {
        if (!out.endBallActive[j]) continue;
        const Ball& hb = balls_[j];
        bool legalForHuman = false;
        if (config_.rule == BilliardsRule::Free || config_.style == BilliardsStyle::Classic) {
            legalForHuman = true;
        } else if (config_.rule == BilliardsRule::EightBall) {
            if (p0OnEight) {
                legalForHuman = (hb.group == BallGroup::Eight);
            } else if (p0Group != BallGroup::FreeBall) {
                legalForHuman = (hb.group == p0Group);
            } else {
                legalForHuman = (hb.group == BallGroup::Solid || hb.group == BallGroup::Stripe);
            }
        } else {
            if (out.nextSnookerTarget == 1) {
                legalForHuman = (hb.group == BallGroup::SnookerRed);
            } else if (out.nextSnookerTarget == 8) {
                legalForHuman = (hb.group == BallGroup::SnookerColor);
            } else {
                legalForHuman = (hb.group == BallGroup::SnookerColor && hb.number == out.nextSnookerTarget);
            }
        }
        if (!legalForHuman) continue;

        const Vec2 posH = out.endBallPos[j];
        const Vec2 toH = posH - w;
        const float dist = toH.length();
        minHumanDist = std::min(minHumanDist, dist);

        if (dist > 2.01f * radius_) {
            const Vec2 dirH = toH / dist;
            const Vec2 perp{-dirH.y, dirH.x};
            for (float edge : {0.0f, 0.72f, -0.72f}) {
                const Vec2 contact = posH - dirH * (2.0f * radius_ * std::sqrt(1.0f - edge * edge)) +
                                     perp * (2.0f * radius_ * edge);
                if (segClearAfter(w, contact, 2.01f * radius_, static_cast<int>(j), -1)) {
                    humanCanSeeAnyLegal = true;
                    break;
                }
            }
        }

        for (std::size_t pk = 0; pk < pockets_.size(); ++pk) {
            const Vec2 throat = pocketThroatMeters(pk);
            const Vec2 entry = (posH - pockets_[pk]).lengthSquared() < (throat - pockets_[pk]).lengthSquared()
                                   ? pockets_[pk]
                                   : throat;
            if (!pocketPathClearJaws(pk, posH, entry)) continue;
            if (!segClearAfter(posH, entry, 2.02f * radius_, static_cast<int>(j), -1)) continue;
            const Vec2 potDir = normalizedOr(entry - posH, {1.0f, 0.0f});
            const Vec2 ghost = posH - potDir * (2.0f * radius_);
            const Vec2 toG = ghost - w;
            const float dG = toG.length();
            if (dG < 2.02f * radius_) continue;
            const Vec2 cueDir = toG / dG;
            const float cutCos = cueDir.x * potDir.x + cueDir.y * potDir.y;
            if (cutCos < 0.14f) continue;
            if (!segClearAfter(w, ghost, 2.02f * radius_, static_cast<int>(j), -1)) continue;

            const float potDist = (entry - posH).length();
            const float potThreat =
                std::max(4.0f, cutCos * 75.0f - dG * 4.5f - potDist * 7.0f + (p0OnEight ? 60.0f : 0.0f));
            maxHumanPot = std::max(maxHumanPot, potThreat);
            ++humanPotCount;
        }
    }

    // WPBSA Snooker Baulk-zone tactical safety adjustment: strongly reward returning cue ball behind the
    // Baulk line / near the Baulk rail, and penalize leaving cue ball down in the Red/Black scoring zone!
    float snookerZoneAdj = 0.0f;
    if (config_.rule == BilliardsRule::Snooker && config_.style == BilliardsStyle::Desktop) {
        const float baulkX = tableMin_.x + tableSize_.x * BilliardsConfig::kSnookerBaulkRatio;
        if (w.x <= baulkX + 1.2f * radius_) {
            snookerZoneAdj -= 26.0f;
            if (w.x <= tableMin_.x + 3.8f * radius_) snookerZoneAdj -= 10.0f;
        } else if (out.nextSnookerTarget == 1 && w.x >= tableMin_.x + tableSize_.x * 0.62f) {
            snookerZoneAdj += 20.0f;
        }
    }

    if (!humanCanSeeAnyLegal) {
        // Successfully hooked / snookered the human player behind a blocker ball!
        return -95.0f - std::min(25.0f, minHumanDist * 2.5f) + snookerZoneAdj * 0.35f;
    }
    const float distSafetyBonus = std::min(22.0f, minHumanDist * 3.2f);
    return maxHumanPot + static_cast<float>(std::min(humanPotCount, 5)) * 7.0f - distSafetyBonus +
           snookerZoneAdj;
}

float BilliardsMode::evaluateNextBallPosition(Vec2 estWhiteRest, std::size_t pottedBallIdx) const {
    const float left = tableMin_.x + 1.6f * radius_;
    const float right = tableMin_.x + tableSize_.x - 1.6f * radius_;
    const float bottom = tableMin_.y + 1.6f * radius_;
    const float top = tableMin_.y + tableSize_.y - 1.6f * radius_;

    float railPenalty = 0.0f;
    if (estWhiteRest.x < left || estWhiteRest.x > right) railPenalty -= 8.0f;
    if (estWhiteRest.y < bottom || estWhiteRest.y > top) railPenalty -= 8.0f;

    // Determine which balls will be legal on the AI's NEXT shot after potting `pottedBallIdx`.
    int nextSnookerTarget = snookerTarget_;
    if (config_.rule == BilliardsRule::Snooker) {
        const Ball& potted = balls_[pottedBallIdx];
        if (snookerTarget_ == 1 && potted.group == BallGroup::SnookerRed) {
            nextSnookerTarget = 8;  // Any color after a Red
        } else if (snookerTarget_ == 8) {
            nextSnookerTarget = remainingReds() > 0 ? 1 : lowestRemainingColorValue();
        } else {
            nextSnookerTarget = 0;
            for (std::size_t j = 0; j < balls_.size(); ++j) {
                if (j == pottedBallIdx || balls_[j].pocketed || balls_[j].group != BallGroup::SnookerColor)
                    continue;
                if (nextSnookerTarget == 0 || balls_[j].number < nextSnookerTarget) {
                    nextSnookerTarget = balls_[j].number;
                }
            }
        }
    }

    BallGroup nextEightGroup = playerGroups_[1];
    bool nextIsEight = false;
    if (config_.rule == BilliardsRule::EightBall) {
        const Ball& potted = balls_[pottedBallIdx];
        if (nextEightGroup == BallGroup::FreeBall &&
            (potted.group == BallGroup::Solid || potted.group == BallGroup::Stripe)) {
            nextEightGroup = potted.group;
        }
        if (nextEightGroup == BallGroup::Solid || nextEightGroup == BallGroup::Stripe) {
            int rem = 0;
            for (std::size_t j = 0; j < balls_.size(); ++j) {
                if (j != pottedBallIdx && !balls_[j].pocketed && balls_[j].group == nextEightGroup) ++rem;
            }
            nextIsEight = (rem == 0);
        }
    }

    float bestNext = -12.0f;
    for (std::size_t j = 0; j < balls_.size(); ++j) {
        if (balls_[j].pocketed) continue;
        // Note: in Snooker when potting a Color on `snookerTarget_ == 8`, that color re-spots, so skip it
        // only when it stays pocketed.
        const bool staysPocketed = !(config_.rule == BilliardsRule::Snooker && snookerTarget_ == 8 &&
                                     balls_[pottedBallIdx].group == BallGroup::SnookerColor);
        if (j == pottedBallIdx && staysPocketed) continue;

        const Ball& nb = balls_[j];
        bool legalNext = false;
        float valBonus = 0.0f;
        if (config_.rule == BilliardsRule::Free) {
            legalNext = true;
        } else if (config_.rule == BilliardsRule::EightBall) {
            if (nextIsEight) {
                legalNext = (nb.group == BallGroup::Eight);
                valBonus = 12.0f;
            } else if (nextEightGroup != BallGroup::FreeBall) {
                legalNext = (nb.group == nextEightGroup);
            } else {
                legalNext = (nb.group != BallGroup::Eight);
            }
        } else {
            if (nextSnookerTarget == 1) {
                legalNext = (nb.group == BallGroup::SnookerRed);
            } else if (nextSnookerTarget == 8) {
                legalNext = (nb.group == BallGroup::SnookerColor);
                valBonus = static_cast<float>(nb.number) * 2.2f;
            } else {
                legalNext = (nb.group == BallGroup::SnookerColor && nb.number == nextSnookerTarget);
                valBonus = 10.0f;
            }
        }
        if (!legalNext) continue;

        const Vec2 posN =
            (j == pottedBallIdx && !staysPocketed) ? nb.homeSpot : world_.state(nb.body).position;
        for (std::size_t pk = 0; pk < pockets_.size(); ++pk) {
            const Vec2 throat = pocketThroatMeters(pk);
            const Vec2 entry = (posN - pockets_[pk]).lengthSquared() < (throat - pockets_[pk]).lengthSquared()
                                   ? pockets_[pk]
                                   : throat;
            if (!pocketPathClearJaws(pk, posN, entry)) continue;
            if (!segmentClearOfBalls(posN, entry, 2.02f * radius_, static_cast<int>(j),
                                     staysPocketed ? static_cast<int>(pottedBallIdx) : -1)) {
                continue;
            }
            const Vec2 potDir = normalizedOr(entry - posN, {1.0f, 0.0f});
            const Vec2 ghost = posN - potDir * (2.0f * radius_);
            const Vec2 toGhost = ghost - estWhiteRest;
            const float dist = toGhost.length();
            if (dist < 2.2f * radius_ || dist > tableSize_.x * 0.85f) continue;
            const Vec2 cueDir = toGhost / dist;
            const float cutCos = cueDir.x * potDir.x + cueDir.y * potDir.y;
            if (cutCos < 0.35f) continue;
            if (!segmentClearOfBalls(estWhiteRest, ghost, 2.02f * radius_, static_cast<int>(j),
                                     staysPocketed ? static_cast<int>(pottedBallIdx) : -1)) {
                continue;
            }
            const float idealDist = tableSize_.y * 0.38f;
            const float distQuality = std::max(0.0f, 1.0f - std::abs(dist - idealDist) / tableSize_.x);
            const float score = cutCos * 18.0f + distQuality * 10.0f + valBonus;
            bestNext = std::max(bestNext, score);
        }
    }
    return bestNext + railPenalty;
}

Vec2 BilliardsMode::planAiWhitePlacementPhys() const {
    // In Snooker, prefer authentic D-zone candidates first; in 8-Ball / Free, place behind the best runout
    // starter!
    std::vector<Vec2> candidates;
    const float left = tableMin_.x, bottom = tableMin_.y;
    const float midY = bottom + tableSize_.y * 0.5f;

    if (config_.rule == BilliardsRule::Snooker) {
        const float baulkX = left + tableSize_.x * BilliardsConfig::kSnookerBaulkRatio;
        const float dR = tableSize_.y * BilliardsConfig::kSnookerDRatio;
        for (int ri = 1; ri <= 4; ++ri) {
            const float rr = dR * (0.22f * static_cast<float>(ri));
            for (int ai = -6; ai <= 6; ++ai) {
                const float ang = static_cast<float>(ai) * (1.45f / 6.0f);
                const Vec2 cand{baulkX - std::cos(ang) * rr, midY + std::sin(ang) * rr};
                if (canPlaceWhite(coords_.toScene(cand))) candidates.push_back(cand);
            }
        }
    }

    if (candidates.empty() || config_.aiDifficulty != BilliardsAiDifficulty::Easy) {
        const bool snookerOnlyD = (config_.rule == BilliardsRule::Snooker && !candidates.empty());
        if (!snookerOnlyD) {
            for (std::size_t bi = 0; bi < balls_.size(); ++bi) {
                if (!isLegalTargetBall(bi, 1)) continue;
                const Vec2 posB = world_.state(balls_[bi].body).position;
                for (std::size_t pk = 0; pk < pockets_.size(); ++pk) {
                    const Vec2 throat = pocketThroatMeters(pk);
                    const Vec2 entry =
                        (posB - pockets_[pk]).lengthSquared() < (throat - pockets_[pk]).lengthSquared()
                            ? pockets_[pk]
                            : throat;
                    if (!pocketPathClearJaws(pk, posB, entry)) continue;
                    if (!segmentClearOfBalls(posB, entry, 2.02f * radius_, static_cast<int>(bi))) continue;
                    const Vec2 potDir = normalizedOr(entry - posB, {1.0f, 0.0f});
                    // Include natural 12-22 deg shape angles so Master gets natural cue-ball drift to next
                    // ball.
                    for (float backMul : {4.8f, 7.2f, 10.2f}) {
                        for (float sideAngle : {0.0f, 0.22f, -0.22f, 0.36f, -0.36f}) {
                            const float cs = std::cos(sideAngle), sn = std::sin(sideAngle);
                            const Vec2 backDir{-(potDir.x * cs - potDir.y * sn),
                                               -(potDir.x * sn + potDir.y * cs)};
                            const Vec2 cand = posB + backDir * (backMul * radius_);
                            if (canPlaceWhite(coords_.toScene(cand))) candidates.push_back(cand);
                        }
                    }
                }
            }
        }
    }

    // Fallback grid scan across table so placement is guaranteed to succeed even on crowded tables.
    if (candidates.empty()) {
        for (int ix = 1; ix <= 15; ++ix) {
            for (int iy = 1; iy <= 9; ++iy) {
                const Vec2 cand{left + tableSize_.x * (static_cast<float>(ix) / 16.0f),
                                bottom + tableSize_.y * (static_cast<float>(iy) / 10.0f)};
                if (canPlaceWhite(coords_.toScene(cand))) candidates.push_back(cand);
            }
        }
    }

    if (candidates.empty()) return whiteSpawn_;
    if (config_.aiDifficulty == BilliardsAiDifficulty::Easy) {
        return candidates.front();
    }

    const int solidCount = remainingGroupBalls(BallGroup::Solid);
    const int stripeCount = remainingGroupBalls(BallGroup::Stripe);

    // Score each candidate placement by immediate pot reliability + 2-ply follow-up position!
    Vec2 bestSpot = candidates.front();
    float bestScore = -1e6f;
    for (const Vec2& cand : candidates) {
        for (std::size_t bi = 0; bi < balls_.size(); ++bi) {
            if (!isLegalTargetBall(bi, 1)) continue;
            const Vec2 posB = world_.state(balls_[bi].body).position;
            for (std::size_t pk = 0; pk < pockets_.size(); ++pk) {
                const Vec2 throat = pocketThroatMeters(pk);
                const Vec2 entry =
                    (posB - pockets_[pk]).lengthSquared() < (throat - pockets_[pk]).lengthSquared()
                        ? pockets_[pk]
                        : throat;
                if (!pocketPathClearJaws(pk, posB, entry)) continue;
                if (!segmentClearOfBalls(posB, entry, 2.02f * radius_, static_cast<int>(bi))) continue;
                const Vec2 potDir = normalizedOr(entry - posB, {1.0f, 0.0f});
                const Vec2 ghost = posB - potDir * (2.0f * radius_);
                const Vec2 toGhost = ghost - cand;
                const float d = toGhost.length();
                if (d < 2.2f * radius_) continue;
                const Vec2 cueDir = toGhost / d;
                const float cutCos = cueDir.x * potDir.x + cueDir.y * potDir.y;
                if (cutCos < 0.45f) continue;
                if (!segmentClearOfBalls(cand, ghost, 2.04f * radius_, static_cast<int>(bi))) continue;

                const float potDist = (entry - posB).length();
                float sc = cutCos * 56.0f - (d + potDist * 1.4f) * 3.4f +
                           static_cast<float>(balls_[bi].number) *
                               (config_.rule == BilliardsRule::Snooker ? 3.2f : 0.0f);
                if (config_.rule == BilliardsRule::EightBall && playerGroups_[1] == BallGroup::FreeBall) {
                    if (balls_[bi].group == BallGroup::Solid && solidCount < stripeCount) sc += 10.0f;
                    if (balls_[bi].group == BallGroup::Stripe && stripeCount < solidCount) sc += 10.0f;
                }
                if (config_.aiDifficulty == BilliardsAiDifficulty::Master ||
                    config_.aiDifficulty == BilliardsAiDifficulty::Goat) {
                    const Vec2 vTan = cueDir - potDir * cutCos;
                    const Vec2 estNaturalRest = ghost + vTan * (3.5f * radius_);
                    const float wMul = (config_.aiDifficulty == BilliardsAiDifficulty::Goat) ? 1.55f : 1.15f;
                    sc += evaluateNextBallPosition(estNaturalRest, bi) * wMul;
                }
                if (sc > bestScore) {
                    bestScore = sc;
                    bestSpot = cand;
                }
            }
        }
    }
    return bestSpot;
}

void BilliardsMode::planAiShot() {
    aiTempoScale_ = 1.0f;
    const Vec2 whitePos = world_.state(white_).position;
    const bool desktop = (config_.style == BilliardsStyle::Desktop);
    const float topSpeed = desktop ? std::clamp(tableSize_.x * 2.0f, 16.0f, 36.0f) : config_.maxShotSpeed;
    const float minWallX = tableMin_.x + radius_ * 1.03f;
    const float maxWallX = tableMin_.x + tableSize_.x - radius_ * 1.03f;
    const float minWallY = tableMin_.y + radius_ * 1.03f;
    const float maxWallY = tableMin_.y + tableSize_.y - radius_ * 1.03f;

    // Advance deterministic AI RNG mixed with current table state.
    const auto qx = static_cast<std::uint32_t>(std::lround(std::abs(whitePos.x) * 1000.0f));
    const auto qy = static_cast<std::uint32_t>(std::lround(std::abs(whitePos.y) * 1000.0f));
    aiRngState_ ^=
        (qx * 2654435761u) ^ (qy * 2246822519u) ^ (static_cast<std::uint32_t>(shots_ + 1) * 3266489917u);
    auto nextRand01 = [&]() -> float {
        aiRngState_ ^= aiRngState_ << 13;
        aiRngState_ ^= aiRngState_ >> 17;
        aiRngState_ ^= aiRngState_ << 5;
        return static_cast<float>(aiRngState_ & 0x00FFFFFFu) / static_cast<float>(0x01000000u);
    };
    auto nextSignedJitter = [&]() -> float { return (nextRand01() - nextRand01()); };

    const BilliardsAiDifficulty diff = config_.aiDifficulty;
    const bool isPro = (diff == BilliardsAiDifficulty::Master || diff == BilliardsAiDifficulty::Goat);
    const bool isGoat = (diff == BilliardsAiDifficulty::Goat);
    const float ghostWallPad = isGoat ? (radius_ * 0.985f) : (radius_ * 1.03f);
    const float minGhostX = tableMin_.x + ghostWallPad;
    const float maxGhostX = tableMin_.x + tableSize_.x - ghostWallPad;
    const float minGhostY = tableMin_.y + ghostWallPad;
    const float maxGhostY = tableMin_.y + tableSize_.y - ghostWallPad;
    const float potClearR = isGoat ? (1.94f * radius_) : (2.02f * radius_);
    const float cueClearR = isGoat ? (1.95f * radius_) : (2.03f * radius_);
    aiIntentTag_[0] = '\0';

    // Dedicated AI Opening Break Routine (`shots_ == 0` when AI breaks in alternating-break matches):
    // - 8-Ball / Free: Power stun-draw rack smash (`大力冲球`) with rollout scratch-prevention.
    // - Snooker: WPBSA tour-style thin safety off outer corner Red returning cue ball to Baulk (`防守开球`).
    const bool isOpeningRackBreak = (shots_ == 0 && startingPlayer_ == 1 && desktop && balls_.size() >= 6 &&
                                     pocketedCount_ == 0 && playerGroups_[0] == BallGroup::FreeBall &&
                                     playerGroups_[1] == BallGroup::FreeBall && snookerTarget_ == 1);
    if (isOpeningRackBreak) {
        if (config_.rule != BilliardsRule::Snooker) {
            std::size_t apexIdx = 0;
            float bestApexX = 1e9f;
            for (std::size_t bi = 0; bi < balls_.size(); ++bi) {
                if (!isLegalTargetBall(bi, 1)) continue;
                const float bx = world_.state(balls_[bi].body).position.x;
                if (bx < bestApexX) {
                    bestApexX = bx;
                    apexIdx = bi;
                }
            }
            const Vec2 apexPos = world_.state(balls_[apexIdx].body).position;
            const Vec2 baseDir = normalizedOr(apexPos - whitePos, {1.0f, 0.0f});
            const float cueDist = std::max(2.0f * radius_, (apexPos - whitePos).length());
            std::snprintf(aiIntentTag_.data(), aiIntentTag_.size(), "大力冲球");
            aiTempoScale_ = isGoat ? 1.04f : 1.0f;

            if (diff == BilliardsAiDifficulty::Easy) {
                const float jAng = std::atan2((nextRand01() * 2.0f - 1.0f) * 0.45f * radius_, cueDist);
                const float cs = std::cos(jAng), sn = std::sin(jAng);
                aiPlannedAimDir_ = normalizedOr(coords_.velocityToScene({baseDir.x * cs - baseDir.y * sn,
                                                                         baseDir.x * sn + baseDir.y * cs}),
                                                {1.0f, 0.0f});
                aiPlannedPower_ = 0.78f;
                aiPlannedSpin_ = {0.0f, 0.0f};
                return;
            }

            const float brkPwr = isGoat ? 0.95f : (isPro ? 0.91f : 0.85f);
            const Vec2 brkSpin{0.0f, -0.24f};
            Vec2 bestBrkDir = baseDir;
            float bestBrkScore = -1e9f;
            for (float gr : {0.0f, 0.06f, -0.06f, 0.14f, -0.14f}) {
                const float rad = std::atan2(gr * radius_, cueDist);
                const float mc = std::cos(rad), ms = std::sin(rad);
                const Vec2 testDir{baseDir.x * mc - baseDir.y * ms, baseDir.x * ms + baseDir.y * mc};
                const AiSimOutcome bOut = simulateShotRollout(whitePos, testDir, brkPwr, brkSpin);
                if (bOut.foul || bOut.lostGame) continue;
                const float sc =
                    static_cast<float>(bOut.ownPotted) * 48.0f + evaluatePostRolloutPosition(bOut, -1);
                if (sc > bestBrkScore) {
                    bestBrkScore = sc;
                    bestBrkDir = testDir;
                }
            }
            aiPlannedAimDir_ = normalizedOr(coords_.velocityToScene(bestBrkDir), {1.0f, 0.0f});
            aiPlannedPower_ = brkPwr;
            aiPlannedSpin_ = brkSpin;
            return;
        }

        // Snooker opening safety break: clip outer corner Red & send cue ball back behind Baulk line!
        const float midY = tableMin_.y + tableSize_.y * 0.5f;
        std::size_t topCornerRed = 0, botCornerRed = 0;
        float maxY = -1e9f, minY = 1e9f;
        for (std::size_t bi = 0; bi < balls_.size(); ++bi) {
            if (!isLegalTargetBall(bi, 1)) continue;
            const Vec2 p = world_.state(balls_[bi].body).position;
            if (p.y > maxY) {
                maxY = p.y;
                topCornerRed = bi;
            }
            if (p.y < minY) {
                minY = p.y;
                botCornerRed = bi;
            }
        }
        std::snprintf(aiIntentTag_.data(), aiIntentTag_.size(), "防守开球");
        aiTempoScale_ = isGoat ? 1.08f : 1.0f;

        Vec2 bestSnkDir =
            normalizedOr(world_.state(balls_[botCornerRed].body).position - whitePos, {1.0f, 0.0f});
        float bestSnkPwr = 0.44f;
        Vec2 bestSnkSpin{0.0f, 0.20f};
        float bestSnkSafety = -1e9f;

        if (diff != BilliardsAiDifficulty::Easy) {
            for (std::size_t cIdx : {botCornerRed, topCornerRed}) {
                const Vec2 posR = world_.state(balls_[cIdx].body).position;
                const Vec2 toR = normalizedOr(posR - whitePos, {1.0f, 0.0f});
                const Vec2 perp{-toR.y, toR.x};
                const float outSign = (posR.y >= midY) ? 1.0f : -1.0f;
                for (float latMag : {0.92f, 0.84f}) {
                    const float lat = latMag * outSign;
                    const Vec2 ghost = posR - toR * (2.0f * radius_ * std::sqrt(1.0f - lat * lat)) +
                                       perp * (2.0f * radius_ * lat);
                    const Vec2 aimDir = normalizedOr(ghost - whitePos, toR);
                    for (float pwr : {0.66f, 0.76f, 0.85f, 0.94f}) {
                        for (Vec2 sp : {Vec2{0.0f, 0.65f}, Vec2{0.45f * outSign, 0.60f},
                                        Vec2{-0.45f * outSign, 0.60f}}) {
                            const AiSimOutcome sOut = simulateShotRollout(whitePos, aimDir, pwr, sp, 420);
                            if (sOut.foul || sOut.lostGame) continue;
                            const float sc =
                                -evaluateOpponentThreat(sOut) - (sOut.whiteEndPhys.x - tableMin_.x) * 3.0f;
                            if (sc > bestSnkSafety) {
                                bestSnkSafety = sc;
                                bestSnkDir = aimDir;
                                bestSnkPwr = pwr;
                                bestSnkSpin = sp;
                            }
                        }
                    }
                }
            }
        }
        aiPlannedAimDir_ = normalizedOr(coords_.velocityToScene(bestSnkDir), {1.0f, 0.0f});
        aiPlannedPower_ = bestSnkPwr;
        aiPlannedSpin_ = bestSnkSpin;
        return;
    }
    const float minCutCos = diff == BilliardsAiDifficulty::Easy
                                ? 0.26f
                                : (diff == BilliardsAiDifficulty::Medium ? 0.14f : (isGoat ? 0.025f : 0.06f));

    const int solidCount = remainingGroupBalls(BallGroup::Solid);
    const int stripeCount = remainingGroupBalls(BallGroup::Stripe);

    int tiedBefore = -1;
    if (isGoat && desktop) {
        std::vector<Vec2> curPos(balls_.size());
        std::vector<bool> curAct(balls_.size(), false);
        for (std::size_t i = 0; i < balls_.size(); ++i) {
            const BodyState bs = world_.state(balls_[i].body);
            if (!balls_[i].pocketed && bs.enabled) {
                curPos[i] = bs.position;
                curAct[i] = true;
            }
        }
        tiedBefore = countTiedOwnBalls(curPos, curAct, playerGroups_[1], snookerTarget_);
    }

    enum class ShotKind : std::uint8_t { DirectPot = 0, ComboPot = 1, BankPot = 2, Safety = 3, Kick = 4 };
    struct CandidateShot {
        ShotKind kind = ShotKind::DirectPot;
        Vec2 aimPhysDir{1.0f, 0.0f};
        float power = 0.45f;
        Vec2 spin{0.0f, 0.0f};
        float heurScore = -1e9f;
        float cueDist = 2.0f;
        float cutCos = 0.8f;
        float difficultyFactor = 1.0f;
        int targetBall = -1;
    };

    std::vector<CandidateShot> potCandidates;
    potCandidates.reserve(64);
    std::vector<CandidateShot> safetyAndKicks;
    safetyAndKicks.reserve(96);

    // 1. Direct Pot Candidates across all legal object balls & 6 pockets.
    for (std::size_t bi = 0; bi < balls_.size(); ++bi) {
        if (!isLegalTargetBall(bi, 1)) continue;
        const Ball& b = balls_[bi];
        const Vec2 posB = world_.state(b.body).position;

        float groupBonus = 0.0f;
        if (diff != BilliardsAiDifficulty::Easy) {
            if (config_.rule == BilliardsRule::EightBall && playerGroups_[1] == BallGroup::FreeBall) {
                if (b.group == BallGroup::Solid && solidCount < stripeCount) groupBonus = 10.0f;
                if (b.group == BallGroup::Stripe && stripeCount < solidCount) groupBonus = 10.0f;
            } else if (config_.rule == BilliardsRule::Snooker && snookerTarget_ == 8) {
                groupBonus = static_cast<float>(b.number) * (isPro ? 6.5f : 3.5f);
            }
        }

        for (std::size_t pk = 0; pk < pockets_.size(); ++pk) {
            const Vec2 throat = pocketThroatMeters(pk);
            const bool insideJaws =
                (posB - pockets_[pk]).lengthSquared() < (throat - pockets_[pk]).lengthSquared();
            const Vec2 entry = insideJaws ? pockets_[pk] : throat;
            if (!pocketPathClearJaws(pk, posB, entry)) continue;
            if (!segmentClearOfBalls(posB, entry, potClearR, static_cast<int>(bi))) continue;

            const Vec2 potDir = normalizedOr(entry - posB, {1.0f, 0.0f});
            Vec2 contactNormal = potDir;
            if (diff != BilliardsAiDifficulty::Easy) {
                const Vec2 roughGhost = posB - potDir * (2.0f * radius_);
                const Vec2 roughAim = normalizedOr(roughGhost - whitePos, potDir);
                const float roughCos = std::clamp(roughAim.x * potDir.x + roughAim.y * potDir.y, -1.0f, 1.0f);
                const float alpha = std::acos(roughCos);
                const float muEff = std::min(0.05f, std::tan(std::min(alpha, 1.48f)) / (6.0f * 0.96f));
                const float cross = roughAim.x * potDir.y - roughAim.y * potDir.x;
                const float throwAng = 0.35f * std::atan(muEff) * (cross >= 0.0f ? 1.0f : -1.0f);
                const float tc = std::cos(throwAng), ts = std::sin(throwAng);
                contactNormal = {potDir.x * tc - potDir.y * ts, potDir.x * ts + potDir.y * tc};
            }
            const Vec2 ghost = posB - contactNormal * (2.0f * radius_);
            if (ghost.x < minGhostX || ghost.x > maxGhostX || ghost.y < minGhostY || ghost.y > maxGhostY) {
                continue;
            }
            if (!segmentClearOfBalls(ghost, ghost, isGoat ? (1.92f * radius_) : (1.98f * radius_),
                                     static_cast<int>(bi)))
                continue;

            const Vec2 aimVec = ghost - whitePos;
            const float cueDist = aimVec.length();
            if (cueDist < 0.25f * radius_) continue;
            const Vec2 aimDir = aimVec / cueDist;
            const float cutCos = aimDir.x * contactNormal.x + aimDir.y * contactNormal.y;
            if (cutCos < minCutCos) continue;
            if (!segmentClearOfBalls(whitePos, ghost, cueClearR, static_cast<int>(bi))) continue;

            const float potDist = (entry - posB).length();
            const float vObjNeed = 2.20f + 1.08f * potDist;
            const float vImpactNeed = vObjNeed / std::max(isGoat ? 0.08f : 0.12f, cutCos * 0.95f);
            const float v0Need = vImpactNeed + 1.02f * cueDist;
            if (v0Need > topSpeed * (isGoat ? 1.24f : 1.12f)) continue;
            const float basePower = std::clamp(v0Need / topSpeed, 0.14f, 0.95f);

            const float sidePocketPenalty = (pk == 1 || pk == 4) ? 4.0f : 0.0f;
            float heur =
                135.0f + cutCos * 72.0f - cueDist * 4.0f - potDist * 7.2f + groupBonus - sidePocketPenalty;
            if (isPro) {
                const Vec2 vTan = aimDir - contactNormal * cutCos;
                heur += evaluateNextBallPosition(ghost + vTan * (3.2f * radius_), bi) * 0.55f;
            }

            CandidateShot cs;
            cs.kind = ShotKind::DirectPot;
            cs.aimPhysDir = aimDir;
            cs.power = basePower;
            cs.spin = {0.0f, 0.0f};
            cs.heurScore = heur;
            cs.cueDist = std::max(2.0f * radius_, cueDist);
            cs.cutCos = cutCos;
            cs.difficultyFactor = std::clamp(0.75f + 0.55f * (1.0f - cutCos), 0.7f, 1.45f);
            cs.targetBall = static_cast<int>(bi);
            potCandidates.push_back(cs);
        }
    }

    // 2. Two-Ball Combination / Plant Pots ("传球") & Cue-Ball Caroms ("借力球") for Medium, Master & Goat.
    if (diff != BilliardsAiDifficulty::Easy) {
        auto canPlantBall = [&](std::size_t b1, std::size_t b2) {
            if (b1 == b2 || !isLegalTargetBall(b1, 1)) return false;
            if (balls_[b2].pocketed || !world_.state(balls_[b2].body).enabled) return false;
            if (config_.rule == BilliardsRule::Free || !desktop) return true;
            if (config_.rule == BilliardsRule::EightBall) {
                const BallGroup g = playerGroups_[1];
                if (g == BallGroup::FreeBall) return balls_[b2].group != BallGroup::Eight;
                if (remainingGroupBalls(g) > 0) return balls_[b2].group == g;
                return false;
            }
            return snookerTarget_ == 1 && balls_[b1].group == BallGroup::SnookerRed &&
                   balls_[b2].group == BallGroup::SnookerRed;
        };

        const float maxPlantDist = (isGoat ? 24.0f : 18.0f) * radius_;
        const float minPlantCos = isGoat ? 0.30f : 0.42f;
        const float minComboCut1 = isGoat ? 0.22f : 0.32f;

        for (std::size_t b2 = 0; b2 < balls_.size(); ++b2) {
            if (balls_[b2].pocketed || !world_.state(balls_[b2].body).enabled) continue;
            const Vec2 posB2 = world_.state(balls_[b2].body).position;
            for (std::size_t pk = 0; pk < pockets_.size(); ++pk) {
                const Vec2 throat = pocketThroatMeters(pk);
                const Vec2 entry =
                    (posB2 - pockets_[pk]).lengthSquared() < (throat - pockets_[pk]).lengthSquared()
                        ? pockets_[pk]
                        : throat;
                const float potDist2 = (entry - posB2).length();
                if (potDist2 > maxPlantDist) continue;
                if (!pocketPathClearJaws(pk, posB2, entry)) continue;
                if (!segmentClearOfBalls(posB2, entry, 2.01f * radius_, static_cast<int>(b2))) continue;

                const Vec2 potDir2 = normalizedOr(entry - posB2, {1.0f, 0.0f});
                const Vec2 ghost2 = posB2 - potDir2 * (2.0f * radius_);

                for (std::size_t b1 = 0; b1 < balls_.size(); ++b1) {
                    if (!canPlantBall(b1, b2)) continue;
                    const Vec2 posB1 = world_.state(balls_[b1].body).position;
                    const Vec2 toGhost2 = ghost2 - posB1;
                    const float d12 = toGhost2.length();
                    if (d12 >= 0.15f * radius_ && d12 <= maxPlantDist) {
                        const Vec2 u12 = toGhost2 / d12;
                        const float plantCos = u12.x * potDir2.x + u12.y * potDir2.y;
                        if (plantCos >= minPlantCos &&
                            segmentClearOfBalls(posB1, ghost2, 2.01f * radius_, static_cast<int>(b1),
                                                static_cast<int>(b2))) {
                            const Vec2 ghost1 = posB1 - u12 * (2.0f * radius_);
                            if (ghost1.x >= minWallX && ghost1.x <= maxWallX && ghost1.y >= minWallY &&
                                ghost1.y <= maxWallY &&
                                segmentClearOfBalls(ghost1, ghost1, 1.98f * radius_, static_cast<int>(b1),
                                                    static_cast<int>(b2))) {
                                const Vec2 aimVec = ghost1 - whitePos;
                                const float cueDist = aimVec.length();
                                if (cueDist >= 0.3f * radius_) {
                                    const Vec2 aimDir = aimVec / cueDist;
                                    const float cutCos1 = aimDir.x * u12.x + aimDir.y * u12.y;
                                    if (cutCos1 >= minComboCut1 &&
                                        segmentClearOfBalls(whitePos, ghost1, 2.03f * radius_,
                                                            static_cast<int>(b1))) {
                                        const float v2Need = 2.35f + 1.12f * potDist2;
                                        const float v1Need =
                                            v2Need / std::max(0.30f, plantCos * 0.92f) + 1.05f * d12;
                                        const float v0Need =
                                            v1Need / std::max(0.24f, cutCos1 * 0.92f) + 1.02f * cueDist;
                                        if (v0Need <= topSpeed * 1.12f) {
                                            CandidateShot cs;
                                            cs.kind = ShotKind::ComboPot;
                                            cs.aimPhysDir = aimDir;
                                            cs.power = std::clamp(v0Need / topSpeed, 0.20f, 0.94f);
                                            cs.spin = {0.0f, 0.0f};
                                            cs.heurScore = 104.0f + (cutCos1 * plantCos) * 62.0f -
                                                           (cueDist + d12 + potDist2) * 4.2f;
                                            cs.cueDist = std::max(2.0f * radius_, cueDist);
                                            cs.cutCos = cutCos1;
                                            cs.difficultyFactor = 1.35f;
                                            cs.targetBall = static_cast<int>(b2);
                                            potCandidates.push_back(cs);
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // Goat Cue-Ball Carom ("母球90°切线借力传进袋口球"): hit legal b1 and stun-tangent cue
                    // ball into hanger b2!
                    if (isGoat && potDist2 <= 6.5f * radius_) {
                        const Vec2 b1ToG2 = ghost2 - posB1;
                        const float caromDist = b1ToG2.length();
                        if (caromDist > 2.2f * radius_ && caromDist < 12.0f * radius_) {
                            const Vec2 tanDir = b1ToG2 / caromDist;
                            if (tanDir.x * potDir2.x + tanDir.y * potDir2.y > 0.45f) {
                                for (float sgn : {1.0f, -1.0f}) {
                                    const Vec2 nCarom{-tanDir.y * sgn, tanDir.x * sgn};
                                    const Vec2 ghostCarom = posB1 - nCarom * (2.0f * radius_);
                                    if (ghostCarom.x < minWallX || ghostCarom.x > maxWallX ||
                                        ghostCarom.y < minWallY || ghostCarom.y > maxWallY) {
                                        continue;
                                    }
                                    const Vec2 aimVec = ghostCarom - whitePos;
                                    const float cDist = aimVec.length();
                                    if (cDist < 0.5f * radius_) continue;
                                    const Vec2 aimDir = aimVec / cDist;
                                    if (aimDir.x * nCarom.x + aimDir.y * nCarom.y < 0.35f) continue;
                                    if (aimDir.x * tanDir.x + aimDir.y * tanDir.y < 0.25f) continue;
                                    if (!segmentClearOfBalls(whitePos, ghostCarom, 2.02f * radius_,
                                                             static_cast<int>(b1))) {
                                        continue;
                                    }
                                    CandidateShot cs;
                                    cs.kind = ShotKind::ComboPot;
                                    cs.aimPhysDir = aimDir;
                                    cs.power = std::clamp((5.5f + 1.1f * (cDist + caromDist)) / topSpeed,
                                                          0.26f, 0.85f);
                                    cs.spin = {0.0f, -0.22f};  // Pure stun tangent
                                    cs.heurScore = 92.0f - (cDist + caromDist) * 3.8f;
                                    cs.cueDist = std::max(2.0f * radius_, cDist);
                                    cs.cutCos = 0.5f;
                                    cs.targetBall = static_cast<int>(b2);
                                    potCandidates.push_back(cs);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // 3. One-Cushion Bank Pots ("一库翻袋") for Medium, Master & Goat.
    if (diff != BilliardsAiDifficulty::Easy) {
        constexpr float kBankRatio = 0.89f;
        const std::array<float, 4> walls = {
            tableMin_.x + radius_,
            tableMin_.x + tableSize_.x - radius_,
            tableMin_.y + radius_,
            tableMin_.y + tableSize_.y - radius_,
        };
        for (std::size_t bi = 0; bi < balls_.size(); ++bi) {
            if (!isLegalTargetBall(bi, 1)) continue;
            const Vec2 posB = world_.state(balls_[bi].body).position;
            for (std::size_t pk = 0; pk < pockets_.size(); ++pk) {
                const Vec2 entry = pocketThroatMeters(pk);
                for (int rail = 0; rail < 4; ++rail) {
                    if (rail == 0 && (pk == 0 || pk == 3)) continue;
                    if (rail == 1 && (pk == 2 || pk == 5)) continue;
                    if (rail == 2 && (pk >= 3)) continue;
                    if (rail == 3 && (pk <= 2)) continue;

                    Vec2 virtEntry = entry;
                    if (rail == 0 || rail == 1) {
                        const float dOut = entry.x - walls[static_cast<std::size_t>(rail)];
                        virtEntry.x = walls[static_cast<std::size_t>(rail)] - dOut / kBankRatio;
                    } else {
                        const float dOut = entry.y - walls[static_cast<std::size_t>(rail)];
                        virtEntry.y = walls[static_cast<std::size_t>(rail)] - dOut / kBankRatio;
                    }

                    const Vec2 dirToVirt = virtEntry - posB;
                    float u = -1.0f;
                    if (rail == 0 || rail == 1) {
                        if (std::abs(dirToVirt.x) > 1e-4f)
                            u = (walls[static_cast<std::size_t>(rail)] - posB.x) / dirToVirt.x;
                    } else {
                        if (std::abs(dirToVirt.y) > 1e-4f)
                            u = (walls[static_cast<std::size_t>(rail)] - posB.y) / dirToVirt.y;
                    }
                    if (u <= 0.05f || u >= 0.95f) continue;
                    const Vec2 bouncePt = posB + dirToVirt * u;
                    if (!cushionBounceValid(rail, bouncePt)) continue;
                    if (!pocketPathClearJaws(pk, bouncePt, entry)) continue;
                    if (!segmentClearOfBalls(posB, bouncePt, 2.03f * radius_, static_cast<int>(bi))) continue;
                    if (!segmentClearOfBalls(bouncePt, entry, 2.03f * radius_, static_cast<int>(bi)))
                        continue;

                    const Vec2 potDir = normalizedOr(bouncePt - posB, {1.0f, 0.0f});
                    const Vec2 ghost = posB - potDir * (2.0f * radius_);
                    if (ghost.x < minWallX || ghost.x > maxWallX || ghost.y < minWallY || ghost.y > maxWallY)
                        continue;
                    const Vec2 aimVec = ghost - whitePos;
                    const float cueDist = aimVec.length();
                    if (cueDist < 0.3f * radius_) continue;
                    const Vec2 aimDir = aimVec / cueDist;
                    const float cutCos = aimDir.x * potDir.x + aimDir.y * potDir.y;
                    if (cutCos < (isGoat ? 0.22f : 0.32f)) continue;
                    if (!segmentClearOfBalls(whitePos, ghost, 2.03f * radius_, static_cast<int>(bi)))
                        continue;

                    const float bankTotalDist = (bouncePt - posB).length() + (entry - bouncePt).length();
                    const float vObjNeed = 2.65f + 1.28f * bankTotalDist;
                    const float v0Need = vObjNeed / std::max(0.22f, cutCos * 0.92f) + 1.02f * cueDist;
                    if (v0Need > topSpeed * (isGoat ? 1.12f : 1.05f)) continue;

                    CandidateShot cs;
                    cs.kind = ShotKind::BankPot;
                    cs.aimPhysDir = aimDir;
                    cs.power = std::clamp(v0Need / topSpeed, 0.25f, 0.94f);
                    cs.spin = {0.0f, 0.0f};
                    cs.heurScore = 88.0f + cutCos * 45.0f - (cueDist + bankTotalDist) * 4.2f;
                    cs.cueDist = std::max(2.0f * radius_, cueDist);
                    cs.cutCos = cutCos;
                    cs.difficultyFactor = 1.45f;
                    cs.targetBall = static_cast<int>(bi);
                    potCandidates.push_back(cs);
                }
            }
        }
    }

    // 4. Tactical Safety / Snooker Hook & Direct Legal Contact Candidates.
    for (std::size_t bi = 0; bi < balls_.size(); ++bi) {
        if (!isLegalTargetBall(bi, 1)) continue;
        const Vec2 posB = world_.state(balls_[bi].body).position;
        const Vec2 toB = posB - whitePos;
        const float dist = toB.length();
        if (dist < 0.2f * radius_) continue;
        const Vec2 baseDir = toB / dist;
        const Vec2 perp{-baseDir.y, baseDir.x};

        const std::array<float, 7> goatFrac = {0.93f, -0.93f, 0.85f, -0.85f, 0.64f, -0.64f, 0.0f};
        const std::array<float, 5> stdFrac = {0.86f, -0.86f, 0.62f, -0.62f, 0.0f};
        const float* fracPtr = isGoat ? goatFrac.data() : stdFrac.data();
        const std::size_t fracCount = isGoat ? goatFrac.size() : stdFrac.size();

        for (std::size_t fi = 0; fi < fracCount; ++fi) {
            const float lateralFrac = fracPtr[fi];
            const Vec2 ghost =
                posB -
                baseDir * (2.0f * radius_ * std::sqrt(std::max(0.04f, 1.0f - lateralFrac * lateralFrac))) +
                perp * (2.0f * radius_ * lateralFrac);
            if (ghost.x < minWallX || ghost.x > maxWallX || ghost.y < minWallY || ghost.y > maxWallY)
                continue;
            const Vec2 aimVec = ghost - whitePos;
            const float cueDist = aimVec.length();
            if (cueDist < 0.2f * radius_) continue;
            const Vec2 aimDir = aimVec / cueDist;
            if (!segmentClearOfBalls(whitePos, ghost, 2.02f * radius_, static_cast<int>(bi))) continue;

            const bool thinClip = std::abs(lateralFrac) > 0.72f;
            float pwr = std::clamp((3.4f + 0.92f * cueDist) / topSpeed, 0.16f, 0.52f);
            float sc = 36.0f - cueDist * 1.8f;
            if (isPro && thinClip) {
                pwr = std::clamp((2.8f + 0.88f * cueDist) / topSpeed, 0.15f, 0.42f);
                sc += 16.0f;
            } else if (isPro && std::abs(lateralFrac) < 0.05f) {
                // Soft roll-up touch safety ("轻贴球")
                pwr = std::clamp((1.05f + 1.01f * cueDist) / topSpeed, 0.12f, 0.34f);
                sc += 8.0f;
            }
            if (pathRisksScratch(ghost, aimDir, cueDist)) sc -= 85.0f;

            CandidateShot cs;
            cs.kind = ShotKind::Safety;
            cs.aimPhysDir = aimDir;
            cs.power = pwr;
            cs.spin = thinClip ? Vec2{0.0f, 0.18f} : Vec2{0.0f, -0.25f};
            cs.heurScore = sc;
            cs.cueDist = std::max(2.0f * radius_, cueDist);
            cs.difficultyFactor = 0.9f;
            cs.targetBall = static_cast<int>(bi);
            safetyAndKicks.push_back(cs);

            // Goat also tests softer & firmer safety speeds + side-english rail-hugger clips to manufacture
            // snooker hooks behind nearby blockers!
            if (isGoat && thinClip) {
                CandidateShot softClip = cs;
                softClip.power = std::clamp((1.85f + 0.92f * cueDist) / topSpeed, 0.12f, 0.32f);
                softClip.spin = {lateralFrac > 0.0f ? 0.38f : -0.38f, -0.18f};
                softClip.heurScore = sc - 1.5f;
                safetyAndKicks.push_back(softClip);

                CandidateShot twoRailSafe = cs;
                twoRailSafe.power = std::clamp((5.2f + 0.95f * cueDist) / topSpeed, 0.24f, 0.62f);
                twoRailSafe.spin = {lateralFrac > 0.0f ? -0.42f : 0.42f, 0.25f};
                twoRailSafe.heurScore = sc - 3.0f;
                safetyAndKicks.push_back(twoRailSafe);
            } else if (isPro && config_.rule == BilliardsRule::Snooker && thinClip) {
                // Snooker Baulk-return safety speed for Master as well.
                CandidateShot baulkRet = cs;
                baulkRet.power = std::clamp((5.6f + 0.95f * cueDist) / topSpeed, 0.28f, 0.58f);
                baulkRet.spin = {0.0f, 0.28f};
                baulkRet.heurScore = sc - 1.0f;
                safetyAndKicks.push_back(baulkRet);
            }
        }
    }

    // 5. 1-Cushion & Multi-Cushion Kick Escapes ("解斯诺克").
    {
        constexpr float kKickRatio = 0.89f;
        const std::array<float, 4> walls = {
            tableMin_.x + radius_,
            tableMin_.x + tableSize_.x - radius_,
            tableMin_.y + radius_,
            tableMin_.y + tableSize_.y - radius_,
        };
        const std::array<float, 5> goatEdges = {0.0f, 0.65f, -0.65f, 1.20f, -1.20f};
        const std::array<float, 3> stdEdges = {0.0f, 0.9f, -0.9f};
        const float* edgePtr = isGoat ? goatEdges.data() : stdEdges.data();
        const std::size_t edgeCnt = isGoat ? goatEdges.size() : stdEdges.size();

        for (std::size_t bi = 0; bi < balls_.size(); ++bi) {
            if (!isLegalTargetBall(bi, 1)) continue;
            const Vec2 posB = world_.state(balls_[bi].body).position;

            // Goat 1-Cushion Kick-Pot ("一库解传进袋"): kick cue ball off a rail directly onto the exact
            // ghost-ball spot of any pocketable legal ball when direct line is obstructed!
            if (isGoat) {
                for (std::size_t pk = 0; pk < pockets_.size(); ++pk) {
                    const Vec2 throat = pocketThroatMeters(pk);
                    const Vec2 entry =
                        (posB - pockets_[pk]).lengthSquared() < (throat - pockets_[pk]).lengthSquared()
                            ? pockets_[pk]
                            : throat;
                    const float potDist = (entry - posB).length();
                    if (potDist > 18.0f * radius_) continue;
                    if (!pocketPathClearJaws(pk, posB, entry)) continue;
                    if (!segmentClearOfBalls(posB, entry, potClearR, static_cast<int>(bi))) continue;
                    const Vec2 potDir = normalizedOr(entry - posB, {1.0f, 0.0f});
                    const Vec2 ghost = posB - potDir * (2.0f * radius_);
                    if (ghost.x < minGhostX || ghost.x > maxGhostX || ghost.y < minGhostY ||
                        ghost.y > maxGhostY)
                        continue;
                    for (int rail = 0; rail < 4; ++rail) {
                        Vec2 virtGhost = ghost;
                        if (rail == 0 || rail == 1) {
                            const float dOut = ghost.x - walls[static_cast<std::size_t>(rail)];
                            virtGhost.x = walls[static_cast<std::size_t>(rail)] - dOut / kKickRatio;
                        } else {
                            const float dOut = ghost.y - walls[static_cast<std::size_t>(rail)];
                            virtGhost.y = walls[static_cast<std::size_t>(rail)] - dOut / kKickRatio;
                        }
                        const Vec2 dirToVirt = virtGhost - whitePos;
                        float u = -1.0f;
                        if (rail == 0 || rail == 1) {
                            if (std::abs(dirToVirt.x) > 1e-4f)
                                u = (walls[static_cast<std::size_t>(rail)] - whitePos.x) / dirToVirt.x;
                        } else {
                            if (std::abs(dirToVirt.y) > 1e-4f)
                                u = (walls[static_cast<std::size_t>(rail)] - whitePos.y) / dirToVirt.y;
                        }
                        if (u <= 0.05f || u >= 0.95f) continue;
                        const Vec2 bouncePt = whitePos + dirToVirt * u;
                        if (!cushionBounceValid(rail, bouncePt)) continue;
                        if (!segmentClearOfBalls(whitePos, bouncePt, cueClearR)) continue;
                        if (!segmentClearOfBalls(bouncePt, ghost, cueClearR, static_cast<int>(bi))) continue;
                        const Vec2 inDir = normalizedOr(ghost - bouncePt, potDir);
                        const float cutCos = inDir.x * potDir.x + inDir.y * potDir.y;
                        if (cutCos < 0.28f) continue;
                        const float kickDist = (bouncePt - whitePos).length() + (ghost - bouncePt).length();
                        const float vNeed =
                            (2.35f + 1.15f * potDist) / std::max(0.28f, cutCos * 0.88f) + 1.12f * kickDist;
                        if (vNeed > topSpeed * 1.18f) continue;
                        CandidateShot kp;
                        kp.kind = ShotKind::Kick;
                        kp.aimPhysDir = normalizedOr(bouncePt - whitePos, {1.0f, 0.0f});
                        kp.power = std::clamp(vNeed / topSpeed, 0.26f, 0.94f);
                        kp.spin = {0.0f, 0.08f};
                        kp.heurScore = 82.0f + cutCos * 42.0f - (kickDist + potDist) * 3.8f;
                        kp.cueDist = std::max(2.0f * radius_, kickDist);
                        kp.cutCos = cutCos;
                        kp.difficultyFactor = 1.45f;
                        kp.targetBall = static_cast<int>(bi);
                        potCandidates.push_back(kp);
                    }
                }
            }

            const Vec2 perpB = normalizedOr({-(posB.y - whitePos.y), posB.x - whitePos.x}, {0.0f, 1.0f});
            for (std::size_t ei = 0; ei < edgeCnt; ++ei) {
                const float edgeOff = edgePtr[ei];
                const Vec2 aimSpot = posB + perpB * (edgeOff * radius_);
                for (int rail = 0; rail < 4; ++rail) {
                    Vec2 virtTarget = aimSpot;
                    if (rail == 0 || rail == 1) {
                        const float dOut = aimSpot.x - walls[static_cast<std::size_t>(rail)];
                        virtTarget.x = walls[static_cast<std::size_t>(rail)] - dOut / kKickRatio;
                    } else {
                        const float dOut = aimSpot.y - walls[static_cast<std::size_t>(rail)];
                        virtTarget.y = walls[static_cast<std::size_t>(rail)] - dOut / kKickRatio;
                    }
                    const Vec2 dirToVirt = virtTarget - whitePos;
                    float u = -1.0f;
                    if (rail == 0 || rail == 1) {
                        if (std::abs(dirToVirt.x) > 1e-4f)
                            u = (walls[static_cast<std::size_t>(rail)] - whitePos.x) / dirToVirt.x;
                    } else {
                        if (std::abs(dirToVirt.y) > 1e-4f)
                            u = (walls[static_cast<std::size_t>(rail)] - whitePos.y) / dirToVirt.y;
                    }
                    if (u <= 0.05f || u >= 0.95f) continue;
                    const Vec2 bouncePt = whitePos + dirToVirt * u;
                    if (!cushionBounceValid(rail, bouncePt)) continue;
                    if (!segmentClearOfBalls(whitePos, bouncePt, 2.02f * radius_)) continue;
                    const Vec2 contactPt =
                        aimSpot - normalizedOr(aimSpot - bouncePt, {1.0f, 0.0f}) * (2.0f * radius_);
                    if (!segmentClearOfBalls(bouncePt, contactPt, 2.02f * radius_, static_cast<int>(bi)))
                        continue;

                    const float kickDist = (bouncePt - whitePos).length() + (contactPt - bouncePt).length();
                    CandidateShot cs;
                    cs.kind = ShotKind::Kick;
                    cs.aimPhysDir = normalizedOr(bouncePt - whitePos, {1.0f, 0.0f});
                    cs.power = std::clamp((4.2f + 1.18f * kickDist) / topSpeed, 0.24f, 0.86f);
                    cs.spin = {0.0f, 0.0f};
                    cs.heurScore = 22.0f - kickDist * 1.4f - std::abs(edgeOff) * 3.0f;
                    cs.cueDist = std::max(2.0f * radius_, kickDist);
                    cs.difficultyFactor = 1.55f;
                    cs.targetBall = static_cast<int>(bi);
                    safetyAndKicks.push_back(cs);
                }
            }
        }

        // If completely locked in a multi-ball snooker with 0 direct or 1-rail lines, add 24-angle 2-rail
        // sweep for Medium & Master (Goat uses a dedicated 72+36 ray 360° zero-foul escape verifier below).
        if (potCandidates.empty() && safetyAndKicks.empty() && diff != BilliardsAiDifficulty::Easy &&
            !isGoat) {
            for (int k = 0; k < 24; ++k) {
                const float ang = static_cast<float>(k) * (6.283185307f / 24.0f);
                CandidateShot cs;
                cs.kind = ShotKind::Kick;
                cs.aimPhysDir = {std::cos(ang), std::sin(ang)};
                cs.power = 0.62f;
                cs.spin = {0.0f, 0.15f};
                cs.heurScore = 1.0f;
                cs.cueDist = tableSize_.y * 0.8f;
                safetyAndKicks.push_back(cs);
            }
        }
    }

    auto byHeurDesc = [](const CandidateShot& a, const CandidateShot& b) {
        return a.heurScore > b.heurScore;
    };
    std::sort(potCandidates.begin(), potCandidates.end(), byHeurDesc);
    std::sort(safetyAndKicks.begin(), safetyAndKicks.end(), byHeurDesc);

    // Ultimate fallback if table has no candidates at all.
    CandidateShot fallbackShot;
    if (!potCandidates.empty()) {
        fallbackShot = potCandidates.front();
    } else if (!safetyAndKicks.empty()) {
        fallbackShot = safetyAndKicks.front();
    } else {
        for (std::size_t bi = 0; bi < balls_.size(); ++bi) {
            if (!isLegalTargetBall(bi, 1)) continue;
            fallbackShot.aimPhysDir =
                normalizedOr(world_.state(balls_[bi].body).position - whitePos, {1.0f, 0.0f});
            fallbackShot.power = 0.42f;
            break;
        }
    }

    // --- Easy Tier: Fast analytical choice + beginner spatial error ---
    if (diff == BilliardsAiDifficulty::Easy || !desktop) {
        std::snprintf(aiIntentTag_.data(), aiIntentTag_.size(),
                      !potCandidates.empty() ? "进攻下球" : "基础击球");
        const float rawSample = nextRand01() * 2.0f - 1.0f;
        const float ghostLateralErr = rawSample * 0.62f * radius_ * fallbackShot.difficultyFactor;
        const float angleJitter = std::atan2(ghostLateralErr, std::max(2.0f * radius_, fallbackShot.cueDist));
        const float cs = std::cos(angleJitter), sn = std::sin(angleJitter);
        const Vec2 dir{fallbackShot.aimPhysDir.x * cs - fallbackShot.aimPhysDir.y * sn,
                       fallbackShot.aimPhysDir.x * sn + fallbackShot.aimPhysDir.y * cs};
        aiPlannedAimDir_ = normalizedOr(coords_.velocityToScene(dir), {1.0f, 0.0f});
        aiPlannedPower_ = std::clamp(fallbackShot.power * (1.0f + nextSignedJitter() * 0.10f), 0.12f, 0.96f);
        aiPlannedSpin_ = {0.0f, 0.0f};
        return;
    }

    // --- Medium, Master & Goat Tiers: Multi-Candidate Box2D Rollout + Lookahead & Safety Search ---
    const std::size_t maxPotChecks = isGoat ? 18u : (diff == BilliardsAiDifficulty::Master ? 10u : 5u);
    const std::size_t maxConfirmed = isGoat ? 6u : (diff == BilliardsAiDifficulty::Master ? 4u : 2u);
    constexpr std::array<float, 11> kGoatMicroR = {0.0f,    -0.015f, 0.015f,  -0.038f, 0.038f, -0.072f,
                                                   0.072f,  -0.118f, 0.118f,  -0.180f, 0.180f};
    constexpr std::array<float, 7> kMasterMicroR = {0.0f,    -0.025f, 0.025f, -0.060f,
                                                    0.060f,  -0.115f, 0.115f};
    constexpr std::array<float, 3> kMedMicroR = {0.0f, -0.045f, 0.045f};

    struct VerifiedPot {
        ShotKind kind = ShotKind::DirectPot;
        Vec2 calibratedPhysDir{1.0f, 0.0f};
        float basePower = 0.45f;
        Vec2 baseSpin{0.0f, 0.0f};
        float cutCos = 0.8f;
        float cueDist = 2.0f;
        float difficultyFactor = 1.0f;
        AiSimOutcome baseOutcome;
    };
    std::vector<VerifiedPot> confirmedPots;
    confirmedPots.reserve(12);

    bool haveSafeLegalFallback = false;
    CandidateShot bestLegalFallback = fallbackShot;
    float bestLegalFallbackScore = -1e9f;
    AiSimOutcome bestLegalFallbackOutcome;

    const std::size_t potLimit = std::min(potCandidates.size(), maxPotChecks);
    for (std::size_t ci = 0; ci < potLimit; ++ci) {
        const CandidateShot& cand = potCandidates[ci];
        const float effDist = std::max(2.0f * radius_, cand.cueDist);
        const float* microPtr =
            isGoat ? kGoatMicroR.data()
                   : (diff == BilliardsAiDifficulty::Master ? kMasterMicroR.data() : kMedMicroR.data());
        const std::size_t microCount =
            isGoat ? kGoatMicroR.size()
                   : (diff == BilliardsAiDifficulty::Master ? kMasterMicroR.size() : kMedMicroR.size());

        for (std::size_t mi = 0; mi < microCount; ++mi) {
            const float gr = microPtr[mi];
            const float rad = std::atan2(gr * radius_, effDist);
            const float mc = std::cos(rad), ms = std::sin(rad);
            const Vec2 testDir{cand.aimPhysDir.x * mc - cand.aimPhysDir.y * ms,
                               cand.aimPhysDir.x * ms + cand.aimPhysDir.y * mc};
            const AiSimOutcome simOut = simulateShotRollout(whitePos, testDir, cand.power, cand.spin);
            if (!simOut.foul && !simOut.lostGame) {
                const float safeSc =
                    -evaluateOpponentThreat(simOut) - static_cast<float>(simOut.oppPotted) * 65.0f;
                if (!haveSafeLegalFallback || safeSc > bestLegalFallbackScore) {
                    haveSafeLegalFallback = true;
                    bestLegalFallbackScore = safeSc;
                    bestLegalFallback = cand;
                    bestLegalFallback.aimPhysDir = testDir;
                    bestLegalFallbackOutcome = simOut;
                }
                if (simOut.continuesTurn || simOut.wonGame) {
                    confirmedPots.push_back(VerifiedPot{cand.kind, testDir, cand.power, cand.spin,
                                                        cand.cutCos, cand.cueDist, cand.difficultyFactor,
                                                        simOut});
                    break;  // Found exact pocketing ray for this candidate!
                }
            }
        }
        if (confirmedPots.size() >= maxConfirmed) break;
    }

    ShotKind chosenKind = fallbackShot.kind;
    Vec2 chosenPhysDir = fallbackShot.aimPhysDir;
    float chosenPower = fallbackShot.power;
    Vec2 chosenSpin = fallbackShot.spin;
    float chosenCueDist = fallbackShot.cueDist;
    float chosenCutCos = fallbackShot.cutCos;
    float chosenDiffFactor = fallbackShot.difficultyFactor;

    // Reusable defensive & 360° multi-rail zero-foul escape evaluator.
    struct DefResult {
        bool valid = false;
        bool foundPot = false;
        float bestScore = -1e9f;
        float bestThreat = 1e9f;
        ShotKind kind = ShotKind::Safety;
        Vec2 dir{1.0f, 0.0f};
        float power = 0.42f;
        Vec2 spin{0.0f, 0.0f};
        float cueDist = 2.0f;
        float diffFactor = 1.0f;
        AiSimOutcome outcome;
    };

    auto runDefensiveSearch = [&]() -> DefResult {
        DefResult res;
        if (haveSafeLegalFallback) {
            res.valid = true;
            res.bestScore = bestLegalFallbackScore;
            res.bestThreat = evaluateOpponentThreat(bestLegalFallbackOutcome);
            res.kind = bestLegalFallback.kind;
            res.dir = bestLegalFallback.aimPhysDir;
            res.power = bestLegalFallback.power;
            res.spin = bestLegalFallback.spin;
            res.cueDist = bestLegalFallback.cueDist;
            res.diffFactor = bestLegalFallback.difficultyFactor;
            res.outcome = bestLegalFallbackOutcome;
        }

        const std::size_t defLimit =
            std::min<std::size_t>(safetyAndKicks.size(), isGoat ? 28u : (isPro ? 18u : 6u));
        for (std::size_t di = 0; di < defLimit; ++di) {
            const CandidateShot& dc = safetyAndKicks[di];
            const AiSimOutcome defOut = simulateShotRollout(whitePos, dc.aimPhysDir, dc.power, dc.spin);
            if (defOut.lostGame || defOut.foul) continue;

            if (defOut.continuesTurn || defOut.wonGame) {
                const float potSc = 600.0f + evaluatePostRolloutPosition(defOut, tiedBefore);
                if (!res.foundPot || potSc > res.bestScore) {
                    res.valid = true;
                    res.foundPot = true;
                    res.bestScore = potSc;
                    res.bestThreat = -200.0f;
                    res.kind = dc.kind;
                    res.dir = dc.aimPhysDir;
                    res.power = dc.power;
                    res.spin = dc.spin;
                    res.cueDist = dc.cueDist;
                    res.diffFactor = dc.difficultyFactor;
                    res.outcome = defOut;
                }
                continue;
            }
            if (res.foundPot) continue;

            const float threat = evaluateOpponentThreat(defOut);
            const float safetyScore = -threat - static_cast<float>(defOut.oppPotted) * 65.0f;
            if (!res.valid || safetyScore > res.bestScore) {
                res.valid = true;
                res.bestScore = safetyScore;
                res.bestThreat = threat;
                res.kind = dc.kind;
                res.dir = dc.aimPhysDir;
                res.power = dc.power;
                res.spin = dc.spin;
                res.cueDist = dc.cueDist;
                res.diffFactor = dc.difficultyFactor;
                res.outcome = defOut;
            }
        }

        // Goat Ironclad Zero-Foul Guarantee: if still snookered (`!res.valid`), sweep 72 directions (every
        // 5°) + 36 side-english multi-cushion Z-kicks + fine ±1.5° refinement!
        if (isGoat && !res.valid) {
            auto trySweepRay = [&](float angRad, float pwr, Vec2 sp) -> bool {
                const Vec2 rayDir{std::cos(angRad), std::sin(angRad)};
                const AiSimOutcome sOut = simulateShotRollout(whitePos, rayDir, pwr, sp);
                if (sOut.lostGame || sOut.foul) return false;
                if (sOut.continuesTurn || sOut.wonGame) {
                    const float potSc = 600.0f + evaluatePostRolloutPosition(sOut, tiedBefore);
                    if (!res.foundPot || potSc > res.bestScore) {
                        res.valid = true;
                        res.foundPot = true;
                        res.bestScore = potSc;
                        res.bestThreat = -200.0f;
                        res.kind = ShotKind::Kick;
                        res.dir = rayDir;
                        res.power = pwr;
                        res.spin = sp;
                        res.cueDist = tableSize_.y;
                        res.outcome = sOut;
                    }
                    return true;
                }
                if (res.foundPot) return true;
                const float threat = evaluateOpponentThreat(sOut);
                const float sc = -threat - static_cast<float>(sOut.oppPotted) * 65.0f;
                if (!res.valid || sc > res.bestScore) {
                    res.valid = true;
                    res.bestScore = sc;
                    res.bestThreat = threat;
                    res.kind = ShotKind::Kick;
                    res.dir = rayDir;
                    res.power = pwr;
                    res.spin = sp;
                    res.cueDist = tableSize_.y;
                    res.outcome = sOut;
                }
                return true;
            };

            float firstGoodAng = 0.0f;
            bool hitGoodAng = false;
            for (int k = 0; k < 72; ++k) {
                const float ang = static_cast<float>(k) * (6.283185307f / 72.0f);
                if (trySweepRay(ang, 0.68f, {0.0f, 0.12f}) && !hitGoodAng) {
                    firstGoodAng = ang;
                    hitGoodAng = true;
                }
            }
            if (!res.valid) {
                for (int k = 0; k < 36; ++k) {
                    const float ang = (static_cast<float>(k) + 0.5f) * (6.283185307f / 36.0f);
                    if (trySweepRay(ang, 0.85f, {0.46f, 0.20f}) && !hitGoodAng) {
                        firstGoodAng = ang;
                        hitGoodAng = true;
                    }
                }
            }
            if (hitGoodAng) {
                for (float dDeg : {-0.032f, -0.016f, 0.016f, 0.032f}) {
                    trySweepRay(firstGoodAng + dDeg, 0.54f, {0.0f, 0.0f});
                    trySweepRay(firstGoodAng + dDeg, 0.72f, {0.0f, 0.12f});
                }
            }
        }
        return res;
    };

    if (!confirmedPots.empty()) {
        struct SpinPowerProfile {
            Vec2 spin;
            float powerMul;
            bool needFullCut;
        };
        constexpr std::array<SpinPowerProfile, 9> kGoatProfiles = {{
            {{0.0f, -0.12f}, 0.80f, false},   // Soft pocket hold
            {{0.0f, 0.24f}, 0.98f, false},    // Crisp half-ball roll
            {{0.0f, 0.58f}, 1.08f, false},    // Medium follow
            {{0.0f, 0.94f}, 1.32f, false},    // Power follow / K-ball cluster breakout
            {{0.0f, -0.34f}, 1.04f, false},   // Crisp stun stop
            {{0.0f, -0.68f}, 1.14f, false},   // Firm screw draw
            {{0.0f, -0.95f}, 1.32f, false},   // Deep pro power draw
            {{0.42f, 0.30f}, 1.08f, true},    // Top right english 2-rail shape
            {{-0.42f, 0.30f}, 1.08f, true},   // Top left english 2-rail shape
        }};
        constexpr std::array<SpinPowerProfile, 7> kMasterProfiles = {{
            {{0.0f, -0.14f}, 0.85f, false},  // Soft hold speed
            {{0.0f, 0.48f}, 1.05f, false},   // Smooth top-spin follow
            {{0.0f, 0.86f}, 1.22f, false},   // Power follow / cluster breakout
            {{0.0f, -0.52f}, 1.08f, false},  // Smooth screw draw
            {{0.0f, -0.88f}, 1.24f, false},  // Deep power draw
            {{0.32f, 0.26f}, 1.06f, true},   // Right english rail shape
            {{-0.32f, 0.26f}, 1.06f, true},  // Left english rail shape
        }};
        constexpr std::array<SpinPowerProfile, 2> kMedProfiles = {{
            {{0.0f, 0.42f}, 1.04f, false},
            {{0.0f, -0.45f}, 1.06f, false},
        }};

        struct PotFinalist {
            ShotKind kind = ShotKind::DirectPot;
            Vec2 dir{1.0f, 0.0f};
            float power = 0.45f;
            Vec2 spin{0.0f, 0.0f};
            float cueDist = 2.0f;
            float cutCos = 0.8f;
            float diffFactor = 1.0f;
            float score1Ply = -1e9f;
            AiSimOutcome outcome;
        };
        std::vector<PotFinalist> finalists;
        finalists.reserve(48);

        auto scorePot1Ply = [&](const AiSimOutcome& o, Vec2 sp) {
            if (o.wonGame) return 100000.0f - sp.length();
            return 600.0f + static_cast<float>(o.gainedScore) * 18.0f +
                   static_cast<float>(o.ownPotted) * 32.0f - static_cast<float>(o.oppPotted) * 60.0f +
                   evaluatePostRolloutPosition(o, tiedBefore) - sp.length() * (isGoat ? 0.6f : 1.2f);
        };

        for (const VerifiedPot& vp : confirmedPots) {
            finalists.push_back(PotFinalist{vp.kind, vp.calibratedPhysDir, vp.basePower, vp.baseSpin,
                                            vp.cueDist, vp.cutCos, vp.difficultyFactor,
                                            scorePot1Ply(vp.baseOutcome, vp.baseSpin), vp.baseOutcome});
            if (vp.baseOutcome.wonGame) break;

            const auto* profPtr = isGoat ? kGoatProfiles.data()
                                         : (diff == BilliardsAiDifficulty::Master ? kMasterProfiles.data()
                                                                                  : kMedProfiles.data());
            const std::size_t profCount =
                isGoat
                    ? kGoatProfiles.size()
                    : (diff == BilliardsAiDifficulty::Master ? kMasterProfiles.size() : kMedProfiles.size());

            for (std::size_t pi = 0; pi < profCount; ++pi) {
                const SpinPowerProfile& pf = profPtr[pi];
                if (pf.needFullCut &&
                    (vp.cutCos < (isGoat ? 0.52f : 0.65f) || vp.cueDist > 18.0f * radius_)) {
                    continue;
                }
                const float testPwr = std::clamp(vp.basePower * pf.powerMul, 0.14f, 0.95f);
                const AiSimOutcome spinOut =
                    simulateShotRollout(whitePos, vp.calibratedPhysDir, testPwr, pf.spin);
                if (spinOut.foul || spinOut.lostGame || (!spinOut.continuesTurn && !spinOut.wonGame)) {
                    continue;
                }
                finalists.push_back(PotFinalist{vp.kind, vp.calibratedPhysDir, testPwr, pf.spin, vp.cueDist,
                                                vp.cutCos, vp.difficultyFactor,
                                                scorePot1Ply(spinOut, pf.spin), spinOut});
            }
        }

        std::sort(finalists.begin(), finalists.end(),
                  [](const PotFinalist& a, const PotFinalist& b) { return a.score1Ply > b.score1Ply; });

        PotFinalist bestFin = finalists.front();
        bool bestHasVerifiedPly2 = bestFin.outcome.wonGame;

        // Goat 2-Ply Full Physics Rollout Continuation Verification across top 5 finalists!
        if (isGoat && !bestFin.outcome.wonGame) {
            float bestCombined = -1e9f;
            const std::size_t verifyCount = std::min<std::size_t>(finalists.size(), 5u);
            for (std::size_t fi = 0; fi < verifyCount; ++fi) {
                float ply2PosScore = -30.0f;
                const bool ok2 = hasVerifiedSecondPlyPot(finalists[fi].outcome, &ply2PosScore);
                const float total =
                    finalists[fi].score1Ply + (ok2 ? (220.0f + ply2PosScore * 0.72f) : -45.0f);
                if (total > bestCombined) {
                    bestCombined = total;
                    bestFin = finalists[fi];
                    bestHasVerifiedPly2 = ok2;
                }
            }
        }

        chosenKind = bestFin.kind;
        chosenPhysDir = bestFin.dir;
        chosenPower = bestFin.power;
        chosenSpin = bestFin.spin;
        chosenCueDist = bestFin.cueDist;
        chosenCutCos = bestFin.cutCos;
        chosenDiffFactor = bestFin.diffFactor;

        if (bestFin.outcome.wonGame) {
            std::snprintf(aiIntentTag_.data(), aiIntentTag_.size(), "制胜收官");
        } else if (chosenKind == ShotKind::Kick) {
            std::snprintf(aiIntentTag_.data(), aiIntentTag_.size(), "解球传进");
        } else if (chosenKind == ShotKind::ComboPot) {
            std::snprintf(aiIntentTag_.data(), aiIntentTag_.size(), "传球进攻");
        } else if (chosenKind == ShotKind::BankPot) {
            std::snprintf(aiIntentTag_.data(), aiIntentTag_.size(), "翻袋进攻");
        } else if (tiedBefore > 0 &&
                   countTiedOwnBalls(bestFin.outcome.endBallPos, bestFin.outcome.endBallActive,
                                     bestFin.outcome.nextAiGroup,
                                     bestFin.outcome.nextSnookerTarget) < tiedBefore) {
            std::snprintf(aiIntentTag_.data(), aiIntentTag_.size(), "K球连清");
        } else if (isPro && (bestHasVerifiedPly2 || chosenSpin.length() > 0.25f)) {
            std::snprintf(aiIntentTag_.data(), aiIntentTag_.size(), "走位连清");
        } else {
            std::snprintf(aiIntentTag_.data(), aiIntentTag_.size(), "进攻下球");
        }

        // Snooker-Only Tactical Offence vs Lockdown Snooker Trade-off:
        // In 8-Ball / Free Clear, Goat NEVER passes up a confirmed legal pot (it runs out the rack!).
        // Only in WPBSA Snooker, if the sole pot is a dead-end isolated ball that opens the table, Goat may
        // lay a Lockdown Snooker instead.
        if (isGoat && config_.rule == BilliardsRule::Snooker && !bestFin.outcome.wonGame &&
            !bestHasVerifiedPly2) {
            const float deadEndExposure = evaluateOpponentThreat(bestFin.outcome);
            if (deadEndExposure > 24.0f) {
                const DefResult defAlt = runDefensiveSearch();
                if (defAlt.valid && !defAlt.foundPot && defAlt.bestThreat <= -90.0f &&
                    defAlt.outcome.oppPotted == 0) {
                    chosenKind = defAlt.kind;
                    chosenPhysDir = defAlt.dir;
                    chosenPower = defAlt.power;
                    chosenSpin = defAlt.spin;
                    chosenCueDist = defAlt.cueDist;
                    chosenCutCos = 0.3f;
                    chosenDiffFactor = defAlt.diffFactor;
                    std::snprintf(aiIntentTag_.data(), aiIntentTag_.size(), "弃攻做球");
                }
            }
        }
    } else {
        const bool isSnookerRule = (config_.rule == BilliardsRule::Snooker);
        const DefResult defRes = runDefensiveSearch();
        if (defRes.valid) {
            chosenKind = defRes.kind;
            chosenPhysDir = defRes.dir;
            chosenPower = defRes.power;
            chosenSpin = defRes.spin;
            chosenCueDist = defRes.cueDist;
            chosenCutCos = 0.3f;
            chosenDiffFactor = defRes.diffFactor;
            if (defRes.foundPot) {
                std::snprintf(aiIntentTag_.data(), aiIntentTag_.size(), "解球进袋");
            } else if (defRes.bestThreat <= -90.0f) {
                std::snprintf(aiIntentTag_.data(), aiIntentTag_.size(),
                              isSnookerRule ? "做斯诺克" : "做障碍球");
            } else if (chosenKind == ShotKind::Kick) {
                std::snprintf(aiIntentTag_.data(), aiIntentTag_.size(), "多库解球");
            } else if (isSnookerRule) {
                std::snprintf(aiIntentTag_.data(), aiIntentTag_.size(), "回顶库防守");
            } else {
                std::snprintf(aiIntentTag_.data(), aiIntentTag_.size(), "安全防守");
            }
        } else {
            std::snprintf(aiIntentTag_.data(), aiIntentTag_.size(),
                          isSnookerRule ? "解斯诺克" : "解障碍球");
        }
    }

    // Medium adds realistic club-player imperfection; Master & Goat execute physics-verified plans cleanly!
    if (diff == BilliardsAiDifficulty::Medium) {
        const float ghostErr = nextSignedJitter() * 0.085f * radius_ * chosenDiffFactor;
        const float angErr = std::atan2(ghostErr, std::max(2.0f * radius_, chosenCueDist));
        const float cs = std::cos(angErr), sn = std::sin(angErr);
        chosenPhysDir = {chosenPhysDir.x * cs - chosenPhysDir.y * sn,
                         chosenPhysDir.x * sn + chosenPhysDir.y * cs};
        chosenPower = std::clamp(chosenPower * (1.0f + nextSignedJitter() * 0.025f), 0.12f, 0.96f);
    }

    // Crisp, decisive pro shot pacing for Goat ("球王"): ~1.75s routine pots, ~2.15s positional spin, ~2.55s
    // complex K-balls / combos / banks / tactical hooks / multi-rail escapes!
    if (isGoat) {
        if (chosenKind == ShotKind::DirectPot && chosenCutCos >= 0.62f && chosenSpin.length() < 0.45f &&
            chosenPower < 0.72f) {
            aiTempoScale_ = 0.68f;
        } else if (chosenKind == ShotKind::DirectPot && chosenPower < 0.80f) {
            aiTempoScale_ = 0.84f;
        } else {
            aiTempoScale_ = 1.00f;
        }
    }

    aiPlannedAimDir_ = normalizedOr(coords_.velocityToScene(chosenPhysDir), {1.0f, 0.0f});
    aiPlannedPower_ = chosenPower;
    aiPlannedSpin_ = chosenSpin;
}

void BilliardsMode::updateAiTurn(float dt) {
    if (!isAiTurn()) return;

    // Sub-phase 1: Deliberate ball-in-hand placement (`PlacingWhite`, ~1.40s total).
    if (state_ == BilliardsState::PlacingWhite) {
        constexpr float kPlacePause = 0.35f;
        constexpr float kPlaceGlide = 0.80f;
        constexpr float kPlaceSettle = 0.25f;
        constexpr float kPlaceTotal = kPlacePause + kPlaceGlide + kPlaceSettle;
        if (aiPhase_ != AiPhase::Placing) {
            aiPhase_ = AiPhase::Placing;
            aiPhaseTime_ = 0.0f;
            const Vec2 targetPhys = planAiWhitePlacementPhys();
            aiPlaceTargetScene_ = coords_.toScene(targetPhys);
            aiPlaceStartScene_ = placeScene_.isFinite() ? placeScene_ : coords_.toScene(whiteSpawn_);
        }
        aiPhaseTime_ += dt;
        const float u = smoothStep01((aiPhaseTime_ - kPlacePause) / kPlaceGlide);
        placeScene_ = aiPlaceStartScene_ + (aiPlaceTargetScene_ - aiPlaceStartScene_) * u;
        aimScene_ = placeScene_;
        if (aiPhaseTime_ >= kPlaceTotal) {
            if (!canPlaceWhite(aiPlaceTargetScene_)) {
                aiPlaceTargetScene_ = coords_.toScene(planAiWhitePlacementPhys());
            }
            if (canPlaceWhite(aiPlaceTargetScene_)) {
                placeScene_ = aiPlaceTargetScene_;
                world_.setTransform(white_, coords_.toPhysics(placeScene_), 0.0f);
                world_.setVelocity(white_, {0.0f, 0.0f}, 0.0f);
                world_.setEnabled(white_, true);
                world_.wake(white_);
                whitePocketed_ = false;
                ++whitePlacements_;
                state_ = BilliardsState::Ready;
            } else if (respawnWhite()) {
                ++whitePlacements_;
                state_ = BilliardsState::Ready;
            }
            aiPhase_ = AiPhase::Idle;
            aiPhaseTime_ = 0.0f;
        }
        return;
    }

    // Sub-phase 2: Calm table-reading pause + smooth cue sweep & spin adjustment (`Ready`).
    if (state_ == BilliardsState::Ready) {
        if (aiPhase_ != AiPhase::Aiming) {
            aiPhase_ = AiPhase::Aiming;
            aiPhaseTime_ = 0.0f;
            aiStartAimDir_ = normalizedOr(aimDir_, {1.0f, 0.0f});
            aiStartSpin_ = tipOffset_;
            planAiShot();
        }
        const float tempo = std::clamp(aiTempoScale_, 0.70f, 1.30f);
        const float readDelay = 0.55f * tempo;
        const float aimDuration = 0.95f * tempo;
        aiPhaseTime_ += dt;
        const float u = smoothStep01((aiPhaseTime_ - readDelay) / aimDuration);
        const float a0 = std::atan2(aiStartAimDir_.y, aiStartAimDir_.x);
        const float a1 = std::atan2(aiPlannedAimDir_.y, aiPlannedAimDir_.x);
        const float delta = std::remainder(a1 - a0, 6.283185307f);
        const float curA = a0 + delta * u;
        aimDir_ = {std::cos(curA), std::sin(curA)};
        aimScene_ = whiteSceneCenter() + aimDir_ * 140.0f;
        setTipOffset(aiStartSpin_ + (aiPlannedSpin_ - aiStartSpin_) * u);

        if (aiPhaseTime_ >= (readDelay + aimDuration)) {
            aimDir_ = aiPlannedAimDir_;
            lockedAimDir_ = aiPlannedAimDir_;
            setTipOffset(aiPlannedSpin_);
            pullDistancePt_ = 0.0f;
            state_ = BilliardsState::Charging;
            aiPhase_ = AiPhase::Charging;
            aiPhaseTime_ = 0.0f;
        }
        return;
    }

    // Sub-phase 3: Pre-shot feather stroke + deliberate pull-back + backswing pause (`Charging`).
    if (state_ == BilliardsState::Charging) {
        const float tempo = std::clamp(aiTempoScale_, 0.70f, 1.30f);
        const float featherDuration = 0.38f * tempo;
        const float pullDuration = 0.52f * tempo;
        const float holdDuration = 0.25f * tempo;
        const float chargeTotal = featherDuration + pullDuration + holdDuration;
        if (aiPhase_ != AiPhase::Charging) {
            aiPhase_ = AiPhase::Charging;
            aiPhaseTime_ = 0.0f;
            lockedAimDir_ = normalizedOr(aimDir_, {1.0f, 0.0f});
        }
        aiPhaseTime_ += dt;
        const float targetPull =
            std::max(config_.cancelPullPoints + 4.0f, aiPlannedPower_ * config_.maxPullPoints);
        if (aiPhaseTime_ < featherDuration) {
            const float f = std::clamp(aiPhaseTime_ / featherDuration, 0.0f, 1.0f);
            const float featherAmp = std::min(22.0f, targetPull * 0.28f);
            pullDistancePt_ = featherAmp * std::sin(f * 3.14159265f);
        } else {
            const float u = smoothStep01((aiPhaseTime_ - featherDuration) / pullDuration);
            pullDistancePt_ = targetPull * u;
        }

        if (aiPhaseTime_ >= chargeTotal) {
            pullDistancePt_ = 0.0f;
            aiPhase_ = AiPhase::Idle;
            aiPhaseTime_ = 0.0f;
            shootDirection(lockedAimDir_, aiPlannedPower_);
        }
    }
}

Color BilliardsMode::standardBallColor(int number, BilliardsRule rule) {
    if (rule == BilliardsRule::Snooker) {
        switch (number) {
            case 1:
                return {214, 38, 38, 255};
            case 2:
                return {238, 198, 36, 255};
            case 3:
                return {34, 154, 84, 255};
            case 4:
                return {146, 86, 44, 255};
            case 5:
                return {38, 108, 218, 255};
            case 6:
                return {238, 118, 168, 255};
            case 7:
                return {24, 26, 32, 255};
            default:
                return {214, 38, 38, 255};
        }
    }
    const int wrapped = ((std::max(1, number) - 1) % 15) + 1;
    if (wrapped == 8) return {20, 22, 28, 255};
    switch (((wrapped - 1) % 7) + 1) {
        case 1:
            return {240, 196, 32, 255};  // Yellow (1, 9)
        case 2:
            return {36, 102, 214, 255};  // Blue (2, 10)
        case 3:
            return {218, 44, 44, 255};  // Red (3, 11)
        case 4:
            return {126, 58, 188, 255};  // Purple (4, 12)
        case 5:
            return {238, 122, 34, 255};  // Orange (5, 13)
        case 6:
            return {32, 152, 78, 255};  // Green (6, 14)
        default:
            return {142, 42, 48, 255};  // Maroon (7, 15)
    }
}

void BilliardsMode::drawStyledBall(DrawList& out, const Ball& b, Vec2 center, float r) const {
    const bool hasIcon = b.texture != kNoTexture;
    const Color col = standardBallColor(b.number, config_.rule);

    if (config_.style == BilliardsStyle::Classic) {
        out.add(CircleItem{center, r, Color{235, 235, 228, 255}, Color{40, 40, 40, 160}, 1.0f});
        if (hasIcon) {
            SpriteItem s;
            s.texture = b.texture;
            s.center = center;
            s.size = {r * 1.5f, r * 1.5f};
            s.clipCircle = true;
            out.add(s);
        }
        return;
    }

    // Borderless icon-ball mode when user hides ball borders (`!config_.showBallBorders`).
    if (hasIcon && !config_.showBallBorders) {
        SpriteItem sp;
        sp.texture = b.texture;
        sp.center = center;
        sp.size = {r * 1.90f, r * 1.90f};
        sp.clipCircle = true;
        sp.interactive = true;
        out.add(sp);
        if (config_.showBallNumbers) {
            out.add(CircleItem{center, std::max(4.8f, r * 0.42f), Color{250, 250, 246, 220},
                               Color{20, 24, 32, 165}, 0.8f});
            char nbuf[16];
            std::snprintf(nbuf, sizeof(nbuf), "%d", b.number);
            const float fs = std::clamp(r * 0.58f, 8.0f, 11.5f);
            out.addText({center.x, center.y - fs * 0.64f}, nbuf, Color{16, 20, 28, 255}, fs, true);
        }
        return;
    }

    switch (b.group) {
        case BallGroup::FreeBall: {
            if (hasIcon) {
                out.add(CircleItem{center, r, Color{238, 238, 232, 255}, Color{36, 40, 48, 175}, 1.0f});
                SpriteItem sp;
                sp.texture = b.texture;
                sp.center = center;
                sp.size = {r * 1.58f, r * 1.58f};
                sp.clipCircle = true;
                sp.interactive = true;
                out.add(sp);
            } else {
                out.add(CircleItem{center, r, col, Color{28, 30, 36, 185}, 1.0f});
                out.add(CircleItem{center, r * 0.45f, Color{248, 248, 244, 255}, {}, 0.0f});
            }
            break;
        }
        case BallGroup::Solid: {
            out.add(CircleItem{center, r, col, Color{22, 24, 30, 195}, 1.0f});
            if (hasIcon) {
                out.add(CircleItem{center, r * 0.74f, Color{246, 246, 242, 248}, {}, 0.0f});
                SpriteItem sp;
                sp.texture = b.texture;
                sp.center = center;
                sp.size = {r * 1.42f, r * 1.42f};
                sp.clipCircle = true;
                sp.interactive = true;
                out.add(sp);
            } else {
                out.add(CircleItem{center, r * 0.45f, Color{248, 248, 244, 255}, {}, 0.0f});
            }
            break;
        }
        case BallGroup::Stripe: {
            // Strictly circular ivory sphere; curved left/right colored stripe arcs stay 100% inside r.
            out.add(CircleItem{center, r, Color{248, 248, 242, 255}, {}, 0.0f});
            if (hasIcon) {
                const float arcR = r * 0.84f;
                const float arcW = std::max(1.8f, r * 0.24f);
                constexpr int kSegs = 2;
                for (int sideSign : {-1, 1}) {
                    const float baseAngle = sideSign > 0 ? 0.0f : 3.14159265f;
                    for (int k = 0; k < kSegs; ++k) {
                        const float t0 = -0.72f + 1.44f * (static_cast<float>(k) / kSegs);
                        const float t1 = -0.72f + 1.44f * (static_cast<float>(k + 1) / kSegs);
                        const Vec2 p0{center.x + std::cos(baseAngle + t0) * arcR,
                                      center.y + std::sin(baseAngle + t0) * arcR};
                        const Vec2 p1{center.x + std::cos(baseAngle + t1) * arcR,
                                      center.y + std::sin(baseAngle + t1) * arcR};
                        out.add(LineItem{p0, p1, col, arcW, false});
                    }
                }
                out.add(CircleItem{center, r * 0.72f, Color{247, 247, 242, 250}, {}, 0.0f});
                SpriteItem sp;
                sp.texture = b.texture;
                sp.center = center;
                sp.size = {r * 1.38f, r * 1.38f};
                sp.clipCircle = true;
                sp.interactive = true;
                out.add(sp);
            } else {
                out.add(LineItem{{center.x - r * 0.44f, center.y},
                                 {center.x + r * 0.44f, center.y},
                                 col,
                                 r * 0.94f,
                                 false});
                out.add(CircleItem{center, r * 0.44f, Color{248, 248, 244, 255}, {}, 0.0f});
            }
            // Crisp outer circular rim drawn last so perimeter is a razor-clean circle.
            out.add(CircleItem{center, r, {0, 0, 0, 0}, Color{28, 30, 38, 195}, 1.0f});
            break;
        }
        case BallGroup::Eight: {
            out.add(CircleItem{center, r, Color{18, 20, 26, 255}, Color{238, 196, 78, 245},
                               std::max(1.5f, r * 0.14f)});
            if (hasIcon) {
                out.add(
                    CircleItem{center, r * 0.73f, Color{34, 38, 48, 245}, Color{238, 196, 78, 165}, 0.9f});
                SpriteItem sp;
                sp.texture = b.texture;
                sp.center = center;
                sp.size = {r * 1.38f, r * 1.38f};
                sp.clipCircle = true;
                sp.interactive = true;
                out.add(sp);
            } else {
                out.add(CircleItem{center, r * 0.45f, Color{248, 248, 244, 255}, {}, 0.0f});
            }
            break;
        }
        case BallGroup::SnookerRed:
        case BallGroup::SnookerColor: {
            const Color rim = b.number == 7 ? Color{210, 215, 228, 215} : Color{22, 24, 30, 185};
            out.add(CircleItem{center, r, col, rim, 1.0f});
            if (hasIcon) {
                out.add(CircleItem{center, r * 0.73f, Color{246, 246, 242, 246}, {}, 0.0f});
                SpriteItem sp;
                sp.texture = b.texture;
                sp.center = center;
                sp.size = {r * 1.40f, r * 1.40f};
                sp.clipCircle = true;
                sp.interactive = true;
                out.add(sp);
            } else {
                out.add(CircleItem{{center.x - r * 0.30f, center.y - r * 0.30f},
                                   std::max(1.3f, r * 0.18f),
                                   Color{255, 255, 255, 115},
                                   {},
                                   0.0f});
            }
            break;
        }
    }

    const bool drawNum = config_.showBallNumbers || (!hasIcon && config_.rule != BilliardsRule::Snooker);
    if (drawNum) {
        if (hasIcon || config_.rule == BilliardsRule::Snooker) {
            out.add(CircleItem{center, std::max(4.8f, r * 0.42f), Color{250, 250, 246, 226},
                               Color{20, 24, 32, 180}, 0.8f});
        }
        char nbuf[16];
        std::snprintf(nbuf, sizeof(nbuf), "%d", b.number);
        const float fs = std::clamp(r * 0.58f, 8.0f, 11.5f);
        out.addText({center.x, center.y - fs * 0.64f}, nbuf, Color{16, 20, 28, 255}, fs, true);
    }
}

void BilliardsMode::buildDrawList(DrawList& out) const {
    if (extraWorld_) {
        for (const ExtraIcon& ex : extraIcons_) {
            const BodyState bs = extraWorld_->state(ex.body);
            SpriteItem sp;
            sp.texture = ex.texture;
            sp.center = coords_.toScene(bs.position);
            sp.size = ex.visualSize;
            sp.rotation = SceneToPhysics::angleToScene(bs.angle);
            sp.clipCircle = false;
            sp.interactive = true;
            out.add(sp);
        }
    }

    const Vec2 tl = coords_.toScene({tableMin_.x, tableMin_.y + tableSize_.y});
    const Vec2 br = coords_.toScene({tableMin_.x + tableSize_.x, tableMin_.y});
    const float r = coords_.lengthToScene(radius_);
    const float prCorner = coords_.lengthToScene(pocketRadius_);

    if (config_.style == BilliardsStyle::Classic) {
        const Color rail{120, 72, 40, 230};
        out.add(LineItem{tl, {br.x, tl.y}, rail, 6.0f, false});
        out.add(LineItem{{br.x, tl.y}, br, rail, 6.0f, false});
        out.add(LineItem{br, {tl.x, br.y}, rail, 6.0f, false});
        out.add(LineItem{{tl.x, br.y}, tl, rail, 6.0f, false});
        for (const Vec2& p : pockets_)
            out.add(CircleItem{coords_.toScene(p), prCorner, Color{10, 10, 10, 230}, {}, 0.0f});
    } else if (config_.effectiveTableTheme() == BilliardsTableTheme::Full) {
        const bool snk = (config_.rule == BilliardsRule::Snooker);
        const float tableL = br.x - tl.x;
        const float tableW = br.y - tl.y;
        const float midSceneX = (tl.x + br.x) * 0.5f;
        const float midSceneY = (tl.y + br.y) * 0.5f;

        // 1. Table bed markings (wallpaper remains uncovered).
        if (snk) {
            const float baulkX = tl.x + tableL * BilliardsConfig::kSnookerBaulkRatio;
            const float dR = tableW * BilliardsConfig::kSnookerDRatio;
            out.add(LineItem{
                {baulkX, tl.y + 2.0f}, {baulkX, br.y - 2.0f}, Color{255, 255, 255, 58}, 1.1f, false});
            constexpr int kDSegs = 14;
            for (int i = 0; i < kDSegs; ++i) {
                const float a0 = -1.5707963f + 3.1415926f * (static_cast<float>(i) / kDSegs);
                const float a1 = -1.5707963f + 3.1415926f * (static_cast<float>(i + 1) / kDSegs);
                const Vec2 p0{baulkX - std::cos(a0) * dR, midSceneY + std::sin(a0) * dR};
                const Vec2 p1{baulkX - std::cos(a1) * dR, midSceneY + std::sin(a1) * dR};
                out.add(LineItem{p0, p1, Color{255, 255, 255, 58}, 1.1f, false});
            }
            for (float fx : {0.50f, 0.75f, BilliardsConfig::kSnookerBlackRatio}) {
                out.add(
                    CircleItem{{tl.x + tableL * fx, midSceneY}, 2.0f, Color{255, 255, 255, 115}, {}, 0.0f});
            }
        } else {
            const float headX = tl.x + tableL * 0.25f;
            const float footX = tl.x + tableL * 0.75f;
            out.add(
                LineItem{{headX, tl.y + 2.0f}, {headX, br.y - 2.0f}, Color{255, 255, 255, 45}, 1.0f, false});
            out.add(CircleItem{{headX, midSceneY}, 2.2f, Color{255, 255, 255, 100}, {}, 0.0f});
            out.add(CircleItem{{footX, midSceneY}, 2.5f, Color{255, 255, 255, 120}, {}, 0.0f});
        }

        // 2. Broad wooden rail apron + metallic corner/middle pocket castings.
        const float railOuter = railOuterWidthScene();
        const float cDepth = coords_.lengthToScene((snk ? 1.28f : 1.20f) * radius_);
        const float woodSubW = railOuter - cDepth * 0.25f;
        const float woodSubOff = cDepth * 0.25f + woodSubW * 0.5f;
        const float woodBandW = std::max(4.0f, railOuter - cDepth);
        const float woodCenterOff = cDepth + woodBandW * 0.5f;
        const Color woodFill = snk ? Color{82, 40, 22, 246} : Color{72, 46, 28, 245};
        const Color castingFill = snk ? Color{198, 162, 86, 245} : Color{168, 176, 188, 245};
        const Color outerTrim = snk ? Color{216, 180, 102, 195} : Color{192, 200, 212, 175};

        // Solid wooden sub-rail apron (eliminates any wallpaper gaps behind bevelled cushion jaws).
        const float sxL = tl.x - woodSubOff, sxR = br.x + woodSubOff;
        const float syT = tl.y - woodSubOff, syB = br.y + woodSubOff;
        out.add(LineItem{{sxL, syT}, {sxR, syT}, woodFill, woodSubW, false});
        out.add(LineItem{{sxR, syT}, {sxR, syB}, woodFill, woodSubW, false});
        out.add(LineItem{{sxR, syB}, {sxL, syB}, woodFill, woodSubW, false});
        out.add(LineItem{{sxL, syB}, {sxL, syT}, woodFill, woodSubW, false});

        const float wxL = tl.x - woodCenterOff, wxR = br.x + woodCenterOff;
        const float wyT = tl.y - woodCenterOff, wyB = br.y + woodCenterOff;

        // Subtle metallic outer edge trim aligned with rounded outer rail corners.
        const float oxL = tl.x - railOuter, oxR = br.x + railOuter;
        const float oyT = tl.y - railOuter, oyB = br.y + railOuter;
        const float trimInset = woodSubW * 0.52f;
        out.add(LineItem{{oxL + trimInset, oyT}, {oxR - trimInset, oyT}, outerTrim, 1.3f, false});
        out.add(LineItem{{oxL + trimInset, oyB}, {oxR - trimInset, oyB}, outerTrim, 1.3f, false});
        out.add(LineItem{{oxL, oyT + trimInset}, {oxL, oyB - trimInset}, outerTrim, 1.3f, false});
        out.add(LineItem{{oxR, oyT + trimInset}, {oxR, oyB - trimInset}, outerTrim, 1.3f, false});

        // 3. Six tournament cushion cloth strips between pocket mouths + bevelled jaw edges.
        const float cMouth = coords_.lengthToScene(cornerMouth_);
        const float sMouth = coords_.lengthToScene(sideMouth_);
        const float cushMid = cDepth * 0.5f;
        const Color cushBody = snk ? Color{28, 118, 66, 244} : Color{32, 96, 152, 242};
        const Color cushNose = snk ? Color{76, 188, 114, 245} : Color{82, 164, 228, 245};

        const float capIn = cDepth * 0.42f;
        out.add(LineItem{{tl.x + cMouth + capIn, tl.y - cushMid},
                         {midSceneX - sMouth - capIn, tl.y - cushMid},
                         cushBody,
                         cDepth,
                         false});
        out.add(LineItem{{midSceneX + sMouth + capIn, tl.y - cushMid},
                         {br.x - cMouth - capIn, tl.y - cushMid},
                         cushBody,
                         cDepth,
                         false});
        out.add(LineItem{{tl.x + cMouth + capIn, br.y + cushMid},
                         {midSceneX - sMouth - capIn, br.y + cushMid},
                         cushBody,
                         cDepth,
                         false});
        out.add(LineItem{{midSceneX + sMouth + capIn, br.y + cushMid},
                         {br.x - cMouth - capIn, br.y + cushMid},
                         cushBody,
                         cDepth,
                         false});
        out.add(LineItem{{tl.x - cushMid, tl.y + cMouth + capIn},
                         {tl.x - cushMid, br.y - cMouth - capIn},
                         cushBody,
                         cDepth,
                         false});
        out.add(LineItem{{br.x + cushMid, tl.y + cMouth + capIn},
                         {br.x + cushMid, br.y - cMouth - capIn},
                         cushBody,
                         cDepth,
                         false});

        // Crisp inner cushion nose lines + bevelled pocket jaw lines into the rail.
        const float cBack = coords_.lengthToScene((snk ? 0.66f : 0.62f) * radius_);
        const float sBack = coords_.lengthToScene((snk ? 1.06f : 1.02f) * radius_);
        auto drawCushionEdge = [&](Vec2 aBack, Vec2 aNose, Vec2 bNose, Vec2 bBack) {
            out.add(LineItem{aBack, aNose, cushNose, 1.8f, false});
            out.add(LineItem{aNose, bNose, cushNose, 1.8f, false});
            out.add(LineItem{bNose, bBack, cushNose, 1.8f, false});
        };
        drawCushionEdge({tl.x + cBack, tl.y - cDepth}, {tl.x + cMouth, tl.y}, {midSceneX - sMouth, tl.y},
                        {midSceneX - sBack, tl.y - cDepth});
        drawCushionEdge({midSceneX + sBack, tl.y - cDepth}, {midSceneX + sMouth, tl.y}, {br.x - cMouth, tl.y},
                        {br.x - cBack, tl.y - cDepth});
        drawCushionEdge({tl.x + cBack, br.y + cDepth}, {tl.x + cMouth, br.y}, {midSceneX - sMouth, br.y},
                        {midSceneX - sBack, br.y + cDepth});
        drawCushionEdge({midSceneX + sBack, br.y + cDepth}, {midSceneX + sMouth, br.y}, {br.x - cMouth, br.y},
                        {br.x - cBack, br.y + cDepth});
        drawCushionEdge({tl.x - cDepth, tl.y + cBack}, {tl.x, tl.y + cMouth}, {tl.x, br.y - cMouth},
                        {tl.x - cDepth, br.y - cBack});
        drawCushionEdge({br.x + cDepth, tl.y + cBack}, {br.x, tl.y + cMouth}, {br.x, br.y - cMouth},
                        {br.x + cDepth, br.y - cBack});

        // 4. Six recessed pocket wells nestled strictly inside the rail & castings.
        for (std::size_t i = 0; i < pockets_.size(); ++i) {
            const Vec2 pc = coords_.toScene(pockets_[i]);
            const float holeR = pocketHoleRadiusScene(i);
            out.add(CircleItem{pc, holeR, Color{10, 12, 16, 248}, castingFill, std::max(2.0f, r * 0.16f)});
            // Subtle inner dark leather pocket shadow ring.
            out.add(CircleItem{pc, holeR * 0.80f, Color{5, 6, 9, 252}, Color{45, 50, 60, 120}, 1.0f});
        }

        // 5. 18 diamond sights on Heyball / 8-Ball / Free tables only (Snooker tables have no pool diamonds).
        if (!snk) {
            const float sightR = std::clamp(r * 0.12f, 1.5f, 2.5f);
            const Color sightColor{245, 240, 226, 205};
            for (int k : {1, 2, 3, 5, 6, 7}) {
                const float sx = tl.x + tableL * (static_cast<float>(k) / 8.0f);
                out.add(CircleItem{{sx, wyT}, sightR, sightColor, {}, 0.0f});
                out.add(CircleItem{{sx, wyB}, sightR, sightColor, {}, 0.0f});
            }
            for (int k : {1, 2, 3}) {
                const float sy = tl.y + tableW * (static_cast<float>(k) / 4.0f);
                out.add(CircleItem{{wxL, sy}, sightR, sightColor, {}, 0.0f});
                out.add(CircleItem{{wxR, sy}, sightR, sightColor, {}, 0.0f});
            }
        }

        // 6. Two proportional side ball-return tubes.
        for (int side = 0; side < 2; ++side) {
            const TubeGeometry tg = tubeGeometry(side);
            out.add(
                LineItem{{tg.cx, tg.topY}, {tg.cx, tg.botY}, Color{16, 20, 28, 95}, tg.capR * 2.0f, false});
            out.add(CircleItem{
                {tg.cx, tg.botY}, tg.capR, Color{24, 30, 40, 120}, Color{230, 240, 255, 145}, 1.8f});
            out.add(CircleItem{
                {tg.cx, tg.topY}, tg.capR, Color{20, 24, 32, 75}, Color{230, 240, 255, 110}, 1.5f});
            out.add(LineItem{{tg.cx - tg.capR, tg.topY},
                             {tg.cx - tg.capR, tg.botY},
                             Color{230, 240, 255, 140},
                             1.8f,
                             false});
            out.add(LineItem{{tg.cx + tg.capR, tg.topY},
                             {tg.cx + tg.capR, tg.botY},
                             Color{230, 240, 255, 140},
                             1.8f,
                             false});

            const auto& order = tubeOrder_[static_cast<std::size_t>(side)];
            for (std::size_t s = tg.firstVisible; s < tg.firstVisible + tg.visibleCount && s < order.size();
                 ++s) {
                const std::size_t bi = order[s];
                drawStyledBall(out, balls_[bi], tubeBallSceneCenter(bi, tg), r);
            }
            if (config_.matchMode != BilliardsMatchMode::Solo) {
                char hbuf[32];
                const char* pTag = playerLabel(side);
                if (tg.overflow > 0) {
                    std::snprintf(hbuf, sizeof(hbuf), "%s +%zu", pTag, tg.overflow);
                } else {
                    std::snprintf(hbuf, sizeof(hbuf), "%s (%zu)", pTag, order.size());
                }
                const Color pCol = (side == 0) ? Color{252, 214, 136, 245} : Color{148, 206, 255, 245};
                out.addText({tg.cx, std::max(2.0f, tg.topY - tg.capR - 18.0f)}, hbuf, pCol, 11.5f, true);
            } else if (tg.overflow > 0) {
                char obuf[32];
                std::snprintf(obuf, sizeof(obuf), "+%zu", tg.overflow);
                out.addText({tg.cx, std::max(2.0f, tg.topY - 7.0f)}, obuf, Color{255, 240, 180, 245}, 11.5f,
                            true);
            }
        }
    } else if (config_.effectiveTableTheme() == BilliardsTableTheme::Minimal) {
        // Purist minimalist wireframe table: translucent thin cushion/jaw lines + 6 hollow pocket rings.
        const bool snk = (config_.rule == BilliardsRule::Snooker);
        const float midSceneX = (tl.x + br.x) * 0.5f;
        const float cDepth = coords_.lengthToScene((snk ? 1.28f : 1.20f) * radius_);
        const float cMouth = coords_.lengthToScene(cornerMouth_);
        const float sMouth = coords_.lengthToScene(sideMouth_);
        const float cBack = coords_.lengthToScene((snk ? 0.66f : 0.62f) * radius_);
        const float sBack = coords_.lengthToScene((snk ? 1.06f : 1.02f) * radius_);

        auto clipJawToPocket = [&](Vec2 nose, Vec2 back, std::size_t pocketIdx) -> Vec2 {
            if (pocketIdx >= pockets_.size()) return back;
            const Vec2 pc = coords_.toScene(pockets_[pocketIdx]);
            const float holeR = pocketHoleRadiusScene(pocketIdx);
            const Vec2 u = nose - pc;
            const Vec2 d = back - nose;
            const float a = d.lengthSquared();
            if (a <= 1e-6f) return back;
            const float bHalf = u.x * d.x + u.y * d.y;
            const float c = u.lengthSquared() - holeR * holeR;
            const float disc = bHalf * bHalf - a * c;
            if (disc < 0.0f) return back;
            const float t = (-bHalf - std::sqrt(disc)) / a;
            if (t > 0.02f && t < 1.35f) return nose + d * t;
            return back;
        };

        struct MinimalRail {
            Vec2 aEnd;
            Vec2 aNose;
            Vec2 bNose;
            Vec2 bEnd;
        };
        const std::array<MinimalRail, 6> rails = {{
            {clipJawToPocket({tl.x + cMouth, tl.y}, {tl.x + cBack, tl.y - cDepth}, 0),
             {tl.x + cMouth, tl.y},
             {midSceneX - sMouth, tl.y},
             clipJawToPocket({midSceneX - sMouth, tl.y}, {midSceneX - sBack, tl.y - cDepth}, 1)},
            {clipJawToPocket({midSceneX + sMouth, tl.y}, {midSceneX + sBack, tl.y - cDepth}, 1),
             {midSceneX + sMouth, tl.y},
             {br.x - cMouth, tl.y},
             clipJawToPocket({br.x - cMouth, tl.y}, {br.x - cBack, tl.y - cDepth}, 2)},
            {clipJawToPocket({tl.x + cMouth, br.y}, {tl.x + cBack, br.y + cDepth}, 3),
             {tl.x + cMouth, br.y},
             {midSceneX - sMouth, br.y},
             clipJawToPocket({midSceneX - sMouth, br.y}, {midSceneX - sBack, br.y + cDepth}, 4)},
            {clipJawToPocket({midSceneX + sMouth, br.y}, {midSceneX + sBack, br.y + cDepth}, 4),
             {midSceneX + sMouth, br.y},
             {br.x - cMouth, br.y},
             clipJawToPocket({br.x - cMouth, br.y}, {br.x - cBack, br.y + cDepth}, 5)},
            {clipJawToPocket({tl.x, tl.y + cMouth}, {tl.x - cDepth, tl.y + cBack}, 0),
             {tl.x, tl.y + cMouth},
             {tl.x, br.y - cMouth},
             clipJawToPocket({tl.x, br.y - cMouth}, {tl.x - cDepth, br.y - cBack}, 3)},
            {clipJawToPocket({br.x, tl.y + cMouth}, {br.x + cDepth, tl.y + cBack}, 2),
             {br.x, tl.y + cMouth},
             {br.x, br.y - cMouth},
             clipJawToPocket({br.x, br.y - cMouth}, {br.x + cDepth, br.y - cBack}, 5)},
        }};

        const Color shadowStroke{10, 16, 26, 86};
        const Color lineStroke{236, 244, 255, 182};
        const Color pocketWellTint{10, 15, 24, 26};

        // Pass 1: subtle dark halo so minimalist lines remain legible on bright/light wallpapers.
        for (std::size_t i = 0; i < pockets_.size(); ++i) {
            out.add(CircleItem{
                coords_.toScene(pockets_[i]), pocketHoleRadiusScene(i), {0, 0, 0, 0}, shadowStroke, 3.2f});
        }
        for (const MinimalRail& mr : rails) {
            out.add(LineItem{mr.aEnd, mr.aNose, shadowStroke, 3.2f, false});
            out.add(LineItem{mr.aNose, mr.bNose, shadowStroke, 3.2f, false});
            out.add(LineItem{mr.bNose, mr.bEnd, shadowStroke, 3.2f, false});
        }

        // Pass 2: crisp translucent frosted cushion lines & hollow pocket rings.
        for (std::size_t i = 0; i < pockets_.size(); ++i) {
            out.add(CircleItem{
                coords_.toScene(pockets_[i]), pocketHoleRadiusScene(i), pocketWellTint, lineStroke, 1.5f});
        }
        for (const MinimalRail& mr : rails) {
            out.add(LineItem{mr.aEnd, mr.aNose, lineStroke, 1.5f, false});
            out.add(LineItem{mr.aNose, mr.bNose, lineStroke, 1.5f, false});
            out.add(LineItem{mr.bNose, mr.bEnd, lineStroke, 1.5f, false});
        }
    }

    for (const Ball& b : balls_) {
        if (b.pocketed) continue;
        drawStyledBall(out, b, coords_.toScene(world_.state(b.body).position), r);
    }

    if (config_.style == BilliardsStyle::Desktop && state_ == BilliardsState::PlacingWhite) {
        const Vec2 gpos{std::clamp(placeScene_.x, r + 2.0f, sceneSize_.x - r - 2.0f),
                        std::clamp(placeScene_.y, r + 2.0f, sceneSize_.y - r - 2.0f)};
        const bool valid = canPlaceWhite(placeScene_);
        out.add(CircleItem{gpos, r, valid ? Color{255, 255, 255, 242} : Color{255, 255, 255, 85},
                           valid ? Color{52, 211, 153, 255} : Color{248, 113, 113, 195}, 2.2f});
    } else if (!whitePocketed_) {
        const Vec2 w = whiteSceneCenter();
        out.add(CircleItem{w, r, Color{255, 255, 255, 255}, Color{90, 90, 90, 200}, 1.0f});
        if (state_ == BilliardsState::Ready || state_ == BilliardsState::Charging) {
            if (config_.style == BilliardsStyle::Classic) {
                const Vec2 dir = normalizedOr(aimScene_ - w, Vec2{1.0f, 0.0f});
                out.add(LineItem{w + dir * r, w + dir * (r + 220.0f), Color{255, 255, 255, 150}, 1.5f, true});
                float charge = 0.0f;
                if (state_ == BilliardsState::Charging) {
                    charge = std::clamp(static_cast<float>(chargeElapsed_ / config_.fullChargeSeconds), 0.0f,
                                        1.0f);
                }
                const float gap = r + 6.0f + charge * 40.0f;
                out.add(
                    LineItem{w - dir * gap, w - dir * (gap + 180.0f), Color{196, 150, 92, 255}, 5.0f, false});
            } else {
                const Vec2 dir = state_ == BilliardsState::Charging ? lockedAimDir_ : aimDir_;
                const float cueLen = std::clamp(r * 13.5f, 140.0f, 340.0f);
                const float cueW = std::clamp(r * 0.26f, 3.2f, 5.2f);
                const Color cueWood =
                    currentPlayer_ == 1 ? Color{102, 154, 218, 255} : Color{196, 150, 92, 255};
                if (config_.showAimGuide) {
                    // Exact ray-cast aiming guide: finds first object-ball contact (ghost ball + cut angle)
                    // or first cushion nose contact!
                    float bestT = std::max(200.0f, (br.x - tl.x) * 1.1f);
                    const float minRX = tl.x + r, maxRX = br.x - r;
                    const float minRY = tl.y + r, maxRY = br.y - r;
                    if (dir.x > 1e-4f) bestT = std::min(bestT, std::max(0.0f, (maxRX - w.x) / dir.x));
                    if (dir.x < -1e-4f) bestT = std::min(bestT, std::max(0.0f, (minRX - w.x) / dir.x));
                    if (dir.y > 1e-4f) bestT = std::min(bestT, std::max(0.0f, (maxRY - w.y) / dir.y));
                    if (dir.y < -1e-4f) bestT = std::min(bestT, std::max(0.0f, (minRY - w.y) / dir.y));

                    bool hitBall = false;
                    Vec2 hitCenter{};
                    const float twoR = 2.0f * r;
                    const float twoR2 = twoR * twoR;
                    for (const Ball& b : balls_) {
                        if (b.pocketed) continue;
                        const Vec2 bc = coords_.toScene(world_.state(b.body).position);
                        const Vec2 oc = bc - w;
                        const float proj = oc.x * dir.x + oc.y * dir.y;
                        if (proj <= 0.0f) continue;
                        const float perp2 = oc.lengthSquared() - proj * proj;
                        if (perp2 >= twoR2) continue;
                        const float tHit = proj - std::sqrt(std::max(0.0f, twoR2 - perp2));
                        if (tHit >= 0.0f && tHit < bestT) {
                            bestT = tHit;
                            hitBall = true;
                            hitCenter = bc;
                        }
                    }

                    const Vec2 impact = w + dir * bestT;
                    if (bestT > r + 2.0f) {
                        out.add(
                            LineItem{w + dir * (r + 2.0f), impact, Color{255, 255, 255, 175}, 1.4f, true});
                    }
                    if (hitBall) {
                        // Ghost cue ball at exact point of contact + target-ball cut trajectory!
                        out.add(
                            CircleItem{impact, r, Color{255, 255, 255, 34}, Color{255, 255, 255, 185}, 1.3f});
                        const Vec2 cutDir = normalizedOr(hitCenter - impact, dir);
                        const float guideLen = std::clamp(r * 4.5f, 44.0f, 96.0f);
                        out.add(LineItem{hitCenter + cutDir * r, hitCenter + cutDir * (r + guideLen),
                                         Color{110, 236, 178, 220}, 1.6f, true});
                    } else {
                        out.add(CircleItem{
                            impact, std::max(2.4f, r * 0.22f), Color{255, 255, 255, 150}, {}, 0.0f});
                    }
                }
                const float pullback = state_ == BilliardsState::Charging ? pullDistancePt_ * 0.45f : 0.0f;
                const float gap = r + 5.0f + pullback;
                out.add(LineItem{w - dir * gap, w - dir * (gap + cueLen), cueWood, cueW, false});
                out.add(LineItem{w - dir * gap, w - dir * (gap + std::max(4.5f, r * 0.35f)),
                                 Color{245, 245, 240, 255}, cueW, false});
                if (tipOffset_.lengthSquared() > 1e-4f) {
                    const Vec2 dot{w.x + tipOffset_.x * r * 0.58f, w.y - tipOffset_.y * r * 0.58f};
                    out.add(CircleItem{dot, std::max(2.0f, r * 0.16f), Color{225, 58, 62, 245}, {}, 0.0f});
                }

                if (state_ == BilliardsState::Charging) {
                    const float power = std::clamp(pullDistancePt_ / config_.maxPullPoints, 0.0f, 1.0f);
                    const bool willCancel = pullDistancePt_ < config_.cancelPullPoints;
                    const float barW = 68.0f;
                    const float barX =
                        std::clamp(w.x, barW * 0.5f + 12.0f, sceneSize_.x - barW * 0.5f - 12.0f);
                    float barY = w.y + r + 14.0f;
                    if (barY + 24.0f > sceneSize_.y) barY = w.y - r - 28.0f;
                    out.add(LineItem{{barX - barW * 0.5f, barY},
                                     {barX + barW * 0.5f, barY},
                                     Color{15, 20, 28, 190},
                                     5.5f,
                                     false});
                    if (!willCancel && power > 0.0f) {
                        const auto cr =
                            static_cast<std::uint8_t>(std::clamp(74.0f + 174.0f * power, 0.0f, 255.0f));
                        const auto cg = static_cast<std::uint8_t>(
                            std::clamp(222.0f - 110.0f * power * power, 0.0f, 255.0f));
                        out.add(LineItem{{barX - barW * 0.5f, barY},
                                         {barX - barW * 0.5f + barW * power, barY},
                                         Color{cr, cg, 96, 245},
                                         5.5f,
                                         false});
                    }
                    char pbuf[48];
                    if (willCancel) {
                        std::snprintf(pbuf, sizeof(pbuf), "松开取消");
                    } else {
                        std::snprintf(pbuf, sizeof(pbuf), "%d%%",
                                      static_cast<int>(std::lround(power * 100.0f)));
                    }
                    out.addText({barX, barY + 5.0f}, pbuf,
                                willCancel ? Color{220, 225, 235, 225} : Color{255, 255, 255, 245}, 12.0f,
                                true);
                }
            }
        }
    }

    char buf[192];
    if (config_.style == BilliardsStyle::Classic) {
        std::snprintf(buf, sizeof(buf), "Pocketed %d/%zu  Shots %d%s", pocketedCount_, balls_.size(), shots_,
                      state_ == BilliardsState::Finished ? "  - cleared!" : "");
        out.addText({tl.x, tl.y - 28.0f}, buf, Color{255, 255, 255, 230}, 14.0f);
    } else {
        const float outerTop = tl.y - railOuterWidthScene();
        const float bannerY = std::clamp(outerTop - 30.0f, 6.0f, std::max(6.0f, outerTop - 22.0f));
        const bool twoPlayer = config_.matchMode != BilliardsMatchMode::Solo;
        const bool aiTurnNow = isAiTurn();
        const char* p0Name = playerLabel(0);
        const char* p1Name = playerLabel(1);
        char turnTagBuf[96] = "";
        const char* turnTag = "";
        if (config_.matchMode == BilliardsMatchMode::Hotseat) {
            turnTag = currentPlayer_ == 0 ? "P1回合 · " : "P2回合 · ";
        } else if (config_.matchMode == BilliardsMatchMode::VsAi) {
            if (currentPlayer_ == 0) {
                turnTag = "我方回合 · ";
            } else if (config_.showAiIntent && aiIntentTag_[0] != '\0') {
                std::snprintf(turnTagBuf, sizeof(turnTagBuf), "人机回合(%s)[%s] · ",
                              aiDifficultyName(config_.aiDifficulty), aiIntentTag_.data());
                turnTag = turnTagBuf;
            } else {
                std::snprintf(turnTagBuf, sizeof(turnTagBuf), "人机回合(%s) · ",
                              aiDifficultyName(config_.aiDifficulty));
                turnTag = turnTagBuf;
            }
        } else if (config_.matchMode == BilliardsMatchMode::Lan) {
            if (lanLocalPlayer_ >= 0) {
                turnTag = currentPlayer_ == lanLocalPlayer_ ? "我方回合 · " : "对方回合 · ";
            } else {
                turnTag = currentPlayer_ == 0 ? "房主回合 · " : "客机回合 · ";
            }
        }

        char seriesSuffix[40] = "";
        if (twoPlayer && (seriesWins_[0] + seriesWins_[1] > 0)) {
            std::snprintf(seriesSuffix, sizeof(seriesSuffix), " · 局分%d:%d", seriesWins_[0], seriesWins_[1]);
        }
        char breakSuffix[32] = "";
        if (config_.rule == BilliardsRule::Snooker && currentBreak_ > 0 &&
            state_ != BilliardsState::Finished) {
            std::snprintf(breakSuffix, sizeof(breakSuffix), " · 单杆%d", currentBreak_);
        }

        if (config_.rule == BilliardsRule::Free && !twoPlayer) {
            if (state_ == BilliardsState::Finished) {
                if (bestShots_ > 0) {
                    std::snprintf(buf, sizeof(buf), "全部进袋 · %d 杆（最少 %d 杆） · 点再来一局", shots_,
                                  bestShots_);
                } else {
                    std::snprintf(buf, sizeof(buf), "全部进袋 · 点再来一局");
                }
            } else if (state_ == BilliardsState::PlacingWhite) {
                std::snprintf(buf, sizeof(buf), "白球进袋 · 点击台面摆放白球 · 进袋 %d/%zu · %d 杆",
                              pocketedCount_, balls_.size(), shots_);
            } else if (bestShots_ > 0) {
                std::snprintf(buf, sizeof(buf), "进袋 %d/%zu · %d 杆（最少 %d 杆）", pocketedCount_,
                              balls_.size(), shots_, bestShots_);
            } else {
                std::snprintf(buf, sizeof(buf), "进袋 %d/%zu · %d 杆", pocketedCount_, balls_.size(), shots_);
            }
        } else if (config_.rule == BilliardsRule::Free) {
            if (state_ == BilliardsState::Finished) {
                std::snprintf(buf, sizeof(buf), "全部进袋 · %s %d : %d %s · %s获胜！%s", p0Name, scores_[0],
                              scores_[1], p1Name, winner_ == 1 ? p1Name : p0Name, seriesSuffix);
            } else if (state_ == BilliardsState::PlacingWhite) {
                std::snprintf(buf, sizeof(buf), "%s%s · %s %d : %d %s%s", turnTag,
                              aiTurnNow ? "人机正在摆放白球" : "白球进袋请摆球", p0Name, scores_[0],
                              scores_[1], p1Name, seriesSuffix);
            } else {
                std::snprintf(buf, sizeof(buf), "%s%s %d : %d %s · 进袋 %d/%zu%s", turnTag, p0Name,
                              scores_[0], scores_[1], p1Name, pocketedCount_, balls_.size(), seriesSuffix);
            }
        } else if (config_.rule == BilliardsRule::EightBall) {
            const BallGroup curGrp = playerGroups_[static_cast<std::size_t>(currentPlayer_ == 1 ? 1 : 0)];
            const char* targetStr =
                curGrp == BallGroup::FreeBall
                    ? "开放台"
                    : (remainingGroupBalls(curGrp) == 0 ? "击打黑八" : groupShortName(curGrp));
            if (state_ == BilliardsState::Finished) {
                if (turnNote_[0] != '\0') {
                    std::snprintf(buf, sizeof(buf), "八球结束 · %s · %d 杆%s", turnNote_.data(), shots_,
                                  seriesSuffix);
                } else {
                    std::snprintf(buf, sizeof(buf), "八球结束 · 点再来一局%s", seriesSuffix);
                }
            } else if (state_ == BilliardsState::PlacingWhite) {
                std::snprintf(buf, sizeof(buf), "%s%s · 目标:%s%s%s%s", turnTag,
                              aiTurnNow ? "自由球自动选位中" : "自由球点击台面摆白球", targetStr,
                              turnNote_[0] ? " · " : "", turnNote_.data(), seriesSuffix);
            } else if (twoPlayer) {
                std::snprintf(buf, sizeof(buf), "%s目标:%s · %s(%s)%d : %d(%s)%s%s", turnTag, targetStr,
                              p0Name, groupShortName(playerGroups_[0]), scores_[0], scores_[1],
                              groupShortName(playerGroups_[1]), p1Name, seriesSuffix);
            } else {
                std::snprintf(buf, sizeof(buf), "国际黑八 · 目标:%s · 进袋 %d/15 · %d 杆%s%s", targetStr,
                              pocketedCount_, shots_, turnNote_[0] ? " · " : "", turnNote_.data());
            }
        } else {
            if (state_ == BilliardsState::Finished) {
                if (twoPlayer) {
                    std::snprintf(buf, sizeof(buf), "斯诺克清台 · %s %d : %d %s · %s获胜！%s", p0Name,
                                  scores_[0], scores_[1], p1Name, winner_ == 1 ? p1Name : p0Name,
                                  seriesSuffix);
                } else {
                    std::snprintf(buf, sizeof(buf), "斯诺克清台 · 得分 %d · %d 杆 · 点再来一局", scores_[0],
                                  shots_);
                }
            } else if (state_ == BilliardsState::PlacingWhite) {
                std::snprintf(
                    buf, sizeof(buf), "%s%s · 目标:%s · %s%s%s", turnTag,
                    aiTurnNow ? "正在摆放白球" : "白球落袋请摆球", snookerTargetName(snookerTarget_),
                    config_.matchMode == BilliardsMatchMode::VsAi ? "人机对战"
                                                                  : (twoPlayer ? "双人对局" : "单人斯诺克"),
                    breakSuffix, seriesSuffix);
            } else if (twoPlayer) {
                std::snprintf(buf, sizeof(buf), "%s目标:%s · %s %d : %d %s · 剩红%d%s%s", turnTag,
                              snookerTargetName(snookerTarget_), p0Name, scores_[0], scores_[1], p1Name,
                              remainingReds(), breakSuffix, seriesSuffix);
            } else {
                std::snprintf(buf, sizeof(buf), "斯诺克 · 目标:%s · 得分 %d · 剩红%d%s%s%s",
                              snookerTargetName(snookerTarget_), scores_[0], remainingReds(), breakSuffix,
                              turnNote_[0] ? " · " : "", turnNote_.data());
            }
        }
        out.addText({sceneSize_.x * 0.5f, bannerY}, buf, Color{255, 255, 255, 245}, 14.5f, true);

        if (state_ == BilliardsState::Finished && lanLocalPlayer_ != 1) {
            const Rect rb = rematchButtonRect();
            const float midY = rb.y + rb.height * 0.5f;
            out.add(LineItem{
                {rb.x + 11.0f, midY}, {rb.x + rb.width - 11.0f, midY}, Color{26, 98, 72, 225}, 22.0f, false});
            out.addText({rb.x + rb.width * 0.5f, midY - 8.0f}, "再来一局 ↻", Color{178, 255, 212, 255}, 12.5f,
                        true);
        }

        if ((state_ == BilliardsState::Ready || state_ == BilliardsState::Charging ||
             state_ == BilliardsState::PlacingWhite) &&
            (lanLocalPlayer_ < 0 || currentPlayer_ == lanLocalPlayer_)) {
            if (state_ == BilliardsState::Ready || state_ == BilliardsState::Charging) {
                const SpinWidgetGeometry sw = spinWidgetGeometry();
                out.add(CircleItem{sw.center, sw.radius, Color{248, 248, 244, 242}, Color{40, 50, 68, 225},
                                   2.0f});
                out.add(
                    CircleItem{sw.center, sw.radius * 0.78f, {0, 0, 0, 0}, Color{150, 160, 175, 110}, 1.0f});
                out.add(LineItem{{sw.center.x - sw.radius * 0.82f, sw.center.y},
                                 {sw.center.x + sw.radius * 0.82f, sw.center.y},
                                 Color{135, 145, 160, 145},
                                 1.0f,
                                 false});
                out.add(LineItem{{sw.center.x, sw.center.y - sw.radius * 0.82f},
                                 {sw.center.x, sw.center.y + sw.radius * 0.82f},
                                 Color{135, 145, 160, 145},
                                 1.0f,
                                 false});
                const float maxTip = sw.radius * 0.78f;
                const Vec2 tipPt{sw.center.x + tipOffset_.x * maxTip, sw.center.y - tipOffset_.y * maxTip};
                out.add(CircleItem{tipPt, std::clamp(sw.radius * 0.19f, 3.2f, 5.0f), Color{225, 58, 62, 255},
                                   Color{255, 255, 255, 235}, 1.2f});

                char sbuf[128];
                const char* resetSuffix = aiTurnNow ? "" : " [复位]";
                if (tipOffset_.lengthSquared() <= 1e-4f) {
                    std::snprintf(sbuf, sizeof(sbuf),
                                  aiTurnNow ? "AI击点：中心" : "击点：中心（拖白球加塞）");
                } else {
                    const int vPct = static_cast<int>(std::lround(std::abs(tipOffset_.y) * 100.0f));
                    const int hPct = static_cast<int>(std::lround(std::abs(tipOffset_.x) * 100.0f));
                    const char* vName = tipOffset_.y > 0.06f ? "高杆" : (tipOffset_.y < -0.06f ? "低杆" : "");
                    const char* hName = tipOffset_.x > 0.06f ? "右塞" : (tipOffset_.x < -0.06f ? "左塞" : "");
                    const char* aiPrefix = aiTurnNow ? "AI击点：" : "";
                    if (vName[0] && hName[0]) {
                        std::snprintf(sbuf, sizeof(sbuf), "%s%s%d%% · %s%d%%%s", aiPrefix, vName, vPct, hName,
                                      hPct, resetSuffix);
                    } else if (vName[0]) {
                        std::snprintf(sbuf, sizeof(sbuf), "%s%s %d%%%s", aiPrefix, vName, vPct, resetSuffix);
                    } else {
                        std::snprintf(sbuf, sizeof(sbuf), "%s%s %d%%%s", aiPrefix, hName, hPct, resetSuffix);
                    }
                }
                out.addText({sw.center.x + sw.radius + 8.0f, sw.center.y - 8.0f}, sbuf,
                            Color{240, 245, 255, 238}, 12.5f, false);
            }

            // Top-right on-canvas quick toggle pill for AI Intent (`AI意图: 开 / 关`) in VsAi mode.
            if (config_.matchMode == BilliardsMatchMode::VsAi) {
                const Rect ib = aiIntentToggleRect();
                const float iMidY = ib.y + ib.height * 0.5f;
                const Color iBg = config_.showAiIntent ? Color{44, 62, 108, 198} : Color{28, 34, 46, 185};
                out.add(LineItem{{ib.x + 11.0f, iMidY}, {ib.x + ib.width - 11.0f, iMidY}, iBg, 21.0f, false});
                out.addText({ib.x + ib.width * 0.5f, iMidY - 8.0f},
                            config_.showAiIntent ? "AI意图: 开" : "AI意图: 关",
                            config_.showAiIntent ? Color{180, 222, 255, 248} : Color{198, 206, 220, 225},
                            12.0f, true);
            }

            // Top-right on-canvas quick toggle pill for Aiming Guide (`辅助线: 开 / 关`).
            if (state_ == BilliardsState::Ready || state_ == BilliardsState::Charging) {
                const Rect ab = aimToggleRect();
                const float pillMidY = ab.y + ab.height * 0.5f;
                const Color pillBg = config_.showAimGuide ? Color{22, 74, 62, 195} : Color{28, 34, 46, 185};
                out.add(LineItem{
                    {ab.x + 11.0f, pillMidY}, {ab.x + ab.width - 11.0f, pillMidY}, pillBg, 21.0f, false});
                out.addText({ab.x + ab.width * 0.5f, pillMidY - 8.0f},
                            config_.showAimGuide ? "辅助线: 开" : "辅助线: 关",
                            config_.showAimGuide ? Color{152, 248, 198, 248} : Color{198, 206, 220, 225},
                            12.0f, true);
            }
        }
    }
}

bool BilliardsMode::isIdle() const {
    if (extraWorld_ && !extraWorld_->allAsleep()) return false;
    if (isAiTurn()) return false;
    if (lanLocalPlayer_ == 1) {
        for (const Ball& b : balls_) {
            if (b.pocketed && b.dropProgress < 1.0f) return false;
        }
        return state_ == BilliardsState::Ready || state_ == BilliardsState::Finished ||
               state_ == BilliardsState::PlacingWhite;
    }
    if (state_ != BilliardsState::Ready && state_ != BilliardsState::Finished &&
        state_ != BilliardsState::PlacingWhite) {
        return false;
    }
    for (const Ball& b : balls_) {
        if (b.pocketed && b.dropProgress < 1.0f) return false;
    }
    return world_.allAsleep();
}

ModeStats BilliardsMode::stats() const {
    ModeStats s;
    s.bodies = static_cast<int>(balls_.size()) + 1 + static_cast<int>(extraWorld_ ? extraIcons_.size() : 0);
    s.awakeBodies = world_.awakeBodyCount() + (extraWorld_ ? extraWorld_->awakeBodyCount() : 0);
    s.grabbing = (!isAiTurn() && state_ == BilliardsState::Charging) || adjustingSpin_;
    s.pocketed = pocketedCount_;
    s.targets = static_cast<int>(balls_.size());
    s.shots = shots_;
    s.finished = state_ == BilliardsState::Finished;
    return s;
}

}  // namespace dpp
