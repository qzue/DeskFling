#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <string>
#include <variant>

#include "core/modes/billiards_lan_codec.h"
#include "core/modes/billiards_mode.h"
#include "core/scene/playground.h"
#include "platforms/display_migration.h"
#include "platforms/filename_geometry.h"
#include "platforms/mirror_hit.h"

using namespace dpp;

namespace {

SceneSetup desktopScene(std::size_t count = 8, Vec2 size = {1280, 800}) {
    return syntheticScene(count, size);
}

const BilliardsMode& billiards(const Playground& p) { return dynamic_cast<const BilliardsMode&>(p.mode()); }

BilliardsMode& mutableBilliards(Playground& p) {
    return const_cast<BilliardsMode&>(dynamic_cast<const BilliardsMode&>(p.mode()));
}

bool drawListContainsText(const DrawList& dl, const char* needle) {
    for (const auto& cmd : dl.commands()) {
        if (const auto* t = std::get_if<TextItem>(&cmd)) {
            if (std::strstr(t->text.data(), needle) != nullptr) return true;
        }
    }
    return false;
}

}  // namespace

TEST(BilliardsMirror, InternationalStandardProportionsAcrossDisplays) {
    for (Vec2 screen : {Vec2{1728, 1080}, Vec2{1920, 1080}, Vec2{2560, 1080}, Vec2{1440, 900}}) {
        // 1. International Heyball / 8-Ball (2540 mm x 1270 mm, 57.15 mm ball).
        BilliardsConfig eightCfg = BilliardsConfig::desktop();
        eightCfg.rule = BilliardsRule::EightBall;
        BilliardsMode m8(desktopScene(15, screen), eightCfg);
        const Vec2 ts8 = m8.tableSizeScene();
        EXPECT_NEAR(ts8.x / ts8.y, 2540.0f / 1270.0f, 1e-4f);
        EXPECT_NEAR((2.0f * m8.ballRadiusScene()) / ts8.y, 57.15f / 1270.0f, 1e-4f);
        const float left8 = m8.tableMinScene().x;
        EXPECT_NEAR(m8.whiteSpawnScene().x - left8, ts8.x * 0.25f, 0.5f);
        EXPECT_NEAR(m8.objectBallScene(0).x - left8, ts8.x * 0.75f, 0.5f);

        // 2. International WPBSA 12-foot Snooker (3569 mm x 1778 mm, 52.5 mm ball).
        BilliardsConfig snkCfg = BilliardsConfig::desktop();
        snkCfg.rule = BilliardsRule::Snooker;
        BilliardsMode ms(desktopScene(15, screen), snkCfg);
        const Vec2 tss = ms.tableSizeScene();
        EXPECT_NEAR(tss.x / tss.y, 3569.0f / 1778.0f, 1e-4f);
        EXPECT_NEAR((2.0f * ms.ballRadiusScene()) / tss.y, 52.5f / 1778.0f, 1e-4f);
        EXPECT_LT(ms.ballRadiusScene(), m8.ballRadiusScene() * 0.75f);
        const float leftS = ms.tableMinScene().x;
        // Pink (index 19) on pyramid spot 0.75 L, Black (index 20) at (3569 - 324) / 3569 L.
        EXPECT_NEAR(ms.objectBallScene(19).x - leftS, tss.x * 0.75f, 0.6f);
        EXPECT_NEAR(ms.objectBallScene(20).x - leftS, tss.x * ((3569.0f - 324.0f) / 3569.0f), 0.6f);
    }
}

TEST(BilliardsMirror, DirectionLockAndLateralDriftInvariant) {
    BilliardsMode m(desktopScene(6), BilliardsConfig::desktop());
    const Vec2 w = m.whiteSceneCenter();
    const Vec2 target{w.x + 200.0f, w.y};
    m.handleInput({InputType::PointerMove, target, 0.1});
    m.handleInput({InputType::PointerDown, target, 0.2});
    ASSERT_EQ(m.state(), BilliardsState::Charging);
    EXPECT_NEAR(m.lockedAimDirection().x, 1.0f, 1e-4f);
    EXPECT_NEAR(m.lockedAimDirection().y, 0.0f, 1e-4f);

    // Pull 120 pt backward (-X) with a large 180 pt lateral (+Y) drift.
    m.handleInput({InputType::PointerMove, {target.x - 120.0f, target.y + 180.0f}, 0.3});
    EXPECT_NEAR(m.lockedAimDirection().x, 1.0f, 1e-4f);
    EXPECT_NEAR(m.lockedAimDirection().y, 0.0f, 1e-4f);
    EXPECT_NEAR(m.pullDistancePoints(), 120.0f, 1e-3f);

    m.handleInput({InputType::PointerUp, {target.x - 120.0f, target.y + 180.0f}, 0.4});
    EXPECT_EQ(m.state(), BilliardsState::Rolling);
    EXPECT_EQ(m.stats().shots, 1);
    const BodyState ws = m.world().state(0);
    const float expectedTop = std::clamp(m.tableSizeScene().x / 100.0f * 2.0f, 16.0f, 36.0f);
    EXPECT_NEAR(ws.linearVelocity.x, expectedTop * (120.0f / 200.0f), 1e-2f);
    EXPECT_NEAR(ws.linearVelocity.y, 0.0f, 1e-3f);
}

TEST(BilliardsMirror, PullBackThresholdCapOutsideReleaseAndTableHit) {
    BilliardsMode m(desktopScene(6), BilliardsConfig::desktop());
    // Clicking outside table margin does not start charging.
    EXPECT_FALSE(m.tableContains({6.0f, 400.0f}));
    m.handleInput({InputType::PointerDown, {6.0f, 400.0f}, 0.1});
    EXPECT_EQ(m.state(), BilliardsState::Ready);

    // Pull < 10 pt cancels on release without firing.
    const Vec2 w = m.whiteSceneCenter();
    const Vec2 start{w.x + 100.0f, w.y};
    m.handleInput({InputType::PointerDown, start, 1.0});
    ASSERT_EQ(m.state(), BilliardsState::Charging);
    m.handleInput({InputType::PointerMove, {start.x - 9.0f, start.y}, 1.1});
    DrawList dl;
    m.buildDrawList(dl);
    EXPECT_TRUE(drawListContainsText(dl, "松开取消"));
    m.handleInput({InputType::PointerUp, {start.x - 9.0f, start.y}, 1.2});
    EXPECT_EQ(m.state(), BilliardsState::Ready);
    EXPECT_EQ(m.stats().shots, 0);

    // Dragging well past the left edge of the screen caps at 200 pt (100%) and still shoots on release.
    m.handleInput({InputType::PointerDown, start, 2.0});
    m.handleInput({InputType::PointerMove, {-150.0f, start.y}, 2.1});
    EXPECT_FLOAT_EQ(m.pullDistancePoints(), 200.0f);
    m.handleInput({InputType::PointerUp, {-150.0f, start.y}, 2.2});
    EXPECT_EQ(m.state(), BilliardsState::Rolling);
    EXPECT_EQ(m.stats().shots, 1);
    const float expectedTop = std::clamp(m.tableSizeScene().x / 100.0f * 2.0f, 16.0f, 36.0f);
    EXPECT_NEAR(m.world().state(0).linearVelocity.length(), expectedTop, 1e-2f);

    // Left-click while rolling does nothing.
    m.handleInput({InputType::PointerDown, start, 2.3});
    EXPECT_EQ(m.state(), BilliardsState::Rolling);
    EXPECT_EQ(m.stats().shots, 1);
}

TEST(BilliardsMirror, AllCancelPathsAbortWithoutShooting) {
    Playground p(desktopScene(6));
    p.setBilliards(true);
    ASSERT_TRUE(p.billiardsEnabled());
    const Vec2 w = billiards(p).whiteSceneCenter();
    const Vec2 start{w.x + 120.0f, w.y};

    // PointerCancel (right-click / focus loss / menu).
    p.input({InputType::PointerDown, start, 1.0});
    p.input({InputType::PointerMove, {start.x - 100.0f, start.y}, 1.1});
    ASSERT_EQ(billiards(p).state(), BilliardsState::Charging);
    p.input({InputType::PointerCancel, {}, 1.2});
    EXPECT_EQ(billiards(p).state(), BilliardsState::Ready);
    EXPECT_EQ(p.mode().stats().shots, 0);

    // TogglePause while charging.
    p.input({InputType::PointerDown, start, 2.0});
    p.input({InputType::PointerMove, {start.x - 100.0f, start.y}, 2.1});
    p.input({InputType::TogglePause, {}, 2.2});
    EXPECT_TRUE(p.paused());
    EXPECT_EQ(billiards(p).state(), BilliardsState::Ready);
    EXPECT_EQ(p.mode().stats().shots, 0);
    p.input({InputType::TogglePause, {}, 2.3});

    // RequestExit while charging.
    p.input({InputType::PointerDown, start, 3.0});
    p.input({InputType::PointerMove, {start.x - 100.0f, start.y}, 3.1});
    p.input({InputType::RequestExit, {}, 3.2});
    EXPECT_EQ(billiards(p).state(), BilliardsState::Ready);
    EXPECT_EQ(p.mode().stats().shots, 0);
}

TEST(BilliardsMirror, SideReturnTubesAssignmentDropPauseOverflowAndOpenHit) {
    Playground p(desktopScene(42, {900, 520}));
    p.setBilliards(true);
    auto& bm = mutableBilliards(p);

    // Pocket 0 (top-left) -> left tube; Pocket 2/5 (right) -> right tube;
    // Pocket 1 (top-middle) -> balances to emptier side.
    const float inset = bm.ballRadiusScene() * 1.15f;
    const Vec2 p0 = bm.pocketScene(0) + Vec2{inset, inset};
    const Vec2 p2 = bm.pocketScene(2) + Vec2{-inset, inset};
    const Vec2 p5 = bm.pocketScene(5) + Vec2{-inset, -inset};
    const Vec2 p1 = bm.pocketScene(1) + Vec2{0.0f, inset};
    bm.debugPlaceBall(0, p0, {});
    bm.debugPlaceBall(1, p2, {});
    bm.debugPlaceBall(2, p5, {});
    bm.debugPlaceBall(3, p1, {});
    p.tick(1.0 / 60.0);
    EXPECT_EQ(bm.tubeBallCount(0), 2u);
    EXPECT_EQ(bm.tubeBallCount(1), 2u);
    EXPECT_FALSE(p.idle());  // Drop animation in progress.

    // Pause freezes drop animation.
    const Vec2 midDrop = bm.iconCurrentSceneCenter(0);
    p.input({InputType::TogglePause, {}, 1.0});
    p.tick(0.5);
    EXPECT_EQ(bm.iconCurrentSceneCenter(0), midDrop);
    p.input({InputType::TogglePause, {}, 2.0});
    for (int i = 0; i < 60; ++i) p.tick(1.0 / 60.0);
    const Vec2 settled = bm.iconCurrentSceneCenter(0);
    EXPECT_GT(settled.y, midDrop.y);

    // Tube ball is hit-testable for right-click open, identified as a tube hit, and excluded from table
    // shots.
    EXPECT_NE(mirrorHit(p.drawList(), settled), kNoTexture);
    EXPECT_TRUE(p.billiardsTubeHit(settled));
    EXPECT_FALSE(p.billiardsTableContains(settled));

    // Overflow: send all remaining 38 balls into the left tube one step at a time and verify containment +
    // "+N".
    for (std::size_t i = 4; i < 42; ++i) {
        bm.debugPlaceBall(i, p0, {});
        p.tick(1.0 / 60.0);
    }
    for (int i = 0; i < 60; ++i) p.tick(1.0 / 60.0);
    EXPECT_EQ(bm.tubeBallCount(0), 40u);
    EXPECT_TRUE(drawListContainsText(p.drawList(), "+"));
    for (const auto& cmd : p.drawList().commands()) {
        std::visit(
            [](const auto& item) {
                using T = std::decay_t<decltype(item)>;
                if constexpr (std::is_same_v<T, CircleItem>) {
                    EXPECT_GE(item.center.x - item.radius, -1.0f);
                    EXPECT_LE(item.center.x + item.radius, 901.0f);
                    EXPECT_GE(item.center.y - item.radius, -1.0f);
                    EXPECT_LE(item.center.y + item.radius, 521.0f);
                } else if constexpr (std::is_same_v<T, SpriteItem>) {
                    EXPECT_GE(item.center.x - item.size.x * 0.5f, -1.0f);
                    EXPECT_LE(item.center.x + item.size.x * 0.5f, 901.0f);
                    EXPECT_GE(item.center.y - item.size.y * 0.5f, -1.0f);
                    EXPECT_LE(item.center.y + item.size.y * 0.5f, 521.0f);
                }
            },
            cmd);
    }
}

TEST(BilliardsMirror, ScratchEntersManualBallInHandPlacement) {
    Playground p(desktopScene(4));
    p.setBilliards(true);
    auto& bm = mutableBilliards(p);

    bm.debugPlaceWhite(bm.pocketScene(1), {10.0f, 0.0f});
    for (int i = 0; i < 10; ++i) p.tick(1.0 / 60.0);
    ASSERT_TRUE(bm.whitePocketed());
    ASSERT_EQ(bm.state(), BilliardsState::PlacingWhite);
    EXPECT_TRUE(drawListContainsText(p.drawList(), "白球进袋"));

    // Invalid spots: pocket mouth, outside table, overlapping object ball 0.
    EXPECT_FALSE(bm.canPlaceWhite(bm.pocketScene(0)));
    EXPECT_FALSE(bm.canPlaceWhite({10.0f, 10.0f}));
    EXPECT_FALSE(bm.canPlaceWhite(bm.objectBallScene(0)));
    p.input({InputType::PointerDown, bm.objectBallScene(0), 1.0});
    EXPECT_EQ(bm.state(), BilliardsState::PlacingWhite);

    // Valid placement puts cue ball on table and enters Ready without firing a shot.
    const Vec2 spot = bm.whiteSpawnScene();
    ASSERT_TRUE(bm.canPlaceWhite(spot));
    p.input({InputType::PointerMove, spot, 1.1});
    p.input({InputType::PointerDown, spot, 1.2});
    EXPECT_EQ(bm.state(), BilliardsState::Ready);
    EXPECT_FALSE(bm.whitePocketed());
    EXPECT_EQ(p.mode().stats().shots, 0);
    EXPECT_EQ(p.billiardsSessionStats().totalWhitePlacements, 1);
}

TEST(BilliardsMirror, FinishSummarySessionBestMutualExclusionAndGravityDrop) {
    Playground p(desktopScene(2));
    p.setBlackHole(true);
    ASSERT_TRUE(static_cast<const GravityMode&>(p.mode()).blackHole().enabled);

    // Enabling billiards turns off black hole automatically.
    p.setBilliards(true);
    ASSERT_TRUE(p.billiardsEnabled());

    // Take one valid shot and pocket both balls to finish the rack.
    auto& bm = mutableBilliards(p);
    const Vec2 w = bm.whiteSceneCenter();
    p.input({InputType::PointerDown, {w.x + 100.0f, w.y}, 0.1});
    p.input({InputType::PointerMove, {w.x - 20.0f, w.y}, 0.2});
    p.input({InputType::PointerUp, {w.x - 20.0f, w.y}, 0.3});
    ASSERT_EQ(p.mode().stats().shots, 1);
    const float rIn = bm.ballRadiusScene() * 1.15f;
    mutableBilliards(p).debugPlaceBall(0, bm.pocketScene(0) + Vec2{rIn, rIn}, {});
    mutableBilliards(p).debugPlaceBall(1, bm.pocketScene(2) + Vec2{-rIn, rIn}, {});
    for (int i = 0; i < 600 && billiards(p).state() == BilliardsState::Rolling; ++i) p.tick(1.0 / 60.0);
    EXPECT_EQ(billiards(p).state(), BilliardsState::Finished);
    EXPECT_EQ(p.billiardsSessionStats().bestShots, 1);
    EXPECT_TRUE(drawListContainsText(p.drawList(), "全部进袋"));

    // Reset starts a fresh rack while retaining session bestShots.
    p.reset();
    p.tick(0);
    ASSERT_TRUE(p.billiardsEnabled());
    EXPECT_EQ(p.mode().stats().pocketed, 0);
    EXPECT_EQ(billiards(p).bestShots(), 1);

    // Pocket ball 0 into left tube, leave ball 1 on table, then disable billiards -> both drop under gravity!
    mutableBilliards(p).debugPlaceBall(0, billiards(p).pocketScene(0) + Vec2{rIn, rIn}, {});
    for (int i = 0; i < 40; ++i) p.tick(1.0 / 60.0);
    const Vec2 tubePos = billiards(p).iconCurrentSceneCenter(0);
    const Vec2 tablePos = billiards(p).iconCurrentSceneCenter(1);

    p.setBilliards(false);
    ASSERT_FALSE(p.billiardsEnabled());
    const auto& gm = static_cast<const GravityMode&>(p.mode());
    EXPECT_FALSE(gm.blackHole().enabled);
    EXPECT_NEAR(gm.bodySceneCenter(0).x, tubePos.x, 24.0f);
    EXPECT_NEAR(gm.bodySceneCenter(1).x, tablePos.x, 1.0f);
    const float yBefore = gm.bodySceneCenter(1).y;
    for (int i = 0; i < 20; ++i) p.tick(1.0 / 60.0);
    EXPECT_GT(gm.bodySceneCenter(1).y, yBefore + 5.0f);
}

