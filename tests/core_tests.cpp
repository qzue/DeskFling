#include <gtest/gtest.h>

#include <limits>

#include "core/modes/billiards_mode.h"
#include "core/modes/gravity_mode.h"
#include "core/scene/playground.h"
#include "core/scene/user_prefs.h"
#include "platforms/mirror_hit.h"
#include "platforms/windows/direct_input.h"
using namespace dpp;
TEST(Coordinates, RoundTripAndOrigins) {
    SceneToPhysics c({1920, 1080});
    auto p = c.toScene(c.toPhysics({250, 300}));
    EXPECT_NEAR(p.x, 250, 1e-4);
    EXPECT_NEAR(p.y, 300, 1e-4);
    EXPECT_FLOAT_EQ(c.toPhysics({0, 1080}).y, 0);
    PixelRect wa{-1920, 50, 0, 1080};
    auto w = windowsScreenPixelsToScene(-1720, 250, wa, 2);
    EXPECT_EQ(w, (Vec2{100, 100}));
    EXPECT_EQ(windowsSceneToScreenPixels(w, wa, 2), (Vec2{-1720, 250}));
    Rect r{-500, 40, 1000, 700};
    auto m = macGlobalPointsToScene({0, 600}, r);
    EXPECT_EQ(macSceneToGlobalPoints(m, r), (Vec2{0, 600}));
    EXPECT_EQ(macSceneToBackingPixels(m, 2), m * 2);
    EXPECT_EQ(rectCenter(rectFromCenter({50, 60}, {32, 40})), (Vec2{50, 60}));
}
TEST(Clock, InvalidDeltaAndBoundedCatchup) {
    FixedStepClock c;
    EXPECT_EQ(c.advance(-1), 0);
    EXPECT_EQ(c.advance(std::numeric_limits<double>::infinity()), 0);
    EXPECT_EQ(c.advance(std::numeric_limits<double>::quiet_NaN()), 0);
    EXPECT_EQ(c.advance(60), 4);
    EXPECT_GT(c.droppedSteps(), 0u);
    c.reset();
    EXPECT_EQ(c.advance(1.0 / 120), 0);
    EXPECT_EQ(c.advance(1.0 / 120), 1);
}
TEST(Gravity, FallsAndSettlesWithinBoundary) {
    GravityMode m(syntheticScene(1));
    const auto initial = m.world().state(0).position;
    for (int i = 0; i < 30; ++i) m.stepPhysics(1.f / 60);
    EXPECT_LT(m.world().state(0).position.y, initial.y);
    for (int i = 0; i < 1800; ++i) m.stepPhysics(1.f / 60);
    const auto s = m.world().state(0);
    EXPECT_GT(s.position.y, 0.15f);
    EXPECT_LT(s.position.y, 0.3f);
    EXPECT_LT(s.linearVelocity.length(), 0.02f);
    EXPECT_TRUE(m.isIdle());
}
TEST(Gravity, DenseStackFiniteAndStable) {
    auto setup = syntheticScene(50);
    for (std::size_t i = 0; i < setup.icons.size(); ++i)
        setup.icons[i].center = {400 + static_cast<float>(i % 5) * 45, 50 + static_cast<float>(i / 5) * 45};
    GravityMode m(setup);
    for (int i = 0; i < 1800; ++i) m.stepPhysics(1.f / 60);
    for (std::size_t i = 0; i < 50; ++i) {
        auto s = m.world().state(static_cast<BodyIndex>(i));
        EXPECT_TRUE(s.position.isFinite());
        EXPECT_TRUE(s.linearVelocity.isFinite());
        EXPECT_TRUE(std::isfinite(s.angle));
        EXPECT_GT(s.position.y, 0);
        EXPECT_LT(s.position.y, 7);
        EXPECT_LT(s.linearVelocity.length(), .1f);
    }
}
TEST(Gravity, GrabConstraintThrowClampAndCancel) {
    GravityMode m(syntheticScene(1));
    auto p = m.bodySceneCenter(0);
    m.handleInput({InputType::PointerDown, p, 1});
    ASSERT_TRUE(m.stats().grabbing);
    m.handleInput({InputType::PointerMove, p + Vec2{100, 0}, 1.05});
    m.stepPhysics(1.f / 60);
    EXPECT_GT(m.world().state(0).linearVelocity.x, 0);
    m.handleInput({InputType::PointerUp, p + Vec2{200, 0}, 1.1});
    EXPECT_FALSE(m.stats().grabbing);
    EXPECT_LE(m.world().state(0).linearVelocity.length(), 25.001f);
    p = m.bodySceneCenter(0);
    m.handleInput({InputType::PointerDown, p, 2});
    m.handleInput({InputType::PointerCancel, {}, 2});
    EXPECT_FALSE(m.stats().grabbing);
}
TEST(Input, VelocityAndStaleHistory) {
    PointerVelocityTracker t;
    t.addSample({0, 0}, 1);
    t.addSample({100, 0}, 1.05);
    EXPECT_NEAR(t.velocity(1.05).x, 2000, .1);
    EXPECT_EQ(t.velocity(1.2), Vec2{});
    t.addSample({3, 4}, .5);
    EXPECT_EQ(t.sampleCount(), 1u);
}
TEST(Playground, PauseResetAndTwentyModeSwitches) {
    Playground p(syntheticScene(50));
    p.tick(1.0 / 60);
    p.input({InputType::TogglePause, {}, 0});
    const auto pos = p.mode().world().state(0).position;
    for (int i = 0; i < 100; ++i) p.tick(1);
    EXPECT_EQ(p.mode().world().state(0).position, pos);
    p.input({InputType::TogglePause, {}, 0});
    p.tick(1.0 / 60);
    EXPECT_NE(p.mode().world().state(0).position, pos);
    for (int i = 0; i < 20; ++i) {
        p.switchMode(ModeKind::Billiards);
        EXPECT_EQ(p.mode().stats().bodies, 51);
        p.switchMode(ModeKind::Gravity);
        EXPECT_EQ(p.mode().stats().bodies, 50);
    }
    p.input({InputType::Reset, {}, 0});
    EXPECT_FALSE(p.mode().stats().grabbing);
    p.input({InputType::RequestExit, {}, 0});
    EXPECT_TRUE(p.exitRequested());
}
TEST(Billiards, CountsPocketOnceAndRespawnsWhite) {
    BilliardsMode m(syntheticScene(5));
    m.debugPlaceBall(0, m.pocketScene(0), {});
    m.updateRules(1.f / 60);
    m.updateRules(1.f / 60);
    EXPECT_EQ(m.stats().pocketed, 1);
    m.debugPlaceWhite(m.pocketScene(1), {1, 0});
    m.updateRules(1.f / 60);
    EXPECT_FALSE(m.whitePocketed());
    EXPECT_TRUE(m.world().state(0).enabled);
    EXPECT_GT((m.whiteSceneCenter() - m.pocketScene(1)).length(), m.ballRadiusScene());
}
TEST(Billiards, ChargeCancelAndShot) {
    BilliardsMode m(syntheticScene(10));
    m.handleInput({InputType::PointerDown, {800, 350}, 1});
    EXPECT_EQ(m.state(), BilliardsState::Charging);
    m.handleInput({InputType::PointerCancel, {}, 1.2});
    EXPECT_EQ(m.stats().shots, 0);
    m.handleInput({InputType::PointerDown, {800, 350}, 2});
    m.handleInput({InputType::PointerUp, {800, 350}, 3});
    EXPECT_EQ(m.stats().shots, 1);
    EXPECT_GT(m.world().state(0).linearVelocity.length(), 10);
    for (int i = 0; i < 1800; ++i) {
        m.stepPhysics(1.f / 60);
        m.updateRules(1.f / 60);
    }
    EXPECT_NE(m.state(), BilliardsState::Rolling);
}
TEST(Validation, RejectsUnsafeConfigurationAndInvalidDelta) {
    EXPECT_THROW(FixedStepClock(FixedStepConfig{std::numeric_limits<double>::infinity(), 4, .25}),
                 std::invalid_argument);
    EXPECT_THROW(FixedStepClock(FixedStepConfig{1e-300, 4, .25}), std::invalid_argument);
    EXPECT_THROW(SceneToPhysics(Vec2{0, 700}), std::invalid_argument);
    PhysicsConfig cfg;
    cfg.subSteps = 0;
    EXPECT_THROW(PhysicsWorld{cfg}, std::invalid_argument);
    GravityMode m(syntheticScene(1));
    auto before = m.world().state(0).position;
    m.stepPhysics(std::numeric_limits<float>::quiet_NaN());
    m.stepPhysics(1000);
    EXPECT_EQ(m.world().state(0).position, before);
    m.handleInput({InputType::PointerDown, {std::numeric_limits<float>::quiet_NaN(), 0}, 0});
    EXPECT_FALSE(m.stats().grabbing);
    auto scene = syntheticScene(1);
    scene.icons[0].iconSize.x = std::numeric_limits<float>::infinity();
    EXPECT_THROW(GravityMode{scene}, std::invalid_argument);
}
TEST(Billiards, ChargingVisualAdvancesWithoutMouseMovement) {
    BilliardsMode m(syntheticScene(10));
    m.handleInput({InputType::PointerDown, {800, 350}, 1});
    DrawList before, after;
    m.buildDrawList(before);
    for (int i = 0; i < 30; ++i) m.updateRules(1.f / 60);
    m.buildDrawList(after);
    const auto& a = std::get<LineItem>(before.commands()[before.size() - 2]);
    const auto& b = std::get<LineItem>(after.commands()[after.size() - 2]);
    EXPECT_GT((a.a - b.a).length(), 10);
}
TEST(Playground, PausingCancelsGrabAndDoesNotReplayPausedTime) {
    Playground p(syntheticScene(1));
    auto pos = std::get<SpriteItem>((p.tick(0), p.drawList().commands()[0])).center;
    p.input({InputType::PointerDown, pos, 1});
    ASSERT_TRUE(p.mode().stats().grabbing);
    p.input({InputType::TogglePause, {}, 1.1});
    EXPECT_FALSE(p.mode().stats().grabbing);
    p.tick(1000);
    p.input({InputType::TogglePause, {}, 1001});
    auto before = p.mode().world().state(0).position;
    p.tick(1.0 / 60);
    EXPECT_LT((p.mode().world().state(0).position - before).length(), .01f);
}
TEST(Billiards, UnsafeWhiteRespawnStaysDisabled) {
    BilliardsMode m(syntheticScene(5));
    // Artificially block every candidate. Rules must never enable an overlapping cue ball.
    m.mutableWorld().addBox({5.5f, 3.5f}, {6, 4}, 0, BodyMaterial{});
    m.debugPlaceWhite(m.pocketScene(1), {1, 0});
    m.updateRules(1.f / 60);
    EXPECT_TRUE(m.whitePocketed());
    EXPECT_FALSE(m.world().state(0).enabled);
    EXPECT_EQ(m.state(), BilliardsState::Rolling);
}
TEST(Playground, CallbackIntervalsExcludeIdleWakeResetAndPauseTransitions) {
    Playground p(syntheticScene(1));
    p.tick(1.0 / 60);
    EXPECT_EQ(p.perf.interval.count(), 0u);
    p.tick(1.0 / 60);
    EXPECT_EQ(p.perf.interval.count(), 1u);
    p.reset();
    p.tick(.2);  // The native timer can wake from 5 Hz idle scheduling.
    EXPECT_EQ(p.perf.interval.count(), 1u);
    p.tick(1.0 / 60);
    EXPECT_EQ(p.perf.interval.count(), 2u);
    p.input({InputType::TogglePause, {}, 1});
    p.tick(.2);
    p.input({InputType::TogglePause, {}, 2});
    p.tick(.2);
    EXPECT_EQ(p.perf.interval.count(), 2u);
    for (int i = 0; i < 1000 && !p.idle(); ++i) p.tick(1.0 / 60);
    ASSERT_TRUE(p.idle());
    const auto count = p.perf.interval.count();
    p.tick(.2);
    auto pos = SceneToPhysics({1100, 700}).toScene(p.mode().world().state(0).position);
    p.input({InputType::PointerDown, pos, 3});
    ASSERT_TRUE(p.mode().stats().grabbing);
    p.tick(.2);
    EXPECT_EQ(p.perf.interval.count(), count);
    EXPECT_EQ(p.perf.stalls, 0u);
}

