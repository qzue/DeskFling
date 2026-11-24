#pragma once
// Desktop billiards: authentic International Heyball/8-Ball (2540x1270mm, 57.15mm ball) &
// WPBSA Championship 12-foot Snooker (3569x1778mm, 52.5mm ball) proportions,
// bevelled 6-cushion physics with recessed pockets, toggleable ball/table/aim borders,
// flexible icon selection + standard fillers + bottom-gravity surplus, 3D spin, sound & LAN sync.

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "core/modes/game_mode.h"
#include "core/scene/coordinates.h"

namespace dpp {

enum class BilliardsStyle { Classic, Desktop };

enum class BilliardsRule : std::uint8_t {
    Free = 0,       // Clear all balls in any order (International 8-Ball table proportions)
    EightBall = 1,  // 15 balls: International Heyball / 8-Ball (2540x1270mm, 57.15mm ball)
    Snooker = 2,    // 21 balls: WPBSA 12-foot Snooker (3569x1778mm, 52.5mm ball)
};

enum class BilliardsMatchMode : std::uint8_t {
    Solo = 0,     // Single-player challenge
    Hotseat = 1,  // Two-player local alternating turns
    Lan = 2,      // Two-player LAN (Player 0 = Host, Player 1 = Guest)
    VsAi = 3,     // Player (P1 = 0) vs AI opponent (P2 = 1)
};

enum class BilliardsAiDifficulty : std::uint8_t {
    Easy = 0,    // 简单: direct shots, noticeable angle/power error, no spin
    Medium = 1,  // 标准: direct + 1-rail bank/kick, moderate precision, basic follow/draw
    Master = 2,  // 大师: precision pots, combos/banks, scratch-avoidance & 2-ply spin
    Goat = 3,    // 球王: zero-error multi-ply runout + K-ball breakouts + lockdown snooker & 360° escape
};

enum class IconSelectionPolicy : std::uint8_t {
    FirstN = 0,  // First N desktop icons in snapshot order
    Random = 1,  // Shuffled subset of desktop icons
    Custom = 2,  // User-picked desktop icon indices
};

enum class ExtraIconsPolicy : std::uint8_t {
    Hide = 0,           // Hidden during billiards, restored when leaving billiards
    BottomGravity = 1,  // Drop to bottom of desktop under gravity during billiards
};

enum class BilliardsTableTheme : std::uint8_t {
    Full = 0,        // Wooden rails, tournament cushions, castings, sights & side return tubes
    Minimal = 1,     // Translucent thin cushion/jaw lines & 6 hollow pocket rings (no wood/tubes)
    Borderless = 2,  // Invisible boundaries & pockets on desktop wallpaper
};

enum class BallGroup : std::uint8_t {
    FreeBall = 0,
    Solid = 1,
    Stripe = 2,
    Eight = 3,
    SnookerRed = 4,
    SnookerColor = 5,
};

struct BilliardsConfig {
    BilliardsStyle style = BilliardsStyle::Classic;
    BilliardsRule rule = BilliardsRule::Free;
    BilliardsMatchMode matchMode = BilliardsMatchMode::Solo;
    BilliardsAiDifficulty aiDifficulty = BilliardsAiDifficulty::Medium;
    IconSelectionPolicy iconSelection = IconSelectionPolicy::FirstN;
    ExtraIconsPolicy extraIcons = ExtraIconsPolicy::Hide;
    bool showBallBorders = true;   // Outer colored/ivory ring & border around icon balls
    bool showBallNumbers = false;  // Ball number / Snooker value badge at ball center
    BilliardsTableTheme tableTheme = BilliardsTableTheme::Full;
    bool showTableChrome = true;   // Synced with tableTheme == Full (false maps Full -> Borderless)
    bool showAimGuide = true;      // Dashed aiming guide line
    bool showAiIntent = true;      // Show AI tactical intent tag (e.g. [K球破局], [做斯诺克]) in VsAi HUD