TEST(BilliardsMirror, MigrationReRacksAndPreservesPauseAndSessionStats) {
    for (bool paused : {false, true}) {
        Playground p(desktopScene(4, {1280, 800}));
        p.setBilliards(true);
        const Vec2 w = billiards(p).whiteSceneCenter();
        p.input({InputType::PointerDown, {w.x + 80.0f, w.y}, 0.1});
        p.input({InputType::PointerMove, {w.x - 40.0f, w.y}, 0.2});
        p.input({InputType::PointerUp, {w.x - 40.0f, w.y}, 0.3});
        mutableBilliards(p).debugPlaceBall(0, billiards(p).pocketScene(0), {});
        p.tick(1.0 / 60.0);
        if (paused) p.input({InputType::TogglePause, {}, 0.4});

        DisplayMigration tx;
        tx.begin(p, 10.0);
        tx.prepare({1600, 1000});
        auto migrated = tx.commit(tx.revision(), 10.2, true);
        ASSERT_TRUE(migrated);
        EXPECT_TRUE(migrated->billiardsEnabled());
        EXPECT_EQ(migrated->paused(), paused);
        EXPECT_EQ(migrated->mode().stats().pocketed, 0);
        EXPECT_EQ(billiards(*migrated).tubeBallCount(0), 0u);
        EXPECT_EQ(migrated->billiardsSessionStats().totalShots, 1);
        EXPECT_EQ(migrated->billiardsSessionStats().totalPocketed, 1);
    }
}

TEST(BilliardsMirror, SingleIconWorksAndTooManyIconsGuardRefusesCleanly) {
    Playground single(desktopScene(1, {1280, 800}));
    EXPECT_TRUE(single.canEnableBilliards());
    single.setBilliards(true);
    EXPECT_TRUE(single.billiardsEnabled());
    EXPECT_EQ(single.mode().stats().targets, 1);

    Playground crowded(desktopScene(120, {700, 480}));
    EXPECT_FALSE(crowded.canEnableBilliards());
    crowded.setBilliards(true);
    EXPECT_FALSE(crowded.billiardsEnabled());
    EXPECT_EQ(crowded.mode().kind(), ModeKind::Gravity);
}

TEST(BilliardsMirror, SoundEventsCoverStrikeBallCollideRailBounceAndPocketWithVelocityScaling) {
    // 1. CueStrike scales with shot power.
    BilliardsMode soft(desktopScene(3), BilliardsConfig::desktop());
    const Vec2 ws = soft.whiteSceneCenter();
    soft.handleInput({InputType::PointerDown, {ws.x + 100.0f, ws.y}, 0.1});
    soft.handleInput({InputType::PointerMove, {ws.x + 80.0f, ws.y}, 0.2});  // 20 pt pull = 10%
    soft.handleInput({InputType::PointerUp, {ws.x + 80.0f, ws.y}, 0.3});
    std::vector<SoundEvent> softSounds;
    soft.drainSounds(softSounds);
    ASSERT_EQ(softSounds.size(), 1u);
    EXPECT_EQ(softSounds[0].kind, SoundKind::CueStrike);

    BilliardsMode hard(desktopScene(3), BilliardsConfig::desktop());
    const Vec2 wh = hard.whiteSceneCenter();
    hard.handleInput({InputType::PointerDown, {wh.x + 100.0f, wh.y}, 0.1});
    hard.handleInput({InputType::PointerMove, {wh.x - 100.0f, wh.y}, 0.2});  // 200 pt pull = 100%
    hard.handleInput({InputType::PointerUp, {wh.x - 100.0f, wh.y}, 0.3});
    std::vector<SoundEvent> hardSounds;
    hard.drainSounds(hardSounds);
    ASSERT_EQ(hardSounds.size(), 1u);
    EXPECT_EQ(hardSounds[0].kind, SoundKind::CueStrike);
    EXPECT_GT(hardSounds[0].gain, softSounds[0].gain);

    // 2. BallCollide via Box2D contact hit events: hard impact louder than soft impact.
    auto measureBallHitGain = [](float speedPtPerSec) {
        BilliardsMode m(desktopScene(1), BilliardsConfig::desktop());
        const Vec2 target = m.objectBallScene(0);
        const float r = m.ballRadiusScene();
        m.debugPlaceWhite({target.x - r * 3.2f, target.y}, {speedPtPerSec, 0.0f});
        std::vector<SoundEvent> evs;
        for (int i = 0; i < 20; ++i) {
            m.stepPhysics(1.0f / 60.0f);
            m.drainSounds(evs);
            for (const auto& e : evs) {
                if (e.kind == SoundKind::BallCollide) return e.gain;
            }
        }
        return 0.0f;
    };
    const float slowBallGain = measureBallHitGain(240.0f);
    const float fastBallGain = measureBallHitGain(1800.0f);
    EXPECT_GT(slowBallGain, 0.0f);
    EXPECT_GT(fastBallGain, slowBallGain);

    // 3. RailBounce via Box2D contact hit events (away from pockets).
    BilliardsMode railMode(desktopScene(1), BilliardsConfig::desktop());
    const float topRailY = railMode.tableMinScene().y;
    const float safeX = (railMode.tableMinScene().x + railMode.pocketScene(1).x) * 0.5f;
    railMode.debugPlaceWhite({safeX, topRailY + railMode.ballRadiusScene() * 2.5f}, {0.0f, -900.0f});
    bool sawRail = false;
    std::vector<SoundEvent> railEvents;
    for (int i = 0; i < 20 && !sawRail; ++i) {
        railMode.stepPhysics(1.0f / 60.0f);
        railMode.drainSounds(railEvents);
        for (const auto& e : railEvents) {
            if (e.kind == SoundKind::RailBounce) {
                sawRail = true;
                EXPECT_GT(e.gain, 0.1f);
                EXPECT_GE(e.pan, -1.0f);
                EXPECT_LE(e.pan, 1.0f);
            }
        }
    }
    EXPECT_TRUE(sawRail);

    // 4. PocketDrop emitted when ball enters pocket.
    const float inset = railMode.ballRadiusScene() * 1.1f;
    railMode.debugPlaceBall(0, railMode.pocketScene(0) + Vec2{inset, inset}, {200.0f, -200.0f});
    railMode.updateRules(1.0f / 60.0f);
    std::vector<SoundEvent> dropEvents;
    railMode.drainSounds(dropEvents);
    ASSERT_FALSE(dropEvents.empty());
    EXPECT_EQ(dropEvents.back().kind, SoundKind::PocketDrop);
    EXPECT_GE(dropEvents.back().gain, 0.55f);
}

TEST(BilliardsMirror, MuteToggleSuppressesSoundsAndSurvivesMigration) {
    Playground p(desktopScene(6));
    p.setBilliards(true);
    const Vec2 w = billiards(p).whiteSceneCenter();
    p.input({InputType::PointerDown, {w.x + 100.0f, w.y}, 0.1});
    p.input({InputType::PointerMove, {w.x - 100.0f, w.y}, 0.2});
    p.input({InputType::PointerUp, {w.x - 100.0f, w.y}, 0.3});
    std::vector<SoundEvent> out;
    p.drainSounds(out);
    EXPECT_EQ(out.size(), 1u);
    EXPECT_GE(p.billiardsSessionStats().soundEvents, 1);

    // Muting suppresses all subsequent collision and shot sounds.
    p.setMuted(true);
    EXPECT_TRUE(p.muted());
    for (int i = 0; i < 60; ++i) p.tick(1.0 / 60.0);
    p.drainSounds(out);
    EXPECT_TRUE(out.empty());

    // Mute setting survives live display migration.
    DisplayMigration tx;
    tx.begin(p, 5.0);
    tx.prepare({1440, 900});
    auto migrated = tx.commit(tx.revision(), 5.2, true);
    ASSERT_TRUE(migrated);
    EXPECT_TRUE(migrated->muted());

    // Unmuting restores sound generation.
    migrated->setMuted(false);
    const Vec2 mw = billiards(*migrated).whiteSceneCenter();
    migrated->input({InputType::PointerDown, {mw.x + 100.0f, mw.y}, 6.0});
    migrated->input({InputType::PointerMove, {mw.x - 80.0f, mw.y}, 6.1});
    migrated->input({InputType::PointerUp, {mw.x - 80.0f, mw.y}, 6.2});
    migrated->drainSounds(out);
    EXPECT_EQ(out.size(), 1u);
}

TEST(BilliardsMirror, SpinWidgetDragClampResetAndCancelRetention) {
    Playground p(desktopScene(4, {1440, 900}));
    p.setBilliards(true);
    const auto& bm = billiards(p);
    const Vec2 sw = bm.spinWidgetCenterScene();
    const float wr = bm.spinWidgetRadiusScene();
    EXPECT_TRUE(p.billiardsTableContains(sw));

    // Drag up-right well outside the widget circle -> clamps to unit circle, does not shoot.
    p.input({InputType::PointerDown, sw, 0.1});
    p.input({InputType::PointerMove, {sw.x + wr * 2.0f, sw.y - wr * 2.0f}, 0.2});
    p.input({InputType::PointerUp, {sw.x + wr * 2.0f, sw.y - wr * 2.0f}, 0.3});
    EXPECT_EQ(bm.state(), BilliardsState::Ready);
    EXPECT_EQ(p.mode().stats().shots, 0);
    EXPECT_NEAR(bm.tipOffset().length(), 1.0f, 1e-3f);
    EXPECT_GT(bm.tipOffset().x, 0.65f);
    EXPECT_GT(bm.tipOffset().y, 0.65f);
    p.tick(0);
    EXPECT_TRUE(drawListContainsText(p.drawList(), "高杆"));
    EXPECT_TRUE(drawListContainsText(p.drawList(), "右塞"));

    // Canceled pull-back shot preserves selected spin.
    const Vec2 w = bm.whiteSceneCenter();
    p.input({InputType::PointerDown, {w.x + 100.0f, w.y}, 1.0});
    p.input({InputType::PointerMove, {w.x + 95.0f, w.y}, 1.1});  // 5 pt < 10 pt => cancel
    p.input({InputType::PointerUp, {w.x + 95.0f, w.y}, 1.2});
    EXPECT_NEAR(bm.tipOffset().length(), 1.0f, 1e-3f);

    // Clicking the [复位] label area next to the widget clears spin to center.
    const Vec2 resetPt{sw.x + wr + 30.0f, sw.y};
    EXPECT_TRUE(p.billiardsTableContains(resetPt));
    p.input({InputType::PointerDown, resetPt, 2.0});
    p.input({InputType::PointerUp, resetPt, 2.1});
    EXPECT_FLOAT_EQ(bm.tipOffset().x, 0.0f);
    EXPECT_FLOAT_EQ(bm.tipOffset().y, 0.0f);
}

TEST(BilliardsMirror, TopspinFollowStunAndBackspinDrawProduceDistinctPostImpactPaths) {
    auto runHeadOnShot = [](float spinTop) {
        BilliardsMode m(desktopScene(1, {1440, 900}), BilliardsConfig::desktop());
        const Vec2 w0 = m.whiteSceneCenter();
        const float r = m.ballRadiusScene();
        const Vec2 ballPos{w0.x + r * 8.0f, w0.y};
        m.debugPlaceBall(0, ballPos, {});
        m.setTipOffset({0.0f, spinTop});
        // Shoot straight right (+X) at 70% power (140 pt pull).
        const Vec2 press{w0.x + 100.0f, w0.y};
        m.handleInput({InputType::PointerMove, press, 0.1});
        m.handleInput({InputType::PointerDown, press, 0.2});
        m.handleInput({InputType::PointerMove, {press.x - 140.0f, press.y}, 0.3});
        m.handleInput({InputType::PointerUp, {press.x - 140.0f, press.y}, 0.4});
        for (int i = 0; i < 420 && m.state() == BilliardsState::Rolling; ++i) {
            m.stepPhysics(1.0f / 60.0f);
            m.updateRules(1.0f / 60.0f);
        }
        return m.whiteSceneCenter().x;
    };

    const float xDraw = runHeadOnShot(-1.0f);
    const float xStun = runHeadOnShot(0.0f);
    const float xFollow = runHeadOnShot(1.0f);

    EXPECT_LT(xDraw, xStun - 60.0f);
    EXPECT_GT(xFollow, xStun + 60.0f);
    EXPECT_GT(xFollow - xDraw, 150.0f);
}

TEST(BilliardsMirror, LeftAndRightEnglishDeflectCushionReboundOppositeWays) {
    auto runCushionShot = [](float spinSide) {
        BilliardsMode m(desktopScene(1, {1440, 900}), BilliardsConfig::desktop());
        // Move object ball out of the way near the right rail.
        m.debugPlaceBall(0, {m.pocketScene(2).x - m.ballRadiusScene() * 4.0f, m.whiteSceneCenter().y}, {});
        const float safeX = (m.pocketScene(0).x + m.pocketScene(1).x) * 0.5f;
        const float startY = m.whiteSceneCenter().y;
        m.debugPlaceWhite({safeX, startY}, {});
        m.setTipOffset({spinSide, 0.0f});

        // Shoot straight UP (-Y in scene space) toward the top cushion at 60% power (120 pt pull downward).
        const Vec2 press{safeX, startY - 80.0f};
        m.handleInput({InputType::PointerMove, press, 0.1});
        m.handleInput({InputType::PointerDown, press, 0.2});
        m.handleInput({InputType::PointerMove, {press.x, press.y + 120.0f}, 0.3});
        m.handleInput({InputType::PointerUp, {press.x, press.y + 120.0f}, 0.4});
        for (int i = 0; i < 90; ++i) {
            m.stepPhysics(1.0f / 60.0f);
            m.updateRules(1.0f / 60.0f);
        }
        return m.whiteSceneCenter().x - safeX;
    };

    const float dxLeft = runCushionShot(-1.0f);
    const float dxCenter = runCushionShot(0.0f);
    const float dxRight = runCushionShot(1.0f);

    EXPECT_NEAR(dxCenter, 0.0f, 4.0f);
    EXPECT_LT(dxLeft, -50.0f);
    EXPECT_GT(dxRight, 50.0f);
}

TEST(BilliardsMirror, PlayModeSelectorUnifiesGravityBlackHoleAndBilliards) {
    Playground p(desktopScene(8, {1280, 800}));
    EXPECT_EQ(p.playMode(), PlayMode::Gravity);

    p.setPlayMode(PlayMode::BlackHole);
    EXPECT_EQ(p.playMode(), PlayMode::BlackHole);
    EXPECT_FALSE(p.billiardsEnabled());

    p.setPlayMode(PlayMode::Billiards);
    EXPECT_EQ(p.playMode(), PlayMode::Billiards);
    EXPECT_TRUE(p.billiardsEnabled());

    p.setPlayMode(PlayMode::BlackHole);
    EXPECT_EQ(p.playMode(), PlayMode::BlackHole);
    EXPECT_FALSE(p.billiardsEnabled());

    p.setPlayMode(PlayMode::Gravity);
    EXPECT_EQ(p.playMode(), PlayMode::Gravity);
}

