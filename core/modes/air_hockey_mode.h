#pragma once
// Desktop Air Hockey: 2:1 rounded arcade rink on wallpaper with zero-tunneling
// swept mallet physics, 3-tier predictive AI, 2P local play, cheering spectator
// desktop icons, procedural clack/goal audio, and normalized cross-platform LAN 1v1.

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/modes/billiards_mode.h"
#include "core/modes/game_mode.h"
#include "core/scene/coordinates.h"

namespace dpp {

enum class AirHockeyMatchMode : std::uint8_t {
    SoloAI = 0,     // Player (Left) vs AI (Right)
    Hotseat2P = 1,  // Left = Mouse/P1, Right = Keyboard WASD/Arrows or right-half touch
    Lan = 2,        // Host = Left (P1), Guest = Right (P2) over normalized TCP 39457
};

enum class AirHockeyDifficulty : std::uint8_t {
    Casual = 0,    // 休闲 AI
    Standard = 1,  // 标准 AI
    Master = 2,    // 大师 AI
};

struct AirHockeyConfig {
    AirHockeyMatchMode matchMode = AirHockeyMatchMode::SoloAI;
    AirHockeyDifficulty difficulty = AirHockeyDifficulty::Standard;
    bool showRinkChrome = true;  // Neon/glass outer rails & goal boxes
    bool showSpectators = true;  // Remaining desktop icons cheer along rink sidelines
    int targetGoals = 7;         // First to N goals wins (5 / 7 / 11)
};

struct AirHockeyRemoteInput {
    Vec2 normTarget{0.82f, 0.50f};  // Guest mallet target normalized to rink [0,1]x[0,1]
    bool requestRematch = false;
};

struct AirHockeyLanState {
    Vec2 puckNorm{0.50f, 0.50f};
    Vec2 puckVelNorm{0.0f, 0.0f};
    Vec2 leftMalletNorm{0.15f, 0.50f};
    Vec2 rightMalletNorm{0.85f, 0.50f};
    std::array<int, 2> scores{0, 0};
    int targetGoals = 7;
    int winner = -1;  // -1 playing, 0 P1 won, 1 P2 won
    float goalFlash = 0.0f;
};

class AirHockeyMode final : public GameMode {
   public:
    explicit AirHockeyMode(const SceneSetup& setup, const AirHockeyConfig& config = {});

    [[nodiscard]] ModeKind kind() const override { return ModeKind::AirHockey; }
    void handleInput(const InputEvent& event) override;
    void stepPhysics(float dt) override;
    void updateRules(float dt) override;
    void buildDrawList(DrawList& out) const override;
    void drainSounds(std::vector<SoundEvent>& out) override;

    [[nodiscard]] bool isIdle() const override;
    [[nodiscard]] ModeStats stats() const override;
    [[nodiscard]] const PhysicsWorld& world() const override { return world_; }

    [[nodiscard]] const AirHockeyConfig& config() const { return config_; }
    void setVisualOptions(bool showRinkChrome, bool showSpectators);
    void setDifficulty(AirHockeyDifficulty diff) { config_.difficulty = diff; }

    // Optional keyboard velocity vector in [-1, 1]^2 for 2P Hotseat right-side mallet.
    void setPlayer2KeyDirection(Vec2 dir);

    // LAN 1v1 helpers (-1 local, 0 LAN Host/Left, 1 LAN Guest/Right).
    void setLanLocalPlayer(int role);
    [[nodiscard]] int lanLocalPlayer() const { return lanLocalPlayer_; }
    [[nodiscard]] bool consumePendingRemoteInput(AirHockeyRemoteInput& out);
    void applyRemoteInput(const AirHockeyRemoteInput& in);
    [[nodiscard]] AirHockeyLanState exportLanState() const;
    void applyLanState(const AirHockeyLanState& st);

    [[nodiscard]] std::array<int, 2> scores() const { return scores_; }
    [[nodiscard]] int winner() const { return winner_; }
    [[nodiscard]] Vec2 rinkMinScene() const { return rinkMin_; }
    [[nodiscard]] Vec2 rinkSizeScene() const { return rinkSize_; }
    [[nodiscard]] Rect chromeToggleRect() const { return chromeToggleRect_; }
    [[nodiscard]] Rect difficultyCycleRect() const { return diffCycleRect_; }

    [[nodiscard]] Vec2 iconCurrentSceneCenter(std::size_t iconIndex) const;
    [[nodiscard]] bool interactiveContains(Vec2 scenePoint) const;

   private:
    void buildRinkWalls();
    void resetFaceOff(int serveTowardPlayer);
    void updateAiTarget(float dt);
    [[nodiscard]] Vec2 clampLeftMalletScene(Vec2 p) const;
    [[nodiscard]] Vec2 clampRightMalletScene(Vec2 p) const;
    [[nodiscard]] Vec2 sceneToRinkNorm(Vec2 scene) const;
    [[nodiscard]] Vec2 rinkNormToScene(Vec2 norm) const;
    void emitSound(SoundKind kind, float gain, Vec2 scenePos, float pitch = 1.0f);

    SceneSetup setup_;
    AirHockeyConfig config_;
    SceneToPhysics coords_;
    PhysicsWorld world_;

    Vec2 rinkMin_{};
    Vec2 rinkSize_{};
    float cornerCut_ = 28.0f;
    float goalHalfH_ = 64.0f;
    float puckRadius_ = 19.0f;
    float malletRadius_ = 27.0f;
    Rect chromeToggleRect_{};
    Rect diffCycleRect_{};

    BodyIndex puckBody_ = 0;
    BodyIndex leftMalletBody_ = 0;
    BodyIndex rightMalletBody_ = 0;

    TextureId puckTex_ = kNoTexture;
    TextureId leftTex_ = kNoTexture;
    TextureId rightTex_ = kNoTexture;

    Vec2 leftTargetScene_{};
    Vec2 rightTargetScene_{};
    Vec2 p2KeyDir_{};

    std::array<int, 2> scores_{0, 0};
    int winner_ = -1;
    float goalPauseTimer_ = 0.0f;
    int nextServe_ = 0;
    float goalCelebration_ = 0.0f;
    float stuckTimer_ = 0.0f;
    float aiServeTimer_ = 0.0f;
    double animTime_ = 0.0;

    int lanLocalPlayer_ = -1;
    bool hasPendingRemoteInput_ = false;
    AirHockeyRemoteInput pendingRemoteInput_{};

    std::array<Vec2, 6> puckTrail_{};
    std::size_t trailHead_ = 0;
    float trailSampleAccum_ = 0.0f;

    std::vector<std::size_t> spectatorIcons_;
    std::vector<Vec2> spectatorBasePos_;
    std::vector<SoundEvent> pendingSounds_;
};

}  // namespace dpp