TEST(Coordinates, ShellIconPixelSizeDoesNotGrowWithDpi) {
    for (unsigned dpi : {96u, 120u, 144u, 192u}) {
        const float scale = windowsDpiScale(dpi);
        const auto r = windowsIconImageRect({-20, 2}, {64, 64}, 48, scale);
        EXPECT_FLOAT_EQ(r.x, -20);
        EXPECT_FLOAT_EQ(r.y, 2);
        EXPECT_FLOAT_EQ(r.width * scale, 48);
        EXPECT_FLOAT_EQ(r.height * scale, 48);
        const auto shifted = windowsIconImageRect({-20, 2}, {64, 64}, 48, scale, 4);
        EXPECT_FLOAT_EQ(shifted.x, r.x);
        EXPECT_FLOAT_EQ(shifted.width, r.width);
        EXPECT_FLOAT_EQ(shifted.height, r.height);
        EXPECT_NEAR((shifted.y - r.y) * scale, 4, 0.0001f);
        const auto up = windowsIconImageRect({-20, 2}, {64, 64}, 48, scale, -4);
        EXPECT_NEAR((up.y - r.y) * scale, -4, 0.0001f);
    }
}
TEST(Coordinates, IconFitPreservesAspectAndRefusesUnknownGeometry) {
    auto r = windowsIconImageRect({0, 0}, {128, 64}, 48, 1.25f);
    EXPECT_FLOAT_EQ(r.width * 1.25f, 48);
    EXPECT_FLOAT_EQ(r.height * 1.25f, 24);
    EXPECT_FLOAT_EQ(r.y * 1.25f, 12);
    r = windowsIconImageRect({0, 0}, {16, 16}, 48, 1.25f);
    EXPECT_FLOAT_EQ(r.width * 1.25f, 16);
    EXPECT_FLOAT_EQ(r.x * 1.25f, 16);
    EXPECT_THROW((void)windowsIconImageRect({0, 0}, {64, 64}, 48, 1, 17), std::invalid_argument);
    EXPECT_THROW((void)windowsIconImageRect({0, 0}, {64, 64}, 0, 1), std::invalid_argument);
    EXPECT_THROW((void)windowsIconImageRect({0, 0}, {0, 64}, 48, 1), std::invalid_argument);
    EXPECT_THROW((void)windowsIconImageRect({0, 0}, {64, 64}, 48, 0), std::invalid_argument);
    EXPECT_THROW((void)windowsIconImageRect({0, 0}, {64, 64}, 48, std::numeric_limits<float>::quiet_NaN()),
                 std::invalid_argument);
}

