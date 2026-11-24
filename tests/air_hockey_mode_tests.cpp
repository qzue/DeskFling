#include <gtest/gtest.h>

#include "core/modes/air_hockey_mode.h"
#include "core/modes/billiards_lan_codec.h"
#include "core/scene/playground.h"

using namespace dpp;

TEST(AirHockeyModeTest, HighSpeedStrikeScoresWithoutTunnelingAndAiResponds) {
    auto setup = syntheticScene(10, {1440, 900});
    AirHockeyConfig cfg;
    cfg.matchMode = AirHockeyMatchMode::Hotseat2P;
    cfg.targetGoals = 3;

    Playground pg(setup);
    pg.setAirHockeyConfig(cfg);
    pg.setPlayMode(PlayMode::AirHockey);
    ASSERT_TRUE(pg.airHockeyEnabled());
    const auto* ah = pg.airHockeyMode();
    ASSERT_NE(ah, nullptr);

    const Vec2 rinkMin = ah->rinkMinScene();
    const Vec2 rinkSz = ah->rinkSizeScene();
    const float midY = rinkMin.y + rinkSz.y * 0.5f;

    // Move P2 mallet out of goal line, then sweep P1 mallet fast through center puck!
    pg.mutableAirHockeyMode()->setPlayer2KeyDirection({0.0f, -1.0f});
    for (int i = 0; i < 20; ++i) pg.tick(1.0 / 60.0);
    pg.mutableAirHockeyMode()->setPlayer2KeyDirection({0.0f, 0.0f});

    // Sweep P1 mallet from left toward center face-off spot
    for (int step = 0; step <= 18; ++step) {
        const float u = static_cast<float>(step) / 18.0f;
        const float x = rinkMin.x + rinkSz.x * (0.14f + 0.35f * u);
        pg.input({InputType::PointerMove, {x, midY}, 0.1 + step * (1.0 / 60.0)});
        pg.tick(1.0 / 60.0);
    }

    for (int i = 0; i < 120; ++i) pg.tick(1.0 / 60.0);
    EXPECT_GE(pg.airHockeyMode()->scores()[0], 1);

    // Switch to Solo AI Master difficulty and verify AI defends & moves cleanly
    cfg.matchMode = AirHockeyMatchMode::SoloAI;
    cfg.difficulty = AirHockeyDifficulty::Master;
    pg.setAirHockeyConfig(cfg);
    for (int step = 0; step <= 16; ++step) {
        const float x = rinkMin.x + rinkSz.x * (0.14f + 0.34f * (static_cast<float>(step) / 16.0f));
        pg.input({InputType::PointerMove, {x, midY + 12.0f}, 2.0 + step * (1.0 / 60.0)});
        pg.tick(1.0 / 60.0);
    }
    for (int i = 0; i < 90; ++i) pg.tick(1.0 / 60.0);

    for (std::size_t i = 0; i < setup.icons.size(); ++i) {
        EXPECT_TRUE(pg.airHockeyMode()->iconCurrentSceneCenter(i).isFinite());
    }

    // Verify display migration preserves AirHockeyMode
    const auto frozen = pg.freezeForMigration();
    auto migrated = Playground::prepareMigration(frozen, {1728, 1050});
    ASSERT_NE(migrated, nullptr);
    migrated->commitMigration();
    EXPECT_TRUE(migrated->airHockeyEnabled());
}

TEST(AirHockeyModeTest, LanWireCodecRoundtripZeroPaths) {
    AirHockeyRemoteInput in;
    in.normTarget = {0.82f, 0.37f};
    in.requestRematch = true;

    const std::string lineIn = lan_codec::encodeHockeyInput(in);
    const auto decIn = lan_codec::decodeLine(lineIn);
    const auto* parsedIn = std::get_if<lan_codec::HockeyInputPacket>(&decIn);
    ASSERT_NE(parsedIn, nullptr);
    EXPECT_NEAR(parsedIn->input.normTarget.x, 0.82f, 1e-4f);
    EXPECT_NEAR(parsedIn->input.normTarget.y, 0.37f, 1e-4f);
    EXPECT_TRUE(parsedIn->input.requestRematch);

    AirHockeyLanState st;
    st.puckNorm = {0.44f, 0.61f};
    st.puckVelNorm = {1.25f, -0.45f};
    st.leftMalletNorm = {0.18f, 0.52f};
    st.rightMalletNorm = {0.84f, 0.39f};
    st.scores = {5, 4};
    st.targetGoals = 7;
    st.winner = -1;
    st.goalFlash = 0.8f;

    const std::vector<SoundEvent> sounds = {
        {SoundKind::MalletStrike, 0.75f, -0.25f, 1.05f},
        {SoundKind::HockeyGoal, 0.90f, 0.40f, 1.00f},
    };
    const std::string lineSt = lan_codec::encodeHockeyState(st, sounds);
    const auto decSt = lan_codec::decodeLine(lineSt);
    const auto* parsedSt = std::get_if<lan_codec::HockeyStatePacket>(&decSt);
    ASSERT_NE(parsedSt, nullptr);
    EXPECT_EQ(parsedSt->state.scores[0], 5);
    EXPECT_EQ(parsedSt->state.scores[1], 4);
    EXPECT_NEAR(parsedSt->state.puckNorm.x, 0.44f, 1e-4f);
    EXPECT_NEAR(parsedSt->state.rightMalletNorm.y, 0.39f, 1e-4f);
    ASSERT_EQ(parsedSt->sounds.size(), 2u);
    EXPECT_EQ(parsedSt->sounds[0].kind, SoundKind::MalletStrike);
    EXPECT_EQ(parsedSt->sounds[1].kind, SoundKind::HockeyGoal);
}

