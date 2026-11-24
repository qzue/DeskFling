#include <gtest/gtest.h>

#include "core/modes/slingshot_mode.h"
#include "core/scene/playground.h"
#include "core/scene/user_prefs.h"

using namespace dpp;

TEST(SlingshotModeTest, FortressStableBeforeShotAndLaunchesWithDamageAndDash) {
    auto setup = syntheticScene(12, {1440, 900});
    Playground pg(setup);
    pg.setPlayMode(PlayMode::Slingshot);
    ASSERT_TRUE(pg.slingshotEnabled());
    ASSERT_NE(pg.slingshotMode(), nullptr);

    // Fortress starts undisturbed with 0 score and full ammo
    for (int i = 0; i < 30; ++i) pg.tick(1.0 / 60.0);
    EXPECT_EQ(pg.slingshotMode()->score(), 0);
    const int initialDefenders = pg.slingshotMode()->remainingDefenders();
    const int initialAmmo = pg.slingshotMode()->remainingAmmo();
    EXPECT_GE(initialDefenders, 4);
    EXPECT_EQ(initialAmmo, 4);

    // Pull back slingshot toward bottom-left and launch toward fortress
    const Vec2 anchor = pg.slingshotMode()->pouchRestScene();
    const float pull = pg.slingshotMode()->maxPullScene() * 0.92f;
    pg.input({InputType::PointerDown, anchor, 0.1});
    pg.input({InputType::PointerMove, {anchor.x - pull * 0.88f, anchor.y + pull * 0.36f}, 0.2});
    pg.tick(0.0);

    // Trajectory dots, BoxItem planks, and PolygonItem sculpted slingshot/bevels must appear in DrawList
    bool sawBox = false;
    bool sawPoly = false;
    for (const auto& cmd : pg.drawList().commands()) {
        if (std::holds_alternative<BoxItem>(cmd)) sawBox = true;
        if (std::holds_alternative<PolygonItem>(cmd)) sawPoly = true;
    }
    EXPECT_TRUE(sawBox);
    EXPECT_TRUE(sawPoly);

    pg.input({InputType::PointerUp, {anchor.x - pull * 0.88f, anchor.y + pull * 0.36f}, 0.3});
    EXPECT_EQ(pg.slingshotMode()->remainingAmmo(), initialAmmo - 1);

    std::vector<SoundEvent> sounds;
    pg.drainSounds(sounds);
    EXPECT_FALSE(sounds.empty());

    // Step 12 frames, trigger mid-flight kinetic dash, and simulate impact
    for (int i = 0; i < 12; ++i) pg.tick(1.0 / 60.0);
    pg.input({InputType::PointerDown, {720.0f, 400.0f}, 0.55});
    for (int i = 0; i < 240; ++i) pg.tick(1.0 / 60.0);

    EXPECT_GT(pg.slingshotMode()->score(), 0);

    // Every desktop icon reports a finite in-bounds center and survives return to Gravity
    for (std::size_t i = 0; i < setup.icons.size(); ++i) {
        const Vec2 c = pg.slingshotMode()->iconCurrentSceneCenter(i);
        EXPECT_TRUE(c.isFinite());
    }
    pg.setPlayMode(PlayMode::Gravity);
    EXPECT_EQ(pg.playMode(), PlayMode::Gravity);
}

TEST(SlingshotModeTest, PresetsIconCountsStageAdvanceAndDisplayMigration) {
    for (std::size_t count : {std::size_t{1}, std::size_t{6}, std::size_t{40}}) {
        auto setup = syntheticScene(count, {1280, 800});
        for (int p = 0; p <= 4; ++p) {
            SlingshotConfig cfg;
            cfg.preset = static_cast<SlingshotPreset>(p);
            cfg.iconSelection = (p % 2 == 0) ? IconSelectionPolicy::FirstN : IconSelectionPolicy::Random;
            SlingshotMode mode(setup, cfg);
            EXPECT_GT(mode.remainingDefenders(), 0);
            DrawList dl;
            mode.buildDrawList(dl);
            EXPECT_GT(dl.size(), 5u);
        }
    }

    Playground pg(syntheticScene(10, {1600, 960}));
    pg.setPlayMode(PlayMode::Slingshot);
    const Rect nextBtn = pg.slingshotMode()->nextStageButtonRect();
    pg.input({InputType::PointerDown, {nextBtn.x + 10.0f, nextBtn.y + 10.0f}, 1.0});
    EXPECT_EQ(pg.slingshotMode()->stageIndex(), 2);

    const auto frozen = pg.freezeForMigration();
    auto migrated = Playground::prepareMigration(frozen, {1920, 1080});
    ASSERT_NE(migrated, nullptr);
    migrated->commitMigration();
    EXPECT_TRUE(migrated->slingshotEnabled());
    EXPECT_EQ(migrated->slingshotMode()->stageIndex(), 2);
}