TEST(Coordinates, StartupDpiMatrixAndInvalidGeometry) {
    for (unsigned dpi : {96u, 120u, 144u, 192u}) {
        const float scale = windowsDpiScale(dpi);
        const PixelRect work{-1600, 40, 320, 1040};
        for (Vec2 scene : {Vec2{0, 0}, Vec2{80, 160}, Vec2{400, 200}}) {
            const auto pixel = windowsSceneToScreenPixels(scene, work, scale);
            EXPECT_EQ(
                windowsScreenPixelsToScene(static_cast<int>(pixel.x), static_cast<int>(pixel.y), work, scale),
                scene);
            const auto image = windowsIconImageRect(scene, {64, 64}, 64, scale);
            EXPECT_FLOAT_EQ(image.width * scale, 64);
            EXPECT_EQ(rectCenter(image), (scene + Vec2{32 / scale, 32 / scale}));
        }
        EXPECT_EQ(windowsWorkAreaSceneSize(work, scale), (Vec2{1920 / scale, 1000 / scale}));
    }
    EXPECT_THROW((void)windowsDpiScale(0), std::invalid_argument);
    EXPECT_THROW((void)windowsWorkAreaSceneSize({0, 0, 0, 10}, 1), std::invalid_argument);
    EXPECT_THROW((void)windowsScreenPixelsToScene(0, 0, {0, 0, 10, 10}, 0), std::invalid_argument);
    EXPECT_THROW((void)macSceneToBackingPixels({10, 20}, 0), std::invalid_argument);
    for (float scale : {1.f, 1.5f, 2.f}) {
        Rect work{-400, 30, 1000, 600};
        Vec2 scene{80, 120};
        EXPECT_EQ(macGlobalPointsToScene(macSceneToGlobalPoints(scene, work), work), scene);
        EXPECT_EQ(macSceneToBackingPixels(scene, scale), scene * scale);
    }
}