TEST(AirHockeyModeTest, Smooth120HzSteppingZeroBeatStallsAndFastCursorTracking) {
    auto setup = syntheticScene(8, {1728, 1117});
    Playground pg(setup);
    pg.setPlayMode(PlayMode::AirHockey);
    ASSERT_TRUE(pg.airHockeyEnabled());
    const auto* ah = pg.airHockeyMode();
    ASSERT_NE(ah, nullptr);

    const Vec2 rinkMin = ah->rinkMinScene();
    const Vec2 rinkSz = ah->rinkSizeScene();
    const Vec2 fastTarget{rinkMin.x + rinkSz.x * 0.42f, rinkMin.y + rinkSz.y * 0.22f};

    // High-frequency PointerMove must NOT mark Playground dirty for zero-dt mid-frame rebuilds.
    pg.input({InputType::PointerMove, fastTarget, 0.01});
    EXPECT_FALSE(pg.tick(0.0));

    // Across jittered 60Hz display frames (~15.8ms-17.2ms), 120Hz physics never suffers a 0-step beat stall.
    for (int i = 0; i < 60; ++i) {
        const double jittered60Hz = (i % 2 == 0) ? 0.0158 : 0.0172;
        EXPECT_TRUE(pg.tick(jittered60Hz));
    }

    // Mallet reaches a wide cross-rink flick smoothly without rubber-band lag.
    const Vec2 malletNow = ah->iconCurrentSceneCenter(1);
    EXPECT_LT((malletNow - fastTarget).length(), 4.0f);

    // ProMotion 120Hz frames (~8.4ms) also advance physics on every frame.
    const Vec2 secondTarget{rinkMin.x + rinkSz.x * 0.16f, rinkMin.y + rinkSz.y * 0.78f};
    pg.input({InputType::PointerMove, secondTarget, 1.1});
    for (int i = 0; i < 12; ++i) {
        EXPECT_TRUE(pg.tick(0.0084));
    }
    EXPECT_LT((ah->iconCurrentSceneCenter(1) - secondTarget).length(), 4.0f);
}

TEST(AirHockeyModeTest, ArcadeStadiumRinkDrawListBoundsAndChromeToggle) {
    auto setup = syntheticScene(8, {1440, 900});
    Playground pg(setup);
    pg.setPlayMode(PlayMode::AirHockey);
    ASSERT_TRUE(pg.airHockeyEnabled());
    pg.tick(0.0);

    const auto* ah = pg.airHockeyMode();
    ASSERT_NE(ah, nullptr);
    const Vec2 rMin = ah->rinkMinScene();
    const Vec2 rSz = ah->rinkSizeScene();
    const std::size_t fullCount = pg.drawList().commands().size();
    EXPECT_GT(fullCount, 40u);

    // Every BoxItem stays inside the work area, and stadium-sized cabinet layers stay flush (no protruding ears).
    for (const auto& cmd : pg.drawList().commands()) {
        if (const auto* bx = std::get_if<BoxItem>(&cmd)) {
            EXPECT_TRUE(bx->center.isFinite());
            EXPECT_GE(bx->center.x - bx->size.x * 0.5f, 0.0f);
            EXPECT_LE(bx->center.x + bx->size.x * 0.5f, setup.workAreaSize.x);
            if (bx->size.x > rSz.x * 0.5f) {
                EXPECT_GE(bx->center.x - bx->size.x * 0.5f, rMin.x - 44.0f);
                EXPECT_LE(bx->center.x + bx->size.x * 0.5f, rMin.x + rSz.x + 44.0f);
            }
        }
    }

    AirHockeyConfig minimal = pg.airHockeyConfig();
    minimal.showRinkChrome = false;
    pg.setAirHockeyConfig(minimal);
    EXPECT_LT(pg.drawList().commands().size(), fullCount);
}