    [[nodiscard]] BilliardsTableTheme effectiveTableTheme() const {
        if (tableTheme != BilliardsTableTheme::Full) return tableTheme;
        return showTableChrome ? BilliardsTableTheme::Full : BilliardsTableTheme::Borderless;
    }
    std::uint32_t randomSeed = 0x20261122u;
    std::size_t freeModeMaxRackBalls = 0;  // 0 = uncapped; >0 = auto-cap Free rack & route rest to extraIcons
    std::vector<std::size_t> customIconIndices;

    float tableInsetX = 0.06f;   // Fraction of work area width reserved on each side
    float tableInsetY = 0.09f;   // Fraction of work area height reserved top & bottom
    float maxShotSpeed = 14.0f;  // m/s baseline
    float minCharge = 0.15f;
    double fullChargeSeconds = 0.9;
    float maxPullPoints = 200.0f;    // Desktop pull-back distance for 100% power
    float cancelPullPoints = 10.0f;  // Desktop pull-back below this cancels shot
    float tubeDropSeconds = 0.30f;   // Side return tube drop animation duration
    float restSpeed = 0.04f;         // m/s: below this everything counts as stopped
    double maxRollSeconds = 25.0;    // safety: force a stop after this long

    // International Heyball / 8-Ball 9-foot tournament specification (2540 mm x 1270 mm, 57.15 mm ball).
    static constexpr float kEightBallAspect = 2540.0f / 1270.0f;  // 2.0000
    static constexpr float kEightBallToWidth = 57.15f / 1270.0f;  // 0.0450
    static constexpr float kStandardAspect = kEightBallAspect;
    static constexpr float kStandardBallToWidth = kEightBallToWidth;

    // International WPBSA 12-foot Championship Snooker specification (3569 mm x 1778 mm, 52.5 mm ball).
    static constexpr float kSnookerAspect = 3569.0f / 1778.0f;                 // 2.00731
    static constexpr float kSnookerBallToWidth = 52.5f / 1778.0f;              // 0.029528
    static constexpr float kSnookerBaulkRatio = 737.0f / 3569.0f;              // 0.20650
    static constexpr float kSnookerDRatio = 292.0f / 1778.0f;                  // 0.16423
    static constexpr float kSnookerBlackRatio = (3569.0f - 324.0f) / 3569.0f;  // 0.90922

    static constexpr float kMinBallDiameterPoints = 11.5f;

    static BilliardsConfig desktop() {
        BilliardsConfig c;
        c.style = BilliardsStyle::Desktop;
        c.tableInsetX = 0.095f;
        c.tableInsetY = 0.125f;
        c.maxShotSpeed = 22.0f;
        return c;
    }
};

enum class BilliardsState { Ready, Charging, Rolling, PlacingWhite, Finished };

struct BilliardsLanBall {
    Vec2 normPos{};  // Normalized to table playing surface (x in [0,1], y in [0,1])
    int number = 1;
    BallGroup group = BallGroup::FreeBall;
    bool pocketed = false;
    int tube = 0;
    int tubeSlot = 0;
    float dropProgress = 1.0f;
};

struct BilliardsLanSnapshot {
    BilliardsState state = BilliardsState::Ready;
    BilliardsRule rule = BilliardsRule::Free;
    int currentPlayer = 0;  // 0 = Host (P1), 1 = Guest (P2)
    int winner = -1;        // -1 = ongoing, 0 = P1, 1 = P2
    std::array<int, 2> scores{0, 0};
    std::array<BallGroup, 2> playerGroups{BallGroup::FreeBall, BallGroup::FreeBall};
    int snookerTarget = 1;  // 1=Red, 8=Any Color, 2..7=Ordered Color
    int shots = 0;
    int pocketedCount = 0;
    bool whitePocketed = false;
    Vec2 whiteNormPos{0.25f, 0.5f};
    Vec2 placeNormPos{0.25f, 0.5f};
    Vec2 aimDir{1.0f, 0.0f};
    Vec2 tipOffset{0.0f, 0.0f};
    float pullRatio = 0.0f;
    std::array<char, 128> statusNote{};
    std::vector<BilliardsLanBall> balls;
};

struct BilliardsRemoteAction {
    enum class Kind : std::uint8_t { Aim, Shoot, PlaceWhite };
    Kind kind = Kind::Aim;
    Vec2 aimDir{1.0f, 0.0f};
    float power = 0.0f;
    Vec2 tipOffset{0.0f, 0.0f};
    Vec2 placeNormPos{0.25f, 0.5f};
    float pullRatio = 0.0f;
};

class BilliardsMode final : public GameMode {
   public:
    explicit BilliardsMode(const SceneSetup& setup, const BilliardsConfig& config = {},
                           int initialBestShots = 0, int startingPlayer = 0,
                           std::array<int, 2> initialSeriesWins = {0, 0});

