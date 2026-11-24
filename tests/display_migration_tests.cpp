#include <gtest/gtest.h>

#include "platforms/display_migration.h"
#include "platforms/mirror_hit.h"
#include "tests/display_migration_shrink_fixture.h"
using namespace dpp;
namespace {
SceneSetup scene() {
    return {
        {800, 600},
        {{IconId{17}, TextureId{3}, {100, 100}, {64, 48}}, {IconId{29}, TextureId{7}, {600, 400}, {48, 64}}}};
}
}  // namespace
TEST(DisplayMigration, RotatedLegalCenterMappingAndBackingOnlyIdentity) {
    const MigrationIcon icon{{100, 120}, {80, 40}, .7f};
    const auto h = rotatedIconHalfExtent(icon.size, icon.rotation);
    auto p = migrateIconCenters({&icon, 1}, {800, 600}, {500, 400});
    EXPECT_NEAR(p[0].x, h.x + (100 - h.x) / (800 - 2 * h.x) * (500 - 2 * h.x), .001);
    EXPECT_NEAR(p[0].y, h.y + (120 - h.y) / (600 - 2 * h.y) * (400 - 2 * h.y), .001);
    EXPECT_GE(p[0].x, h.x);
    EXPECT_LE(p[0].y + h.y, 400);
    EXPECT_EQ(migrateIconCenters({&icon, 1}, {800, 600}, {800, 600})[0], icon.center);
}
TEST(DisplayMigration, RunningAndUserPausePreserveMotionIdentityAndSettings) {
    for (bool paused : {false, true}) {
        Playground game(scene());
        auto& world = const_cast<PhysicsWorld&>(game.mode().world());
        world.setTransform(0, SceneToPhysics({800, 600}).toPhysics({100, 100}), .4f);
        world.setVelocity(0, {2, -3}, 1.25f);
        if (paused) game.input({InputType::TogglePause, {}, 0});
        auto frozen = game.freezeForMigration();
        for (Vec2 size : {Vec2{1200, 900}, Vec2{500, 400}, Vec2{800, 600}}) {
            auto candidate = Playground::prepareMigration(frozen, size);
            EXPECT_EQ(candidate->paused(), paused);
            EXPECT_TRUE(candidate->migrationPaused());
            const auto state = candidate->mode().world().state(0);
            EXPECT_EQ(state.linearVelocity, (Vec2{2, -3}));
            EXPECT_FLOAT_EQ(state.angularVelocity, 1.25f);
            EXPECT_FLOAT_EQ(state.angle, frozen.bodies[0].angle);
            EXPECT_EQ(state.unitRotation, frozen.bodies[0].unitRotation);
            candidate->commitMigration();
            auto before = candidate->mode().world().state(0);
            candidate->tick(.02);
            if (paused) {
                EXPECT_EQ(candidate->mode().world().state(0).position, before.position);
            }
            auto again = candidate->freezeForMigration();
            EXPECT_EQ(again.setup.icons[0].id, IconId{17});
            EXPECT_EQ(again.setup.icons[0].texture, TextureId{3});
            EXPECT_EQ(again.setup.icons[0].iconSize, (Vec2{64, 48}));
        }
    }
}
TEST(DisplayMigration, ConfigurationStormReusesSingleSnapshotAndFixedWaitLimit) {
    Playground game(scene());
    DisplayMigration tx;
    tx.begin(game, 10);
    tx.prepare({500, 400});
    const auto oldToken = tx.revision();
    tx.changed(10.1);
    EXPECT_EQ(tx.candidate(), nullptr);
    tx.begin(game, 10.2);  // Must not freeze candidate or refresh deadline.
    tx.prepare({1200, 900});
    EXPECT_FALSE(tx.commit(oldToken, 10.3, true));
    auto result = tx.commit(tx.revision(), 10.3, true);
    ASSERT_TRUE(result);
    auto expected = Playground::prepareMigration(game.freezeForMigration(), {1200, 900});
    EXPECT_EQ(result->mode().world().state(0).position, expected->mode().world().state(0).position);
    EXPECT_EQ(tx.generation(), 1u);
    tx.begin(*result, 20);
    for (double t = 20.1; t < 23; t += .1) {
        tx.changed(t);
        tx.begin(*result, t);
    }
    tx.prepare({800, 600});
    EXPECT_TRUE(tx.expired(23));
    EXPECT_FALSE(tx.commit(tx.revision(), 23, true));
}
TEST(DisplayMigration, CorrectsAllCandidatesOrFailsWithoutPartialPublication) {
    std::vector<MigrationIcon> icons{{{100, 100}, {64, 64}, 0}, {{180, 100}, {64, 64}, 0}};
    auto p = migrateIconCenters(icons, {800, 600}, {200, 200});
    EXPECT_GE(std::abs(p[0].x - p[1].x), 63);
    icons = {{{50, 50}, {90, 90}, 0}, {{150, 50}, {90, 90}, 0}};
    EXPECT_THROW((void)migrateIconCenters(icons, {200, 200}, {100, 100}), std::invalid_argument);
    MigrationIcon rotated{{100, 100}, {90, 90}, .785398f};
    EXPECT_THROW((void)migrateIconCenters({&rotated, 1}, {200, 200}, {100, 100}), std::invalid_argument);
}
TEST(DisplayMigration, GrabCanceledAndNewHitStillResolvesOriginalTarget) {
    Playground game(scene());
    game.tick(0);
    game.input({InputType::PointerDown, {100, 100}, 1});
    ASSERT_TRUE(game.mode().stats().grabbing);
    DisplayMigration tx;
    tx.begin(game, 1);
    EXPECT_FALSE(game.mode().stats().grabbing);
    tx.prepare({1200, 900});
    auto next = tx.commit(tx.revision(), 1.2, true);
    ASSERT_TRUE(next);
    const auto* sprite = std::get_if<SpriteItem>(&next->drawList().commands()[0]);
    ASSERT_NE(sprite, nullptr);
    auto center = sprite->center;
    EXPECT_EQ(mirrorHit(next->drawList(), center), TextureId{3});
    auto snapshot = next->freezeForMigration();
    EXPECT_EQ(snapshot.setup.icons[0].id, IconId{17});
    next->commitMigration();
    next->input({InputType::PointerDown, center, 2});
    EXPECT_TRUE(next->mode().stats().grabbing);
    next->input({InputType::PointerCancel, {}, 2});
    next->reset();
    next->tick(0);
    EXPECT_EQ(mirrorHit(next->drawList(), center), TextureId{3});
}
TEST(DisplayMigration, HelperFailureDeadlineAndRecoveryInvalidateCandidates) {
    Playground game(scene());
    DisplayMigration tx;
    tx.begin(game, 1);
    tx.prepare({500, 400});
    EXPECT_FALSE(tx.commit(tx.revision(), 1.2, false));
    const auto token = tx.revision();
    tx.invalidate();
    EXPECT_EQ(tx.candidate(), nullptr);
    EXPECT_FALSE(tx.commit(token, 1.2, true));
    tx.begin(game, 2);
    tx.prepare({1200, 900});
    EXPECT_FALSE(tx.commit(token, 2.2, true));
    EXPECT_FALSE(tx.commit(tx.revision(), 5, true));
    EXPECT_EQ(game.mode().world().state(0).linearVelocity, (Vec2{}));
}
TEST(DisplayMigration, RotatedBoundaryMappingDoesNotRejectLegalSingleIcon) {
    for (int i = 0; i < 4096; ++i) {
        const float angle = static_cast<float>(i) * .0001f;
        const auto half = rotatedIconHalfExtent({64, 64}, angle);
        const MigrationIcon icon{{200, 1009 - half.y}, {64, 64}, angle};
        std::vector<Vec2> positions;
        ASSERT_NO_THROW(positions = migrateIconCenters({&icon, 1}, {1728, 1009}, {3456, 2094}));
        ASSERT_EQ(positions.size(), 1u);
        EXPECT_GE(positions[0].x, half.x);
        EXPECT_GE(positions[0].y, half.y);
        EXPECT_LE(positions[0].x, 3456 - half.x);
        EXPECT_LE(positions[0].y, 2094 - half.y);
    }
}
TEST(DisplayMigration, FailureDiagnosticsDistinguishCurrentAndResetLayoutWithoutPrivateText) {
    auto frozen = Playground(scene()).freezeForMigration();
    MigrationPreparationStage stage = MigrationPreparationStage::Validation;
    auto invalidCurrent = frozen;
    invalidCurrent.setup.icons[0].iconSize = {600, 600};
    EXPECT_THROW((void)Playground::prepareMigration(invalidCurrent, {500, 400}, &stage),
                 std::invalid_argument);
    EXPECT_EQ(stage, MigrationPreparationStage::CurrentLayout);
    auto invalidReset = frozen;
    invalidReset.resetSetup.icons[0].iconSize = {600, 600};
    EXPECT_THROW((void)Playground::prepareMigration(invalidReset, {500, 400}, &stage), std::invalid_argument);
    EXPECT_EQ(stage, MigrationPreparationStage::ResetLayout);
    EXPECT_STREQ(migrationFailureCategory(std::runtime_error("private-file-placeholder")),
                 "unclassified_cpp_exception");
    EXPECT_STREQ(migrationFailureCategory(std::invalid_argument("rotated icon cannot fit")),
                 "rotated icon cannot fit");
}

