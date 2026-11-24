#include "core/scene/playground.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace dpp {
SceneSetup syntheticScene(std::size_t n, Vec2 size) {
    SceneSetup s;
    s.workAreaSize = size;
    const auto cols = static_cast<std::size_t>(
        std::ceil(std::sqrt(static_cast<double>(std::max(n, std::size_t{1})) * size.x / size.y)));
    const auto rows = (n + cols - 1) / cols;
    const float pitch = std::min((size.x - 80) / static_cast<float>(cols),
                                 (size.y - 120) / static_cast<float>(std::max(rows, std::size_t{1})));
    const float icon = std::min(48.0f, pitch * 0.75f);
    for (std::size_t i = 0; i < n; ++i)
        s.icons.push_back({IconId{static_cast<std::uint32_t>(i + 1)},
                           TextureId{static_cast<std::uint32_t>(i % 6 + 1)},
                           {40 + pitch * (static_cast<float>(i % cols) + 0.5f),
                            60 + pitch * (static_cast<float>(i / cols) + 0.5f)},
                           {icon, icon}});
    return s;
}
Playground::Playground(SceneSetup s) : setup_(std::move(s)) {
    if (!setup_.workAreaSize.isFinite() || setup_.workAreaSize.x < 100 || setup_.workAreaSize.y < 100)
        throw std::invalid_argument("work area must be finite and at least 100 points");
    draw_.reserve(setup_.icons.size() * 3 + 48);
    switchMode(ModeKind::Gravity);
}
void Playground::syncBilliardsStats() {
    if (auto* sl = dynamic_cast<SlingshotMode*>(mode_.get())) {
        slingshotConfig_.showTrajectory = sl->config().showTrajectory;
        slingshotConfig_.preset = sl->config().preset;
        if (!sl->config().customPieces.empty() || sl->config().preset == SlingshotPreset::CustomBuilder) {
            slingshotConfig_.customPieces = sl->config().customPieces;
        }
        slingshotHighScore_ = std::max(slingshotHighScore_, sl->highScore());
        slingshotProgress_ = sl->progress();
        slingshotProgress_.highScore = std::max(slingshotProgress_.highScore, slingshotHighScore_);
        if (sl->takePersistenceDirty()) {
            slingshotPersistenceDirty_ = true;
        }
    }
    if (const auto* ah = dynamic_cast<const AirHockeyMode*>(mode_.get())) {
        airHockeyConfig_.showRinkChrome = ah->config().showRinkChrome;
        airHockeyConfig_.difficulty = ah->config().difficulty;
    }
    auto* b = dynamic_cast<BilliardsMode*>(mode_.get());
    if (!b) return;
    billiardsConfig_.showAimGuide = b->config().showAimGuide;
    billiardsConfig_.showAiIntent = b->config().showAiIntent;
    billiardsStats_.seriesWins = b->seriesWinsArray();
    if (b->state() == BilliardsState::Finished && !b->frameResultRecorded()) {
        if (b->config().matchMode != BilliardsMatchMode::Solo && (b->winner() == 0 || b->winner() == 1)) {
            billiardsStats_.nextBreakPlayer = 1 - b->startingPlayer();
        }
        b->markFrameResultRecorded();
    }
    const auto st = b->stats();
    if (st.shots > rackObservedShots_) {
        billiardsStats_.totalShots += (st.shots - rackObservedShots_);
        rackObservedShots_ = st.shots;
    }
    if (st.pocketed > rackObservedPocketed_) {
        billiardsStats_.totalPocketed += (st.pocketed - rackObservedPocketed_);
        rackObservedPocketed_ = st.pocketed;
    }
    if (b->whitePlacements() > rackObservedPlacements_) {
        billiardsStats_.totalWhitePlacements += (b->whitePlacements() - rackObservedPlacements_);
        rackObservedPlacements_ = b->whitePlacements();
    }
    if (b->bestShots() > 0 &&
        (billiardsStats_.bestShots == 0 || b->bestShots() < billiardsStats_.bestShots)) {
        billiardsStats_.bestShots = b->bestShots();
    }
}
void Playground::collectModeSounds() {
    if (!mode_) return;
    scratchSounds_.clear();
    mode_->drainSounds(scratchSounds_);
    if (muted_ || paused_ || migrationPaused_ || exit_) return;
    constexpr std::size_t kMaxPlaygroundSounds = 24;
    for (const SoundEvent& ev : scratchSounds_) {
        if (billiardsEnabled()) ++billiardsStats_.soundEvents;
        if (pendingSounds_.size() < kMaxPlaygroundSounds) {
            pendingSounds_.push_back(ev);
        } else {
            auto quietest =
                std::min_element(pendingSounds_.begin(), pendingSounds_.end(),
                                 [](const SoundEvent& a, const SoundEvent& b) { return a.gain < b.gain; });
            if (quietest != pendingSounds_.end() && ev.gain > quietest->gain) *quietest = ev;
        }
    }
}
void Playground::setMuted(bool muted) {
    muted_ = muted;
    if (muted_) pendingSounds_.clear();
}
void Playground::drainSounds(std::vector<SoundEvent>& out) {
    out.clear();
    if (muted_) {
        pendingSounds_.clear();
        return;
    }
    out.swap(pendingSounds_);
}
PlayMode Playground::playMode() const {
    if (billiardsEnabled()) return PlayMode::Billiards;
    if (slingshotEnabled()) return PlayMode::Slingshot;
    if (airHockeyEnabled()) return PlayMode::AirHockey;
    const auto* g = dynamic_cast<const GravityMode*>(mode_.get());
    if (g && g->blackHole().enabled) return PlayMode::BlackHole;
    return PlayMode::Gravity;
}
void Playground::setPlayMode(PlayMode pm) {
    if (exit_ || migrationPaused_) return;
    switch (pm) {
        case PlayMode::Gravity:
            if (billiardsEnabled() || slingshotEnabled() || airHockeyEnabled()) {
                leaveNonGravityToGravity();
            } else {
                setBlackHole(false);
            }
            break;
        case PlayMode::BlackHole:
            if (billiardsEnabled() || slingshotEnabled() || airHockeyEnabled()) {
                leaveNonGravityToGravity();
            }
            setBlackHole(true);
            break;
        case PlayMode::Billiards:
            enableBilliardsAutoFit();
            break;
        case PlayMode::Slingshot:
            setSlingshot(true);
            break;
        case PlayMode::AirHockey:
            setAirHockey(true);
            break;
    }
}
SceneSetup Playground::arenaSceneSetup() const {
    if (!hasArenaSubRect()) return setup_;
    SceneSetup s = setup_;
    const Vec2 orig = arenaOrigin();
    s.workAreaSize = arenaWorkAreaSize();
    s.screenWorkAreas.clear();
    s.primaryArenaRect = {};
    for (auto& ic : s.icons) {
        const Vec2 half = ic.iconSize * 0.5f;
        ic.center = {std::clamp(ic.center.x - orig.x, half.x, std::max(half.x, s.workAreaSize.x - half.x)),
                     std::clamp(ic.center.y - orig.y, half.y, std::max(half.y, s.workAreaSize.y - half.y))};
    }
    return s;
}
GravityModeConfig Playground::currentGravityConfig() const {
    GravityModeConfig cfg;
    cfg.physics.gravity = gravityVectorFrom(gravityMagnitude_, gravityDirection_);
    cfg.material.restitution = gravityRestitution_;
    return cfg;
}
void Playground::switchMode(ModeKind k) {
    syncBilliardsStats();
    pendingSounds_.clear();
    auto replacement = (k == ModeKind::Gravity)
                           ? std::unique_ptr<GameMode>(std::make_unique<GravityMode>(setup_, currentGravityConfig()))
                           : createMode(k, arenaSceneSetup());  // strong guarantee if allocation fails
    mode_ = std::move(replacement);
    rackObservedShots_ = rackObservedPocketed_ = rackObservedPlacements_ = 0;
    clock_ = (k == ModeKind::AirHockey) ? FixedStepClock({1.0 / 120.0, 8, 0.25})
                                        : FixedStepClock({1.0 / 60.0, 4, 0.25});
    dirty_ = true;
    previousTickActive_ = false;
}
void Playground::reset() {
    pendingSounds_.clear();
    if (auto* b = dynamic_cast<BilliardsMode*>(mode_.get())) {
        syncBilliardsStats();
        const auto cfg = b->config();
        const int lanRole = b->lanLocalPlayer();
        auto replacement =
            std::make_unique<BilliardsMode>(arenaSceneSetup(), cfg, billiardsStats_.bestShots,
                                            billiardsStats_.nextBreakPlayer, billiardsStats_.seriesWins);
        replacement->setLanLocalPlayer(lanRole);
        mode_ = std::move(replacement);
        rackObservedShots_ = rackObservedPocketed_ = rackObservedPlacements_ = 0;
        clock_ = FixedStepClock({1.0 / 60.0, 4, 0.25});
        dirty_ = true;
        previousTickActive_ = false;
        return;
    }
    if (auto* sl = dynamic_cast<SlingshotMode*>(mode_.get())) {
        syncBilliardsStats();
        const auto cfg = sl->config();
        mode_ = std::make_unique<SlingshotMode>(arenaSceneSetup(), cfg, 1, 0, slingshotHighScore_, slingshotProgress_);
        clock_ = FixedStepClock({1.0 / 60.0, 4, 0.25});
        dirty_ = true;
        previousTickActive_ = false;
        return;
    }
    if (auto* ah = dynamic_cast<AirHockeyMode*>(mode_.get())) {
        syncBilliardsStats();
        const auto cfg = ah->config();
        const int lanRole = ah->lanLocalPlayer();
        auto replacement = std::make_unique<AirHockeyMode>(arenaSceneSetup(), cfg);
        replacement->setLanLocalPlayer(lanRole);
        mode_ = std::move(replacement);
        clock_ = FixedStepClock({1.0 / 120.0, 8, 0.25});
        dirty_ = true;
        previousTickActive_ = false;
        return;
    }
    auto* g = dynamic_cast<GravityMode*>(mode_.get());
    auto hole = g ? g->blackHole() : BlackHoleState{};
    switchMode(mode_->kind());
    if (g) {
        hole.objects.assign(setup_.icons.size(), {});
        hole.devourElapsed = 0;  // devour ramp restarts with the restored layout
        static_cast<GravityMode*>(mode_.get())->restoreBlackHole(std::move(hole));
    }
}
void Playground::setBilliardsConfig(const BilliardsConfig& cfg) {
    if (exit_ || migrationPaused_) return;
    BilliardsConfig next = cfg;
    next.style = BilliardsStyle::Desktop;
    next.tableInsetX = BilliardsConfig::desktop().tableInsetX;
    next.tableInsetY = BilliardsConfig::desktop().tableInsetY;
    next.maxShotSpeed = BilliardsConfig::desktop().maxShotSpeed;
    if (billiardsConfig_.tableTheme != next.tableTheme) {
        next.showTableChrome = (next.tableTheme == BilliardsTableTheme::Full);
    } else if (billiardsConfig_.showTableChrome != next.showTableChrome) {
        next.tableTheme = next.showTableChrome ? BilliardsTableTheme::Full : BilliardsTableTheme::Borderless;
    } else {
        next.tableTheme = next.effectiveTableTheme();
        next.showTableChrome = (next.tableTheme == BilliardsTableTheme::Full);
    }

    const bool matchContextChanged =
        (billiardsConfig_.rule != next.rule || billiardsConfig_.matchMode != next.matchMode);
    if (matchContextChanged) {
        billiardsStats_.seriesWins = {0, 0};
        billiardsStats_.nextBreakPlayer = 0;
    } else {
        syncBilliardsStats();
    }

    const Vec2 arenaSz = arenaWorkAreaSize();
    if (next.rule == BilliardsRule::Free && next.freeModeMaxRackBalls == 0 &&
        !BilliardsMode::canFitIcons(arenaSz, setup_.icons.size(), next)) {
        const std::size_t fitCap = BilliardsMode::maxFittingRackBalls(arenaSz, next);
        if (fitCap > 0) next.freeModeMaxRackBalls = fitCap;
    }

    auto effectiveFreeRackCount = [&](const BilliardsConfig& c) -> std::size_t {
        if (c.rule != BilliardsRule::Free) return 0u;
        const std::size_t avail = (c.iconSelection == IconSelectionPolicy::Custom)
                                      ? std::min(c.customIconIndices.size(), setup_.icons.size())
                                      : setup_.icons.size();
        return c.freeModeMaxRackBalls > 0 ? std::min(avail, c.freeModeMaxRackBalls) : avail;
    };

    const bool sameSelection =
        (billiardsConfig_.iconSelection == next.iconSelection) &&
        (next.iconSelection != IconSelectionPolicy::Random ||
         billiardsConfig_.randomSeed == next.randomSeed) &&
        (next.iconSelection != IconSelectionPolicy::Custom ||
         billiardsConfig_.customIconIndices == next.customIconIndices);

    const bool sameTopology =
        (billiardsConfig_.rule == next.rule && billiardsConfig_.matchMode == next.matchMode &&
         sameSelection && billiardsConfig_.extraIcons == next.extraIcons &&
         effectiveFreeRackCount(billiardsConfig_) == effectiveFreeRackCount(next));

    billiardsConfig_ = next;
    auto* b = dynamic_cast<BilliardsMode*>(mode_.get());
    if (!b) return;

    if (sameTopology) {
        b->setVisualOptions(billiardsConfig_.showBallBorders, billiardsConfig_.showBallNumbers,
                            billiardsConfig_.tableTheme, billiardsConfig_.showAimGuide,
                            billiardsConfig_.showAiIntent);
        b->setAiDifficulty(billiardsConfig_.aiDifficulty);
        dirty_ = true;
        tick(0);
        return;
    }

    if (!canEnableBilliards()) {
        setBilliards(false);
        return;
    }
    pendingSounds_.clear();
    const int lanRole = b->lanLocalPlayer();
    b->handleInput({InputType::PointerCancel, {}, 0});
    auto replacement =
        std::make_unique<BilliardsMode>(arenaSceneSetup(), billiardsConfig_, billiardsStats_.bestShots,
                                        billiardsStats_.nextBreakPlayer, billiardsStats_.seriesWins);
    replacement->setLanLocalPlayer(lanRole);
    mode_ = std::move(replacement);
    rackObservedShots_ = rackObservedPocketed_ = rackObservedPlacements_ = 0;
    clock_.reset();
    dirty_ = true;
    previousTickActive_ = false;
    tick(0);
}
void Playground::leaveNonGravityToGravity() {
    if (!mode_ || mode_->kind() == ModeKind::Gravity) return;
    syncBilliardsStats();
    pendingSounds_.clear();
    mode_->handleInput({InputType::PointerCancel, {}, 0});
    const auto* b = dynamic_cast<const BilliardsMode*>(mode_.get());
    const auto* sl = dynamic_cast<const SlingshotMode*>(mode_.get());
    const auto* ah = dynamic_cast<const AirHockeyMode*>(mode_.get());
    const Vec2 orig = arenaOrigin();
    std::vector<Vec2> centers;
    std::vector<BodyState> states;
    centers.reserve(setup_.icons.size());
    states.reserve(setup_.icons.size());
    for (std::size_t i = 0; i < setup_.icons.size(); ++i) {
        const Vec2 half = setup_.icons[i].iconSize * 0.5f;
        Vec2 raw = setup_.icons[i].center;
        if (b)
            raw = b->iconCurrentSceneCenter(i) + orig;
        else if (sl)
            raw = sl->iconCurrentSceneCenter(i) + orig;
        else if (ah)
            raw = ah->iconCurrentSceneCenter(i) + orig;
        if (!raw.isFinite()) raw = setup_.icons[i].center;
        centers.push_back({std::clamp(raw.x, half.x, setup_.workAreaSize.x - half.x),
                           std::clamp(raw.y, half.y, setup_.workAreaSize.y - half.y)});
        BodyState st{};
        st.enabled = true;
        st.awake = true;
        states.push_back(st);
    }
    auto gravity = std::make_unique<GravityMode>(setup_, currentGravityConfig());
    gravity->restoreStates(states, centers);
    mode_ = std::move(gravity);
    rackObservedShots_ = rackObservedPocketed_ = rackObservedPlacements_ = 0;
    clock_ = FixedStepClock({1.0 / 60.0, 4, 0.25});
    dirty_ = true;
    previousTickActive_ = false;
    tick(0);
}
bool Playground::canEnableAnyBilliards() const {
    if (canEnableBilliards()) return true;
    return BilliardsMode::maxFittingRackBalls(arenaWorkAreaSize(), billiardsConfig_) > 0;
}
void Playground::enableBilliardsAutoFit() {
    if (exit_ || migrationPaused_ || billiardsEnabled()) return;
    if (!canEnableBilliards() && billiardsConfig_.rule == BilliardsRule::Free) {
        const std::size_t fitCap = BilliardsMode::maxFittingRackBalls(arenaWorkAreaSize(), billiardsConfig_);
        if (fitCap > 0) {
            billiardsConfig_.freeModeMaxRackBalls = fitCap;
        }
    }
    setBilliards(true);
}
void Playground::setBilliards(bool enabled) {
    if (exit_ || migrationPaused_) return;
    pendingSounds_.clear();
    if (enabled) {
        if (billiardsEnabled() || !canEnableBilliards()) return;
        syncBilliardsStats();
        mode_->handleInput({InputType::PointerCancel, {}, 0});
        auto replacement =
            std::make_unique<BilliardsMode>(arenaSceneSetup(), billiardsConfig_, billiardsStats_.bestShots,
                                            billiardsStats_.nextBreakPlayer, billiardsStats_.seriesWins);
        mode_ = std::move(replacement);
        rackObservedShots_ = rackObservedPocketed_ = rackObservedPlacements_ = 0;
        clock_ = FixedStepClock({1.0 / 60.0, 4, 0.25});
        dirty_ = true;
        previousTickActive_ = false;
        tick(0);
        return;
    }
    if (!billiardsEnabled()) return;
    leaveNonGravityToGravity();
}
void Playground::setSlingshot(bool enabled) {
    if (exit_ || migrationPaused_) return;
    pendingSounds_.clear();
    if (enabled) {
        if (slingshotEnabled()) return;
        syncBilliardsStats();
        mode_->handleInput({InputType::PointerCancel, {}, 0});
        mode_ = std::make_unique<SlingshotMode>(arenaSceneSetup(), slingshotConfig_, 1, 0, slingshotHighScore_,
                                                slingshotProgress_);
        rackObservedShots_ = rackObservedPocketed_ = rackObservedPlacements_ = 0;
        clock_ = FixedStepClock({1.0 / 60.0, 4, 0.25});
        dirty_ = true;
        previousTickActive_ = false;
        tick(0);
        return;
    }
    if (!slingshotEnabled()) return;
    leaveNonGravityToGravity();
}
void Playground::setSlingshotConfig(const SlingshotConfig& cfg) {
    if (exit_ || migrationPaused_) return;
    syncBilliardsStats();
    SlingshotConfig next = cfg;
    if (next.customPieces.empty() && !slingshotConfig_.customPieces.empty()) {
        next.customPieces = slingshotConfig_.customPieces;
    }
    auto* sl = dynamic_cast<SlingshotMode*>(mode_.get());
    const bool sameSlingSelection =
        (slingshotConfig_.iconSelection == next.iconSelection) &&
        (next.iconSelection != IconSelectionPolicy::Random ||
         slingshotConfig_.randomSeed == next.randomSeed) &&
        (next.iconSelection != IconSelectionPolicy::Custom ||
         slingshotConfig_.customIconIndices == next.customIconIndices);
    const bool sameTopology =
        (slingshotConfig_.preset == next.preset && sameSlingSelection &&
         slingshotConfig_.ammoCount == next.ammoCount &&
         slingshotConfig_.startInEditor == next.startInEditor &&
         (!sl || sl->editing() == next.startInEditor) &&
         (next.preset != SlingshotPreset::CustomBuilder ||
          slingshotConfig_.customPieces == next.customPieces));
    slingshotConfig_ = next;
    if (!sl) return;
    if (sameTopology) {
        sl->setVisualOptions(slingshotConfig_.showTrajectory, slingshotConfig_.showGroundChrome);
        dirty_ = true;
        tick(0);
        return;
    }
    pendingSounds_.clear();
    sl->handleInput({InputType::PointerCancel, {}, 0});
    mode_ = std::make_unique<SlingshotMode>(arenaSceneSetup(), slingshotConfig_, 1, 0, slingshotHighScore_,
                                            slingshotProgress_);
    clock_.reset();
    dirty_ = true;
    previousTickActive_ = false;
    tick(0);
}
void Playground::setSlingshotProgress(const SlingshotProgress& progress) {
    slingshotProgress_ = progress;
    slingshotHighScore_ = std::max(slingshotHighScore_, progress.highScore);
    slingshotProgress_.highScore = slingshotHighScore_;
}
void Playground::setAirHockey(bool enabled) {
    if (exit_ || migrationPaused_) return;
    pendingSounds_.clear();
    if (enabled) {
        if (airHockeyEnabled()) return;
        syncBilliardsStats();
        mode_->handleInput({InputType::PointerCancel, {}, 0});
        mode_ = std::make_unique<AirHockeyMode>(arenaSceneSetup(), airHockeyConfig_);
        rackObservedShots_ = rackObservedPocketed_ = rackObservedPlacements_ = 0;
        clock_ = FixedStepClock({1.0 / 120.0, 8, 0.25});
        dirty_ = true;
        previousTickActive_ = false;
        tick(0);
        return;
    }
    if (!airHockeyEnabled()) return;
    leaveNonGravityToGravity();
}
void Playground::setAirHockeyConfig(const AirHockeyConfig& cfg) {
    if (exit_ || migrationPaused_) return;
    syncBilliardsStats();
    const bool sameTopology =
        (airHockeyConfig_.matchMode == cfg.matchMode && airHockeyConfig_.targetGoals == cfg.targetGoals);
    airHockeyConfig_ = cfg;
    auto* ah = dynamic_cast<AirHockeyMode*>(mode_.get());
    if (!ah) return;
    if (sameTopology) {
        ah->setDifficulty(airHockeyConfig_.difficulty);
        ah->setVisualOptions(airHockeyConfig_.showRinkChrome, airHockeyConfig_.showSpectators);
        dirty_ = true;
        tick(0);
        return;
    }
    pendingSounds_.clear();
    const int lanRole = ah->lanLocalPlayer();
    ah->handleInput({InputType::PointerCancel, {}, 0});
    auto replacement = std::make_unique<AirHockeyMode>(arenaSceneSetup(), airHockeyConfig_);
    replacement->setLanLocalPlayer(lanRole);
    mode_ = std::move(replacement);
    clock_ = FixedStepClock({1.0 / 120.0, 8, 0.25});
    dirty_ = true;
    previousTickActive_ = false;
    tick(0);
}
bool Playground::interactiveArenaContains(Vec2 p) const {
    const Vec2 local = p - arenaOrigin();
    if (const auto* b = dynamic_cast<const BilliardsMode*>(mode_.get())) {
        return b->interactiveContains(local) || b->isTubeBallHit(local);
    }
    if (const auto* sl = dynamic_cast<const SlingshotMode*>(mode_.get())) {
        return sl->interactiveContains(local);
    }
    if (const auto* ah = dynamic_cast<const AirHockeyMode*>(mode_.get())) {
        return ah->interactiveContains(local);
    }
    return false;
}
std::vector<Vec2> Playground::currentIconSceneCenters() const {
    std::vector<Vec2> out;
    out.reserve(setup_.icons.size());
    const auto* g = dynamic_cast<const GravityMode*>(mode_.get());
    const auto* b = dynamic_cast<const BilliardsMode*>(mode_.get());
    const auto* sl = dynamic_cast<const SlingshotMode*>(mode_.get());
    const auto* ah = dynamic_cast<const AirHockeyMode*>(mode_.get());
    const Vec2 orig = arenaOrigin();
    for (std::size_t i = 0; i < setup_.icons.size(); ++i) {
        const Vec2 half = setup_.icons[i].iconSize * 0.5f;
        Vec2 raw = setup_.icons[i].center;
        if (g)
            raw = g->bodySceneCenter(i);
        else if (b)
            raw = b->iconCurrentSceneCenter(i) + orig;
        else if (sl)
            raw = sl->iconCurrentSceneCenter(i) + orig;
        else if (ah)
            raw = ah->iconCurrentSceneCenter(i) + orig;
        if (!raw.isFinite()) raw = setup_.icons[i].center;
        out.push_back({std::clamp(raw.x, half.x, std::max(half.x, setup_.workAreaSize.x - half.x)),
                       std::clamp(raw.y, half.y, std::max(half.y, setup_.workAreaSize.y - half.y))});
    }
    return out;
}
void Playground::setBlackHole(bool enabled) {
    if (exit_ || migrationPaused_) return;
    if (billiardsEnabled() || slingshotEnabled() || airHockeyEnabled()) {
        if (!enabled) return;
        leaveNonGravityToGravity();
    }
    auto* g = dynamic_cast<GravityMode*>(mode_.get());
    if (!g) return;
    g->handleInput({InputType::PointerCancel, {}, 0});
    g->enableBlackHole(enabled);
    if (enabled) g->setBlackHoleDevour(g->blackHole().devourAll);  // restart ramp
    if (!enabled) reset();
    dirty_ = true;
    tick(0);
}
void Playground::setBlackHoleDevour(bool devour) {
    if (exit_ || migrationPaused_) return;
    auto* g = dynamic_cast<GravityMode*>(mode_.get());
    if (!g) return;
    g->setBlackHoleDevour(devour);
    dirty_ = true;
}
void Playground::setBlackHoleStyle(BlackHoleStyle style) {
    auto* g = dynamic_cast<GravityMode*>(mode_.get());
    if (!g || exit_) return;
    g->setBlackHoleStyle(style);
    dirty_ = true;
}
void Playground::setBlackHoleScale(float scale) {
    if (exit_ || migrationPaused_) return;
    auto* g = dynamic_cast<GravityMode*>(mode_.get());
    if (!g) return;
    g->setBlackHoleScale(scale);
    dirty_ = true;
    tick(0);
}
void Playground::setBlackHoleAngles(float elevation, float tilt) {
    if (exit_ || migrationPaused_) return;
    auto* g = dynamic_cast<GravityMode*>(mode_.get());
    if (!g) return;
    g->setBlackHoleAngles(elevation, tilt);
    dirty_ = true;
    tick(0);
}
void Playground::setGravitySettings(float magnitudeMps2, GravityDirection direction, float restitution) {
    if (exit_ || migrationPaused_) return;
    if (std::isfinite(magnitudeMps2)) {
        gravityMagnitude_ = std::clamp(magnitudeMps2, kMinGravityMps2, kMaxGravityMps2);
    }
    if (static_cast<int>(direction) >= 0 && static_cast<int>(direction) <= 3) {
        gravityDirection_ = direction;
    }
    if (std::isfinite(restitution)) {
        gravityRestitution_ =
            std::clamp(restitution, kMinGravityRestitution, kMaxGravityRestitution);
    }
    if (auto* g = dynamic_cast<GravityMode*>(mode_.get())) {
        g->setGravityParameters(gravityMagnitude_, gravityDirection_, gravityRestitution_);
    }
    dirty_ = true;
    tick(0);
}
void Playground::input(const InputEvent& e) {
    const auto t = SteadyClock::now();
    if (migrationPaused_ && e.type != InputType::PointerCancel && e.type != InputType::RequestExit) return;
    if (e.type == InputType::TogglePause) {
        mode_->handleInput({InputType::PointerCancel, {}, e.time});
        paused_ = !paused_;
        clock_.reset();
        previousTickActive_ = false;
    } else if (e.type == InputType::Reset)
        reset();
    else if (e.type == InputType::RequestExit) {
        mode_->handleInput({InputType::PointerCancel, {}, e.time});
        exit_ = true;
    } else if (e.type == InputType::PointerCancel ||
               (!paused_ && e.scenePos.isFinite() && std::isfinite(e.time))) {
        InputEvent forwarded = e;
        if (mode_->kind() != ModeKind::Gravity && hasArenaSubRect()) {
            forwarded.scenePos = forwarded.scenePos - arenaOrigin();
        }
        mode_->handleInput(forwarded);
        if (auto* b = dynamic_cast<BilliardsMode*>(mode_.get()); b && b->takeRematchRequest()) {
            reset();
        }
    }
    syncBilliardsStats();
    collectModeSounds();
    if (!(e.type == InputType::PointerMove && airHockeyEnabled())) {
        dirty_ = true;
    }
    perf.input.add(elapsedMs(t));
}
bool Playground::tick(double dt) {
    const bool active = !idle();
    if (active && previousTickActive_ && std::isfinite(dt) && dt > 0) perf.frame(dt * 1000);
    bool changed = dirty_;
    dirty_ = false;
    if (idle())
        clock_.reset();
    else {
        const int steps = clock_.advance(dt);
        double physics = 0, rules = 0;
        for (int i = 0; i < steps; ++i) {
            auto t = SteadyClock::now();
            mode_->stepPhysics(static_cast<float>(clock_.stepSeconds()));
            physics += elapsedMs(t);
            t = SteadyClock::now();
            mode_->updateRules(static_cast<float>(clock_.stepSeconds()));
            rules += elapsedMs(t);
        }
        perf.physics.add(physics);
        perf.rules.add(rules);
        changed = changed || steps > 0;
    }
    syncBilliardsStats();
    collectModeSounds();
    if (changed) {
        const auto t = SteadyClock::now();
        draw_.clear();
        mode_->buildDrawList(draw_);
        if (mode_->kind() != ModeKind::Gravity && hasArenaSubRect()) {
            draw_.translate(arenaOrigin());
        }
        perf.draw.add(elapsedMs(t));
    }
    previousTickActive_ = !idle();
    return changed;
}
FrozenScene Playground::freezeForMigration() {
    if (exit_ || !mode_) throw std::logic_error("migration requires active gravity scene");
    syncBilliardsStats();
    if (mode_->kind() != ModeKind::Gravity) {
        input({InputType::PointerCancel, {}, 0});
        pendingSounds_.clear();
        migrationPaused_ = true;
        clock_.reset();
        previousTickActive_ = false;
        std::vector<BodyState> bodies(setup_.icons.size());
        const auto* b = dynamic_cast<const BilliardsMode*>(mode_.get());
        const auto* sl = dynamic_cast<const SlingshotMode*>(mode_.get());
        const auto* ah = dynamic_cast<const AirHockeyMode*>(mode_.get());
        FrozenScene frozen{setup_,
                           setup_,
                           std::move(bodies),
                           {},
                           paused_,
                           muted_,
                           {},
                           mode_->kind(),
                           billiardsConfig_,
                           billiardsStats_,
                           slingshotConfig_,
                           sl ? sl->stageIndex() : 1,
                           sl ? sl->score() : 0,
                           slingshotHighScore_,
                           slingshotProgress_,
                           airHockeyConfig_,
                           gravityMagnitude_,
                           gravityDirection_,
                           gravityRestitution_};
        for (size_t i = 0; i < frozen.setup.icons.size(); ++i) {
            if (b)
                frozen.setup.icons[i].center = b->iconCurrentSceneCenter(i);
            else if (sl)
                frozen.setup.icons[i].center = sl->iconCurrentSceneCenter(i);
            else if (ah)
                frozen.setup.icons[i].center = ah->iconCurrentSceneCenter(i);
        }
        return frozen;
    }
    auto* gravity = dynamic_cast<GravityMode*>(mode_.get());
    if (!gravity) throw std::logic_error("migration requires active gravity scene");
    input({InputType::PointerCancel, {}, 0});
    pendingSounds_.clear();
    migrationPaused_ = true;
    clock_.reset();
    previousTickActive_ = false;
    FrozenScene frozen{setup_,
                       setup_,
                       gravity->bodyStates(),
                       gravity->config(),
                       paused_,
                       muted_,
                       {},
                       ModeKind::Gravity,
                       billiardsConfig_,
                       billiardsStats_,
                       slingshotConfig_,
                       1,
                       0,
                       slingshotHighScore_,
                       slingshotProgress_,
                       airHockeyConfig_,
                       gravityMagnitude_,
                       gravityDirection_,
                       gravityRestitution_};
    frozen.blackHole = gravity->blackHole();
    for (size_t i = 0; i < frozen.setup.icons.size(); ++i)
        frozen.setup.icons[i].center = gravity->bodySceneCenter(i);
    return frozen;
}
std::unique_ptr<Playground> Playground::prepareMigration(const FrozenScene& f, Vec2 size,
                                                         MigrationPreparationStage* stage) {
    const auto mark = [stage](MigrationPreparationStage value) {
        if (stage) *stage = value;
    };
    mark(MigrationPreparationStage::Validation);
    if (f.bodies.size() != f.setup.icons.size()) throw std::invalid_argument("invalid frozen scene");
    if (f.mode != ModeKind::Gravity) {
        BilliardsConfig migratedBilliardsCfg = f.billiardsConfig;
        if (f.mode == ModeKind::Billiards &&
            !BilliardsMode::canFitIcons(size, f.resetSetup.icons.size(), migratedBilliardsCfg)) {
            if (migratedBilliardsCfg.rule == BilliardsRule::Free) {
                const std::size_t fitCap = BilliardsMode::maxFittingRackBalls(size, migratedBilliardsCfg);
                if (fitCap > 0) migratedBilliardsCfg.freeModeMaxRackBalls = fitCap;
            }
            if (!BilliardsMode::canFitIcons(size, f.resetSetup.icons.size(), migratedBilliardsCfg)) {
                throw std::invalid_argument("billiards icons cannot fit");
            }
        }
        std::vector<MigrationIcon> resetIcons;
        for (const auto& icon : f.resetSetup.icons) resetIcons.push_back({icon.center, icon.iconSize, 0});
        mark(MigrationPreparationStage::ResetLayout);
        const auto resetCenters = migrateIconCenters(resetIcons, f.resetSetup.workAreaSize, size);
        auto setup = f.resetSetup;
        setup.workAreaSize = size;
        for (size_t i = 0; i < resetCenters.size(); ++i) setup.icons[i].center = resetCenters[i];
        mark(MigrationPreparationStage::World);
        auto result = std::make_unique<Playground>(setup);
        result->muted_ = f.muted;
        result->billiardsConfig_ = migratedBilliardsCfg;
        result->billiardsStats_ = f.billiardsStats;
        result->slingshotConfig_ = f.slingshotConfig;
        result->slingshotHighScore_ = f.slingshotHighScore;
        result->slingshotProgress_ = f.slingshotProgress;
        result->airHockeyConfig_ = f.airHockeyConfig;
        result->gravityMagnitude_ = f.gravityMagnitude;
        result->gravityDirection_ = f.gravityDirection;
        result->gravityRestitution_ = f.gravityRestitution;
        if (f.mode == ModeKind::Billiards) {
            result->mode_ = std::make_unique<BilliardsMode>(
                setup, migratedBilliardsCfg, f.billiardsStats.bestShots, f.billiardsStats.nextBreakPlayer,
                f.billiardsStats.seriesWins);
        } else if (f.mode == ModeKind::Slingshot) {
            result->mode_ =
                std::make_unique<SlingshotMode>(setup, f.slingshotConfig, f.slingshotStage, f.slingshotScore,
                                                f.slingshotHighScore, f.slingshotProgress);
        } else if (f.mode == ModeKind::AirHockey) {
            result->mode_ = std::make_unique<AirHockeyMode>(setup, f.airHockeyConfig);
            result->clock_ = FixedStepClock({1.0 / 120.0, 8, 0.25});
        }
        result->paused_ = f.userPaused;
        result->migrationPaused_ = true;
        mark(MigrationPreparationStage::Draw);
        result->tick(0);
        return result;
    }
    std::vector<MigrationIcon> icons;
    for (size_t i = 0; i < f.setup.icons.size(); ++i)
        icons.push_back({f.setup.icons[i].center, f.setup.icons[i].iconSize,
                         SceneToPhysics::angleToScene(f.bodies[i].angle)});
    mark(MigrationPreparationStage::CurrentLayout);
    std::vector<BodyState> migratedBodies = f.bodies;
    std::vector<Vec2> centers;
    try {
        centers = migrateIconCenters(icons, f.setup.workAreaSize, size);
    } catch (const std::invalid_argument&) {
        // If crowded tumbled icons only failed due to enlarged rotated AABBs on a shrunken screen,
        // retry upright while still strictly validating every upright icon size & 1-pt overlap.
        bool uprightSizesFit = (f.setup.workAreaSize != size);
        for (const auto& ic : f.setup.icons) {
            if (!ic.iconSize.isFinite() || ic.iconSize.x > size.x || ic.iconSize.y > size.y ||
                ic.iconSize.x > f.setup.workAreaSize.x || ic.iconSize.y > f.setup.workAreaSize.y) {
                uprightSizesFit = false;
                break;
            }
        }
        if (!uprightSizesFit) throw;
        std::vector<MigrationIcon> uprightIcons;
        uprightIcons.reserve(icons.size());
        for (const auto& ic : icons) uprightIcons.push_back({ic.center, ic.size, 0.0f});
        centers = migrateIconCenters(uprightIcons, f.setup.workAreaSize, size);
        for (auto& bs : migratedBodies) {
            bs.angle = 0.0f;
            bs.unitRotation = {1.0f, 0.0f};
        }
    }
    std::vector<MigrationIcon> resetIcons;
    for (const auto& icon : f.resetSetup.icons) resetIcons.push_back({icon.center, icon.iconSize, 0});
    mark(MigrationPreparationStage::ResetLayout);
    const auto resetCenters = migrateIconCenters(resetIcons, f.resetSetup.workAreaSize, size);
    auto setup = f.resetSetup;
    setup.workAreaSize = size;
    for (size_t i = 0; i < resetCenters.size(); ++i) setup.icons[i].center = resetCenters[i];
    mark(MigrationPreparationStage::World);
    auto result = std::make_unique<Playground>(setup);
    result->muted_ = f.muted;
    result->billiardsConfig_ = f.billiardsConfig;
    result->billiardsStats_ = f.billiardsStats;
    result->slingshotConfig_ = f.slingshotConfig;
    result->slingshotHighScore_ = f.slingshotHighScore;
    result->slingshotProgress_ = f.slingshotProgress;
    result->airHockeyConfig_ = f.airHockeyConfig;
    result->gravityMagnitude_ = f.gravityMagnitude;
    result->gravityDirection_ = f.gravityDirection;
    result->gravityRestitution_ = f.gravityRestitution;
    auto gravity = std::make_unique<GravityMode>(setup, f.config);
    gravity->restoreStates(migratedBodies, centers);
    auto hole = f.blackHole;
    hole.center = {hole.center.x * size.x / f.setup.workAreaSize.x,
                   hole.center.y * size.y / f.setup.workAreaSize.y};
    hole.fit(size);  // preserves the user's size factor and visual style
    if (hole.objects.size() != f.bodies.size()) hole.objects.resize(f.bodies.size());
    gravity->restoreBlackHole(std::move(hole));
    result->mode_ = std::move(gravity);
    result->paused_ = f.userPaused;
    result->migrationPaused_ = true;
    mark(MigrationPreparationStage::Draw);
    result->tick(0);
    return result;
}
}  // namespace dpp