    [[nodiscard]] ModeKind kind() const override { return ModeKind::Billiards; }
    void handleInput(const InputEvent& e) override;
    void stepPhysics(float dt) override;
    void updateRules(float dt) override;
    void buildDrawList(DrawList& out) const override;
    void drainSounds(std::vector<SoundEvent>& out) override;
    [[nodiscard]] bool isIdle() const override;
    [[nodiscard]] ModeStats stats() const override;
    [[nodiscard]] const PhysicsWorld& world() const override { return world_; }

    [[nodiscard]] static Vec2 computeTableSizeMeters(
        Vec2 workAreaSize, const BilliardsConfig& config = BilliardsConfig::desktop());
    [[nodiscard]] static float computeBallRadiusMeters(
        Vec2 workAreaSize, std::size_t iconCount, const BilliardsConfig& config = BilliardsConfig::desktop());
    [[nodiscard]] static bool canFitIcons(Vec2 workAreaSize, std::size_t iconCount,
                                          const BilliardsConfig& config = BilliardsConfig::desktop());
    [[nodiscard]] static std::size_t maxFittingRackBalls(
        Vec2 workAreaSize, const BilliardsConfig& config = BilliardsConfig::desktop());

    [[nodiscard]] const BilliardsConfig& config() const { return config_; }
    void setShowBallNumbers(bool show) { config_.showBallNumbers = show; }
    void setVisualOptions(bool ballBorders, bool ballNumbers, BilliardsTableTheme theme, bool aimGuide,
                          bool showAiIntent = true) {
        config_.showBallBorders = ballBorders;
        config_.showBallNumbers = ballNumbers;
        config_.tableTheme = theme;
        config_.showTableChrome = (theme == BilliardsTableTheme::Full);
        config_.showAimGuide = aimGuide;
        config_.showAiIntent = showAiIntent;
    }
    void setVisualOptions(bool ballBorders, bool ballNumbers, bool tableChrome, bool aimGuide,
                          bool showAiIntent = true) {
        setVisualOptions(ballBorders, ballNumbers,
                         tableChrome ? BilliardsTableTheme::Full : BilliardsTableTheme::Borderless, aimGuide,
                         showAiIntent);
    }
    void setAiDifficulty(BilliardsAiDifficulty diff);
    [[nodiscard]] bool isAiTurn() const;
    [[nodiscard]] BilliardsState state() const { return state_; }
    [[nodiscard]] bool whitePocketed() const { return whitePocketed_; }
    [[nodiscard]] Vec2 whiteSceneCenter() const;
    [[nodiscard]] Vec2 whiteSpawnScene() const { return coords_.toScene(whiteSpawn_); }
    [[nodiscard]] Vec2 tableMinScene() const {
        return coords_.toScene({tableMin_.x, tableMin_.y + tableSize_.y});
    }
    [[nodiscard]] Vec2 pocketScene(std::size_t i) const { return coords_.toScene(pockets_.at(i)); }
    [[nodiscard]] float pocketHoleRadiusScene(std::size_t i) const {
        return coords_.lengthToScene((i == 1 || i == 4) ? sidePocketHoleRadius_ : pocketHoleRadius_);
    }
    [[nodiscard]] float railOuterWidthScene() const { return coords_.lengthToScene(railOuterMeters()); }
    [[nodiscard]] float ballRadiusScene() const { return coords_.lengthToScene(radius_); }
    [[nodiscard]] Vec2 tableSizeScene() const {
        return {coords_.lengthToScene(tableSize_.x), coords_.lengthToScene(tableSize_.y)};
    }
    [[nodiscard]] std::size_t objectBallCount() const { return balls_.size(); }
    [[nodiscard]] std::size_t extraIconCount() const { return extraIcons_.size(); }
    [[nodiscard]] Vec2 objectBallScene(std::size_t i) const;
    [[nodiscard]] int objectBallNumber(std::size_t i) const { return balls_.at(i).number; }
    [[nodiscard]] BallGroup objectBallGroup(std::size_t i) const { return balls_.at(i).group; }
    [[nodiscard]] TextureId objectBallTexture(std::size_t i) const { return balls_.at(i).texture; }
    [[nodiscard]] Vec2 iconCurrentSceneCenter(std::size_t i) const;