TEST(SlingshotModeTest, StarRatingsProgressSerializationAndCustomBuilderEditor) {
    // 1. Progress stars & unlock tracking + encode/decode + UserPreferences INI v2 round-trip.
    SlingshotProgress prog;
    prog.highScore = 12850;
    prog.maxUnlockedStage = 4;
    prog.recordClear(SlingshotPreset::TwinTowers, 1, 3, 4200);
    prog.recordClear(SlingshotPreset::GlassBridge, 3, 2, 12850);
    prog.recordClear(SlingshotPreset::CustomBuilder, 1, 3, 8800);
    EXPECT_EQ(prog.totalStars(), 5);
    EXPECT_EQ(prog.starsForStage(1), 3);
    EXPECT_EQ(prog.starsForStage(3), 2);
    EXPECT_EQ(prog.starsForPreset(SlingshotPreset::CustomBuilder), 3);

    const std::string encodedProg = encodeSlingshotProgress(prog);
    const SlingshotProgress decodedProg = decodeSlingshotProgress(encodedProg);
    EXPECT_EQ(decodedProg.highScore, 12850);
    EXPECT_GE(decodedProg.maxUnlockedStage, 4);
    EXPECT_EQ(decodedProg.presetBestStars[0], 3);
    EXPECT_EQ(decodedProg.presetBestStars[2], 2);
    EXPECT_EQ(decodedProg.presetBestStars[5], 3);
    EXPECT_EQ(decodedProg.stageStars[0], 3);
    EXPECT_EQ(decodedProg.stageStars[2], 2);

    UserPreferences prefs;
    prefs.lastPlayMode = PlayMode::Slingshot;
    prefs.slingshotPreset = SlingshotPreset::CustomBuilder;
    prefs.slingshotProgress = prog;
    prefs.slingshotHighScore = prog.highScore;
    prefs.slingshotCustomPieces = {
        {SlingshotCustomPieceKind::StoneBlock, -1.25f, 0.85f},
        {SlingshotCustomPieceKind::WoodBeam, -0.50f, 1.90f},
        {SlingshotCustomPieceKind::DefenderSlot, -0.50f, 2.55f},
    };
    const UserPreferences loaded = UserPreferences::parseIni(prefs.serializeIni(), /*forColdBoot=*/false);
    EXPECT_EQ(loaded.slingshotPreset, SlingshotPreset::CustomBuilder);
    EXPECT_EQ(loaded.slingshotProgress.highScore, 12850);
    EXPECT_EQ(loaded.slingshotProgress.presetBestStars[5], 3);
    ASSERT_EQ(loaded.slingshotCustomPieces.size(), 3u);
    EXPECT_EQ(loaded.slingshotCustomPieces[0].kind, SlingshotCustomPieceKind::StoneBlock);
    EXPECT_NEAR(loaded.slingshotCustomPieces[0].normX, -1.25f, 1e-2f);
    EXPECT_EQ(loaded.slingshotCustomPieces[2].kind, SlingshotCustomPieceKind::DefenderSlot);

    // 2. Interactive in-canvas CustomBuilder editor: toggle on, place pieces, drag, alt-erase, battle!
    Playground pg(syntheticScene(10, {1440, 900}));
    pg.setSlingshotProgress(prog);
    pg.setPlayMode(PlayMode::Slingshot);
    auto* sm = pg.mutableSlingshotMode();
    ASSERT_NE(sm, nullptr);
    EXPECT_FALSE(sm->editing());

    // Click `[编辑关卡]` HUD pill -> switches to CustomBuilder and opens blueprint editor!
    const Rect editBtn = sm->editorToggleRect();
    pg.input({InputType::PointerDown, {editBtn.x + 12.0f, editBtn.y + 10.0f}, 1.0});
    pg.input({InputType::PointerUp, {editBtn.x + 12.0f, editBtn.y + 10.0f}, 1.05});
    EXPECT_TRUE(sm->editing());
    EXPECT_EQ(sm->config().preset, SlingshotPreset::CustomBuilder);

    const std::size_t beforePieces = sm->config().customPieces.size();
    const Vec2 buildOrigin = sm->customBuildAnchorScene();
    const float u = sm->customBuildUnitScene();
    const Vec2 newSpot{buildOrigin.x + 1.8f * u, buildOrigin.y - 1.2f * u};
    EXPECT_TRUE(sm->interactiveContains(newSpot));

    // Click empty blueprint canvas -> places a new piece and marks persistence dirty!
    pg.input({InputType::PointerDown, newSpot, 1.2});
    pg.input({InputType::PointerUp, newSpot, 1.25});
    EXPECT_EQ(sm->config().customPieces.size(), beforePieces + 1u);
    EXPECT_TRUE(pg.takeSlingshotPersistenceDirty());

    // Drag the newly placed piece upward -> updates its blueprint height!
    const float hBefore = sm->config().customPieces.back().normHeight;
    pg.input({InputType::PointerDown, newSpot, 1.4});
    pg.input({InputType::PointerMove, {newSpot.x, newSpot.y - 1.5f * u}, 1.5});
    pg.input({InputType::PointerUp, {newSpot.x, newSpot.y - 1.5f * u}, 1.55});
    EXPECT_GT(sm->config().customPieces.back().normHeight, hBefore + 0.5f);

    // Option/Alt-click the dragged piece -> deletes it cleanly!
    const Vec2 draggedSpot{buildOrigin.x + sm->config().customPieces.back().normX * u,
                           buildOrigin.y - sm->config().customPieces.back().normHeight * u};
    pg.input({InputType::PointerDown, draggedSpot, 1.7, kNoTexture, /*alternate=*/true});
    pg.input({InputType::PointerUp, draggedSpot, 1.75, kNoTexture, /*alternate=*/true});
    EXPECT_EQ(sm->config().customPieces.size(), beforePieces);

    // Click `[完成开战 ✓]` -> exits editor and spawns live Box2D fortress from custom blueprint!
    pg.input({InputType::PointerDown, {editBtn.x + 12.0f, editBtn.y + 10.0f}, 2.0});
    pg.input({InputType::PointerUp, {editBtn.x + 12.0f, editBtn.y + 10.0f}, 2.05});
    EXPECT_FALSE(sm->editing());
    EXPECT_GT(sm->remainingDefenders(), 0);
}