TEST(Gravity, CapturedVisualHitUsesTopmostTextureAndNeverRequeriesPhysics) {
    auto setup = syntheticScene(2);
    setup.icons[1].center = setup.icons[0].center;
    GravityMode mode(setup);
    DrawList draw;
    mode.buildDrawList(draw);
    const auto hit = mirrorHit(draw, setup.icons[0].center);
    ASSERT_EQ(hit, setup.icons[1].texture);
    mode.handleInput({InputType::PointerDown, setup.icons[0].center, 1, hit});
    ASSERT_TRUE(mode.world().grabbedBody());
    EXPECT_EQ(*mode.world().grabbedBody(), 1u);
    mode.handleInput({InputType::PointerCancel, {}, 2});
    mode.handleInput({InputType::PointerDown, setup.icons[0].center, 3, TextureId{99999}});
    EXPECT_FALSE(mode.stats().grabbing);
}

TEST(DirectInput, ExplicitDisableSurvivesFocusReleaseAndOnlyManualEnableRestores) {
    winmirror::DirectInput input;
    ASSERT_TRUE(input.claim(TextureId{2}, false, true, 100));
    input.cancel();
    EXPECT_TRUE(input.claim(TextureId{2}, false, true, 101));
    input.disable();
    input.cancel();
    EXPECT_FALSE(input.claim(TextureId{2}, false, true, 102));
    EXPECT_FALSE(input.claim(TextureId{2}, true, true, 103));
    input.enable();
    EXPECT_TRUE(input.claim(TextureId{2}, false, true, 104));
}
TEST(DirectInput, DuplicateDownAndIneligibleHitsNeverDispatch) {
    winmirror::DirectInput input;
    EXPECT_FALSE(input.claim(kNoTexture, false, true, 10));
    EXPECT_FALSE(input.claim(TextureId{1}, false, false, 11));
    EXPECT_TRUE(input.claim(TextureId{1}, false, true, 12));
    EXPECT_FALSE(input.claim(TextureId{2}, false, true, 13));
    input.up(false);
    EXPECT_FALSE(input.claim(TextureId{1}, false, true, 12));
    EXPECT_TRUE(input.claim(TextureId{2}, false, true, 14));
    EXPECT_TRUE(input.claim(TextureId{2}, true, true, 14));
    input.up(true);
    EXPECT_FALSE(input.claim(TextureId{2}, true, true, 14));
}