    [[nodiscard]] bool tableContains(Vec2 scenePos) const;
    [[nodiscard]] bool interactiveContains(Vec2 scenePos) const;
    [[nodiscard]] bool isTubeBallHit(Vec2 scenePos) const;
    [[nodiscard]] bool canPlaceWhite(Vec2 scenePos) const;

    [[nodiscard]] Vec2 tipOffset() const { return tipOffset_; }
    void setTipOffset(Vec2 offset);
    [[nodiscard]] Vec2 spinWidgetCenterScene() const;
    [[nodiscard]] float spinWidgetRadiusScene() const;
    [[nodiscard]] Vec2 aimToggleCenterScene() const;
    [[nodiscard]] Vec2 aiIntentToggleCenterScene() const;
    [[nodiscard]] Vec2 rematchButtonCenterScene() const;
    [[nodiscard]] int ballTubeSide(std::size_t i) const { return balls_.at(i).tube; }

    [[nodiscard]] float pullDistancePoints() const { return pullDistancePt_; }
    [[nodiscard]] Vec2 lockedAimDirection() const { return lockedAimDir_; }
    [[nodiscard]] int whitePlacements() const { return whitePlacements_; }
    [[nodiscard]] int bestShots() const { return bestShots_; }
    [[nodiscard]] int startingPlayer() const { return startingPlayer_; }
    [[nodiscard]] int currentPlayer() const { return currentPlayer_; }
    [[nodiscard]] int winner() const { return winner_; }
    [[nodiscard]] int score(int player) const {
        return scores_[static_cast<std::size_t>(player == 1 ? 1 : 0)];
    }
    [[nodiscard]] int seriesWins(int player) const {
        return seriesWins_[static_cast<std::size_t>(player == 1 ? 1 : 0)];
    }
    [[nodiscard]] std::array<int, 2> seriesWinsArray() const { return seriesWins_; }
    [[nodiscard]] int currentBreak() const { return currentBreak_; }
    [[nodiscard]] const char* aiIntentTag() const { return aiIntentTag_.data(); }
    [[nodiscard]] bool frameResultRecorded() const { return frameResultRecorded_; }
    void markFrameResultRecorded() { frameResultRecorded_ = true; }
    [[nodiscard]] bool takeRematchRequest() {
        const bool r = rematchRequested_;
        rematchRequested_ = false;
        return r;
    }
    [[nodiscard]] BallGroup playerGroup(int player) const {
        return playerGroups_[static_cast<std::size_t>(player == 1 ? 1 : 0)];
    }
    [[nodiscard]] int snookerTarget() const { return snookerTarget_; }
    [[nodiscard]] std::size_t tubeBallCount(int side) const {
        return (side == 0 || side == 1) ? tubeOrder_[static_cast<std::size_t>(side)].size() : 0;
    }