TEST(BilliardsMirror, EightBallRackGroupsFoulBallInHandAndEightWinLoss) {
    BilliardsConfig cfg = BilliardsConfig::desktop();
    cfg.rule = BilliardsRule::EightBall;
    cfg.matchMode = BilliardsMatchMode::Hotseat;

    Playground p(desktopScene(6, {1440, 900}));
    p.setBilliardsConfig(cfg);
    p.setBilliards(true);
    ASSERT_TRUE(p.billiardsEnabled());
    auto& bm = mutableBilliards(p);
    ASSERT_EQ(bm.objectBallCount(), 15u);

    // WPA rack check: slot 4 (col=2,row=1) is #8; slot 10 (col=4,row=0) is Solid; slot 14 (col=4,row=4) is
    // Stripe.
    EXPECT_EQ(bm.objectBallNumber(4), 8);
    EXPECT_EQ(bm.objectBallGroup(4), BallGroup::Eight);
    EXPECT_EQ(bm.objectBallGroup(10), BallGroup::Solid);
    EXPECT_EQ(bm.objectBallGroup(14), BallGroup::Stripe);

    // Deficit check: 6 desktop icons -> 6 icon balls + 9 standard filler balls (kNoTexture).
    int iconBalls = 0, fillerBalls = 0;
    for (std::size_t i = 0; i < bm.objectBallCount(); ++i) {
        if (bm.objectBallTexture(i) != kNoTexture)
            ++iconBalls;
        else
            ++fillerBalls;
    }
    EXPECT_EQ(iconBalls, 6);
    EXPECT_EQ(fillerBalls, 9);
    // Filler balls are NOT hit-testable as openable desktop items.
    EXPECT_EQ(mirrorHit(p.drawList(), bm.objectBallScene(14)), kNoTexture);

    // Shot 1 (Break by P0): hit apex ball and pot a Solid -> break keeps table open and keeps P0's turn.
    std::size_t firstSolid = 0;
    for (std::size_t i = 0; i < bm.objectBallCount(); ++i) {
        if (bm.objectBallGroup(i) == BallGroup::Solid) {
            firstSolid = i;
            break;
        }
    }
    const Vec2 w1 = bm.whiteSceneCenter();
    bm.debugPlaceBall(firstSolid, {w1.x + bm.ballRadiusScene() * 3.0f, w1.y}, {});
    p.input({InputType::PointerMove, {w1.x + 80.0f, w1.y}, 0.1});
    p.input({InputType::PointerDown, {w1.x + 80.0f, w1.y}, 0.2});
    p.input({InputType::PointerMove, {w1.x + 40.0f, w1.y}, 0.3});
    p.input({InputType::PointerUp, {w1.x + 40.0f, w1.y}, 0.4});
    for (int i = 0; i < 6; ++i) p.tick(1.0 / 60.0);
    const float rIn = bm.ballRadiusScene() * 1.1f;
    bm.debugPlaceBall(firstSolid, bm.pocketScene(0) + Vec2{rIn, rIn}, {});
    for (int i = 0; i < 400 && bm.state() == BilliardsState::Rolling; ++i) p.tick(1.0 / 60.0);
    EXPECT_EQ(bm.currentPlayer(), 0);
    EXPECT_EQ(bm.playerGroup(0), BallGroup::FreeBall);  // Break never locks groups.

    // Shot 2 (Post-break by P0): hit and pot another Solid -> assigns Solid to P0, Stripe to P1!
    std::size_t secondSolid = 0;
    for (std::size_t i = 0; i < bm.objectBallCount(); ++i) {
        if (i != firstSolid && bm.objectBallGroup(i) == BallGroup::Solid) {
            secondSolid = i;
            break;
        }
    }
    const Vec2 safeSpot{bm.pocketScene(0).x + 160.0f, bm.pocketScene(0).y + 180.0f};
    bm.debugPlaceWhite(safeSpot, {});
    bm.debugPlaceBall(secondSolid, {safeSpot.x + bm.ballRadiusScene() * 3.0f, safeSpot.y}, {});
    p.input({InputType::PointerMove, {safeSpot.x + 80.0f, safeSpot.y}, 1.0});
    p.input({InputType::PointerDown, {safeSpot.x + 80.0f, safeSpot.y}, 1.1});
    p.input({InputType::PointerMove, {safeSpot.x + 40.0f, safeSpot.y}, 1.2});
    p.input({InputType::PointerUp, {safeSpot.x + 40.0f, safeSpot.y}, 1.3});
    for (int i = 0; i < 6; ++i) p.tick(1.0 / 60.0);
    bm.debugPlaceBall(secondSolid, bm.pocketScene(0) + Vec2{rIn, rIn}, {});
    for (int i = 0; i < 400 && bm.state() == BilliardsState::Rolling; ++i) p.tick(1.0 / 60.0);
    EXPECT_EQ(bm.playerGroup(0), BallGroup::Solid);
    EXPECT_EQ(bm.playerGroup(1), BallGroup::Stripe);
    EXPECT_EQ(bm.currentPlayer(), 0);

    // Shot 3 (Foul by P0 — shoot away into left cushion hitting no ball): awards Ball-in-Hand to P1!
    const Vec2 w3 = bm.whiteSceneCenter();
    p.input({InputType::PointerMove, {w3.x - 80.0f, w3.y}, 2.0});
    p.input({InputType::PointerDown, {w3.x - 80.0f, w3.y}, 2.1});
    p.input({InputType::PointerMove, {w3.x - 45.0f, w3.y}, 2.2});  // Aiming -X, pull +X = 35pt
    p.input({InputType::PointerUp, {w3.x - 45.0f, w3.y}, 2.3});
    for (int i = 0; i < 400 && bm.state() == BilliardsState::Rolling; ++i) p.tick(1.0 / 60.0);
    EXPECT_EQ(bm.currentPlayer(), 1);
    EXPECT_EQ(bm.state(), BilliardsState::PlacingWhite);

    // P1 places white, then illegally pots #8 while Stripes remain -> P0 wins!
    const Vec2 p1Spot = bm.whiteSpawnScene();
    ASSERT_TRUE(bm.canPlaceWhite(p1Spot));
    p.input({InputType::PointerDown, p1Spot, 3.0});
    ASSERT_EQ(bm.state(), BilliardsState::Ready);
    bm.debugPlaceBall(4, {p1Spot.x + bm.ballRadiusScene() * 3.0f, p1Spot.y}, {});
    p.input({InputType::PointerMove, {p1Spot.x + 80.0f, p1Spot.y}, 3.1});
    p.input({InputType::PointerDown, {p1Spot.x + 80.0f, p1Spot.y}, 3.2});
    p.input({InputType::PointerMove, {p1Spot.x + 40.0f, p1Spot.y}, 3.3});
    p.input({InputType::PointerUp, {p1Spot.x + 40.0f, p1Spot.y}, 3.4});
    for (int i = 0; i < 6; ++i) p.tick(1.0 / 60.0);
    bm.debugPlaceBall(4, bm.pocketScene(2) + Vec2{-rIn, rIn}, {});
    for (int i = 0; i < 400 && bm.state() == BilliardsState::Rolling; ++i) p.tick(1.0 / 60.0);
    EXPECT_EQ(bm.state(), BilliardsState::Finished);
    EXPECT_EQ(bm.winner(), 0);
}

TEST(BilliardsMirror, EightBallLegalClearanceWinsAfterOwnGroupCleared) {
    BilliardsConfig cfg = BilliardsConfig::desktop();
    cfg.rule = BilliardsRule::EightBall;
    cfg.matchMode = BilliardsMatchMode::Solo;

    Playground p(desktopScene(15, {1440, 900}));
    p.setBilliardsConfig(cfg);
    p.setBilliards(true);
    auto& bm = mutableBilliards(p);
    const float rIn = bm.ballRadiusScene() * 1.1f;

    // Shot 1 (break without pot).
    const Vec2 w0 = bm.whiteSceneCenter();
    p.input({InputType::PointerMove, {w0.x + 80.0f, w0.y}, 0.1});
    p.input({InputType::PointerDown, {w0.x + 80.0f, w0.y}, 0.2});
    p.input({InputType::PointerMove, {w0.x + 50.0f, w0.y}, 0.3});
    p.input({InputType::PointerUp, {w0.x + 50.0f, w0.y}, 0.4});
    for (int i = 0; i < 400 && bm.state() == BilliardsState::Rolling; ++i) p.tick(1.0 / 60.0);

    // Shot 2: hit & pot all 7 Solids -> locks Solid group and leaves 0 Solids remaining!
    std::vector<std::size_t> solids;
    for (std::size_t i = 0; i < bm.objectBallCount(); ++i) {
        if (bm.objectBallGroup(i) == BallGroup::Solid) solids.push_back(i);
    }
    ASSERT_EQ(solids.size(), 7u);
    const Vec2 sSpot{bm.pocketScene(0).x + 150.0f, bm.pocketScene(0).y + 150.0f};
    bm.debugPlaceWhite(sSpot, {});
    bm.debugPlaceBall(solids[0], {sSpot.x + bm.ballRadiusScene() * 3.0f, sSpot.y}, {});
    p.input({InputType::PointerMove, {sSpot.x + 80.0f, sSpot.y}, 1.0});
    p.input({InputType::PointerDown, {sSpot.x + 80.0f, sSpot.y}, 1.1});
    p.input({InputType::PointerMove, {sSpot.x + 45.0f, sSpot.y}, 1.2});
    p.input({InputType::PointerUp, {sSpot.x + 45.0f, sSpot.y}, 1.3});
    for (int i = 0; i < 6; ++i) p.tick(1.0 / 60.0);
    for (std::size_t idx : solids) {
        bm.debugPlaceBall(idx, bm.pocketScene(0) + Vec2{rIn, rIn}, {});
    }
    for (int i = 0; i < 400 && bm.state() == BilliardsState::Rolling; ++i) p.tick(1.0 / 60.0);
    ASSERT_EQ(bm.playerGroup(0), BallGroup::Solid);
    ASSERT_EQ(bm.state(), BilliardsState::Ready);

    // Shot 3: legally hit #8 first and pot #8 -> Solo 8-Ball victory!
    bm.debugPlaceWhite(sSpot, {});
    bm.debugPlaceBall(4, {sSpot.x + bm.ballRadiusScene() * 3.0f, sSpot.y}, {});
    p.input({InputType::PointerMove, {sSpot.x + 80.0f, sSpot.y}, 2.0});
    p.input({InputType::PointerDown, {sSpot.x + 80.0f, sSpot.y}, 2.1});
    p.input({InputType::PointerMove, {sSpot.x + 45.0f, sSpot.y}, 2.2});
    p.input({InputType::PointerUp, {sSpot.x + 45.0f, sSpot.y}, 2.3});
    for (int i = 0; i < 6; ++i) p.tick(1.0 / 60.0);
    bm.debugPlaceBall(4, bm.pocketScene(0) + Vec2{rIn, rIn}, {});
    for (int i = 0; i < 400 && bm.state() == BilliardsState::Rolling; ++i) p.tick(1.0 / 60.0);
    EXPECT_EQ(bm.state(), BilliardsState::Finished);
    EXPECT_EQ(bm.winner(), 0);
}

TEST(BilliardsMirror, SnookerRedColorRespotFoulPenaltyAndEndgameSequence) {
    BilliardsConfig cfg = BilliardsConfig::desktop();
    cfg.rule = BilliardsRule::Snooker;
    cfg.matchMode = BilliardsMatchMode::Hotseat;

    Playground p(desktopScene(10, {1440, 900}));
    p.setBilliardsConfig(cfg);
    p.setBilliards(true);
    ASSERT_TRUE(p.billiardsEnabled());
    auto& bm = mutableBilliards(p);
    ASSERT_EQ(bm.objectBallCount(), 21u);
    EXPECT_EQ(bm.snookerTarget(), 1);  // Red on

    const float rIn = bm.ballRadiusScene() * 1.1f;
    const Vec2 spot{bm.pocketScene(0).x + 220.0f, bm.pocketScene(0).y + 180.0f};
    const std::size_t blackIdx = 20;  // 15..20 are Yellow(2)..Black(7)
    ASSERT_EQ(bm.objectBallNumber(blackIdx), 7);

    // 1. P0 hits Red(0) and pots it -> +1 pt, target becomes 8 (any Color), P0 keeps turn.
    bm.debugPlaceWhite(spot, {});
    bm.debugPlaceBall(0, {spot.x + bm.ballRadiusScene() * 3.0f, spot.y}, {});
    p.input({InputType::PointerMove, {spot.x + 80.0f, spot.y}, 0.1});
    p.input({InputType::PointerDown, {spot.x + 80.0f, spot.y}, 0.2});
    p.input({InputType::PointerMove, {spot.x + 45.0f, spot.y}, 0.3});
    p.input({InputType::PointerUp, {spot.x + 45.0f, spot.y}, 0.4});
    for (int i = 0; i < 6; ++i) p.tick(1.0 / 60.0);
    bm.debugPlaceBall(0, bm.pocketScene(0) + Vec2{rIn, rIn}, {});
    for (int i = 0; i < 400 && bm.state() == BilliardsState::Rolling; ++i) p.tick(1.0 / 60.0);
    EXPECT_EQ(bm.score(0), 1);
    EXPECT_EQ(bm.snookerTarget(), 8);
    EXPECT_EQ(bm.currentPlayer(), 0);

    // 2. P0 hits Black(7) and pots it while 14 Reds remain -> +7 pts (total 8), Black is RE-SPOTTED!
    bm.debugPlaceWhite(spot, {});
    bm.debugPlaceBall(blackIdx, {spot.x + bm.ballRadiusScene() * 3.0f, spot.y}, {});
    p.input({InputType::PointerMove, {spot.x + 80.0f, spot.y}, 1.0});
    p.input({InputType::PointerDown, {spot.x + 80.0f, spot.y}, 1.1});
    p.input({InputType::PointerMove, {spot.x + 45.0f, spot.y}, 1.2});
    p.input({InputType::PointerUp, {spot.x + 45.0f, spot.y}, 1.3});
    for (int i = 0; i < 6; ++i) p.tick(1.0 / 60.0);
    bm.debugPlaceBall(blackIdx, bm.pocketScene(2) + Vec2{-rIn, rIn}, {});
    for (int i = 0; i < 400 && bm.state() == BilliardsState::Rolling; ++i) p.tick(1.0 / 60.0);
    EXPECT_EQ(bm.score(0), 8);
    EXPECT_EQ(bm.snookerTarget(), 1);
    EXPECT_EQ(bm.stats().pocketed, 1);  // Only the 1 Red stays pocketed; Black was re-spotted!

    // 3. Foul: Red is on, but P0 hits Pink(6, idx=19) first -> awards 6 penalty points to P1 and passes turn!
    const std::size_t pinkIdx = 19;
    ASSERT_EQ(bm.objectBallNumber(pinkIdx), 6);
    bm.debugPlaceWhite(spot, {});
    bm.debugPlaceBall(pinkIdx, {spot.x + bm.ballRadiusScene() * 3.0f, spot.y}, {});
    p.input({InputType::PointerMove, {spot.x + 80.0f, spot.y}, 2.0});
    p.input({InputType::PointerDown, {spot.x + 80.0f, spot.y}, 2.1});
    p.input({InputType::PointerMove, {spot.x + 45.0f, spot.y}, 2.2});
    p.input({InputType::PointerUp, {spot.x + 45.0f, spot.y}, 2.3});
    for (int i = 0; i < 400 && bm.state() == BilliardsState::Rolling; ++i) p.tick(1.0 / 60.0);
    EXPECT_EQ(bm.score(1), 6);
    EXPECT_EQ(bm.currentPlayer(), 1);

    // 4. Pot remaining 14 Reds (indices 1..14), take free Color shot, then clear 2->3->4->5->6->7!
    bm.debugPlaceWhite(spot, {});
    bm.debugPlaceBall(1, {spot.x + bm.ballRadiusScene() * 3.0f, spot.y}, {});
    p.input({InputType::PointerMove, {spot.x + 80.0f, spot.y}, 3.0});
    p.input({InputType::PointerDown, {spot.x + 80.0f, spot.y}, 3.1});
    p.input({InputType::PointerMove, {spot.x + 45.0f, spot.y}, 3.2});
    p.input({InputType::PointerUp, {spot.x + 45.0f, spot.y}, 3.3});
    for (int i = 0; i < 6; ++i) p.tick(1.0 / 60.0);
    for (std::size_t ri = 1; ri < 15; ++ri) {
        bm.debugPlaceBall(ri, bm.pocketScene(0) + Vec2{rIn, rIn}, {});
    }
    for (int i = 0; i < 400 && bm.state() == BilliardsState::Rolling; ++i) p.tick(1.0 / 60.0);
    ASSERT_EQ(bm.snookerTarget(), 8);  // Free color after last red!

    // Miss free color cleanly (hit Yellow 15 without potting) -> enters ordered 2..7 endgame!
    bm.debugPlaceWhite(spot, {});
    bm.debugPlaceBall(15, {spot.x + bm.ballRadiusScene() * 3.0f, spot.y}, {});
    p.input({InputType::PointerMove, {spot.x + 80.0f, spot.y}, 4.0});
    p.input({InputType::PointerDown, {spot.x + 80.0f, spot.y}, 4.1});
    p.input({InputType::PointerMove, {spot.x + 55.0f, spot.y}, 4.2});
    p.input({InputType::PointerUp, {spot.x + 55.0f, spot.y}, 4.3});
    for (int i = 0; i < 400 && bm.state() == BilliardsState::Rolling; ++i) p.tick(1.0 / 60.0);
    ASSERT_EQ(bm.snookerTarget(), 2);

    for (std::size_t ci = 15; ci <= 20; ++ci) {
        ASSERT_EQ(bm.snookerTarget(), static_cast<int>(ci - 13));
        bm.debugPlaceWhite(spot, {});
        bm.debugPlaceBall(ci, {spot.x + bm.ballRadiusScene() * 3.0f, spot.y}, {});
        const double t0 = 5.0 + static_cast<double>(ci);
        p.input({InputType::PointerMove, {spot.x + 80.0f, spot.y}, t0});
        p.input({InputType::PointerDown, {spot.x + 80.0f, spot.y}, t0 + 0.1});
        p.input({InputType::PointerMove, {spot.x + 45.0f, spot.y}, t0 + 0.2});
        p.input({InputType::PointerUp, {spot.x + 45.0f, spot.y}, t0 + 0.3});
        for (int i = 0; i < 6; ++i) p.tick(1.0 / 60.0);
        bm.debugPlaceBall(ci, bm.pocketScene(2) + Vec2{-rIn, rIn}, {});
        for (int i = 0; i < 400 && bm.state() == BilliardsState::Rolling; ++i) p.tick(1.0 / 60.0);
    }
    EXPECT_EQ(bm.state(), BilliardsState::Finished);
    EXPECT_EQ(bm.stats().pocketed, 21);
}

