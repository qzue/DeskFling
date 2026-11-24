#pragma once
// Desktop Slingshot Siege: pull an elastic slingshot on the left to launch chosen
// desktop icons into destructible Box2D wood/stone/glass fortresses housing the
// remaining defender icons. Zero platform types or real file paths in core/.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/modes/billiards_mode.h"
#include "core/modes/game_mode.h"
#include "core/scene/coordinates.h"

namespace dpp {

enum class SlingshotPreset : std::uint8_t {
    TwinTowers = 0,     // 经典双塔
    PyramidFort = 1,    // 金字塔要塞
    GlassBridge = 2,    // 冰晶悬廊
    StoneCitadel = 3,   // 巨石碉堡
    RandomSiege = 4,    // 随机无尽攻城
    CustomBuilder = 5,  // 自建关卡工坊
};

enum class SlingshotBlockKind : std::uint8_t {
    Glass = 0,
    Wood = 1,
    Stone = 2,
};

enum class SlingshotCustomPieceKind : std::uint8_t {
    WoodPillar = 0,
    WoodBeam = 1,
    StoneBlock = 2,
    GlassBeam = 3,
    DefenderSlot = 4,
};

struct SlingshotCustomPiece {
    SlingshotCustomPieceKind kind = SlingshotCustomPieceKind::WoodPillar;
    float normX = 0.0f;       // Offset from fortress anchor X in multiples of unit_ [-2.8, +2.4]
    float normHeight = 0.6f;  // Center height above groundY_ in multiples of unit_ [0.15, 4.8]

    bool operator==(const SlingshotCustomPiece& o) const {
        return kind == o.kind && std::abs(normX - o.normX) < 1e-4f &&
               std::abs(normHeight - o.normHeight) < 1e-4f;
    }
};

struct SlingshotProgress {
    int highScore = 0;
    int maxUnlockedStage = 1;
    std::array<std::uint8_t, 6> presetBestStars{};
    std::array<std::uint8_t, 16> stageStars{};

    [[nodiscard]] int totalStars() const;
    [[nodiscard]] int starsForStage(int stageIndex) const;
    [[nodiscard]] int starsForPreset(SlingshotPreset preset) const;
    void recordClear(SlingshotPreset preset, int stageIndex, int stars, int finalScore);
};

[[nodiscard]] std::string encodeSlingshotProgress(const SlingshotProgress& progress);
[[nodiscard]] SlingshotProgress decodeSlingshotProgress(std::string_view encoded);
[[nodiscard]] std::string encodeSlingshotCustomPieces(std::span<const SlingshotCustomPiece> pieces);
[[nodiscard]] std::vector<SlingshotCustomPiece> decodeSlingshotCustomPieces(std::string_view encoded);
[[nodiscard]] std::vector<SlingshotCustomPiece> defaultSlingshotCustomPieces();

struct SlingshotConfig {
    SlingshotPreset preset = SlingshotPreset::TwinTowers;
    IconSelectionPolicy iconSelection = IconSelectionPolicy::FirstN;
    bool showTrajectory = true;
    bool showGroundChrome = true;
    bool startInEditor = false;
    int ammoCount = 4;  // 3..6 shots per stage
    std::uint32_t randomSeed = 0x20261123u;
    std::vector<std::size_t> customIconIndices;
    std::vector<SlingshotCustomPiece> customPieces;
};

class SlingshotMode final : public GameMode {
   public:
    explicit SlingshotMode(const SceneSetup& setup, const SlingshotConfig& config = {}, int stageIndex = 1,
                           int carryScore = 0, int highScore = 0,
                           const SlingshotProgress& progress = {});

    [[nodiscard]] ModeKind kind() const override { return ModeKind::Slingshot; }
    void handleInput(const InputEvent& event) override;
    void stepPhysics(float dt) override;
    void updateRules(float dt) override;
    void buildDrawList(DrawList& out) const override;
    void drainSounds(std::vector<SoundEvent>& out) override;