TEST(Gravity, EmitsGrabTapAndFloorThudSoundsAndHonorsMute) {
    SceneSetup setup{
        {800, 600},
        {{IconId{1}, TextureId{1}, {400, 120}, {48, 48}}, {IconId{2}, TextureId{2}, {400, 260}, {48, 48}}}};
    Playground p(setup);
    std::vector<SoundEvent> sounds;

    // Grabbing an icon emits tactile IconGrab.
    p.input({InputType::PointerDown, {400, 120}, 0.0, TextureId{1}});
    p.drainSounds(sounds);
    ASSERT_FALSE(sounds.empty());
    EXPECT_EQ(sounds[0].kind, SoundKind::IconGrab);
    p.input({InputType::PointerUp, {400, 120}, 0.02});
    p.drainSounds(sounds);

    // Dropping stacked icons emits both icon-to-icon IconTap and floor IconThud.
    bool sawTap = false, sawThud = false;
    for (int i = 0; i < 180; ++i) {
        p.tick(1.0 / 60.0);
        p.drainSounds(sounds);
        for (const auto& s : sounds) {
            if (s.kind == SoundKind::IconTap) sawTap = true;
            if (s.kind == SoundKind::IconThud) sawThud = true;
            EXPECT_GE(s.gain, 0.04f);
            EXPECT_LE(s.gain, 1.0f);
            EXPECT_GE(s.pan, -0.9f);
            EXPECT_LE(s.pan, 0.9f);
        }
    }
    EXPECT_TRUE(sawTap);
    EXPECT_TRUE(sawThud);
    EXPECT_EQ(p.billiardsSessionStats().soundEvents, 0);

    // Muting suppresses all Gravity sounds.
    p.reset();
    p.setMuted(true);
    for (int i = 0; i < 120; ++i) {
        p.tick(1.0 / 60.0);
        p.drainSounds(sounds);
        EXPECT_TRUE(sounds.empty());
    }
}

TEST(UserPreferences, RoundTripSerializationClampingAndColdBootSanitization) {
    UserPreferences pref;
    pref.lastPlayMode = PlayMode::Slingshot;
    pref.soundMuted = true;
    pref.showFilenames = false;
    pref.blackHoleScale = 1.65f;
    pref.blackHoleElevation = -0.42f;
    pref.blackHoleTilt = 0.78f;
    pref.billiardsRule = BilliardsRule::Snooker;
    pref.billiardsMatchMode = BilliardsMatchMode::VsAi;
    pref.billiardsAiDifficulty = BilliardsAiDifficulty::Goat;
    pref.iconSelection = IconSelectionPolicy::Random;
    pref.extraIcons = ExtraIconsPolicy::BottomGravity;
    pref.showBallBorders = false;
    pref.showBallNumbers = true;
    pref.tableTheme = BilliardsTableTheme::Minimal;
    pref.showTableChrome = false;
    pref.showAimGuide = false;
    pref.showAiIntent = false;
    pref.slingshotPreset = SlingshotPreset::GlassBridge;
    pref.slingshotAmmoCount = 6;
    pref.slingshotShowTrajectory = false;
    pref.slingshotHighScore = 4850;
    pref.hockeyMatchMode = AirHockeyMatchMode::Hotseat2P;
    pref.hockeyDifficulty = AirHockeyDifficulty::Master;
    pref.hockeyTargetGoals = 11;
    pref.hockeyShowRinkChrome = false;
    pref.lastLanEndpoint = "192.168.1.100:39457";

    const std::string text = pref.serializeIni();
    ASSERT_FALSE(text.empty());
    const UserPreferences loaded = UserPreferences::parseIni(text, false);
    EXPECT_EQ(loaded.lastPlayMode, PlayMode::Slingshot);
    EXPECT_TRUE(loaded.soundMuted);
    EXPECT_FALSE(loaded.showFilenames);
    EXPECT_NEAR(loaded.blackHoleScale, 1.65f, 1e-3f);
    EXPECT_NEAR(loaded.blackHoleElevation, -0.42f, 1e-4f);
    EXPECT_NEAR(loaded.blackHoleTilt, 0.78f, 1e-4f);
    EXPECT_EQ(loaded.billiardsRule, BilliardsRule::Snooker);
    EXPECT_EQ(loaded.billiardsMatchMode, BilliardsMatchMode::VsAi);
    EXPECT_EQ(loaded.billiardsAiDifficulty, BilliardsAiDifficulty::Goat);
    EXPECT_EQ(loaded.iconSelection, IconSelectionPolicy::Random);
    EXPECT_EQ(loaded.extraIcons, ExtraIconsPolicy::BottomGravity);
    EXPECT_FALSE(loaded.showBallBorders);
    EXPECT_TRUE(loaded.showBallNumbers);
    EXPECT_EQ(loaded.tableTheme, BilliardsTableTheme::Minimal);
    EXPECT_FALSE(loaded.showTableChrome);
    EXPECT_EQ(loaded.toBilliardsConfig().effectiveTableTheme(), BilliardsTableTheme::Minimal);
    EXPECT_FALSE(loaded.showAimGuide);
    EXPECT_FALSE(loaded.showAiIntent);
    EXPECT_EQ(loaded.slingshotPreset, SlingshotPreset::GlassBridge);
    EXPECT_EQ(loaded.slingshotAmmoCount, 6);
    EXPECT_FALSE(loaded.slingshotShowTrajectory);
    EXPECT_EQ(loaded.slingshotHighScore, 4850);
    EXPECT_EQ(loaded.hockeyMatchMode, AirHockeyMatchMode::Hotseat2P);
    EXPECT_EQ(loaded.hockeyDifficulty, AirHockeyDifficulty::Master);
    EXPECT_EQ(loaded.hockeyTargetGoals, 11);
    EXPECT_FALSE(loaded.hockeyShowRinkChrome);
    EXPECT_EQ(loaded.lastLanEndpoint, "192.168.1.100:39457");

    // Cold-boot sanitization resets transient LAN/Custom modes while clamping out-of-range numbers.
    const UserPreferences cold = UserPreferences::parseIni(
        "bh_scale=99.0\nbilliards_match=2\nhockey_match=2\nicon_selection=2\nsling_ammo=99\nhockey_goals=8\n",
        /*forColdBoot=*/true);
    EXPECT_FLOAT_EQ(cold.blackHoleScale, BlackHoleState::kMaxScale);
    EXPECT_EQ(cold.billiardsMatchMode, BilliardsMatchMode::Solo);
    EXPECT_EQ(cold.hockeyMatchMode, AirHockeyMatchMode::SoloAI);
    EXPECT_EQ(cold.iconSelection, IconSelectionPolicy::FirstN);
    EXPECT_EQ(cold.slingshotAmmoCount, 6);
    EXPECT_EQ(cold.hockeyTargetGoals, 7);

    // Legacy v2 INI with only table_chrome=0 upgrades cleanly to Borderless.
    const UserPreferences legacyOff = UserPreferences::parseIni("table_chrome=0\n", false);
    EXPECT_EQ(legacyOff.tableTheme, BilliardsTableTheme::Borderless);
    EXPECT_FALSE(legacyOff.showTableChrome);
}

