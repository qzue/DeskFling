#pragma once
#include "core/modes/air_hockey_mode.h"
#include "core/modes/billiards_mode.h"
#include "core/modes/game_mode.h"
#include "core/modes/gravity_mode.h"
#include "core/modes/slingshot_mode.h"
#include "core/performance/monitor.h"
#include "core/physics/fixed_step_clock.h"
namespace dpp {
SceneSetup syntheticScene(std::size_t count = 50, Vec2 size = {1100, 700});
enum class PlayMode : std::uint8_t {
    Gravity = 0,
    BlackHole = 1,
    Billiards = 2,
    Slingshot = 3,
    AirHockey = 4
};
enum class MigrationPreparationStage { Validation, CurrentLayout, ResetLayout, World, Draw };
struct BilliardsSessionStats {
    int totalShots = 0;
    int totalPocketed = 0;
    int totalWhitePlacements = 0;
    int bestShots = 0;
    int soundEvents = 0;
    std::array<int, 2> seriesWins{0, 0};
    int nextBreakPlayer = 0;
};
struct FrozenScene {
    SceneSetup setup;
    SceneSetup resetSetup;
    std::vector<BodyState> bodies;
    GravityModeConfig config;
    bool userPaused = false;
    bool muted = false;
    BlackHoleState blackHole;
    ModeKind mode = ModeKind::Gravity;
    BilliardsConfig billiardsConfig = BilliardsConfig::desktop();
    BilliardsSessionStats billiardsStats{};
    SlingshotConfig slingshotConfig{};
    int slingshotStage = 1;
    int slingshotScore = 0;
    int slingshotHighScore = 0;
    SlingshotProgress slingshotProgress{};
    AirHockeyConfig airHockeyConfig{};
    float gravityMagnitude = kEarthGravityMps2;
    GravityDirection gravityDirection = GravityDirection::Down;
    float gravityRestitution = kDefaultGravityRestitution;
};
class Playground {
   public:
    explicit Playground(SceneSetup setup);
    void input(const InputEvent& e);
    bool tick(double dt);
    void switchMode(ModeKind kind);
    void reset();
    PlayMode playMode() const;
    void setPlayMode(PlayMode pm);
    void setBlackHole(bool enabled);
    void setBlackHoleStyle(BlackHoleStyle style);
    void setBlackHoleScale(float scale);
    void setBlackHoleAngles(float elevation, float tilt);
    void setBlackHoleDevour(bool devour);
    void setGravitySettings(float magnitudeMps2, GravityDirection direction, float restitution);
    [[nodiscard]] float gravityMagnitude() const { return gravityMagnitude_; }
    [[nodiscard]] GravityDirection gravityDirection() const { return gravityDirection_; }
    [[nodiscard]] float gravityRestitution() const { return gravityRestitution_; }
    bool blackHoleEdge(Vec2 p, bool alternate = false) const {
        const auto* g = dynamic_cast<const GravityMode*>(mode_.get());
        return g && g->blackHoleEdge(p, alternate);
    }
    bool canEnableBilliards() const {
        return BilliardsMode::canFitIcons(arenaWorkAreaSize(), setup_.icons.size(), billiardsConfig_);
    }
    bool canEnableBilliardsWith(const BilliardsConfig& cfg) const {
        return BilliardsMode::canFitIcons(arenaWorkAreaSize(), setup_.icons.size(), cfg);
    }
    bool canEnableAnyBilliards() const;
    void enableBilliardsAutoFit();
    bool billiardsEnabled() const { return mode_ && mode_->kind() == ModeKind::Billiards; }
    void setBilliards(bool enabled);
    const BilliardsConfig& billiardsConfig() const { return billiardsConfig_; }
    void setBilliardsConfig(const BilliardsConfig& cfg);
    BilliardsMode* mutableBilliardsMode() { return dynamic_cast<BilliardsMode*>(mode_.get()); }
    const BilliardsMode* billiardsMode() const { return dynamic_cast<const BilliardsMode*>(mode_.get()); }
    bool billiardsTableContains(Vec2 p) const {
        const auto* b = dynamic_cast<const BilliardsMode*>(mode_.get());
        return b && b->interactiveContains(p - arenaOrigin());
    }
    bool billiardsTubeHit(Vec2 p) const {
        const auto* b = dynamic_cast<const BilliardsMode*>(mode_.get());
        return b && b->isTubeBallHit(p - arenaOrigin());
    }
    BilliardsSessionStats billiardsSessionStats() const { return billiardsStats_; }