    // LAN role & synchronization hooks (0 = local/host player, 1 = guest player; -1 = local solo/hotseat).
    void setLanLocalPlayer(int localPlayer) { lanLocalPlayer_ = localPlayer; }
    [[nodiscard]] int lanLocalPlayer() const { return lanLocalPlayer_; }
    [[nodiscard]] std::optional<BilliardsRemoteAction> takeLocalLanAction();
    void applyRemoteLanAction(const BilliardsRemoteAction& action);
    [[nodiscard]] BilliardsLanSnapshot exportLanSnapshot() const;
    void applyLanSnapshot(const BilliardsLanSnapshot& snap);

    PhysicsWorld& mutableWorld() { return world_; }
    void debugPlaceBall(std::size_t i, Vec2 scenePos, Vec2 sceneVelocity);
    void debugPlaceWhite(Vec2 scenePos, Vec2 sceneVelocity);
    void debugSetTurnState(int player, BallGroup p0Group = BallGroup::FreeBall,
                           BallGroup p1Group = BallGroup::FreeBall, int snookerTarget = -1);

   private:
    enum class AiPhase : std::uint8_t { Idle = 0, Placing = 1, Aiming = 2, Charging = 3 };
    struct Ball {
        BodyIndex body = 0;
        TextureId texture = kNoTexture;
        int iconIndex = -1;  // -1 => standard non-icon filler ball
        int number = 1;      // 1..15 in 8-ball, point value 1..7 in Snooker
        BallGroup group = BallGroup::FreeBall;
        Vec2 homeSpot{};  // Physics metres (used to re-spot Snooker colors / #8)
        bool pocketed = false;
        int tube = 0;
        int tubeSlot = 0;
        float dropProgress = 1.0f;
    };

    struct ExtraIcon {
        std::size_t iconIndex = 0;
        TextureId texture = kNoTexture;
        Vec2 visualSize{};
        Vec2 originalCenter{};
        BodyIndex body = 0;
    };

    struct TubeGeometry {
        float cx = 0.0f;
        float topY = 0.0f;
        float botY = 0.0f;
        float capR = 0.0f;
        std::size_t firstVisible = 0;
        std::size_t visibleCount = 0;
        std::size_t overflow = 0;
        float step = 0.0f;
        float overflowCenterY = 0.0f;
    };

    struct SpinWidgetGeometry {
        Vec2 center{};
        float radius = 22.0f;
        Rect resetBox{};
    };

    struct PairCooldown {
        std::uint32_t key = 0;
        double until = 0.0;
    };

    struct AiSimOutcome {
        int firstHitBall = -1;
        bool scratched = false;
        bool foul = false;
        bool lostGame = false;
        bool wonGame = false;
        bool continuesTurn = false;
        int gainedScore = 0;
        int ownPotted = 0;
        int oppPotted = 0;
        BallGroup nextAiGroup = BallGroup::FreeBall;
        int nextSnookerTarget = 1;
        Vec2 whiteEndPhys{};
        std::vector<Vec2> endBallPos;
        std::vector<bool> endBallActive;
    };

    [[nodiscard]] float railOuterMeters() const;
    void populateDesktopTableCushions(PhysicsWorld& targetWorld) const;
    void buildDesktopTableCushions();
    void shootDirection(Vec2 dirScene, float power);
    void evaluateTurnRules();
    void rebalanceEightBallPlayerTubes();
    void respotSnookerColor(std::size_t ballIndex);
    [[nodiscard]] int remainingReds() const;
    [[nodiscard]] int lowestRemainingColorValue() const;
    [[nodiscard]] int remainingGroupBalls(BallGroup g) const;
    [[nodiscard]] static Color standardBallColor(int number, BilliardsRule rule);
    void drawStyledBall(DrawList& out, const Ball& b, Vec2 center, float r) const;
    [[nodiscard]] Vec2 toTableNorm(Vec2 physPos) const;
    [[nodiscard]] Vec2 fromTableNorm(Vec2 normPos) const;