namespace {
void expectLegalMigration(std::span<const MigrationIcon> icons, const std::vector<Vec2>& positions,
                          Vec2 size) {
    ASSERT_EQ(icons.size(), positions.size());
    for (size_t i = 0; i < icons.size(); ++i) {
        const auto h = rotatedIconHalfExtent(icons[i].size, icons[i].rotation);
        EXPECT_GE(positions[i].x, h.x);
        EXPECT_LE(positions[i].x, size.x - h.x);
        EXPECT_GE(positions[i].y, h.y);
        EXPECT_LE(positions[i].y, size.y - h.y);
        for (size_t j = 0; j < i; ++j) {
            const auto other = rotatedIconHalfExtent(icons[j].size, icons[j].rotation);
            const auto delta = positions[i] - positions[j];
            EXPECT_TRUE(h.x + other.x - std::abs(delta.x) <= 1 || h.y + other.y - std::abs(delta.y) <= 1);
        }
    }
}
}  // namespace
TEST(DisplayMigration, RecordedShrinkingPileHasDeterministicSafePlacement) {
    std::vector<Vec2> p;
    ASSERT_NO_THROW(p = migrateIconCenters(shrinkFailureIcons, {3456, 2079}, {2056, 1175}));
    expectLegalMigration(shrinkFailureIcons, p, {2056, 1175});
    EXPECT_EQ(p, migrateIconCenters(shrinkFailureIcons, {3456, 2079}, {2056, 1175}));
    // This measured local layout should not be sent to a distant corner.
    for (size_t i = 0; i < p.size(); ++i) {
        const auto mapped = migrateIconCenters({&shrinkFailureIcons[i], 1}, {3456, 2079}, {2056, 1175})[0];
        EXPECT_LT(std::hypot(p[i].x - mapped.x, p[i].y - mapped.y), 160);
    }
}
TEST(DisplayMigration, PlacementDoesNotMoveBackingOnlyOverlapOrAcceptImpossiblePacking) {
    std::vector<MigrationIcon> icons(2, {{50, 50}, {90, 90}, 0});
    EXPECT_THROW((void)migrateIconCenters(icons, {200, 200}, {200, 200}), std::invalid_argument);
    EXPECT_THROW((void)migrateIconCenters(icons, {200, 200}, {100, 100}), std::invalid_argument);
    icons.assign(128, {{400, 300}, {48, 48}, .3f});
    auto p = migrateIconCenters(icons, {800, 600}, {1600, 1200});
    expectLegalMigration(icons, p, {1600, 1200});
    for (size_t i = 0; i < icons.size(); ++i) icons[i].center = p[i];
    EXPECT_EQ(p, migrateIconCenters(icons, {1600, 1200}, {1600, 1200}));
}