    bool slingshotEnabled() const { return mode_ && mode_->kind() == ModeKind::Slingshot; }
    void setSlingshot(bool enabled);
    const SlingshotConfig& slingshotConfig() const { return slingshotConfig_; }
    void setSlingshotConfig(const SlingshotConfig& cfg);
    int slingshotHighScore() const { return slingshotHighScore_; }
    void setSlingshotHighScore(int score) {
        slingshotHighScore_ = std::max(0, score);
        slingshotProgress_.highScore = std::max(slingshotProgress_.highScore, slingshotHighScore_);
    }
    const SlingshotProgress& slingshotProgress() const { return slingshotProgress_; }
    void setSlingshotProgress(const SlingshotProgress& progress);
    bool takeSlingshotPersistenceDirty() {
        const bool d = slingshotPersistenceDirty_;
        slingshotPersistenceDirty_ = false;
        return d;
    }
    SlingshotMode* mutableSlingshotMode() { return dynamic_cast<SlingshotMode*>(mode_.get()); }
    const SlingshotMode* slingshotMode() const { return dynamic_cast<const SlingshotMode*>(mode_.get()); }

    bool airHockeyEnabled() const { return mode_ && mode_->kind() == ModeKind::AirHockey; }
    void setAirHockey(bool enabled);
    const AirHockeyConfig& airHockeyConfig() const { return airHockeyConfig_; }
    void setAirHockeyConfig(const AirHockeyConfig& cfg);
    AirHockeyMode* mutableAirHockeyMode() { return dynamic_cast<AirHockeyMode*>(mode_.get()); }
    const AirHockeyMode* airHockeyMode() const { return dynamic_cast<const AirHockeyMode*>(mode_.get()); }

    bool interactiveArenaContains(Vec2 p) const;
    [[nodiscard]] std::vector<Vec2> currentIconSceneCenters() const;
    [[nodiscard]] const SceneSetup& initialSceneSetup() const { return setup_; }

    void setMuted(bool muted);
    bool muted() const { return muted_; }
    void drainSounds(std::vector<SoundEvent>& out);
    void forceRedraw() {
        dirty_ = true;
        tick(0);
    }
    FrozenScene freezeForMigration();
    static std::unique_ptr<Playground> prepareMigration(const FrozenScene& frozen, Vec2 size,
                                                        MigrationPreparationStage* stage = nullptr);
    bool migrationPaused() const { return migrationPaused_; }
    void commitMigration() {
        migrationPaused_ = false;
        clock_.reset();
        previousTickActive_ = false;
    }

    const DrawList& drawList() const { return draw_; }
    const GameMode& mode() const { return *mode_; }
    bool paused() const { return paused_; }
    bool exitRequested() const { return exit_; }
    bool idle() const { return paused_ || migrationPaused_ || mode_->isIdle(); }
    PerformanceMonitor perf;

   private:
    [[nodiscard]] bool hasArenaSubRect() const {
        return setup_.primaryArenaRect.width >= 100.0f && setup_.primaryArenaRect.height >= 100.0f;
    }
    [[nodiscard]] Vec2 arenaOrigin() const {
        return hasArenaSubRect() ? Vec2{setup_.primaryArenaRect.x, setup_.primaryArenaRect.y} : Vec2{0.0f, 0.0f};
    }
    [[nodiscard]] Vec2 arenaWorkAreaSize() const {
        return hasArenaSubRect() ? Vec2{setup_.primaryArenaRect.width, setup_.primaryArenaRect.height}
                                 : setup_.workAreaSize;
    }
    [[nodiscard]] SceneSetup arenaSceneSetup() const;
    [[nodiscard]] GravityModeConfig currentGravityConfig() const;
    void syncBilliardsStats();
    void collectModeSounds();
    void leaveNonGravityToGravity();

    SceneSetup setup_;
    std::unique_ptr<GameMode> mode_;
    FixedStepClock clock_;
    DrawList draw_;
    std::vector<SoundEvent> pendingSounds_;
    std::vector<SoundEvent> scratchSounds_;
    float gravityMagnitude_ = kEarthGravityMps2;
    GravityDirection gravityDirection_ = GravityDirection::Down;
    float gravityRestitution_ = kDefaultGravityRestitution;
    BilliardsConfig billiardsConfig_ = BilliardsConfig::desktop();
    BilliardsSessionStats billiardsStats_{};
    SlingshotConfig slingshotConfig_{};
    int slingshotHighScore_ = 0;
    SlingshotProgress slingshotProgress_{};
    bool slingshotPersistenceDirty_ = false;
    AirHockeyConfig airHockeyConfig_{};
    int rackObservedShots_ = 0;
    int rackObservedPocketed_ = 0;
    int rackObservedPlacements_ = 0;
    bool paused_ = false, exit_ = false, dirty_ = true;
    bool muted_ = false;
    bool migrationPaused_ = false;
    bool previousTickActive_ = false;
};
}  // namespace dpp