    [[nodiscard]] bool isIdle() const override;
    [[nodiscard]] ModeStats stats() const override;
    [[nodiscard]] const PhysicsWorld& world() const override { return *world_; }

    [[nodiscard]] const SlingshotConfig& config() const { return config_; }
    void setVisualOptions(bool showTrajectory, bool showGroundChrome);
    void advanceStage();

    [[nodiscard]] int score() const { return score_; }
    [[nodiscard]] int highScore() const { return highScore_; }
    [[nodiscard]] int stageIndex() const { return stageIndex_; }
    [[nodiscard]] int lastClearStars() const { return lastClearStars_; }
    [[nodiscard]] const SlingshotProgress& progress() const { return progress_; }
    [[nodiscard]] bool editing() const { return editing_; }
    void setEditing(bool editing);
    [[nodiscard]] bool takePersistenceDirty() {
        const bool d = persistenceDirty_;
        persistenceDirty_ = false;
        return d;
    }
    [[nodiscard]] int remainingAmmo() const;
    [[nodiscard]] int remainingDefenders() const;
    [[nodiscard]] bool stageCleared() const { return stageCleared_; }
    [[nodiscard]] bool stageFailed() const { return stageFailed_; }

    // Scene center for every icon in SceneSetup (including queued/trophy icons).
    [[nodiscard]] Vec2 iconCurrentSceneCenter(std::size_t iconIndex) const;
    // Hit-test for interactive slingshot pouch, HUD pills, editor canvas, and icons.
    [[nodiscard]] bool interactiveContains(Vec2 scenePoint) const;
    [[nodiscard]] Rect trajectoryToggleRect() const { return trajToggleRect_; }
    [[nodiscard]] Rect nextStageButtonRect() const { return nextStageRect_; }
    [[nodiscard]] Rect editorToggleRect() const { return editorToggleRect_; }
    [[nodiscard]] Vec2 pouchRestScene() const { return slingAnchor_; }
    [[nodiscard]] float maxPullScene() const { return maxPull_; }
    [[nodiscard]] Vec2 customBuildAnchorScene() const { return {fortAnchorX_, groundY_}; }
    [[nodiscard]] float customBuildUnitScene() const { return fortUnit_; }

   private:
    enum class Phase : std::uint8_t {
        ReadyToAim = 0,
        Pulling = 1,
        InFlight = 2,
        Settling = 3,
        StageClear = 4,
        StageFail = 5,
    };

    struct Block {
        BodyIndex body = 0;
        SlingshotBlockKind material = SlingshotBlockKind::Wood;
        Vec2 sizeScene{};  // full width, height in scene points
        float hp = 10.0f;
        float maxHp = 10.0f;
        bool destroyed = false;
        std::uint32_t visualSeed = 0;
        Vec2 lastHitLocal{0.0f, 0.0f};  // impact point in block-local coordinates
    };

    struct Defender {
        BodyIndex body = 0;
        std::optional<std::size_t> iconIndex;  // nullopt => synthetic emblem defender
        TextureId texture = kNoTexture;
        float radiusScene = 20.0f;
        float hp = 1.4f;
        bool defeated = false;
        Vec2 trophySlotScene{};
        float trophyAnim = 1.0f;
        Vec2 initialScenePos{};
    };

    struct AmmoSlot {
        std::optional<std::size_t> iconIndex;
        TextureId texture = kNoTexture;
        BodyIndex body = 0;
        bool launched = false;
        bool retired = false;
        Vec2 waitPosScene{};
    };

    struct Debris {
        Vec2 pos{};
        Vec2 vel{};
        Color color{};
        Color accent{255, 255, 255, 180};
        Vec2 shardSize{10.0f, 3.6f};
        float angle = 0.0f;
        float angularVel = 0.0f;
        float radius = 3.0f;
        float life = 0.0f;
        float maxLife = 0.45f;
        SlingshotBlockKind kind = SlingshotBlockKind::Wood;
        bool isShard = false;
    };