TEST(SlingshotMode, SwitchingFromCustomWorkshopBackToPyramidAndBuiltinPresetsRebuildsFortress) {
    Playground pg(syntheticScene(16, {1280, 760}));
    pg.setPlayMode(PlayMode::Slingshot);
    auto* sm = pg.mutableSlingshotMode();
    ASSERT_NE(sm, nullptr);

    // 1. Enter custom workshop, click Clear (tool index 6 = 清空), and click Finish -> 1 fallback defender, 0 blocks.
    const Rect editBtn = sm->editorToggleRect();
    pg.input({InputType::PointerDown, {editBtn.x + 12.0f, editBtn.y + 10.0f}, 0.5});
    pg.input({InputType::PointerUp, {editBtn.x + 12.0f, editBtn.y + 10.0f}, 0.55});
    ASSERT_TRUE(sm->editing());
    const Vec2 clearToolCenter{356.0f + 6.0f * 58.0f + 27.0f, 32.0f};
    pg.input({InputType::PointerDown, clearToolCenter, 0.7});
    pg.input({InputType::PointerUp, clearToolCenter, 0.75});
    EXPECT_TRUE(sm->config().customPieces.empty());

    pg.input({InputType::PointerDown, {editBtn.x + 12.0f, editBtn.y + 10.0f}, 0.9});
    pg.input({InputType::PointerUp, {editBtn.x + 12.0f, editBtn.y + 10.0f}, 0.95});
    sm = pg.mutableSlingshotMode();
    ASSERT_NE(sm, nullptr);
    EXPECT_FALSE(sm->editing());
    EXPECT_EQ(sm->config().preset, SlingshotPreset::CustomBuilder);
    EXPECT_EQ(sm->remainingDefenders(), 1);
    const int customBodies = sm->stats().bodies;

    // 2. Switch from CustomBuilder to built-in PyramidFort -> must NOT stay on CustomBuilder!
    SlingshotConfig pyramidCfg = pg.slingshotConfig();
    pyramidCfg.preset = SlingshotPreset::PyramidFort;
    pg.setSlingshotConfig(pyramidCfg);

    sm = pg.mutableSlingshotMode();
    ASSERT_NE(sm, nullptr);
    EXPECT_FALSE(sm->editing());
    EXPECT_EQ(pg.slingshotConfig().preset, SlingshotPreset::PyramidFort);
    EXPECT_EQ(sm->config().preset, SlingshotPreset::PyramidFort);
    EXPECT_GE(sm->remainingDefenders(), 6);
    EXPECT_GT(sm->stats().bodies, customBodies + 10);

    // 3. Also verify switching directly while editor is open exits editor and builds GlassBridge!
    pg.input({InputType::PointerDown, {editBtn.x + 12.0f, editBtn.y + 10.0f}, 1.2});
    pg.input({InputType::PointerUp, {editBtn.x + 12.0f, editBtn.y + 10.0f}, 1.25});
    ASSERT_TRUE(pg.mutableSlingshotMode()->editing());

    SlingshotConfig bridgeCfg = pg.slingshotConfig();
    bridgeCfg.preset = SlingshotPreset::GlassBridge;
    bridgeCfg.startInEditor = false;
    pg.setSlingshotConfig(bridgeCfg);

    sm = pg.mutableSlingshotMode();
    ASSERT_NE(sm, nullptr);
    EXPECT_FALSE(sm->editing());
    EXPECT_EQ(sm->config().preset, SlingshotPreset::GlassBridge);
    EXPECT_GT(sm->stats().bodies, customBodies + 8);

    // 4. Switching back to CustomBuilder restores the saved custom layout.
    SlingshotConfig backCustom = pg.slingshotConfig();
    backCustom.preset = SlingshotPreset::CustomBuilder;
    pg.setSlingshotConfig(backCustom);
    sm = pg.mutableSlingshotMode();
    ASSERT_NE(sm, nullptr);
    EXPECT_EQ(sm->config().preset, SlingshotPreset::CustomBuilder);
    EXPECT_EQ(sm->remainingDefenders(), 1);
}