TEST(BilliardsMirror, CustomIconsSurplusBottomGravityRandomAndBallNumbers) {
    // 1. 120 desktop icons can enable EightBall (15) and Snooker (21) even though Free (120) refuses.
    Playground large(desktopScene(120, {1280, 800}));
    EXPECT_FALSE(large.canEnableBilliards());
    BilliardsConfig eightCfg = BilliardsConfig::desktop();
    eightCfg.rule = BilliardsRule::EightBall;
    EXPECT_TRUE(large.canEnableBilliardsWith(eightCfg));

    // 2. Custom selection {1, 4, 9} with BottomGravity on a 20-icon desktop.
    Playground p(desktopScene(20, {1280, 800}));
    BilliardsConfig customCfg = BilliardsConfig::desktop();
    customCfg.rule = BilliardsRule::EightBall;
    customCfg.iconSelection = IconSelectionPolicy::Custom;
    customCfg.customIconIndices = {1, 4, 9};
    customCfg.extraIcons = ExtraIconsPolicy::BottomGravity;
    customCfg.showBallNumbers = false;
    p.setBilliardsConfig(customCfg);
    p.setBilliards(true);
    ASSERT_TRUE(p.billiardsEnabled());
    EXPECT_EQ(billiards(p).objectBallCount(), 15u);
    EXPECT_EQ(billiards(p).extraIconCount(), 17u);

    const float extraYStart = billiards(p).iconCurrentSceneCenter(0).y;
    for (int i = 0; i < 120; ++i) p.tick(1.0 / 60.0);
    const float extraYSettled = billiards(p).iconCurrentSceneCenter(0).y;
    // Unselected icon 0 fell downward toward the bottom of the screen under gravity!
    EXPECT_GT(extraYSettled, extraYStart + 80.0f);
    EXPECT_NE(mirrorHit(p.drawList(), billiards(p).iconCurrentSceneCenter(0)), kNoTexture);

    // Toggling showBallNumbers live updates DrawList without re-racking or resetting settled extra icons!
    customCfg.showBallNumbers = true;
    p.setBilliardsConfig(customCfg);
    EXPECT_NEAR(billiards(p).iconCurrentSceneCenter(0).y, extraYSettled, 1.0f);
    EXPECT_TRUE(drawListContainsText(p.drawList(), "8"));

    // Exiting Billiards carries settled floor positions directly into GravityMode.
    p.setBilliards(false);
    const auto& gm = static_cast<const GravityMode&>(p.mode());
    EXPECT_NEAR(gm.bodySceneCenter(0).y, extraYSettled, 2.0f);
}

TEST(BilliardsMirror, NormalizedLanSyncMatchesAcrossDifferentResolutions) {
    BilliardsConfig cfg = BilliardsConfig::desktop();
    cfg.rule = BilliardsRule::EightBall;
    cfg.matchMode = BilliardsMatchMode::Lan;

    BilliardsMode host(desktopScene(15, {1440, 900}), cfg);
    BilliardsMode guest(desktopScene(4, {2560, 1440}), cfg);
    host.setLanLocalPlayer(0);
    guest.setLanLocalPlayer(1);

    // Initially it is Host's turn (player 0) -> Guest ignores local shot input.
    const Vec2 gw0 = guest.whiteSceneCenter();
    guest.handleInput({InputType::PointerDown, {gw0.x + 100.0f, gw0.y}, 0.1});
    EXPECT_EQ(guest.state(), BilliardsState::Ready);
    EXPECT_FALSE(guest.takeLocalLanAction().has_value());

    // Host fouls (shoots left into rail without hitting a ball) -> passes turn + Ball-in-Hand to Guest!
    const Vec2 hw0 = host.whiteSceneCenter();
    host.handleInput({InputType::PointerMove, {hw0.x - 80.0f, hw0.y}, 0.1});
    host.handleInput({InputType::PointerDown, {hw0.x - 80.0f, hw0.y}, 0.2});
    host.handleInput({InputType::PointerMove, {hw0.x - 40.0f, hw0.y}, 0.3});
    host.handleInput({InputType::PointerUp, {hw0.x - 40.0f, hw0.y}, 0.4});
    for (int i = 0; i < 400 && host.state() == BilliardsState::Rolling; ++i) {
        host.stepPhysics(1.0f / 60.0f);
        host.updateRules(1.0f / 60.0f);
    }
    ASSERT_EQ(host.currentPlayer(), 1);
    ASSERT_EQ(host.state(), BilliardsState::PlacingWhite);

    guest.applyLanSnapshot(host.exportLanSnapshot());
    ASSERT_EQ(guest.currentPlayer(), 1);
    ASSERT_EQ(guest.state(), BilliardsState::PlacingWhite);

    // Guest places cue ball on its 2560x1440 table and shoots!
    const Vec2 gPlace = guest.whiteSpawnScene();
    guest.handleInput({InputType::PointerDown, gPlace, 1.0});
    auto placeAct = guest.takeLocalLanAction();
    ASSERT_TRUE(placeAct.has_value());
    EXPECT_EQ(placeAct->kind, BilliardsRemoteAction::Kind::PlaceWhite);
    host.applyRemoteLanAction(*placeAct);
    ASSERT_EQ(host.state(), BilliardsState::Ready);

    const Vec2 gAim{gPlace.x + 120.0f, gPlace.y};
    guest.handleInput({InputType::PointerMove, gAim, 1.1});
    guest.handleInput({InputType::PointerDown, gAim, 1.2});
    guest.handleInput({InputType::PointerMove, {gAim.x - 140.0f, gAim.y}, 1.3});
    guest.handleInput({InputType::PointerUp, {gAim.x - 140.0f, gAim.y}, 1.4});
    auto shootAct = guest.takeLocalLanAction();
    ASSERT_TRUE(shootAct.has_value());
    EXPECT_EQ(shootAct->kind, BilliardsRemoteAction::Kind::Shoot);
    host.applyRemoteLanAction(*shootAct);
    ASSERT_EQ(host.state(), BilliardsState::Rolling);

    for (int i = 0; i < 420 && host.state() == BilliardsState::Rolling; ++i) {
        host.stepPhysics(1.0f / 60.0f);
        host.updateRules(1.0f / 60.0f);
    }
    const BilliardsLanSnapshot finalSnap = host.exportLanSnapshot();
    guest.applyLanSnapshot(finalSnap);
    const BilliardsLanSnapshot guestSnap = guest.exportLanSnapshot();
    ASSERT_EQ(guestSnap.balls.size(), finalSnap.balls.size());
    for (std::size_t i = 0; i < finalSnap.balls.size(); ++i) {
        EXPECT_NEAR(guestSnap.balls[i].normPos.x, finalSnap.balls[i].normPos.x, 1e-4f);
        EXPECT_NEAR(guestSnap.balls[i].normPos.y, finalSnap.balls[i].normPos.y, 1e-4f);
    }
}

TEST(BilliardsMirror, RecessedPocketsOpenJawsRailPassAndToggleableBorders) {
    BilliardsConfig cfg = BilliardsConfig::desktop();
    cfg.rule = BilliardsRule::EightBall;
    Playground p(desktopScene(15, {1440, 900}));
    p.setBilliardsConfig(cfg);
    p.setBilliards(true);
    auto& bm = mutableBilliards(p);

    const Vec2 tl = bm.tableMinScene();
    const Vec2 ts = bm.tableSizeScene();
    const Vec2 br{tl.x + ts.x, tl.y + ts.y};
    const float railOuter = bm.railOuterWidthScene();

    // 1. All 6 pocket holes are recessed outward from the inner cushion nose yet strictly inside the wood
    // apron!
    EXPECT_LT(bm.pocketScene(0).x, tl.x);
    EXPECT_LT(bm.pocketScene(0).y, tl.y);
    EXPECT_LT(bm.pocketScene(1).y, tl.y);
    for (std::size_t i = 0; i < 6; ++i) {
        const Vec2 pc = bm.pocketScene(i);
        const float hr = bm.pocketHoleRadiusScene(i);
        EXPECT_GE(pc.x - hr, tl.x - railOuter);
        EXPECT_LE(pc.x + hr, br.x + railOuter);
        EXPECT_GE(pc.y - hr, tl.y - railOuter);
        EXPECT_LE(pc.y + hr, br.y + railOuter);
    }

    // 2. Ball rolling parallel right along the top cushion passes the middle pocket cleanly without false
    // suction.
    const float r = bm.ballRadiusScene();
    const Vec2 midPocket = bm.pocketScene(1);
    bm.debugPlaceWhite({midPocket.x - r * 5.0f, tl.y + r * 1.15f}, {450.0f, 0.0f});
    for (int i = 0; i < 30; ++i) p.tick(1.0 / 60.0);
    EXPECT_FALSE(bm.whitePocketed());
    EXPECT_GT(bm.whiteSceneCenter().x, midPocket.x + r * 2.0f);

    // 3. Ball aimed directly into the middle pocket mouth enters the open jaw without bouncing off a closed
    // wall.
    bm.debugPlaceWhite({midPocket.x, tl.y + r * 3.0f}, {0.0f, -500.0f});
    for (int i = 0; i < 20 && !bm.whitePocketed(); ++i) p.tick(1.0 / 60.0);
    EXPECT_TRUE(bm.whitePocketed());

    // 4. Live visual border toggles (ball borders, table chrome, aim guide) update DrawList without
    // re-racking!
    p.reset();
    p.tick(0);
    const std::size_t fullCommands = p.drawList().commands().size();

    cfg.showBallBorders = false;
    p.setBilliardsConfig(cfg);
    const std::size_t borderlessCommands = p.drawList().commands().size();
    EXPECT_LT(borderlessCommands, fullCommands);

    cfg.showTableChrome = false;
    cfg.showAimGuide = false;
    p.setBilliardsConfig(cfg);
    const std::size_t minimalCommands = p.drawList().commands().size();
    EXPECT_LT(minimalCommands, borderlessCommands);
    // Icon balls remain hit-testable even with all borders hidden!
    EXPECT_NE(mirrorHit(p.drawList(), billiards(p).objectBallScene(0)), kNoTexture);
}

TEST(BilliardsMirror, AimGuideToggleAndTwoPlayerLeftP1RightP2Tubes) {
    // 1. On-canvas [辅助线: 开/关] toggle pill flips showAimGuide without shooting!
    BilliardsConfig freeTwoP = BilliardsConfig::desktop();
    freeTwoP.rule = BilliardsRule::Free;
    freeTwoP.matchMode = BilliardsMatchMode::Hotseat;
    Playground p(desktopScene(6, {1440, 900}));
    p.setBilliardsConfig(freeTwoP);
    p.setBilliards(true);

    const Vec2 aimBtn = billiards(p).aimToggleCenterScene();
    EXPECT_TRUE(p.billiardsTableContains(aimBtn));
    EXPECT_TRUE(p.billiardsConfig().showAimGuide);
    EXPECT_TRUE(drawListContainsText(p.drawList(), "辅助线: 开"));

    p.input({InputType::PointerDown, aimBtn, 0.1});
    p.input({InputType::PointerUp, aimBtn, 0.2});
    p.tick(0);
    EXPECT_EQ(p.mode().stats().shots, 0);
    EXPECT_FALSE(p.billiardsConfig().showAimGuide);
    EXPECT_TRUE(drawListContainsText(p.drawList(), "辅助线: 关"));

    // 2. Two-player Free mode: P1 pots into RIGHT corner pocket(2) -> goes to LEFT tube(0, P1);
    //    P2 pots into LEFT corner pocket(0) -> goes to RIGHT tube(1, P2)!
    auto& bm = mutableBilliards(p);
    const float rIn = bm.ballRadiusScene() * 1.1f;
    const Vec2 w0 = bm.whiteSceneCenter();
    // P1 misses shot -> switches turn to P2; first let P1 pot ball 0 via right pocket(2):
    p.input({InputType::PointerMove, {w0.x + 80.0f, w0.y}, 1.0});
    p.input({InputType::PointerDown, {w0.x + 80.0f, w0.y}, 1.1});
    p.input({InputType::PointerMove, {w0.x + 45.0f, w0.y}, 1.2});
    p.input({InputType::PointerUp, {w0.x + 45.0f, w0.y}, 1.3});
    p.tick(1.0 / 60.0);
    bm.debugPlaceBall(0, bm.pocketScene(2) + Vec2{-rIn, rIn}, {});  // Right-side pocket!
    for (int i = 0; i < 400 && bm.state() == BilliardsState::Rolling; ++i) p.tick(1.0 / 60.0);
    EXPECT_EQ(bm.tubeBallCount(0), 1u);  // Routed to Left Tube (P1)!
    EXPECT_EQ(bm.tubeBallCount(1), 0u);
    EXPECT_EQ(bm.ballTubeSide(0), 0);

    // P1 misses next shot -> switches turn to P2 (currentPlayer == 1).
    const Vec2 w1 = bm.whiteSceneCenter();
    p.input({InputType::PointerMove, {w1.x - 80.0f, w1.y}, 2.0});
    p.input({InputType::PointerDown, {w1.x - 80.0f, w1.y}, 2.1});
    p.input({InputType::PointerMove, {w1.x - 45.0f, w1.y}, 2.2});
    p.input({InputType::PointerUp, {w1.x - 45.0f, w1.y}, 2.3});
    for (int i = 0; i < 400 && bm.state() == BilliardsState::Rolling; ++i) p.tick(1.0 / 60.0);
    ASSERT_EQ(bm.currentPlayer(), 1);

    // P2 pots ball 1 into LEFT corner pocket(0) -> routed to Right Tube (P2)!
    const Vec2 w2 = bm.whiteSceneCenter();
    p.input({InputType::PointerMove, {w2.x - 80.0f, w2.y}, 3.0});
    p.input({InputType::PointerDown, {w2.x - 80.0f, w2.y}, 3.1});
    p.input({InputType::PointerMove, {w2.x - 45.0f, w2.y}, 3.2});
    p.input({InputType::PointerUp, {w2.x - 45.0f, w2.y}, 3.3});
    p.tick(1.0 / 60.0);
    bm.debugPlaceBall(1, bm.pocketScene(0) + Vec2{rIn, rIn}, {});  // Left-side pocket!
    for (int i = 0; i < 400 && bm.state() == BilliardsState::Rolling; ++i) p.tick(1.0 / 60.0);
    EXPECT_EQ(bm.tubeBallCount(0), 1u);
    EXPECT_EQ(bm.tubeBallCount(1), 1u);
    EXPECT_EQ(bm.ballTubeSide(1), 1);
    EXPECT_TRUE(drawListContainsText(p.drawList(), "P1 (1)"));
    EXPECT_TRUE(drawListContainsText(p.drawList(), "P2 (1)"));

    // 3. Two-player 8-Ball: break-potted Stripe ball automatically re-sorts into P2's right tube once P1
    // locks Solid!
    BilliardsConfig eightTwoP = BilliardsConfig::desktop();
    eightTwoP.rule = BilliardsRule::EightBall;
    eightTwoP.matchMode = BilliardsMatchMode::Hotseat;
    Playground p8(desktopScene(15, {1440, 900}));
    p8.setBilliardsConfig(eightTwoP);
    p8.setBilliards(true);
    auto& bm8 = mutableBilliards(p8);
    const float rIn8 = bm8.ballRadiusScene() * 1.1f;

    std::size_t stripeIdx = 14;  // slot 14 is guaranteed Stripe
    std::size_t solidIdx = 10;   // slot 10 is guaranteed Solid
    ASSERT_EQ(bm8.objectBallGroup(stripeIdx), BallGroup::Stripe);
    ASSERT_EQ(bm8.objectBallGroup(solidIdx), BallGroup::Solid);

    // Shot 1 (Break by P1): hits ball and pots a Stripe -> provisionally in P1's left tube while table is
    // open.
    const Vec2 bw = bm8.whiteSceneCenter();
    bm8.debugPlaceBall(stripeIdx, {bw.x + bm8.ballRadiusScene() * 3.0f, bw.y}, {});
    p8.input({InputType::PointerMove, {bw.x + 80.0f, bw.y}, 0.1});
    p8.input({InputType::PointerDown, {bw.x + 80.0f, bw.y}, 0.2});
    p8.input({InputType::PointerMove, {bw.x + 40.0f, bw.y}, 0.3});
    p8.input({InputType::PointerUp, {bw.x + 40.0f, bw.y}, 0.4});
    for (int i = 0; i < 6; ++i) p8.tick(1.0 / 60.0);
    bm8.debugPlaceBall(stripeIdx, bm8.pocketScene(0) + Vec2{rIn8, rIn8}, {});
    for (int i = 0; i < 400 && bm8.state() == BilliardsState::Rolling; ++i) p8.tick(1.0 / 60.0);
    ASSERT_EQ(bm8.playerGroup(0), BallGroup::FreeBall);

    // Shot 2 (Open table by P1): P1 hits & pots Solid -> P1 locks Solid, P2 gets Stripe,
    // so Solid sits in Left Tube (P1) and the break-potted Stripe moves to Right Tube (P2)!
    const Vec2 spot{bm8.tableMinScene().x + 180.0f, bm8.tableMinScene().y + 180.0f};
    bm8.debugPlaceWhite(spot, {});
    bm8.debugPlaceBall(solidIdx, {spot.x + bm8.ballRadiusScene() * 3.0f, spot.y}, {});
    p8.input({InputType::PointerMove, {spot.x + 80.0f, spot.y}, 1.0});
    p8.input({InputType::PointerDown, {spot.x + 80.0f, spot.y}, 1.1});
    p8.input({InputType::PointerMove, {spot.x + 40.0f, spot.y}, 1.2});
    p8.input({InputType::PointerUp, {spot.x + 40.0f, spot.y}, 1.3});
    for (int i = 0; i < 6; ++i) p8.tick(1.0 / 60.0);
    bm8.debugPlaceBall(solidIdx, bm8.pocketScene(2) + Vec2{-rIn8, rIn8}, {});
    for (int i = 0; i < 400 && bm8.state() == BilliardsState::Rolling; ++i) p8.tick(1.0 / 60.0);

    EXPECT_EQ(bm8.playerGroup(0), BallGroup::Solid);
    EXPECT_EQ(bm8.playerGroup(1), BallGroup::Stripe);
    EXPECT_EQ(bm8.ballTubeSide(solidIdx), 0);   // Left Tube = P1 (Solid)
    EXPECT_EQ(bm8.ballTubeSide(stripeIdx), 1);  // Right Tube = P2 (Stripe)
    EXPECT_EQ(bm8.tubeBallCount(0), 1u);
    EXPECT_EQ(bm8.tubeBallCount(1), 1u);
}