    struct ShockRing {
        Vec2 center{};
        float radius = 10.0f;
        float maxRadius = 70.0f;
        float alpha = 1.0f;
        Color color{255, 220, 110, 230};
    };

    void buildFortress(SlingshotPreset effectivePreset, const std::vector<std::size_t>& defenderIcons);
    void addBlockScene(Vec2 centerScene, Vec2 sizeScene, SlingshotBlockKind mat, float angleRad = 0.0f);
    void addDefenderScene(Vec2 centerScene, std::optional<std::size_t> iconIdx, TextureId tex);
    void spawnDebris(Vec2 centerScene, Color baseColor, int count, float speedScale = 1.0f);
    void spawnBlockShards(Vec2 centerScene, SlingshotBlockKind mat, int count, float speedScale = 1.0f);
    void emitSound(SoundKind kind, float gain, Vec2 scenePos, float pitch = 1.0f);
    void defeatDefender(std::size_t defIdx, Vec2 hitScene);
    void loadNextAmmo();
    [[nodiscard]] Vec2 launchVelocityPhysics(Vec2 pullFromAnchorScene) const;
    [[nodiscard]] SlingshotPreset resolvedPreset() const;
    [[nodiscard]] Vec2 customPieceSceneCenter(const SlingshotCustomPiece& p) const;
    [[nodiscard]] Vec2 customPieceSceneSize(SlingshotCustomPieceKind kind) const;
    [[nodiscard]] SlingshotCustomPiece sceneToCustomPiece(SlingshotCustomPieceKind kind, Vec2 scenePos) const;
    [[nodiscard]] Rect customBuildZoneScene() const;
    [[nodiscard]] Rect editorToolPillRect(int toolIndex) const;
    void recordStageVictory();

    SceneSetup setup_;
    SlingshotConfig config_;
    SceneToPhysics coords_;
    std::unique_ptr<PhysicsWorld> world_;

    float groundY_ = 560.0f;
    float fortAnchorX_ = 720.0f;
    float fortUnit_ = 60.0f;
    Vec2 slingBase_{};
    Vec2 slingAnchor_{};
    Vec2 forkBackTip_{};
    Vec2 forkFrontTip_{};
    float maxPull_ = 96.0f;
    float projRadius_ = 20.0f;
    Rect trajToggleRect_{};
    Rect nextStageRect_{};
    Rect editorToggleRect_{};

    Phase phase_ = Phase::ReadyToAim;
    Vec2 pullPosScene_{};
    float lastStretchSoundDist_ = 0.0f;
    std::size_t currentAmmoIdx_ = 0;
    bool dashUsedThisShot_ = false;
    double flightElapsed_ = 0.0;
    double settleIdleElapsed_ = 0.0;

    int stageIndex_ = 1;
    int score_ = 0;
    int highScore_ = 0;
    int shotsFired_ = 0;
    int lastClearStars_ = 0;
    bool stageCleared_ = false;
    bool stageFailed_ = false;
    bool editing_ = false;
    bool persistenceDirty_ = false;
    int editorTool_ = 0;  // 0=WoodPillar, 1=WoodBeam, 2=StoneBlock, 3=GlassBeam, 4=Defender, 5=Erase
    int draggingPieceIdx_ = -1;
    SlingshotProgress progress_{};
    std::vector<SlingshotCustomPiece> editorPieces_;

    std::vector<AmmoSlot> ammo_;
    std::vector<Block> blocks_;
    std::vector<Defender> defenders_;
    std::vector<std::size_t> parkedIcons_;
    std::vector<Vec2> parkedPositions_;
    std::vector<Debris> debris_;
    std::vector<ShockRing> rings_;
    std::vector<SoundEvent> pendingSounds_;
};

}  // namespace dpp