TEST(DisplayMigration, RecordedPileKeepsPausedMotionBindingsAndResetAcrossCandidateReplacement) {
    for (bool paused : {false, true}) {
        SceneSetup setup{{3456, 2079}, {}};
        for (size_t i = 0; i < shrinkFailureIcons.size(); ++i)
            setup.icons.push_back({IconId{static_cast<uint32_t>(i + 1)},
                                   TextureId{static_cast<uint32_t>(i + 1)}, shrinkResetCenters[i],
                                   shrinkFailureIcons[i].size});
        Playground game(setup);
        auto& world = const_cast<PhysicsWorld&>(game.mode().world());
        for (size_t i = 0; i < shrinkFailureIcons.size(); ++i) {
            world.setTransform(static_cast<BodyIndex>(i),
                               SceneToPhysics(setup.workAreaSize).toPhysics(shrinkFailureIcons[i].center),
                               -shrinkFailureIcons[i].rotation);
            world.setVelocity(static_cast<BodyIndex>(i), {2, -3}, 1.25f);
        }
        if (paused) game.input({InputType::TogglePause, {}, 0});
        DisplayMigration tx;
        tx.begin(game, 10);
        auto frozen = *tx.frozenScene();
        tx.prepare({2056, 1175});
        const auto discarded = tx.revision();
        tx.changed(10.3);
        tx.prepare({2200, 1300});
        EXPECT_FALSE(tx.commit(discarded, 10.5, true));
        tx.changed(10.5);
        tx.prepare({2056, 1175});
        auto next = tx.commit(tx.revision(), 10.7, true);
        ASSERT_TRUE(next);
        EXPECT_EQ(next->paused(), paused);
        auto prepared = next->freezeForMigration();
        std::vector<Vec2> centers;
        for (size_t i = 0; i < prepared.bodies.size(); ++i) {
            EXPECT_EQ(prepared.bodies[i].linearVelocity, frozen.bodies[i].linearVelocity);
            EXPECT_EQ(prepared.bodies[i].angularVelocity, frozen.bodies[i].angularVelocity);
            EXPECT_EQ(prepared.bodies[i].unitRotation, frozen.bodies[i].unitRotation);
            EXPECT_EQ(prepared.setup.icons[i].id, frozen.setup.icons[i].id);
            EXPECT_EQ(prepared.setup.icons[i].texture, frozen.setup.icons[i].texture);
            EXPECT_EQ(prepared.setup.icons[i].iconSize, frozen.setup.icons[i].iconSize);
            centers.push_back(prepared.setup.icons[i].center);
            EXPECT_EQ(mirrorHit(next->drawList(), centers.back()), prepared.setup.icons[i].texture);
        }
        // Physics round-trip is checked separately from strict mapping boundaries.
        auto direct = Playground::prepareMigration(frozen, {2056, 1175});
        for (size_t i = 0; i < prepared.bodies.size(); ++i)
            EXPECT_EQ(prepared.bodies[i].position,
                      direct->mode().world().state(static_cast<BodyIndex>(i)).position);
        next->commitMigration();
        auto before = next->mode().world().state(0);
        next->tick(.02);
        if (paused) {
            EXPECT_EQ(before.position, next->mode().world().state(0).position);
        }
        next->reset();
        next->tick(0);
        auto reset = next->freezeForMigration();
        for (size_t i = 0; i < reset.setup.icons.size(); ++i) {
            EXPECT_NEAR(reset.setup.icons[i].center.x, prepared.resetSetup.icons[i].center.x, .001);
            EXPECT_NEAR(reset.setup.icons[i].center.y, prepared.resetSetup.icons[i].center.y, .001);
        }
    }
}