TEST(BilliardsMirror, LanCodecRoundTripAndArbitraryKeyOrderCompatibility) {
    // 1. Rule packet round-trip.
    const std::string ruleWire = lan_codec::encodeRule(BilliardsRule::Snooker);
    ASSERT_FALSE(ruleWire.empty());
    EXPECT_EQ(ruleWire.back(), '\n');
    const auto ruleDecoded = lan_codec::decodeLine(std::string_view(ruleWire.data(), ruleWire.size() - 1u));
    const auto* rp = std::get_if<lan_codec::RulePacket>(&ruleDecoded);
    ASSERT_NE(rp, nullptr);
    EXPECT_EQ(rp->rule, BilliardsRule::Snooker);

    // 2. Action packet round-trip + arbitrary macOS NSDictionary key order.
    BilliardsRemoteAction act;
    act.kind = BilliardsRemoteAction::Kind::Shoot;
    act.aimDir = {0.8f, -0.6f};
    act.power = 0.72f;
    act.tipOffset = {0.25f, -0.50f};
    act.placeNormPos = {0.22f, 0.48f};
    act.pullRatio = 0.72f;
    const std::string actWire = lan_codec::encodeAction(act);
    const auto actDecoded = lan_codec::decodeLine(std::string_view(actWire.data(), actWire.size() - 1u));
    const auto* ap = std::get_if<lan_codec::ActionPacket>(&actDecoded);
    ASSERT_NE(ap, nullptr);
    EXPECT_EQ(ap->action.kind, BilliardsRemoteAction::Kind::Shoot);
    EXPECT_NEAR(ap->action.power, 0.72f, 1e-4f);
    EXPECT_NEAR(ap->action.aimDir.x, 0.8f, 1e-4f);
    EXPECT_NEAR(ap->action.tipOffset.y, -0.50f, 1e-4f);

    const std::string shuffledMacAct =
        "{\"pw\":0.65,\"pl\":[0.3,0.4],\"ad\":[0.0,1.0],\"t\":\"act\",\"pr\":0.65,\"k\":1,\"to\":[-0.2,0.3]}";
    const auto macActDecoded = lan_codec::decodeLine(shuffledMacAct);
    const auto* map = std::get_if<lan_codec::ActionPacket>(&macActDecoded);
    ASSERT_NE(map, nullptr);
    EXPECT_EQ(map->action.kind, BilliardsRemoteAction::Kind::Shoot);
    EXPECT_NEAR(map->action.power, 0.65f, 1e-4f);
    EXPECT_NEAR(map->action.aimDir.y, 1.0f, 1e-4f);

    // 3. Full Snapshot packet round-trip with UTF-8 Chinese note, balls, and sound events.
    BilliardsConfig cfg = BilliardsConfig::desktop();
    cfg.rule = BilliardsRule::EightBall;
    cfg.matchMode = BilliardsMatchMode::Lan;
    Playground host(desktopScene(15, {1600, 900}));
    host.setBilliardsConfig(cfg);
    host.setBilliards(true);
    BilliardsLanSnapshot snap = billiards(host).exportLanSnapshot();
    std::snprintf(snap.statusNote.data(), snap.statusNote.size(), "%s", "P1定全色 · 继续击球");
    snap.scores = {3, 2};
    snap.playerGroups = {BallGroup::Solid, BallGroup::Stripe};
    const std::vector<SoundEvent> sounds = {{SoundKind::BallCollide, 0.82f, -0.35f, 1.04f},
                                            {SoundKind::PocketDrop, 0.90f, 0.60f, 0.96f}};
    const std::string snapWire = lan_codec::encodeSnapshot(snap, sounds);
    const auto snapDecoded = lan_codec::decodeLine(std::string_view(snapWire.data(), snapWire.size() - 1u));
    const auto* sp = std::get_if<lan_codec::SnapshotPacket>(&snapDecoded);
    ASSERT_NE(sp, nullptr);
    EXPECT_EQ(sp->snapshot.rule, BilliardsRule::EightBall);
    EXPECT_EQ(sp->snapshot.scores[0], 3);
    EXPECT_EQ(sp->snapshot.scores[1], 2);
    EXPECT_EQ(sp->snapshot.playerGroups[0], BallGroup::Solid);
    EXPECT_EQ(sp->snapshot.playerGroups[1], BallGroup::Stripe);
    EXPECT_STREQ(sp->snapshot.statusNote.data(), "P1定全色 · 继续击球");
    ASSERT_EQ(sp->snapshot.balls.size(), 15u);
    ASSERT_EQ(sp->sounds.size(), 2u);
    EXPECT_EQ(sp->sounds[0].kind, SoundKind::BallCollide);
    EXPECT_EQ(sp->sounds[1].kind, SoundKind::PocketDrop);
}

TEST(BilliardsMirror, VsAiPlayerFirstSmoothChoreographyAndInputGuard) {
    BilliardsConfig cfg = BilliardsConfig::desktop();
    cfg.rule = BilliardsRule::Free;
    cfg.matchMode = BilliardsMatchMode::VsAi;
    cfg.aiDifficulty = BilliardsAiDifficulty::Master;
    Playground pg(desktopScene(6, {1440, 900}));
    pg.setBilliardsConfig(cfg);
    pg.setBilliards(true);

    // 1. Player (P1 = 0) always breaks first and scene settles to idle waiting for human input.
    EXPECT_EQ(billiards(pg).currentPlayer(), 0);
    EXPECT_FALSE(billiards(pg).isAiTurn());
    for (int i = 0; i < 60 && !pg.idle(); ++i) pg.tick(1.0 / 60.0);
    EXPECT_TRUE(pg.idle());
    EXPECT_TRUE(drawListContainsText(pg.drawList(), "我方回合"));
    EXPECT_TRUE(drawListContainsText(pg.drawList(), "玩家 (0)"));
    EXPECT_TRUE(drawListContainsText(pg.drawList(), "人机 (0)"));

    // Place ball #0 right in front of top-right corner pocket so AI has a clean pot after human misses.
    auto& bm = mutableBilliards(pg);
    const Vec2 pocket2 = bm.pocketScene(2);
    const float r = bm.ballRadiusScene();
    bm.debugPlaceBall(0, {pocket2.x - 4.2f * r, pocket2.y + 4.2f * r}, {0.0f, 0.0f});

    // Human shoots a gentle vertical miss so turn passes to AI (Player 1).
    const Vec2 w0 = bm.whiteSceneCenter();
    pg.input({InputType::PointerDown, {w0.x, w0.y - 80.0f}, 0.1});
    pg.input({InputType::PointerMove, {w0.x, w0.y - 58.0f}, 0.2});
    pg.input({InputType::PointerUp, {w0.x, w0.y - 58.0f}, 0.3});
    for (int step = 0; step < 600 && billiards(pg).state() == BilliardsState::Rolling; ++step) {
        pg.tick(1.0 / 60.0);
    }

    ASSERT_EQ(billiards(pg).currentPlayer(), 1);
    EXPECT_TRUE(billiards(pg).isAiTurn());
    EXPECT_FALSE(pg.idle());

    // 2. Advance into AI aiming phase (~0.25s) and verify HUD, input guard, and live difficulty update.
    for (int i = 0; i < 15; ++i) pg.tick(1.0 / 60.0);
    EXPECT_EQ(billiards(pg).state(), BilliardsState::Ready);
    EXPECT_TRUE(drawListContainsText(pg.drawList(), "人机回合(大师)"));

    // Human mouse drag during AI turn must not hijack cue or set grabbing=true, while HUD aim pill still
    // works.
    pg.input({InputType::PointerDown, {w0.x - 120.0f, w0.y}, 2.0});
    pg.input({InputType::PointerMove, {w0.x + 120.0f, w0.y}, 2.1});
    EXPECT_FALSE(pg.mode().stats().grabbing);
    const bool guideBefore = pg.billiardsConfig().showAimGuide;
    pg.input({InputType::PointerDown, billiards(pg).aimToggleCenterScene(), 2.2});
    EXPECT_NE(pg.billiardsConfig().showAimGuide, guideBefore);

    // 3. Step through AI feathering + pull-back Charging phase (~2.65s total turn pace) and release into
    // Rolling.
    bool sawChargingPullback = false;
    bool sawCueStrikeSound = false;
    int framesUntilShot = 15;
    std::vector<SoundEvent> sounds;
    for (int i = 0; i < 220 && billiards(pg).state() != BilliardsState::Rolling; ++i) {
        pg.tick(1.0 / 60.0);
        ++framesUntilShot;
        if (billiards(pg).state() == BilliardsState::Charging && billiards(pg).pullDistancePoints() > 12.0f) {
            sawChargingPullback = true;
            EXPECT_FALSE(pg.mode().stats().grabbing);
        }
        pg.drainSounds(sounds);
        for (const auto& s : sounds) {
            if (s.kind == SoundKind::CueStrike) sawCueStrikeSound = true;
        }
    }
    EXPECT_GE(framesUntilShot, 130);  // Deliberate ~2.6s human-like pace (never rushed < 2.1s)
    EXPECT_TRUE(sawChargingPullback);
    EXPECT_TRUE(sawCueStrikeSound);
    EXPECT_EQ(billiards(pg).state(), BilliardsState::Rolling);

    // Let AI's shot finish rolling; AI should pot into Right Tube (side = 1 = AI) and keep its turn!
    for (int step = 0; step < 900 && billiards(pg).state() == BilliardsState::Rolling; ++step) {
        pg.tick(1.0 / 60.0);
    }
    EXPECT_GE(billiards(pg).score(1), 1);
    EXPECT_GE(billiards(pg).tubeBallCount(1), 1u);
    EXPECT_EQ(billiards(pg).currentPlayer(), 1);
}