TEST(SlingshotMode, AllBuiltinPresetsRemainStructurallyStableWithoutShootingAcrossIconCounts) {
    constexpr SlingshotPreset kBuiltins[] = {
        SlingshotPreset::TwinTowers,   SlingshotPreset::PyramidFort, SlingshotPreset::GlassBridge,
        SlingshotPreset::StoneCitadel, SlingshotPreset::RandomSiege,
    };
    for (std::size_t iconCount : {std::size_t{8}, std::size_t{16}, std::size_t{24}}) {
        for (SlingshotPreset preset : kBuiltins) {
            SlingshotConfig cfg;
            cfg.preset = preset;
            cfg.randomSeed = 0x20261123u;
            SlingshotMode mode(syntheticScene(iconCount, {1440, 900}), cfg);

            const int initialDefenders = mode.remainingDefenders();
            ASSERT_GT(initialDefenders, 0);

            for (int step = 0; step < 300; ++step) {
                mode.stepPhysics(1.0f / 60.0f);
                mode.updateRules(1.0f / 60.0f);
            }

            // Zero blocks shattered, zero defenders knocked out, and fortress remains standing!
            EXPECT_EQ(mode.score(), 0) << "Preset " << static_cast<int>(preset) << " icons=" << iconCount;
            EXPECT_EQ(mode.remainingDefenders(), initialDefenders)
                << "Preset " << static_cast<int>(preset) << " icons=" << iconCount;
            EXPECT_FALSE(mode.stageCleared());
            EXPECT_FALSE(mode.stageFailed());

            for (std::size_t b = 0; b < mode.world().bodyCount(); ++b) {
                const auto st = mode.world().state(static_cast<BodyIndex>(b));
                if (!st.enabled) continue;
                EXPECT_TRUE(st.position.isFinite());
                EXPECT_LT(st.linearVelocity.length(), 0.25f)
                    << "Body " << b << " still moving in preset " << static_cast<int>(preset);
            }
        }
    }
}

TEST(SlingshotMode, GentleProjectileTouchDefeatsExposedDefenderImmediately) {
    auto setup = syntheticScene(8, {1440, 900});
    SlingshotConfig cfg;
    cfg.preset = SlingshotPreset::CustomBuilder;
    // Place a single exposed defender right on the ground with zero blocks in front
    cfg.customPieces = {{SlingshotCustomPieceKind::DefenderSlot, -2.2f, 0.35f}};

    SlingshotMode mode(setup, cfg);
    ASSERT_EQ(mode.remainingDefenders(), 1);

    // Fire a gentle low shot so the projectile rolls along the ground and lightly touches the defender
    const Vec2 anchor = mode.pouchRestScene();
    const float pull = mode.maxPullScene() * 0.48f;
    mode.handleInput({InputType::PointerDown, anchor, 0.1});
    mode.handleInput({InputType::PointerMove, {anchor.x - pull, anchor.y + pull * 0.08f}, 0.2});
    mode.handleInput({InputType::PointerUp, {anchor.x - pull, anchor.y + pull * 0.08f}, 0.3});

    for (int step = 0; step < 180 && mode.remainingDefenders() > 0; ++step) {
        mode.stepPhysics(1.0f / 60.0f);
        mode.updateRules(1.0f / 60.0f);
    }
    EXPECT_EQ(mode.remainingDefenders(), 0);
    EXPECT_TRUE(mode.stageCleared());
}