TEST(Gravity, ScientificAstrophysicalGravityKinematicsDirectionRestitutionAndPersistence) {
    auto makeSingleIconMidAir = [] {
        SceneSetup s;
        s.workAreaSize = {1200, 900};
        s.icons.push_back({IconId{1}, TextureId{1}, {600.0f, 360.0f}, {48.0f, 48.0f}});
        return s;
    };

    auto measureFreeFallMeters = [&](float gMps2) {
        Playground pg(makeSingleIconMidAir());
        pg.setGravitySettings(gMps2, GravityDirection::Down, 0.15f);
        const float y0 = pg.mode().world().state(0).position.y;
        for (int i = 0; i < 24; ++i) pg.tick(1.0 / 60.0);
        return y0 - pg.mode().world().state(0).position.y;
    };

    // 1. Zero-G (0.00 m/s^2): motionless object stays motionless (Newton's 1st law).
    EXPECT_NEAR(measureFreeFallMeters(gravityPresetMps2(GravityPreset::ZeroG)), 0.0f, 1e-4f);

    // 2. Moon (1.62 m/s^2), Earth (9.80 m/s^2), Jupiter (24.79 m/s^2) obey s = 1/2 * g * t^2 (t = 0.40 s).
    const float t = 24.0f / 60.0f;
    const float dMoon = measureFreeFallMeters(gravityPresetMps2(GravityPreset::Moon));
    const float dEarth = measureFreeFallMeters(gravityPresetMps2(GravityPreset::Earth));
    const float dJupiter = measureFreeFallMeters(gravityPresetMps2(GravityPreset::Jupiter));
    EXPECT_NEAR(dMoon, 0.5f * 1.62f * t * t, 0.015f);
    EXPECT_NEAR(dEarth, 0.5f * 9.80f * t * t, 0.035f);
    EXPECT_NEAR(dJupiter, 0.5f * 24.79f * t * t, 0.085f);
    EXPECT_LT(dMoon, dEarth);
    EXPECT_LT(dEarth, dJupiter);

    // 3. Reversing gravity direction to Up wakes sleeping floor icons and lifts them to the ceiling!
    Playground pgDir(makeSingleIconMidAir());
    for (int i = 0; i < 600 && !pgDir.idle(); ++i) pgDir.tick(1.0 / 60.0);
    ASSERT_TRUE(pgDir.idle());
    const float settledFloorY = pgDir.mode().world().state(0).position.y;

    pgDir.setGravitySettings(kEarthGravityMps2, GravityDirection::Up, 0.15f);
    EXPECT_FALSE(pgDir.idle());
    for (int i = 0; i < 240; ++i) pgDir.tick(1.0 / 60.0);
    EXPECT_GT(pgDir.mode().world().state(0).position.y, settledFloorY + 4.0f);

    // 4. High elastic restitution (e = 0.90) rebounds higher than inelastic (e = 0.02).
    auto measurePeakRebound = [&](float restitution) {
        Playground pg(makeSingleIconMidAir());
        pg.setGravitySettings(kEarthGravityMps2, GravityDirection::Down, restitution);
        bool touchedFloor = false;
        float minY = 999.0f;
        float reboundPeakY = 0.0f;
        for (int i = 0; i < 180; ++i) {
            pg.tick(1.0 / 60.0);
            const auto st = pg.mode().world().state(0);
            minY = std::min(minY, st.position.y);
            if (!touchedFloor && i > 10 && st.linearVelocity.y > 0.2f) touchedFloor = true;
            if (touchedFloor) reboundPeakY = std::max(reboundPeakY, st.position.y);
        }
        return std::max(0.0f, reboundPeakY - minY);
    };
    EXPECT_GT(measurePeakRebound(0.90f), measurePeakRebound(0.02f) + 1.2f);

    // 5. UserPreferences round-trip & clamping for scientific gravity fields.
    UserPreferences pref;
    pref.gravityMagnitude = gravityPresetMps2(GravityPreset::Mars);
    pref.gravityDirection = GravityDirection::Left;
    pref.gravityRestitution = 0.78f;
    const UserPreferences parsed = UserPreferences::parseIni(pref.serializeIni(), false);
    EXPECT_NEAR(parsed.gravityMagnitude, 3.72f, 1e-3f);
    EXPECT_EQ(parsed.gravityDirection, GravityDirection::Left);
    EXPECT_NEAR(parsed.gravityRestitution, 0.78f, 1e-3f);
    EXPECT_EQ(matchGravityPreset(parsed.gravityMagnitude), GravityPreset::Mars);
}