TEST(BilliardsMirror, VsAiEightBallFoulPlacementSnookerKickAndDifficultyTiers) {
    // 1. 8-Ball foul gives AI ball-in-hand (`PlacingWhite`): AI smoothly places white and pots legally.
    {
        BilliardsConfig cfg = BilliardsConfig::desktop();
        cfg.rule = BilliardsRule::EightBall;
        cfg.matchMode = BilliardsMatchMode::VsAi;
        cfg.aiDifficulty = BilliardsAiDifficulty::Master;
        Playground pg(desktopScene(15, {1440, 900}));
        pg.setBilliardsConfig(cfg);
        pg.setBilliards(true);

        // Human misses everything gently -> foul -> AI gets ball-in-hand (`PlacingWhite`).
        const Vec2 w = billiards(pg).whiteSceneCenter();
        pg.input({InputType::PointerDown, {w.x, w.y - 80.0f}, 0.1});
        pg.input({InputType::PointerMove, {w.x, w.y - 58.0f}, 0.2});
        pg.input({InputType::PointerUp, {w.x, w.y - 58.0f}, 0.3});
        for (int step = 0; step < 600 && billiards(pg).state() == BilliardsState::Rolling; ++step) {
            pg.tick(1.0 / 60.0);
        }
        ASSERT_EQ(billiards(pg).currentPlayer(), 1);
        ASSERT_EQ(billiards(pg).state(), BilliardsState::PlacingWhite);
        EXPECT_TRUE(drawListContainsText(pg.drawList(), "自由球自动选位中"));

        // Run ~4.2s so AI completes deliberate white placement (~1.40s) + aiming (~1.50s) + pullback
        // (~1.15s).
        for (int step = 0; step < 300 && billiards(pg).state() != BilliardsState::Rolling; ++step) {
            pg.tick(1.0 / 60.0);
        }
        EXPECT_EQ(billiards(pg).whitePlacements(), 1);
        EXPECT_FALSE(billiards(pg).whitePocketed());
        EXPECT_EQ(billiards(pg).state(), BilliardsState::Rolling);
    }

    // 2. 1-Cushion Kick Escape when AI's only legal ball is directly snookered behind an opponent ball.
    {
        BilliardsConfig cfg = BilliardsConfig::desktop();
        cfg.rule = BilliardsRule::EightBall;
        cfg.matchMode = BilliardsMatchMode::VsAi;
        cfg.aiDifficulty = BilliardsAiDifficulty::Master;
        Playground pg(desktopScene(15, {1440, 900}));
        pg.setBilliardsConfig(cfg);
        pg.setBilliards(true);
        auto& bm = mutableBilliards(pg);

        const Vec2 tMin = bm.tableMinScene();
        const Vec2 tSize = bm.tableSizeScene();
        const float r = bm.ballRadiusScene();

        // Park unneeded balls near bottom-right; keep one AI Solid (#1) and one blocking P1 Stripe (#9).
        int solidIdx = -1, blockerIdx = -1;
        for (std::size_t i = 0; i < bm.objectBallCount(); ++i) {
            if (bm.objectBallGroup(i) == BallGroup::Solid && solidIdx < 0) {
                solidIdx = static_cast<int>(i);
            } else if (bm.objectBallGroup(i) == BallGroup::Stripe && blockerIdx < 0) {
                blockerIdx = static_cast<int>(i);
            } else {
                bm.mutableWorld().setEnabled(static_cast<BodyIndex>(i + 1), false);
            }
        }
        ASSERT_GE(solidIdx, 0);
        ASSERT_GE(blockerIdx, 0);

        const Vec2 whiteSpot{tMin.x + tSize.x * 0.28f, tMin.y + tSize.y * 0.55f};
        const Vec2 targetSpot{tMin.x + tSize.x * 0.62f, tMin.y + tSize.y * 0.55f};
        // Blocker sits right on the direct line just in front of white ball, leaving top & bottom rails open!
        const Vec2 blockSpot{whiteSpot.x + 4.2f * r, whiteSpot.y};
        bm.debugPlaceWhite(whiteSpot, {0.0f, 0.0f});
        bm.debugPlaceBall(static_cast<std::size_t>(solidIdx), targetSpot, {0.0f, 0.0f});
        bm.debugPlaceBall(static_cast<std::size_t>(blockerIdx), blockSpot, {0.0f, 0.0f});
        bm.debugSetTurnState(1, BallGroup::Stripe, BallGroup::Solid);

        bool bouncedRail = false;
        std::vector<SoundEvent> sounds;
        for (int step = 0; step < 900; ++step) {
            pg.tick(1.0 / 60.0);
            pg.drainSounds(sounds);
            for (const auto& s : sounds) {
                if (s.kind == SoundKind::RailBounce) bouncedRail = true;
            }
            if (billiards(pg).stats().shots >= 1 && billiards(pg).state() != BilliardsState::Rolling) break;
        }
        EXPECT_TRUE(bouncedRail);
        // Escaped the snooker legally (no ball-in-hand foul handed back to P1)!
        EXPECT_NE(billiards(pg).state(), BilliardsState::PlacingWhite);
    }

    // 3. Difficulty calibration: Master pots long thin cut shots consistently better than Easy.
    auto countLongCutPots = [](BilliardsAiDifficulty diff) {
        int pots = 0;
        for (std::uint32_t trial = 0; trial < 16; ++trial) {
            BilliardsConfig cfg = BilliardsConfig::desktop();
            cfg.rule = BilliardsRule::Free;
            cfg.matchMode = BilliardsMatchMode::VsAi;
            cfg.aiDifficulty = diff;
            cfg.randomSeed = 0x515000u + trial * 7919u;
            BilliardsMode m(desktopScene(1, {1440, 900}), cfg);
            const Vec2 tMin = m.tableMinScene();
            const Vec2 tSize = m.tableSizeScene();
            const Vec2 p2 = m.pocketScene(2);
            const float r = m.ballRadiusScene();
            // Long thin cut across the table into top-right corner pocket.
            m.debugPlaceWhite({tMin.x + tSize.x * 0.22f, tMin.y + tSize.y * 0.65f}, {0.0f, 0.0f});
            m.debugPlaceBall(0, {p2.x - 10.0f * r, p2.y + 9.5f * r}, {0.0f, 0.0f});
            m.debugSetTurnState(1);
            for (int step = 0; step < 750; ++step) {
                m.stepPhysics(1.0f / 60.0f);
                m.updateRules(1.0f / 60.0f);
                if (m.stats().shots >= 1 && m.state() != BilliardsState::Rolling) break;
            }
            if (m.stats().pocketed >= 1) ++pots;
        }
        return pots;
    };
    const int easyPots = countLongCutPots(BilliardsAiDifficulty::Easy);
    const int masterPots = countLongCutPots(BilliardsAiDifficulty::Master);
    EXPECT_GT(masterPots, easyPots);
    EXPECT_GE(masterPots, 14);

    // 4. Snooker VS AI + live mid-game difficulty switch without resetting the rack.
    {
        BilliardsConfig cfg = BilliardsConfig::desktop();
        cfg.rule = BilliardsRule::Snooker;
        cfg.matchMode = BilliardsMatchMode::VsAi;
        cfg.aiDifficulty = BilliardsAiDifficulty::Easy;
        Playground pg(desktopScene(21, {1600, 960}));
        pg.setBilliardsConfig(cfg);
        pg.setBilliards(true);

        auto& bm = mutableBilliards(pg);
        const Vec2 p2 = bm.pocketScene(2);
        const float r = bm.ballRadiusScene();
        // Place Black (#7, index 20) in front of top-right corner pocket and cue ball aligned for a clean
        // pot.
        bm.debugPlaceBall(20, {p2.x - 4.5f * r, p2.y + 4.5f * r}, {0.0f, 0.0f});
        bm.debugPlaceWhite({p2.x - 14.0f * r, p2.y + 13.0f * r}, {0.0f, 0.0f});
        bm.debugSetTurnState(1, BallGroup::FreeBall, BallGroup::FreeBall, /*snookerTarget=*/8);

        // Live-switch difficulty Easy -> Master mid-game; verify ball #20 position is NOT reset!
        const Vec2 blackBefore = billiards(pg).objectBallScene(20);
        cfg.aiDifficulty = BilliardsAiDifficulty::Master;
        pg.setBilliardsConfig(cfg);
        EXPECT_EQ(billiards(pg).config().aiDifficulty, BilliardsAiDifficulty::Master);
        EXPECT_NEAR(billiards(pg).objectBallScene(20).x, blackBefore.x, 1e-3f);
        EXPECT_NEAR(billiards(pg).objectBallScene(20).y, blackBefore.y, 1e-3f);

        for (int step = 0; step < 900; ++step) {
            pg.tick(1.0 / 60.0);
            if (billiards(pg).stats().shots >= 1 && billiards(pg).state() != BilliardsState::Rolling) break;
        }
        // AI potted Black (7 points) legally on snookerTarget==8, Black re-spotted, and target returned to
        // Red (1)!
        EXPECT_EQ(billiards(pg).score(1), 7);
        EXPECT_EQ(billiards(pg).snookerTarget(), 1);
        EXPECT_EQ(billiards(pg).currentPlayer(), 1);
    }
}

TEST(BilliardsMirror, VsAiMasterCombinationRunoutAndDefensiveSafety) {
    // 1. Two-ball combination / plant ("传球"): direct pots blocked, Master plants Solid #1 -> Solid #2 into
    // pocket!
    {
        BilliardsConfig cfg = BilliardsConfig::desktop();
        cfg.rule = BilliardsRule::EightBall;
        cfg.matchMode = BilliardsMatchMode::VsAi;
        cfg.aiDifficulty = BilliardsAiDifficulty::Master;
        Playground pg(desktopScene(15, {1440, 900}));
        pg.setBilliardsConfig(cfg);
        pg.setBilliards(true);
        auto& bm = mutableBilliards(pg);

        int solidA = -1, solidB = -1, stripeBlocker = -1, eightIdx = -1;
        for (std::size_t i = 0; i < bm.objectBallCount(); ++i) {
            if (bm.objectBallGroup(i) == BallGroup::Solid) {
                if (solidA < 0) {
                    solidA = static_cast<int>(i);
                } else if (solidB < 0) {
                    solidB = static_cast<int>(i);
                } else {
                    bm.mutableWorld().setEnabled(static_cast<BodyIndex>(i + 1), false);
                }
            } else if (bm.objectBallGroup(i) == BallGroup::Stripe && stripeBlocker < 0) {
                stripeBlocker = static_cast<int>(i);
            } else if (bm.objectBallGroup(i) == BallGroup::Eight) {
                eightIdx = static_cast<int>(i);
            } else {
                bm.mutableWorld().setEnabled(static_cast<BodyIndex>(i + 1), false);
            }
        }
        ASSERT_GE(solidA, 0);
        ASSERT_GE(solidB, 0);
        ASSERT_GE(stripeBlocker, 0);
        ASSERT_GE(eightIdx, 0);

        const Vec2 tMin = bm.tableMinScene();
        const Vec2 tSize = bm.tableSizeScene();
        const Vec2 p2 = bm.pocketScene(2);
        const float r = bm.ballRadiusScene();

        // Solid B sits near top-right corner pocket; Solid A sits further back along the diagonal, so Solid B
        // blocks Solid A's direct pot, while Stripe blocker hides Solid B from direct cue-ball contact!
        const Vec2 diag{-0.70710678f, 0.70710678f};
        const Vec2 posB{p2.x + diag.x * (4.2f * r), p2.y + diag.y * (4.2f * r)};
        const Vec2 posA{p2.x + diag.x * (10.5f * r), p2.y + diag.y * (10.5f * r)};
        const Vec2 whitePos{tMin.x + tSize.x * 0.56f, tMin.y + tSize.y * 0.44f};
        const Vec2 blockDir = (posB - whitePos) * 0.55f;
        bm.debugPlaceWhite(whitePos, {0.0f, 0.0f});
        bm.debugPlaceBall(static_cast<std::size_t>(solidA), posA, {0.0f, 0.0f});
        bm.debugPlaceBall(static_cast<std::size_t>(solidB), posB, {0.0f, 0.0f});
        bm.debugPlaceBall(static_cast<std::size_t>(stripeBlocker), whitePos + blockDir, {0.0f, 0.0f});
        bm.debugPlaceBall(static_cast<std::size_t>(eightIdx),
                          {tMin.x + tSize.x * 0.18f, tMin.y + tSize.y * 0.80f}, {0.0f, 0.0f});
        bm.debugSetTurnState(1, BallGroup::Stripe, BallGroup::Solid);

        for (int step = 0; step < 900; ++step) {
            pg.tick(1.0 / 60.0);
            if (billiards(pg).stats().shots >= 1 && billiards(pg).state() != BilliardsState::Rolling) break;
        }
        EXPECT_GE(billiards(pg).tubeBallCount(1), 1u);
        EXPECT_EQ(billiards(pg).currentPlayer(), 1);
        EXPECT_NE(billiards(pg).state(), BilliardsState::PlacingWhite);
    }

    // 2. Multi-ball 8-Ball endgame runout: Master clears 2 remaining Solids + #8 in one uninterrupted visit!
    {
        BilliardsConfig cfg = BilliardsConfig::desktop();
        cfg.rule = BilliardsRule::EightBall;
        cfg.matchMode = BilliardsMatchMode::VsAi;
        cfg.aiDifficulty = BilliardsAiDifficulty::Master;
        Playground pg(desktopScene(15, {1440, 900}));
        pg.setBilliardsConfig(cfg);
        pg.setBilliards(true);
        auto& bm = mutableBilliards(pg);

        std::vector<int> solids;
        int stripeIdx = -1, eightIdx = -1;
        for (std::size_t i = 0; i < bm.objectBallCount(); ++i) {
            const BallGroup g = bm.objectBallGroup(i);
            if (g == BallGroup::Solid && solids.size() < 2) {
                solids.push_back(static_cast<int>(i));
            } else if (g == BallGroup::Stripe && stripeIdx < 0) {
                stripeIdx = static_cast<int>(i);
            } else if (g == BallGroup::Eight) {
                eightIdx = static_cast<int>(i);
            } else {
                bm.mutableWorld().setEnabled(static_cast<BodyIndex>(i + 1), false);
            }
        }
        ASSERT_EQ(solids.size(), 2u);
        ASSERT_GE(stripeIdx, 0);
        ASSERT_GE(eightIdx, 0);

        const Vec2 tMin = bm.tableMinScene();
        const Vec2 tSize = bm.tableSizeScene();
        const Vec2 p1 = bm.pocketScene(1);  // Top middle
        const Vec2 p2 = bm.pocketScene(2);  // Top right
        const Vec2 p5 = bm.pocketScene(5);  // Bottom right
        const float r = bm.ballRadiusScene();

        bm.debugPlaceWhite({tMin.x + tSize.x * 0.42f, tMin.y + tSize.y * 0.42f}, {0.0f, 0.0f});
        bm.debugPlaceBall(static_cast<std::size_t>(solids[0]), {p1.x, p1.y + 5.0f * r}, {0.0f, 0.0f});
        bm.debugPlaceBall(static_cast<std::size_t>(solids[1]), {p2.x - 5.5f * r, p2.y + 5.5f * r},
                          {0.0f, 0.0f});
        bm.debugPlaceBall(static_cast<std::size_t>(eightIdx), {p5.x - 5.5f * r, p5.y - 5.5f * r},
                          {0.0f, 0.0f});
        bm.debugPlaceBall(static_cast<std::size_t>(stripeIdx),
                          {tMin.x + tSize.x * 0.14f, tMin.y + tSize.y * 0.50f}, {0.0f, 0.0f});
        bm.debugSetTurnState(1, BallGroup::Stripe, BallGroup::Solid);

        // Master AI should pot Solid #0, Solid #1, and #8 consecutively without ever handing turn back to P1!
        for (int step = 0; step < 2600 && billiards(pg).state() != BilliardsState::Finished; ++step) {
            pg.tick(1.0 / 60.0);
            EXPECT_EQ(billiards(pg).currentPlayer(), 1);
        }
        EXPECT_EQ(billiards(pg).state(), BilliardsState::Finished);
        EXPECT_EQ(billiards(pg).winner(), 1);
    }

    // 3. Tactical Safety when no pot/bank exists: Master plays a legal containment shot without fouling!
    {
        BilliardsConfig cfg = BilliardsConfig::desktop();
        cfg.rule = BilliardsRule::EightBall;
        cfg.matchMode = BilliardsMatchMode::VsAi;
        cfg.aiDifficulty = BilliardsAiDifficulty::Master;
        Playground pg(desktopScene(15, {1440, 900}));
        pg.setBilliardsConfig(cfg);
        pg.setBilliards(true);
        auto& bm = mutableBilliards(pg);

        int solidIdx = -1;
        std::vector<int> stripes;
        int eightIdx = -1;
        for (std::size_t i = 0; i < bm.objectBallCount(); ++i) {
            const BallGroup g = bm.objectBallGroup(i);
            if (g == BallGroup::Solid && solidIdx < 0) {
                solidIdx = static_cast<int>(i);
            } else if (g == BallGroup::Stripe && stripes.size() < 4) {
                stripes.push_back(static_cast<int>(i));
            } else if (g == BallGroup::Eight) {
                eightIdx = static_cast<int>(i);
            } else {
                bm.mutableWorld().setEnabled(static_cast<BodyIndex>(i + 1), false);
            }
        }
        ASSERT_GE(solidIdx, 0);
        ASSERT_EQ(stripes.size(), 4u);
        ASSERT_GE(eightIdx, 0);

        const Vec2 tMin = bm.tableMinScene();
        const Vec2 tSize = bm.tableSizeScene();
        const float r = bm.ballRadiusScene();

        // AI's single Solid sits near center of left rail; opponent Stripes guard nearby pockets.
        bm.debugPlaceWhite({tMin.x + tSize.x * 0.48f, tMin.y + tSize.y * 0.50f}, {0.0f, 0.0f});
        bm.debugPlaceBall(static_cast<std::size_t>(solidIdx), {tMin.x + 2.4f * r, tMin.y + tSize.y * 0.50f},
                          {0.0f, 0.0f});
        bm.debugPlaceBall(static_cast<std::size_t>(stripes[0]),
                          {bm.pocketScene(0).x + 3.2f * r, bm.pocketScene(0).y + 3.2f * r}, {0.0f, 0.0f});
        bm.debugPlaceBall(static_cast<std::size_t>(stripes[1]),
                          {bm.pocketScene(3).x + 3.2f * r, bm.pocketScene(3).y - 3.2f * r}, {0.0f, 0.0f});
        bm.debugPlaceBall(static_cast<std::size_t>(stripes[2]),
                          {bm.pocketScene(1).x, bm.pocketScene(1).y + 3.0f * r}, {0.0f, 0.0f});
        bm.debugPlaceBall(static_cast<std::size_t>(stripes[3]),
                          {bm.pocketScene(4).x, bm.pocketScene(4).y - 3.0f * r}, {0.0f, 0.0f});
        bm.debugPlaceBall(static_cast<std::size_t>(eightIdx),
                          {tMin.x + tSize.x * 0.72f, tMin.y + tSize.y * 0.50f}, {0.0f, 0.0f});
        bm.debugSetTurnState(1, BallGroup::Stripe, BallGroup::Solid);

        for (int step = 0; step < 900; ++step) {
            pg.tick(1.0 / 60.0);
            if (billiards(pg).stats().shots >= 1 && billiards(pg).state() != BilliardsState::Rolling) break;
        }
        // Legal contact with zero scratch/foul (no ball-in-hand gifted to P1)!
        EXPECT_GE(billiards(pg).stats().shots, 1);
        EXPECT_NE(billiards(pg).state(), BilliardsState::PlacingWhite);
        EXPECT_EQ(billiards(pg).winner(), -1);
    }

    // 4. 21-Ball Snooker Break Building: Master pots Red (1 pt), plays position onto Black/Pink/Blue, and
    // pots Color without cannon fouls!
    {
        BilliardsConfig cfg = BilliardsConfig::desktop();
        cfg.rule = BilliardsRule::Snooker;
        cfg.matchMode = BilliardsMatchMode::VsAi;
        cfg.aiDifficulty = BilliardsAiDifficulty::Master;
        Playground pg(desktopScene(21, {1600, 960}));
        pg.setBilliardsConfig(cfg);
        pg.setBilliards(true);
        auto& bm = mutableBilliards(pg);

        const Vec2 tMin = bm.tableMinScene();
        const Vec2 tSize = bm.tableSizeScene();
        const Vec2 p2 = bm.pocketScene(2);
        const Vec2 p5 = bm.pocketScene(5);
        const float r = bm.ballRadiusScene();

        // Open two playable Reds near the right corners while Black (#20) sits on its spot for high-value
        // follow-up.
        bm.debugPlaceBall(0, {p2.x - 5.2f * r, p2.y + 5.2f * r}, {0.0f, 0.0f});
        bm.debugPlaceBall(1, {p5.x - 5.6f * r, p5.y - 5.6f * r}, {0.0f, 0.0f});
        bm.debugPlaceWhite({tMin.x + tSize.x * 0.78f, tMin.y + tSize.y * 0.36f}, {0.0f, 0.0f});
        bm.debugSetTurnState(1, BallGroup::FreeBall, BallGroup::FreeBall, /*snookerTarget=*/1);

        // Run ~20s (1200 frames) so AI can execute Red -> Color -> Red break!
        for (int step = 0; step < 1200 && billiards(pg).currentPlayer() == 1; ++step) {
            pg.tick(1.0 / 60.0);
            if (billiards(pg).score(1) >= 8 && billiards(pg).state() != BilliardsState::Rolling) break;
        }
        EXPECT_EQ(billiards(pg).score(0), 0);  // Zero foul points gifted to human!
        EXPECT_GE(billiards(pg).score(1), 6);  // Potted at least Red + Color in one visit!
    }
}