    void updateSpinFromPointer(Vec2 scenePos);
    [[nodiscard]] SpinWidgetGeometry spinWidgetGeometry() const;
    [[nodiscard]] bool spinWidgetContains(Vec2 scenePos) const;
    [[nodiscard]] bool spinResetContains(Vec2 scenePos) const;
    [[nodiscard]] Rect aimToggleRect() const;
    [[nodiscard]] bool aimToggleContains(Vec2 scenePos) const;
    [[nodiscard]] Rect aiIntentToggleRect() const;
    [[nodiscard]] bool aiIntentToggleContains(Vec2 scenePos) const;
    [[nodiscard]] Rect rematchButtonRect() const;
    [[nodiscard]] bool rematchButtonContains(Vec2 scenePos) const;
    void recordSeriesFrameWinIfNeeded();
    void pushSound(SoundKind kind, float gain, float sceneX, float pitch = 1.0f);
    [[nodiscard]] float scenePan(float sceneX) const;
    [[nodiscard]] bool everythingStopped() const;
    bool respawnWhite();
    [[nodiscard]] bool inPocket(Vec2 p) const;
    [[nodiscard]] int matchingPocket(Vec2 p) const;
    [[nodiscard]] float pocketCatchRadius(std::size_t pocketIndex) const;
    [[nodiscard]] TubeGeometry tubeGeometry(int side) const;
    [[nodiscard]] Vec2 tubeBallSceneCenter(std::size_t ballIndex, const TubeGeometry& geom) const;
    [[nodiscard]] const char* playerLabel(int player) const;
    [[nodiscard]] bool isLegalTargetBall(std::size_t ballIndex, int player) const;
    [[nodiscard]] std::pair<Vec2, Vec2> pocketJawCenters(std::size_t pocketIndex) const;
    [[nodiscard]] Vec2 pocketThroatMeters(std::size_t pocketIndex) const;
    [[nodiscard]] bool pocketPathClearJaws(std::size_t pocketIndex, Vec2 fromPhys, Vec2 entryPhys) const;
    [[nodiscard]] bool segmentClearOfBalls(Vec2 a, Vec2 b, float clearance, int ignoreBallA = -1,
                                           int ignoreBallB = -1) const;
    [[nodiscard]] bool cushionBounceValid(int railSide, Vec2 bouncePhys) const;
    [[nodiscard]] bool pathRisksScratch(Vec2 startPhys, Vec2 dirUnit, float travelDist) const;
    [[nodiscard]] AiSimOutcome simulateShotRollout(Vec2 whiteStartPhys, Vec2 aimPhysDir, float power,
                                                   Vec2 spin, int maxSteps = 420) const;
    [[nodiscard]] bool hasVerifiedSecondPlyPot(const AiSimOutcome& afterFirstShot,
                                               float* outBestPly2Score) const;
    [[nodiscard]] int countTiedOwnBalls(const std::vector<Vec2>& ballPos, const std::vector<bool>& ballActive,
                                        BallGroup aiGrp, int snkTarget) const;
    [[nodiscard]] float evaluatePostRolloutPosition(const AiSimOutcome& out, int tiedBefore = -1) const;
    [[nodiscard]] float evaluateOpponentThreat(const AiSimOutcome& out) const;
    [[nodiscard]] float evaluateNextBallPosition(Vec2 estWhiteRest, std::size_t pottedBallIdx) const;
    [[nodiscard]] Vec2 planAiWhitePlacementPhys() const;
    void planAiShot();
    void updateAiTurn(float dt);