TEST(MultiScreen, VoidRectsCrossBoundaryAndLShapeFloor) {
    // Dual-monitor span: Primary 2560x1440 on left, Secondary 1920x1080 aligned at top-right.
    // Union work area is 4480x1440; dead void rect is [2560, 1080, 1920, 360].
    const std::vector<Rect> screens{{0.0f, 0.0f, 2560.0f, 1440.0f}, {2560.0f, 0.0f, 1920.0f, 1080.0f}};
    const auto voids = computeMultiScreenVoidRects({4480.0f, 1440.0f}, screens);
    ASSERT_EQ(voids.size(), 1u);
    EXPECT_FLOAT_EQ(voids[0].x, 2560.0f);
    EXPECT_FLOAT_EQ(voids[0].y, 1080.0f);
    EXPECT_FLOAT_EQ(voids[0].width, 1920.0f);
    EXPECT_FLOAT_EQ(voids[0].height, 360.0f);

    // 1. Icon on secondary monitor settles on secondary monitor's y = 1080 floor (not y = 1440 void).
    SceneSetup setup;
    setup.workAreaSize = {4480.0f, 1440.0f};
    setup.screenWorkAreas = screens;
    setup.primaryArenaRect = screens[0];
    setup.icons = {{IconId{1}, TextureId{1}, {3200.0f, 400.0f}, {64.0f, 64.0f}},
                   {IconId{2}, TextureId{2}, {2450.0f, 300.0f}, {64.0f, 64.0f}}};
    Playground pg(setup);
    for (int i = 0; i < 400 && !pg.idle(); ++i) pg.tick(1.0 / 60.0);
    const auto centers = pg.currentIconSceneCenters();
    ASSERT_EQ(centers.size(), 2u);
    // Icon 1 rests just above y = 1080 on Monitor 2 (never enters [1080..1440] dead corner)
    EXPECT_LT(centers[0].y, 1080.0f);
    EXPECT_GT(centers[0].y, 1020.0f);
    // Icon 2 rests just above y = 1440 on Monitor 1
    EXPECT_GT(centers[1].y, 1380.0f);

    // 2. Drag-throwing Icon 2 rightward across x = 2560 crosses seamlessly into Monitor 2!
    Playground pgThrow(setup);
    pgThrow.setGravitySettings(0.0f, GravityDirection::Down, 0.5f);
    pgThrow.input({InputType::PointerDown, {2450.0f, 300.0f}, 0.0, TextureId{2}});
    pgThrow.input({InputType::PointerMove, {2540.0f, 300.0f}, 0.02});
    pgThrow.tick(0.02);
    pgThrow.input({InputType::PointerMove, {2680.0f, 300.0f}, 0.04});
    pgThrow.tick(0.02);
    pgThrow.input({InputType::PointerUp, {2680.0f, 300.0f}, 0.05});
    for (int i = 0; i < 30; ++i) pgThrow.tick(1.0 / 60.0);
    EXPECT_GT(pgThrow.currentIconSceneCenters()[1].x, 2600.0f);
}