TEST(BilliardsMirror, VsAiGoatMultiBallClearanceDoubleSnookerEscapeAndAdaptivePacing) {
    // 1. HUD label ("球王"), adaptive crisp routine tempo (~2.07s), and uninterrupted 4-ball (3 Solids + #8)
    // runout!
    int routineShotFrames = 0;
    {
        BilliardsConfig cfg = BilliardsConfig::desktop();
        cfg.rule = BilliardsRule::EightBall;
        cfg.matchMode = BilliardsMatchMode::VsAi;
        cfg.aiDifficulty = BilliardsAiDifficulty::Goat;
        Playground pg(desktopScene(15, {1440, 900}));
        pg.setBilliardsConfig(cfg);
        pg.setBilliards(true);
        auto& bm = mutableBilliards(pg);

        std::vector<int> solids;
        int stripeIdx = -1, eightIdx = -1;
        for (std::size_t i = 0; i < bm.objectBallCount(); ++i) {
            const BallGroup g = bm.objectBallGroup(i);
            if (g == BallGroup::Solid && solids.size() < 3) {
                solids.push_back(static_cast<int>(i));
            } else if (g == BallGroup::Stripe && stripeIdx < 0) {
                stripeIdx = static_cast<int>(i);
            } else if (g == BallGroup::Eight) {
                eightIdx = static_cast<int>(i);
            } else {
                bm.mutableWorld().setEnabled(static_cast<BodyIndex>(i + 1), false);
            }
        }
        ASSERT_EQ(solids.size(), 3u);
        ASSERT_GE(stripeIdx, 0);
        ASSERT_GE(eightIdx, 0);

        const Vec2 tMin = bm.tableMinScene();
        const Vec2 tSize = bm.tableSizeScene();
        const Vec2 p0 = bm.pocketScene(0);  // Top left
        const Vec2 p1 = bm.pocketScene(1);  // Top middle
        const Vec2 p2 = bm.pocketScene(2);  // Top right
        const Vec2 p5 = bm.pocketScene(5);  // Bottom right
        const float r = bm.ballRadiusScene();

        // Routine straight starter on p1, followed by p2 -> p0/p5 -> #8!
        bm.debugPlaceBall(static_cast<std::size_t>(solids[0]), {p1.x, p1.y + 4.8f * r}, {0.0f, 0.0f});
        bm.debugPlaceWhite({p1.x, p1.y + 12.5f * r}, {0.0f, 0.0f});
        bm.debugPlaceBall(static_cast<std::size_t>(solids[1]), {p2.x - 5.5f * r, p2.y + 5.5f * r},
                          {0.0f, 0.0f});
        bm.debugPlaceBall(static_cast<std::size_t>(solids[2]), {p0.x + 5.5f * r, p0.y + 5.5f * r},
                          {0.0f, 0.0f});
        bm.debugPlaceBall(static_cast<std::size_t>(eightIdx), {p5.x - 5.2f * r, p5.y - 5.2f * r},
                          {0.0f, 0.0f});
        bm.debugPlaceBall(static_cast<std::size_t>(stripeIdx),
                          {tMin.x + tSize.x * 0.14f, tMin.y + tSize.y * 0.62f}, {0.0f, 0.0f});
        bm.debugSetTurnState(1, BallGroup::Stripe, BallGroup::Solid);

        for (int i = 0; i < 10; ++i) pg.tick(1.0 / 60.0);
        EXPECT_TRUE(drawListContainsText(pg.drawList(), "人机回合(球王)"));
        routineShotFrames = 10;
        while (routineShotFrames < 220 && billiards(pg).state() != BilliardsState::Rolling) {
            pg.tick(1.0 / 60.0);
            ++routineShotFrames;
        }
        EXPECT_EQ(billiards(pg).state(), BilliardsState::Rolling);

        // Let Goat complete the entire 4-ball clearance without ever relinquishing the table!
        for (int step = 0; step < 3600 && billiards(pg).state() != BilliardsState::Finished; ++step) {
            pg.tick(1.0 / 60.0);
            EXPECT_EQ(billiards(pg).currentPlayer(), 1);
        }
        EXPECT_EQ(billiards(pg).state(), BilliardsState::Finished);
        EXPECT_EQ(billiards(pg).winner(), 1);
    }

    // 2. Heavy Multi-Blocker Snooker Escape: direct + 1-rail paths blocked by wall of Stripes -> Goat finds
    // 360° multi-rail zero-foul escape (never gifts ball-in-hand) with longer deliberate pacing (~3.0s)!
    int complexEscapeFrames = 0;
    {
        BilliardsConfig cfg = BilliardsConfig::desktop();
        cfg.rule = BilliardsRule::EightBall;
        cfg.matchMode = BilliardsMatchMode::VsAi;
        cfg.aiDifficulty = BilliardsAiDifficulty::Goat;
        Playground pg(desktopScene(15, {1440, 900}));
        pg.setBilliardsConfig(cfg);
        pg.setBilliards(true);
        auto& bm = mutableBilliards(pg);

        int solidIdx = -1;
        std::vector<int> blockers;
        for (std::size_t i = 0; i < bm.objectBallCount(); ++i) {
            const BallGroup g = bm.objectBallGroup(i);
            if (g == BallGroup::Solid && solidIdx < 0) {
                solidIdx = static_cast<int>(i);
            } else if (g == BallGroup::Stripe && blockers.size() < 3) {
                blockers.push_back(static_cast<int>(i));
            } else {
                bm.mutableWorld().setEnabled(static_cast<BodyIndex>(i + 1), false);
            }
        }
        ASSERT_GE(solidIdx, 0);
        ASSERT_EQ(blockers.size(), 3u);

        const Vec2 tMin = bm.tableMinScene();
        const Vec2 tSize = bm.tableSizeScene();
        const float r = bm.ballRadiusScene();

        // Cue ball surrounded on the forward arc by 3 opponent Stripes, forcing a multi-rail / back-rail
        // Z-kick to reach AI's lone Solid on the right half!
        const Vec2 wSpot{tMin.x + tSize.x * 0.28f, tMin.y + tSize.y * 0.50f};
        bm.debugPlaceWhite(wSpot, {0.0f, 0.0f});
        bm.debugPlaceBall(static_cast<std::size_t>(solidIdx),
                          {tMin.x + tSize.x * 0.72f, tMin.y + tSize.y * 0.50f}, {0.0f, 0.0f});
        bm.debugPlaceBall(static_cast<std::size_t>(blockers[0]), {wSpot.x + 3.6f * r, wSpot.y}, {0.0f, 0.0f});
        bm.debugPlaceBall(static_cast<std::size_t>(blockers[1]), {wSpot.x + 3.3f * r, wSpot.y - 2.3f * r},
                          {0.0f, 0.0f});
        bm.debugPlaceBall(static_cast<std::size_t>(blockers[2]), {wSpot.x + 3.3f * r, wSpot.y + 2.3f * r},
                          {0.0f, 0.0f});
        bm.debugSetTurnState(1, BallGroup::Stripe, BallGroup::Solid);

        while (complexEscapeFrames < 240 && billiards(pg).state() != BilliardsState::Rolling) {
            pg.tick(1.0 / 60.0);
            ++complexEscapeFrames;
        }
        EXPECT_EQ(billiards(pg).state(), BilliardsState::Rolling);
        // Adaptive tempo: complex snooker escape takes noticeably longer than a routine open pot!
        EXPECT_GT(complexEscapeFrames, routineShotFrames + 15);

        for (int step = 0; step < 900 && billiards(pg).state() == BilliardsState::Rolling; ++step) {
            pg.tick(1.0 / 60.0);
        }
        // Ironclad zero-foul escape: never hands ball-in-hand (`PlacingWhite`) to human!
        EXPECT_NE(billiards(pg).state(), BilliardsState::PlacingWhite);
    }
}

TEST(BilliardsMirror, SeriesMatchAlternatingBreakAiIntentToggleSnookerBreakAndBaulkSafety) {
    // 1. Series score + on-table `[再来一局 ↻]` rematch pill + alternating AI power break + `showAiIntent`
    // toggle!
    {
        BilliardsConfig cfg = BilliardsConfig::desktop();
        cfg.rule = BilliardsRule::EightBall;
        cfg.matchMode = BilliardsMatchMode::VsAi;
        cfg.aiDifficulty = BilliardsAiDifficulty::Goat;
        cfg.showAiIntent = true;
        Playground pg(desktopScene(15, {1440, 900}));
        pg.setBilliardsConfig(cfg);
        pg.setBilliards(true);

        EXPECT_EQ(billiards(pg).startingPlayer(), 0);
        EXPECT_EQ(billiards(pg).currentPlayer(), 0);
        EXPECT_EQ(billiards(pg).seriesWins(0), 0);
        EXPECT_EQ(billiards(pg).seriesWins(1), 0);

        // Finish Frame 1 with AI potting legal #8 -> seriesWins becomes 0:1 and `[再来一局 ↻]` pill renders!
        auto& bm = mutableBilliards(pg);
        int eightIdx = -1;
        for (std::size_t i = 0; i < bm.objectBallCount(); ++i) {
            if (bm.objectBallGroup(i) == BallGroup::Eight) {
                eightIdx = static_cast<int>(i);
            } else if (bm.objectBallGroup(i) == BallGroup::Solid) {
                bm.mutableWorld().setEnabled(static_cast<BodyIndex>(i + 1), false);
            }
        }
        ASSERT_GE(eightIdx, 0);
        // Fire 1 dummy human miss first so `shots_ >= 1`, then let AI pot #8 on shot 2!
        const Vec2 w0 = bm.whiteSceneCenter();
        pg.input({InputType::PointerDown, {w0.x, w0.y - 80.0f}, 0.1});
        pg.input({InputType::PointerMove, {w0.x, w0.y - 55.0f}, 0.2});
        pg.input({InputType::PointerUp, {w0.x, w0.y - 55.0f}, 0.3});
        for (int s = 0; s < 500 && billiards(pg).state() == BilliardsState::Rolling; ++s) pg.tick(1.0 / 60.0);

        const Vec2 p2 = bm.pocketScene(2);
        const float r = bm.ballRadiusScene();
        bm.debugPlaceWhite({p2.x - 11.0f * r, p2.y + 11.0f * r}, {0.0f, 0.0f});
        bm.debugPlaceBall(static_cast<std::size_t>(eightIdx), {p2.x - 4.5f * r, p2.y + 4.5f * r},
                          {0.0f, 0.0f});
        bm.debugSetTurnState(1, BallGroup::Stripe, BallGroup::Solid);
        // Settle white if in PlacingWhite, then let AI sink #8!
        for (int s = 0; s < 1200 && billiards(pg).state() != BilliardsState::Finished; ++s) {
            pg.tick(1.0 / 60.0);
        }
        ASSERT_EQ(billiards(pg).state(), BilliardsState::Finished);
        EXPECT_EQ(billiards(pg).winner(), 1);
        EXPECT_EQ(billiards(pg).seriesWins(1), 1);
        EXPECT_TRUE(drawListContainsText(pg.drawList(), "局分0:1"));
        EXPECT_TRUE(drawListContainsText(pg.drawList(), "再来一局"));

        // Click on-table `[再来一局 ↻]` pill -> Frame 2 starts with alternating break (`startingPlayer == 1`
        // = AI)!
        pg.input({InputType::PointerDown, billiards(pg).rematchButtonCenterScene(), 15.0});
        EXPECT_EQ(billiards(pg).state(), BilliardsState::Ready);
        EXPECT_EQ(billiards(pg).startingPlayer(), 1);
        EXPECT_EQ(billiards(pg).currentPlayer(), 1);
        EXPECT_EQ(billiards(pg).seriesWins(1), 1);

        // Advance 10 frames so AI plans its opening break (`大力冲球`).
        for (int i = 0; i < 10; ++i) pg.tick(1.0 / 60.0);
        EXPECT_STREQ(billiards(pg).aiIntentTag(), "大力冲球");
        EXPECT_TRUE(drawListContainsText(pg.drawList(), "人机回合(球王)[大力冲球]"));
        EXPECT_TRUE(drawListContainsText(pg.drawList(), "AI意图: 开"));

        // Click on-table `[AI意图: 开]` pill during AI's turn -> hides `[大力冲球]` tag immediately!
        pg.input({InputType::PointerDown, billiards(pg).aiIntentToggleCenterScene(), 16.0});
        pg.tick(0);
        EXPECT_FALSE(pg.billiardsConfig().showAiIntent);
        EXPECT_TRUE(drawListContainsText(pg.drawList(), "AI意图: 关"));
        EXPECT_FALSE(drawListContainsText(pg.drawList(), "[大力冲球]"));
        EXPECT_TRUE(drawListContainsText(pg.drawList(), "人机回合(球王)"));

        // Toggle back ON via `setBilliardsConfig` without resetting rack!
        cfg.showAiIntent = true;
        pg.setBilliardsConfig(cfg);
        EXPECT_TRUE(drawListContainsText(pg.drawList(), "[大力冲球]"));

        // Let AI complete its opening power break cleanly without scratching/fouling!
        for (int s = 0; s < 1000; ++s) {
            pg.tick(1.0 / 60.0);
            if (billiards(pg).stats().shots >= 1 && billiards(pg).state() != BilliardsState::Rolling) break;
        }
        EXPECT_EQ(billiards(pg).stats().shots, 1);
        EXPECT_NE(billiards(pg).state(), BilliardsState::PlacingWhite);
    }

    // 2. Snooker AI Opening Safety Break (`防守开球`) to Baulk + Live Break Counter (`单杆`)!
    {
        BilliardsConfig cfg = BilliardsConfig::desktop();
        cfg.rule = BilliardsRule::Snooker;
        cfg.matchMode = BilliardsMatchMode::VsAi;
        cfg.aiDifficulty = BilliardsAiDifficulty::Goat;
        BilliardsMode m(desktopScene(21, {1600, 960}), cfg, /*bestShots=*/0, /*startingPlayer=*/1, {1, 0});

        for (int i = 0; i < 10; ++i) {
            m.stepPhysics(1.0f / 60.0f);
            m.updateRules(1.0f / 60.0f);
        }
        EXPECT_STREQ(m.aiIntentTag(), "防守开球");
        for (int s = 0; s < 1200; ++s) {
            m.stepPhysics(1.0f / 60.0f);
            m.updateRules(1.0f / 60.0f);
            if (m.stats().shots >= 1 && m.state() != BilliardsState::Rolling) break;
        }
        EXPECT_EQ(m.score(0), 0);  // Zero foul on opening Snooker safety break!
        const Vec2 tMin = m.tableMinScene();
        const Vec2 tSize = m.tableSizeScene();
        // White ball returned safely into the left half (toward Baulk) rather than lingering in the Red pack!
        EXPECT_LT(m.whiteSceneCenter().x, tMin.x + tSize.x * 0.55f);

        // Verify live Snooker break counter (`currentBreak` & `单杆` HUD display) across Red -> Black!
        Playground pg(desktopScene(21, {1600, 960}));
        pg.setBilliardsConfig(cfg);
        pg.setBilliards(true);
        auto& bm = mutableBilliards(pg);
        const Vec2 p2 = bm.pocketScene(2);
        const Vec2 p5 = bm.pocketScene(5);
        const float r = bm.ballRadiusScene();
        bm.debugPlaceBall(0, {p2.x - 5.2f * r, p2.y + 5.2f * r}, {0.0f, 0.0f});
        bm.debugPlaceBall(1, {p5.x - 5.6f * r, p5.y - 5.6f * r}, {0.0f, 0.0f});
        bm.debugPlaceWhite({tMin.x + tSize.x * 0.78f, tMin.y + tSize.y * 0.36f}, {0.0f, 0.0f});
        bm.debugSetTurnState(1, BallGroup::FreeBall, BallGroup::FreeBall, /*snookerTarget=*/1);

        for (int s = 0; s < 1400 && billiards(pg).currentPlayer() == 1; ++s) {
            pg.tick(1.0 / 60.0);
            if (billiards(pg).currentBreak() >= 6 && billiards(pg).state() != BilliardsState::Rolling) break;
        }
        EXPECT_GE(billiards(pg).currentBreak(), 5);
        EXPECT_TRUE(drawListContainsText(pg.drawList(), "单杆"));
    }
}