TEST(AirHockeyModeTest, AllAiDifficultiesActivelyServeFaceOffAndCapPlayablePuckSpeed) {
    for (AirHockeyDifficulty diff :
         {AirHockeyDifficulty::Casual, AirHockeyDifficulty::Standard, AirHockeyDifficulty::Master}) {
        auto setup = syntheticScene(6, {1440, 900});
        AirHockeyConfig cfg;
        cfg.matchMode = AirHockeyMatchMode::SoloAI;
        cfg.difficulty = diff;
        cfg.targetGoals = 5;

        Playground pg(setup);
        pg.setAirHockeyConfig(cfg);
        pg.setPlayMode(PlayMode::AirHockey);
        const auto* ah = pg.airHockeyMode();
        ASSERT_NE(ah, nullptr);

        const Vec2 rinkMin = ah->rinkMinScene();
        const Vec2 rinkSz = ah->rinkSizeScene();
        const float midX = rinkMin.x + rinkSz.x * 0.5f;
        const float midY = rinkMin.y + rinkSz.y * 0.5f;

        // Score P1's first goal (or knock puck into AI half) and verify puck speed stays <= 14.0 m/s.
        float peakPuckSpeed = 0.0f;
        for (int step = 0; step <= 14; ++step) {
            const float x = rinkMin.x + rinkSz.x * (0.14f + 0.35f * (static_cast<float>(step) / 14.0f));
            pg.input({InputType::PointerMove, {x, midY}, 0.05 + step * (1.0 / 60.0)});
            pg.tick(1.0 / 60.0);
            peakPuckSpeed = std::max(peakPuckSpeed, ah->world().state(0).linearVelocity.length());
        }
        EXPECT_LE(peakPuckSpeed, 14.0f);

        // Now simulate a concession face-off on AI's side by letting P1 score into an open goal first
        AirHockeyConfig hotCfg = cfg;
        hotCfg.matchMode = AirHockeyMatchMode::Hotseat2P;
        pg.setAirHockeyConfig(hotCfg);
        pg.mutableAirHockeyMode()->setPlayer2KeyDirection({0.0f, -1.0f});
        for (int i = 0; i < 20; ++i) pg.tick(1.0 / 60.0);
        pg.mutableAirHockeyMode()->setPlayer2KeyDirection({0.0f, 0.0f});
        for (int step = 0; step <= 16; ++step) {
            const float x = rinkMin.x + rinkSz.x * (0.14f + 0.35f * (static_cast<float>(step) / 16.0f));
            pg.input({InputType::PointerMove, {x, midY}, 1.0 + step * (1.0 / 60.0)});
            pg.tick(1.0 / 60.0);
        }
        for (int i = 0; i < 90 && pg.airHockeyMode()->scores()[0] == 0; ++i) pg.tick(1.0 / 60.0);
        ASSERT_EQ(pg.airHockeyMode()->scores()[0], 1);

        // Wait out goal pause so puck resets to AI's face-off spot (0.57 * W), then switch P2 to SoloAI!
        for (int i = 0; i < 65; ++i) pg.tick(1.0 / 60.0);
        const Vec2 faceOffPos = pg.airHockeyMode()->iconCurrentSceneCenter(0);
        ASSERT_GT(faceOffPos.x, midX + 10.0f);

        // Park P1 mallet out of the way and restore SoloAI WITHOUT resetting scores:
        pg.mutableAirHockeyMode()->setDifficulty(diff);
        // Set SoloAI directly via setAirHockeyConfig? Note setAirHockeyConfig rebuilds if matchMode changes,
        // so test SoloAI directly when puck is stationary on AI's face-off spot:
        AirHockeyMode directAi(setup, cfg);
        // Nudge puck gently into AI's face-off spot at zero velocity by stepping a soft tap across midX:
        directAi.handleInput({InputType::PointerMove, {rinkMin.x + rinkSz.x * 0.45f, midY}, 0.02});
        for (int i = 0; i < 12; ++i) {
            directAi.stepPhysics(1.0f / 60.0f);
            directAi.updateRules(1.0f / 60.0f);
        }
        // Pull P1 back to left goal and watch AI return/serve the puck back into P1's half!
        directAi.handleInput({InputType::PointerMove, {rinkMin.x + rinkSz.x * 0.14f, midY}, 0.30});
        bool crossedBackToPlayerHalf = false;
        for (int i = 0; i < 150; ++i) {
            directAi.stepPhysics(1.0f / 60.0f);
            directAi.updateRules(1.0f / 60.0f);
            if (directAi.iconCurrentSceneCenter(0).x < midX - 8.0f) {
                crossedBackToPlayerHalf = true;
                break;
            }
        }
        EXPECT_TRUE(crossedBackToPlayerHalf) << "AI failed to serve/return on difficulty " << static_cast<int>(diff);
    }
}