TEST(MultiScreen, PrimaryArenaAnchoringAndZeroIconSecondaryDisplay) {
    // Secondary monitor on the left, Primary monitor on the right (offset x = 1920):
    SceneSetup spanSetup;
    spanSetup.workAreaSize = {4480.0f, 1440.0f};
    spanSetup.screenWorkAreas = {{0.0f, 0.0f, 1920.0f, 1080.0f}, {1920.0f, 0.0f, 2560.0f, 1440.0f}};
    spanSetup.primaryArenaRect = {1920.0f, 0.0f, 2560.0f, 1440.0f};
    spanSetup.icons = {{IconId{1}, TextureId{1}, {300.0f, 300.0f}, {64.0f, 64.0f}}};
    Playground pg(spanSetup);
    pg.setPlayMode(PlayMode::Billiards);
    ASSERT_TRUE(pg.billiardsEnabled());
    ASSERT_NE(pg.billiardsMode(), nullptr);
    // Table is centered inside primaryArenaRect (x >= 1920), never split across x = 1920 bezel!
    const float localTableCenterX =
        pg.billiardsMode()->tableMinScene().x + pg.billiardsMode()->tableSizeScene().x * 0.5f;
    EXPECT_NEAR(localTableCenterX, 2560.0f * 0.5f, 2.0f);
    // DrawList sprites are translated into union scene space (x > 1920):
    bool foundTranslatedBall = false;
    for (const auto& cmd : pg.drawList().commands()) {
        if (const auto* s = std::get_if<SpriteItem>(&cmd)) {
            EXPECT_GT(s->center.x, 1920.0f);
            foundTranslatedBall = true;
        }
    }
    EXPECT_TRUE(foundTranslatedBall);

    // Empty secondary monitor (0 desktop icons) still supports all 5 PlayModes cleanly!
    SceneSetup emptyScreen;
    emptyScreen.workAreaSize = {1920.0f, 1040.0f};
    Playground pgEmpty(emptyScreen);
    EXPECT_TRUE(pgEmpty.canEnableAnyBilliards());
    pgEmpty.setPlayMode(PlayMode::Billiards);
    EXPECT_TRUE(pgEmpty.billiardsEnabled());
    pgEmpty.tick(1.0 / 60.0);
    pgEmpty.setPlayMode(PlayMode::Slingshot);
    EXPECT_TRUE(pgEmpty.slingshotEnabled());
    pgEmpty.tick(1.0 / 60.0);
    pgEmpty.setPlayMode(PlayMode::AirHockey);
    EXPECT_TRUE(pgEmpty.airHockeyEnabled());
    pgEmpty.tick(1.0 / 60.0);
    pgEmpty.setPlayMode(PlayMode::BlackHole);
    pgEmpty.tick(1.0 / 60.0);
    pgEmpty.setPlayMode(PlayMode::Gravity);
    pgEmpty.tick(1.0 / 60.0);

    // UserPreferences displayMode persistence (-1 = all screens, 0..15 = single screen index)
    UserPreferences pref;
    pref.displayMode = -1;
    EXPECT_EQ(UserPreferences::parseIni(pref.serializeIni(), true).displayMode, -1);
    pref.displayMode = 2;
    EXPECT_EQ(UserPreferences::parseIni(pref.serializeIni(), true).displayMode, 2);

    // Multi-screen span arena inscribed band: spans full width across mixed-height monitors
    const std::vector<Rect> mixedScreens = {{0.0f, 360.0f, 1728.0f, 1006.0f},
                                            {1728.0f, 0.0f, 2560.0f, 1400.0f}};
    const Rect spanBand = computeMultiScreenSpanArenaRect({4288.0f, 1400.0f}, mixedScreens);
    EXPECT_NEAR(spanBand.x, 0.0f, 0.5f);
    EXPECT_NEAR(spanBand.y, 360.0f, 0.5f);
    EXPECT_NEAR(spanBand.width, 4288.0f, 0.5f);
    EXPECT_NEAR(spanBand.height, 1006.0f, 0.5f);

    SceneSetup fullSpanSetup;
    fullSpanSetup.workAreaSize = {4288.0f, 1400.0f};
    fullSpanSetup.screenWorkAreas = mixedScreens;
    fullSpanSetup.primaryArenaRect = spanBand;
    fullSpanSetup.icons = {{IconId{1}, TextureId{1}, {300.0f, 600.0f}, {64.0f, 64.0f}},
                           {IconId{2}, TextureId{2}, {2600.0f, 600.0f}, {64.0f, 64.0f}}};
    Playground pgFullSpan(fullSpanSetup);
    pgFullSpan.setPlayMode(PlayMode::Slingshot);
    ASSERT_NE(pgFullSpan.slingshotMode(), nullptr);
    // Slingshot sits on Screen 1 (< 1728), while fortress defenders sit on Screen 2 (> 1728)!
    EXPECT_LT(pgFullSpan.slingshotMode()->pouchRestScene().x, 1728.0f);
    float maxSpriteX = 0.0f;
    for (const auto& cmd : pgFullSpan.drawList().commands()) {
        if (const auto* s = std::get_if<SpriteItem>(&cmd)) maxSpriteX = std::max(maxSpriteX, s->center.x);
    }
    EXPECT_GT(maxSpriteX, 2400.0f);
}