TEST(BilliardsMirror, CrowdedDesktopAutoCapEnablesFreeRuleWithoutGreyingOut) {
    // 120 icons on a compact 1180x740 desktop: legacy uncapped Free (120 balls) cannot fit,
    // but canEnableAnyBilliards() is true and enableBilliardsAutoFit() / freeModeMaxRackBalls=21
    // smoothly caps the rack while preserving all remaining icons as extraIcons!
    Playground crowded(desktopScene(120, {1180, 740}));
    EXPECT_FALSE(crowded.canEnableBilliards());
    EXPECT_TRUE(crowded.canEnableAnyBilliards());

    BilliardsConfig uiCfg = BilliardsConfig::desktop();
    uiCfg.rule = BilliardsRule::Free;
    uiCfg.freeModeMaxRackBalls = 21;
    uiCfg.extraIcons = ExtraIconsPolicy::BottomGravity;
    EXPECT_TRUE(crowded.canEnableBilliardsWith(uiCfg));
    crowded.setBilliardsConfig(uiCfg);
    crowded.enableBilliardsAutoFit();
    ASSERT_TRUE(crowded.billiardsEnabled());
    EXPECT_EQ(billiards(crowded).objectBallCount(), 21u);
    EXPECT_EQ(billiards(crowded).extraIconCount(), 99u);
}

TEST(BilliardsMirror, MinimalTableThemeRendersPuristWireframeAndSuppressesTubesLive) {
    BilliardsConfig cfg = BilliardsConfig::desktop();
    cfg.rule = BilliardsRule::EightBall;
    cfg.showBallBorders = false;
    cfg.showAimGuide = false;
    Playground p(desktopScene(15, {1440, 900}));
    p.setBilliardsConfig(cfg);
    p.setBilliards(true);

    // Pot ball 0 into top-left pocket so side return tube has a pocketed icon ball.
    auto& bm = mutableBilliards(p);
    bm.debugPlaceBall(0, bm.pocketScene(0), {0.0f, 0.0f});
    for (int i = 0; i < 40; ++i) p.tick(1.0 / 60.0);
    ASSERT_EQ(p.mode().stats().pocketed, 1);

    const Vec2 pottedPos = bm.iconCurrentSceneCenter(0);
    EXPECT_TRUE(bm.isTubeBallHit(pottedPos));
    const std::size_t fullCmds = p.drawList().commands().size();

    // Switch live to Minimal: fewer commands than Full, more than Borderless, no side tubes, rack preserved!
    cfg.tableTheme = BilliardsTableTheme::Minimal;
    p.setBilliardsConfig(cfg);
    EXPECT_EQ(p.mode().stats().pocketed, 1);
    EXPECT_FALSE(billiards(p).isTubeBallHit(pottedPos));
    const std::size_t minimalCmds = p.drawList().commands().size();
    EXPECT_LT(minimalCmds, fullCmds);

    // Switch live to Borderless: zero table lines/pocket rings, strictly fewer commands than Minimal.
    cfg.tableTheme = BilliardsTableTheme::Borderless;
    p.setBilliardsConfig(cfg);
    EXPECT_EQ(p.mode().stats().pocketed, 1);
    EXPECT_FALSE(billiards(p).isTubeBallHit(pottedPos));
    const std::size_t borderlessCmds = p.drawList().commands().size();
    EXPECT_LT(borderlessCmds, minimalCmds);

    // Switch back to Full: pocketed icon ball in side return tube reappears immediately!
    cfg.tableTheme = BilliardsTableTheme::Full;
    p.setBilliardsConfig(cfg);
    EXPECT_TRUE(billiards(p).isTubeBallHit(pottedPos));
    EXPECT_EQ(p.drawList().commands().size(), fullCmds);
}

TEST(BilliardsMirror, EightBallGoatAiNeverEmitsSnookerTerminologyAndNeverAbandonsOpenPot) {
    BilliardsConfig cfg = BilliardsConfig::desktop();
    cfg.rule = BilliardsRule::EightBall;
    cfg.matchMode = BilliardsMatchMode::VsAi;
    cfg.aiDifficulty = BilliardsAiDifficulty::Goat;
    Playground pg(desktopScene(15, {1600, 960}));
    pg.setBilliardsConfig(cfg);
    pg.setBilliards(true);

    auto& bm = mutableBilliards(pg);
    const Vec2 tMin = bm.tableMinScene();
    const Vec2 tSize = bm.tableSizeScene();
    const float r = bm.ballRadiusScene();

    // Find one Solid target, the Black8 ball, and two Stripe blockers dynamically by group!
    std::size_t solidIdx = 999, blackIdx = 999;
    std::vector<std::size_t> stripeIdxs;
    for (std::size_t i = 0; i < bm.objectBallCount(); ++i) {
        const BallGroup g = bm.objectBallGroup(i);
        if (g == BallGroup::Solid && solidIdx == 999)
            solidIdx = i;
        else if (g == BallGroup::Eight && blackIdx == 999)
            blackIdx = i;
        else if (g == BallGroup::Stripe && stripeIdxs.size() < 2u)
            stripeIdxs.push_back(i);
        else
            bm.debugPlaceBall(i, bm.pocketScene(0), {0.0f, 0.0f});
    }
    ASSERT_NE(solidIdx, 999u);
    ASSERT_NE(blackIdx, 999u);
    ASSERT_EQ(stripeIdxs.size(), 2u);
    for (int i = 0; i < 30; ++i) pg.tick(1.0 / 60.0);
    ASSERT_NE(bm.state(), BilliardsState::Finished);

    // Case 1: Hooked cue ball in 8-Ball -> AI status text & intent tag NEVER contain "斯诺克"!
    bm.debugPlaceBall(blackIdx, {tMin.x + tSize.x * 0.22f, tMin.y + tSize.y * 0.80f}, {0.0f, 0.0f});
    bm.debugPlaceBall(solidIdx, {tMin.x + tSize.x * 0.50f, tMin.y + tSize.y * 0.50f}, {0.0f, 0.0f});
    bm.debugPlaceWhite({tMin.x + tSize.x * 0.20f, tMin.y + tSize.y * 0.50f}, {0.0f, 0.0f});
    bm.debugPlaceBall(stripeIdxs[0], {tMin.x + tSize.x * 0.20f + 2.5f * r, tMin.y + tSize.y * 0.50f - 0.8f * r},
                      {0.0f, 0.0f});
    bm.debugPlaceBall(stripeIdxs[1], {tMin.x + tSize.x * 0.20f + 2.5f * r, tMin.y + tSize.y * 0.50f + 0.8f * r},
                      {0.0f, 0.0f});
    bm.debugSetTurnState(/*player=*/1, /*p0Group=*/BallGroup::Stripe, /*p1Group=*/BallGroup::Solid);

    for (int i = 0; i < 12; ++i) pg.tick(1.0 / 60.0);
    const std::string tag1 = bm.aiIntentTag();
    EXPECT_FALSE(tag1.empty());
    EXPECT_FALSE(drawListContainsText(pg.drawList(), "斯诺克"));
    EXPECT_EQ(tag1.find("斯诺克"), std::string::npos) << "Unexpected Snooker tag in 8-Ball: " << tag1;

    // Case 2: Open 8-Ball pot on solidIdx -> Goat AI NEVER plays "弃攻做球", and pots solidIdx + Black8 cleanly!
    pg.reset();
    auto& bm2 = mutableBilliards(pg);
    for (std::size_t i = 0; i < bm2.objectBallCount(); ++i) {
        if (i == solidIdx || i == blackIdx || i == stripeIdxs[0] || i == stripeIdxs[1]) continue;
        bm2.debugPlaceBall(i, bm2.pocketScene(0), {0.0f, 0.0f});
    }
    for (int i = 0; i < 30; ++i) pg.tick(1.0 / 60.0);
    bm2.debugPlaceBall(blackIdx, {tMin.x + tSize.x * 0.72f, tMin.y + tSize.y * 0.72f}, {0.0f, 0.0f});
    bm2.debugPlaceBall(stripeIdxs[0], {tMin.x + tSize.x * 0.18f, tMin.y + tSize.y * 0.20f}, {0.0f, 0.0f});
    bm2.debugPlaceBall(stripeIdxs[1], {tMin.x + tSize.x * 0.18f, tMin.y + tSize.y * 0.35f}, {0.0f, 0.0f});
    const Vec2 p2 = bm2.pocketScene(2);
    bm2.debugPlaceBall(solidIdx, {p2.x - 4.8f * r, p2.y + 4.8f * r}, {0.0f, 0.0f});
    bm2.debugPlaceWhite({p2.x - 12.5f * r, p2.y + 11.2f * r}, {0.0f, 0.0f});
    bm2.debugSetTurnState(/*player=*/1, /*p0Group=*/BallGroup::Stripe, /*p1Group=*/BallGroup::Solid);

    for (int i = 0; i < 10; ++i) pg.tick(1.0 / 60.0);
    EXPECT_STRNE(bm2.aiIntentTag(), "弃攻做球");
    EXPECT_FALSE(drawListContainsText(pg.drawList(), "斯诺克"));

    const int pocketedBefore = pg.mode().stats().pocketed;
    const int shotsBefore = pg.mode().stats().shots;
    for (int s = 0; s < 900; ++s) {
        pg.tick(1.0 / 60.0);
        if (pg.mode().stats().shots > shotsBefore && bm2.state() != BilliardsState::Rolling) break;
    }
    EXPECT_GT(pg.mode().stats().pocketed, pocketedBefore);
}

TEST(BilliardsMirror, LiveConfigUpdateNeverResetsRackOnEquivalentFreeBallCapOrInactiveSelectionFields) {
    // 1. Free mode: 0 ("no cap" -> clamps to 21) vs explicit 21, plus inactive customIconIndices/randomSeed
    //    changes while iconSelection == FirstN MUST NOT reset shots or pocketed balls!
    BilliardsConfig cfg = BilliardsConfig::desktop();
    cfg.rule = BilliardsRule::Free;
    cfg.iconSelection = IconSelectionPolicy::FirstN;
    cfg.freeModeMaxRackBalls = 0;
    Playground pg(desktopScene(12, {1600, 960}));
    pg.setBilliardsConfig(cfg);
    pg.setBilliards(true);

    const Vec2 w = billiards(pg).whiteSceneCenter();
    pg.input({InputType::PointerDown, {w.x + 80.0f, w.y}, 0.1});
    pg.input({InputType::PointerMove, {w.x - 40.0f, w.y}, 0.2});
    pg.input({InputType::PointerUp, {w.x - 40.0f, w.y}, 0.3});
    mutableBilliards(pg).debugPlaceBall(0, billiards(pg).pocketScene(0), {});
    for (int i = 0; i < 40; ++i) pg.tick(1.0 / 60.0);
    ASSERT_EQ(pg.mode().stats().shots, 1);
    ASSERT_EQ(pg.mode().stats().pocketed, 1);

    BilliardsConfig update = cfg;
    update.freeModeMaxRackBalls = 21;  // Normalized equivalent of 0
    update.randomSeed = 987654321u;    // Inactive when iconSelection == FirstN
    update.customIconIndices = {2, 4, 6};
    update.showAimGuide = !cfg.showAimGuide;
    update.tableTheme = BilliardsTableTheme::Minimal;
    pg.setBilliardsConfig(update);

    EXPECT_EQ(pg.mode().stats().shots, 1);
    EXPECT_EQ(pg.mode().stats().pocketed, 1);
    EXPECT_EQ(pg.billiardsConfig().tableTheme, BilliardsTableTheme::Minimal);

    // 2. EightBall mode: changing freeModeMaxRackBalls while rule == EightBall MUST NOT re-rack!
    BilliardsConfig eightCfg = BilliardsConfig::desktop();
    eightCfg.rule = BilliardsRule::EightBall;
    eightCfg.freeModeMaxRackBalls = 21;
    pg.setBilliardsConfig(eightCfg);
    const Vec2 w8 = billiards(pg).whiteSceneCenter();
    pg.input({InputType::PointerDown, {w8.x + 80.0f, w8.y}, 1.1});
    pg.input({InputType::PointerMove, {w8.x - 40.0f, w8.y}, 1.2});
    pg.input({InputType::PointerUp, {w8.x - 40.0f, w8.y}, 1.3});
    for (int i = 0; i < 40; ++i) pg.tick(1.0 / 60.0);
    ASSERT_EQ(pg.mode().stats().shots, 1);

    BilliardsConfig eightUpdate = eightCfg;
    eightUpdate.freeModeMaxRackBalls = 8;
    eightUpdate.showBallNumbers = !eightCfg.showBallNumbers;
    pg.setBilliardsConfig(eightUpdate);
    EXPECT_EQ(pg.mode().stats().shots, 1);
}

TEST(BilliardsMirror, PrecomputedFilenamePeersMatchReferenceBoundsBitIdentically) {
    Playground pg(desktopScene(24, {1440, 900}));
    for (int i = 0; i < 45; ++i) pg.tick(1.0 / 60.0);
    const DrawList& dl = pg.drawList();
    const Vec2 workArea{1440.0f, 900.0f};
    std::vector<FilenamePeer> peers;
    buildFilenamePeers(dl, workArea, peers);
    std::size_t peerIdx = 0;
    for (const auto& cmd : dl.commands()) {
        const auto* s = std::get_if<SpriteItem>(&cmd);
        if (!s || !s->interactive || s->opacity <= 0.0f) continue;
        ASSERT_LT(peerIdx, peers.size());
        const Rect ref = filenameLabelBounds(*s, workArea, &dl);
        const Rect fast = filenameLabelBoundsFromPeers(*s, peers[peerIdx++].labelY, workArea, peers);
        EXPECT_FLOAT_EQ(fast.x, ref.x);
        EXPECT_FLOAT_EQ(fast.y, ref.y);
        EXPECT_FLOAT_EQ(fast.width, ref.width);
        EXPECT_FLOAT_EQ(fast.height, ref.height);
    }
}