    Vec2 sceneSize_;
    SceneToPhysics coords_;
    BilliardsConfig config_;
    PhysicsWorld world_;
    std::unique_ptr<PhysicsWorld> extraWorld_;
    Vec2 tableMin_, tableSize_;  // metres (playing surface nose-to-nose)
    float radius_ = 0.2f;
    float pocketRadius_ = 0.3f;          // catch radius (corner)
    float sidePocketRadius_ = 0.32f;     // catch radius (middle)
    float pocketHoleRadius_ = 0.3f;      // visual hole radius (corner)
    float sidePocketHoleRadius_ = 0.3f;  // visual hole radius (middle)
    float cornerMouth_ = 0.4f;           // jaw gap along rail from corner nose
    float sideMouth_ = 0.34f;            // jaw half-gap along rail from midX
    std::vector<Vec2> pockets_;          // metres (recessed pocket well centers)
    BodyIndex white_ = 0;
    Vec2 whiteSpawn_;
    std::vector<Ball> balls_;
    std::vector<ExtraIcon> extraIcons_;
    std::vector<int> iconToBall_;   // size == setup.icons.size(), -1 if in extraIcons_
    std::vector<int> iconToExtra_;  // size == setup.icons.size(), -1 if on table
    std::array<std::vector<std::size_t>, 2> tubeOrder_{};
    BilliardsState state_ = BilliardsState::Ready;
    bool whitePocketed_ = false;
    int pocketedCount_ = 0;
    int shots_ = 0;
    int whitePlacements_ = 0;
    int bestShots_ = 0;

    // Match & rule state (EightBall / Snooker / Hotseat / LAN / VsAi).
    int startingPlayer_ = 0;
    int currentPlayer_ = 0;
    int winner_ = -1;
    std::array<int, 2> scores_{0, 0};
    std::array<int, 2> seriesWins_{0, 0};
    bool seriesCountedLocal_ = false;
    bool frameResultRecorded_ = false;
    bool rematchRequested_ = false;
    int currentBreak_ = 0;
    std::array<BallGroup, 2> playerGroups_{BallGroup::FreeBall, BallGroup::FreeBall};
    int snookerTarget_ = 1;  // 1 = Red, 8 = Any Color (free choice after Red), 2..7 = specific color
    int firstHitBall_ = -1;
    bool whitePottedThisShot_ = false;
    std::vector<std::size_t> pottedThisShot_;
    std::array<char, 128> turnNote_{};
    std::array<char, 48> aiIntentTag_{};

    // LAN state.
    int lanLocalPlayer_ = -1;  // -1 = local control, 0 = Host, 1 = Guest
    std::optional<BilliardsRemoteAction> pendingLanAction_;

    // AI opponent state (Player 1 in BilliardsMatchMode::VsAi).
    AiPhase aiPhase_ = AiPhase::Idle;
    float aiPhaseTime_ = 0.0f;
    Vec2 aiPlaceStartScene_{};
    Vec2 aiPlaceTargetScene_{};
    Vec2 aiStartAimDir_{1.0f, 0.0f};
    Vec2 aiPlannedAimDir_{1.0f, 0.0f};
    Vec2 aiStartSpin_{0.0f, 0.0f};
    Vec2 aiPlannedSpin_{0.0f, 0.0f};
    float aiPlannedPower_ = 0.5f;
    float aiTempoScale_ = 1.0f;
    std::uint32_t aiRngState_ = 0x9E3779B9u;

    Vec2 aimScene_;
    Vec2 aimDir_{1.0f, 0.0f};
    Vec2 lockedAimDir_{1.0f, 0.0f};
    Vec2 dragStartScene_{};
    Vec2 placeScene_{};
    float pullDistancePt_ = 0.0f;
    Vec2 tipOffset_{0.0f, 0.0f};
    bool adjustingSpin_ = false;
    Vec2 cueRollVel_{0.0f, 0.0f};
    float cueSideSpin_ = 0.0f;
    bool cueHitBall_ = false;
    double chargeStart_ = 0.0;
    double chargeElapsed_ = 0.0;
    double rollingTime_ = 0.0;
    double simTime_ = 0.0;
    std::vector<SoundEvent> pendingSounds_;
    std::vector<PairCooldown> pairCooldowns_;
};

}  // namespace dpp