TEST(DisplayMigration, CrowdedShrinkShelfPackingUprightFallbackBilliardsAutoCapAndWritebackInverses) {
    // 1. 96 tumbled/rotated icons shrinking from 2560x1440 -> 960x640 succeeds cleanly without overlap!
    SceneSetup crowded{{2560, 1440}, {}};
    for (std::uint32_t i = 0; i < 96; ++i) {
        const float col = static_cast<float>(i % 12);
        const float row = static_cast<float>(i / 12);
        crowded.icons.push_back(
            {IconId{i + 1}, TextureId{i + 1}, {110.0f + col * 195.0f, 95.0f + row * 165.0f}, {64, 64}});
    }
    Playground gravityGame(crowded);
    auto& world = const_cast<PhysicsWorld&>(gravityGame.mode().world());
    for (std::size_t i = 0; i < crowded.icons.size(); ++i) {
        const float rot = 0.785398f * static_cast<float>((i % 3) + 1);  // ~45 deg diagonal AABB expansion
        world.setTransform(static_cast<BodyIndex>(i),
                           SceneToPhysics(crowded.workAreaSize).toPhysics(crowded.icons[i].center), rot);
    }
    const auto frozenGravity = gravityGame.freezeForMigration();
    auto shrunkGravity = Playground::prepareMigration(frozenGravity, {960, 640});
    ASSERT_NE(shrunkGravity, nullptr);
    shrunkGravity->commitMigration();
    const auto afterGravity = shrunkGravity->freezeForMigration();
    ASSERT_EQ(afterGravity.setup.icons.size(), 96u);
    std::vector<MigrationIcon> verifyIcons;
    std::vector<Vec2> verifyCenters;
    for (std::size_t i = 0; i < afterGravity.setup.icons.size(); ++i) {
        const Vec2 ur = afterGravity.bodies[i].unitRotation;
        const float rot = SceneToPhysics::angleToScene(std::atan2(ur.y, ur.x));
        verifyIcons.push_back({afterGravity.setup.icons[i].center, afterGravity.setup.icons[i].iconSize, rot});
        verifyCenters.push_back(afterGravity.setup.icons[i].center);
    }
    expectLegalMigration(verifyIcons, verifyCenters, {960, 640});

    // 2. 96 icons in Billiards Free mode shrinking from 2560x1440 -> 1024x680 auto-caps rack balls cleanly!
    Playground bilGame(crowded);
    BilliardsConfig freeCfg = BilliardsConfig::desktop();
    freeCfg.rule = BilliardsRule::Free;
    freeCfg.freeModeMaxRackBalls = 21;
    bilGame.setBilliardsConfig(freeCfg);
    bilGame.enableBilliardsAutoFit();
    ASSERT_TRUE(bilGame.billiardsEnabled());
    const auto frozenBil = bilGame.freezeForMigration();
    auto shrunkBil = Playground::prepareMigration(frozenBil, {1024, 680});
    ASSERT_NE(shrunkBil, nullptr);
    shrunkBil->commitMigration();
    EXPECT_TRUE(shrunkBil->billiardsEnabled());
    EXPECT_LE(shrunkBil->billiardsMode()->objectBallCount(), 21u);

    // 3. Exact round-trip inverse coordinate transforms for macOS & Windows opt-in layout write-back.
    const Rect macPrimary{0, 0, 1728, 1117};
    const Rect macVisible{0, 0, 1728, 1080};
    const Vec2 macTopLeftOrig{312.0f, 448.0f};
    const Vec2 macGlobal = macTopLeftPointsToGlobal(macTopLeftOrig, macPrimary);
    const Vec2 macScene = macGlobalPointsToScene(macGlobal, macVisible);
    const Vec2 macRoundTrip = macSceneToTopLeftPoints(macScene, macVisible, macPrimary);
    EXPECT_NEAR(macRoundTrip.x, macTopLeftOrig.x, 1e-4f);
    EXPECT_NEAR(macRoundTrip.y, macTopLeftOrig.y, 1e-4f);

    const PixelRect winWork{0, 0, 1920, 1030};
    const float dpiScale = windowsDpiScale(120);
    const Vec2 winAnchorOrig = windowsScreenPixelsToScene(240, 360, winWork, dpiScale);
    const Rect imgRect = windowsIconImageRect(winAnchorOrig, {60, 60}, 60, dpiScale, 5);
    const Vec2 winAnchorBack = windowsSceneCenterToIconAnchor(rectCenter(imgRect), 60, dpiScale, 5);
    EXPECT_NEAR(winAnchorBack.x, winAnchorOrig.x, 1e-4f);
    EXPECT_NEAR(winAnchorBack.y, winAnchorOrig.y, 1e-4f);
}
