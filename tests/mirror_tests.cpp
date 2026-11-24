#include <gtest/gtest.h>

#include "core/modes/gravity_mode.h"
#include "platforms/macos/mirror_session.h"
using namespace dpp;
using namespace dpp::mac;
static MirrorSnapshot mirrorFixture() {
    MirrorSnapshot s;
    s.screen = {0, 0, 1728, 1117};
    s.workArea = {0, 71, 1728, 1008};
    s.capturedAt = 100;
    s.singleScreen = true;
    // Independent, deliberately non-corner logical-point samples, not the historical pair.
    for (const auto& entry :
         std::vector<std::pair<uint32_t, Vec2>>{{71, {320, 240}}, {19, {840, 510}}, {93, {1390, 770}}}) {
        MirrorItem i;
        i.id = IconId{entry.first};
        i.texture = TextureId{static_cast<uint32_t>(s.items.size() + 1)};
        i.rawDpos = entry.second;
        i.coordinateValidated = true;
        i.sceneAnchor = macGlobalPointsToScene(macTopLeftPointsToGlobal(i.rawDpos, s.screen), s.workArea);
        s.items.push_back(i);
    }
    return s;
}
TEST(Mirror, FullDesktopRequiresCompleteCoverageAndCandidateCannotPlay) {
    auto s = mirrorFixture();
    auto extra = s.items.back();
    extra.id = IconId{999};
    extra.texture = TextureId{4};
    s.items.push_back(extra);
    EXPECT_THROW(MirrorSession::scene(s, 101), std::invalid_argument);
    s.fullDesktop = true;
    s.finderItemCount = 4;
    EXPECT_EQ(MirrorSession::scene(s, 101).icons.size(), 4u);
    s.finderItemCount = 5;
    EXPECT_THROW(MirrorSession::scene(s, 101), std::invalid_argument);
    s.finderItemCount = 4;
    s.candidatePreview = true;
    for (auto& item : s.items) item.coordinateValidated = false;
    MirrorSession session;
    session.start(s, 101);
    session.playPause();
    session.interact();
    EXPECT_EQ(session.phase(), MirrorPhase::Static);
    EXPECT_TRUE(session.game()->paused());
    s.candidatePreview = false;
    EXPECT_THROW(MirrorSession::scene(s, 101), std::invalid_argument);
}
TEST(Mirror, AssociatesOpaqueIdentityTextureAndSystemAnchorWithoutCalibrationSubstitution) {
    auto s = mirrorFixture();
    auto setup = MirrorSession::scene(s, 101);
    ASSERT_EQ(setup.icons.size(), 3u);
    EXPECT_EQ(setup.icons[0].id, IconId{71});
    EXPECT_EQ(setup.icons[1].id, IconId{19});
    EXPECT_EQ(setup.icons[2].texture, TextureId{3});
    EXPECT_EQ(setup.icons[0].center, (Vec2{320, 202}));
    EXPECT_EQ(setup.icons[1].center, (Vec2{840, 472}));
    EXPECT_EQ(setup.icons[2].center, (Vec2{1390, 732}));
    EXPECT_EQ(setup.icons[0].iconSize, (Vec2{48, 48}));
    std::swap(s.items[0], s.items[2]);
    setup = MirrorSession::scene(s, 101);
    EXPECT_EQ(setup.icons[0].id, IconId{93});
    EXPECT_EQ(setup.icons[0].texture, TextureId{3});
    EXPECT_EQ(setup.icons[0].center, (Vec2{1390, 732}));
}
TEST(Mirror, RejectsStaleInvalidUnverifiedAndDuplicateIdentities) {
    auto s = mirrorFixture();
    EXPECT_THROW(MirrorSession::scene(s, 400), std::invalid_argument);
    EXPECT_THROW(MirrorSession::scene(s, 99), std::invalid_argument);
    s.items[0].coordinateValidated = false;
    EXPECT_THROW(MirrorSession::scene(s, 101), std::invalid_argument);
    s = mirrorFixture();
    s.items[0].rawDpos = {-1, -1};
    EXPECT_THROW(MirrorSession::scene(s, 101), std::invalid_argument);
    s = mirrorFixture();
    s.items[0].sceneAnchor = {321, 202};
    EXPECT_THROW(MirrorSession::scene(s, 101), std::invalid_argument);
    s = mirrorFixture();
    s.items[1].id = s.items[0].id;
    EXPECT_THROW(MirrorSession::scene(s, 101), std::invalid_argument);
    s = mirrorFixture();
    s.singleScreen = false;
    EXPECT_THROW(MirrorSession::scene(s, 101), std::invalid_argument);
    s = mirrorFixture();
    s.items[0].dppDrawSize = {64, 64};
    EXPECT_THROW(MirrorSession::scene(s, 101), std::invalid_argument);
}
TEST(Mirror, StaticThenGravityCollisionPauseAndResetOnlyOwnedScene) {
    MirrorSession session;
    auto s = mirrorFixture();
    session.start(s, 101);
    ASSERT_NE(session.game(), nullptr);
    EXPECT_EQ(session.phase(), MirrorPhase::Static);
    const auto& gravity = dynamic_cast<const GravityMode&>(session.game()->mode());
    EXPECT_NEAR(gravity.bodySceneCenter(0).y, 202, 0.001);
    session.game()->tick(1);
    EXPECT_NEAR(gravity.bodySceneCenter(0).y, 202, 0.001);
    session.playPause();
    for (int i = 0; i < 180; ++i) session.game()->tick(1.0 / 60);
    EXPECT_GT(gravity.bodySceneCenter(0).y, 202);
    EXPECT_LT(gravity.bodySceneCenter(0).y, 1008);
    session.playPause();
    const auto paused = gravity.bodySceneCenter(0);
    session.game()->tick(.1);
    EXPECT_EQ(gravity.bodySceneCenter(0), paused);
    session.reset();
    EXPECT_EQ(session.phase(), MirrorPhase::Static);
    EXPECT_TRUE(session.game()->paused());
    EXPECT_NEAR(dynamic_cast<const GravityMode&>(session.game()->mode()).bodySceneCenter(0).y, 202, .001);
    EXPECT_EQ(s.items[0].rawDpos, (Vec2{320, 240}));
}
TEST(Mirror, InputRequiresExplicitInteractionAndReleaseCancelsGrabWithoutThrow) {
    MirrorSession session;
    session.start(mirrorFixture(), 101);
    session.input({InputType::PointerDown, {320, 202}, 1});
    EXPECT_FALSE(session.game()->mode().stats().grabbing);
    session.interact();
    session.input({InputType::PointerDown, {320, 202}, 1});
    EXPECT_TRUE(session.game()->mode().stats().grabbing);
    session.releaseInput();
    EXPECT_FALSE(session.game()->mode().stats().grabbing);
    EXPECT_EQ(session.phase(), MirrorPhase::Playing);
    session.interact();
    session.input({InputType::PointerDown, {320, 202}, 2});
    ASSERT_TRUE(session.game()->mode().stats().grabbing);
    session.reset();
    EXPECT_FALSE(session.game()->mode().stats().grabbing);
    session.interact();
    session.input({InputType::PointerDown, {320, 202}, 3});
    session.close();
    EXPECT_EQ(session.game(), nullptr);
}
TEST(Mirror, StartupFailureAndRepeatedLifecycleLeaveNoOwnedScene) {
    MirrorSession session;
    for (int i = 0; i < 20; ++i) {
        session.start(mirrorFixture(), 101);
        session.start(mirrorFixture(), 102);
        EXPECT_EQ(session.game()->mode().stats().bodies, 3);
        auto invalid = mirrorFixture();
        invalid.items[0].texture = kNoTexture;
        EXPECT_THROW(session.start(invalid, 103), std::invalid_argument);
        EXPECT_EQ(session.phase(), MirrorPhase::Closed);
        EXPECT_EQ(session.game(), nullptr);
        EXPECT_TRUE(session.items().empty());
        session.close();
        session.close();
    }
}

#include "platforms/macos/assisted_experiment.h"
TEST(AssistedExperiment, ExplicitConsentAndNoInputWhileWaiting) {
    dpp::mac::AssistedExperiment e;
    using E = dpp::mac::AssistedExperiment;
    EXPECT_FALSE(e.prepare(false, true, E::Original::Visible));
    EXPECT_EQ(e.preparedImages, 0u);
    EXPECT_TRUE(e.prepare(true, true, E::Original::Visible));
    EXPECT_FALSE(e.capturesInput());
    EXPECT_FALSE(e.start());
}
TEST(AssistedExperiment, ConfigurationMismatchAndCancellationReleasePreparation) {
    using E = dpp::mac::AssistedExperiment;
    E e;
    ASSERT_TRUE(e.prepare(true, true, E::Original::Visible));
    EXPECT_FALSE(e.verify(false, true, true, true));
    EXPECT_EQ(e.preparedImages, 0u);
    EXPECT_FALSE(e.start());
    ASSERT_TRUE(e.prepare(true, true, E::Original::Visible));
    e.finish();
    EXPECT_EQ(e.preparedImages, 0u);
}
TEST(AssistedExperiment, HumanVisibilityRequiredSeparatelyFromMachinePrerequisites) {
    using E = dpp::mac::AssistedExperiment;
    E e;
    ASSERT_TRUE(e.prepare(true, true, E::Original::Visible));
    EXPECT_FALSE(e.verify(true, false, true, true));
    EXPECT_FALSE(e.start());
    ASSERT_TRUE(e.prepare(true, true, E::Original::Visible));
    EXPECT_TRUE(e.verify(true, true, true, true));
    EXPECT_TRUE(e.start());
    EXPECT_EQ(e.preparedImages, 0u);
}
TEST(AssistedExperiment, UnknownOriginalCannotClaimRestored) {
    using E = dpp::mac::AssistedExperiment;
    E e;
    ASSERT_TRUE(e.prepare(true, true, E::Original::Unknown));
    e.verify(true, true, true, true);
    e.finish();
    EXPECT_FALSE(e.confirmRestored());
}
TEST(AssistedExperiment, AlreadyHiddenDoesNotRequestShowingDesktop) {
    using E = dpp::mac::AssistedExperiment;
    E e;
    ASSERT_TRUE(e.prepare(true, true, E::Original::Hidden));
    ASSERT_TRUE(e.verify(true, true, true, false));
    ASSERT_TRUE(e.start());
    e.finish();
    EXPECT_FALSE(e.restoreRequired());
    EXPECT_FALSE(e.confirmRestored());
}

TEST(AssistedExperiment, RetiredCandidateNeverEnabledByLegacyState) {
    using E = dpp::mac::AssistedExperiment;
    for (auto phase :
         {E::Phase::Idle, E::Phase::Waiting, E::Phase::Verified, E::Phase::Running, E::Phase::Finished}) {
        E historical;
        historical.phase = phase;
        EXPECT_FALSE(dpp::mac::assistedCandidateAvailable());
    }
    EXPECT_NE(std::string(dpp::mac::assistedUnavailableReason).find("已停用"), std::string::npos);
}

TEST(Mirror, ViewConfiguredSizeChangesDrawingAndCollisionButPreservesAnchors) {
    auto s = mirrorFixture();
    const auto original = s.items[0].sceneAnchor;
    for (auto& item : s.items) {
        item.finderIconSize = 64;
        item.dppDrawSize = {64, 64};
        item.dppCollisionBox = {64 * .9f, 64 * .9f};
    }
    auto setup = MirrorSession::scene(s, 101);
    EXPECT_EQ(setup.icons[0].iconSize, (Vec2{64, 64}));
    EXPECT_EQ(setup.icons[0].center, original);
    s.items[0].dppCollisionBox = {43.2f, 43.2f};
    EXPECT_THROW(MirrorSession::scene(s, 101), std::invalid_argument);
    s.items[0].dppCollisionBox = {64 * .9f, 64 * .9f};
    s.items[0].finderIconSize = 256;
    EXPECT_THROW(MirrorSession::scene(s, 101), std::invalid_argument);
    s.items[0].finderIconSize = 64;
    s.items[0].rawDpos = {28, 240};
    s.items[0].sceneAnchor =
        macGlobalPointsToScene(macTopLeftPointsToGlobal(s.items[0].rawDpos, s.screen), s.workArea);
    EXPECT_THROW(MirrorSession::scene(s, 101), std::invalid_argument);
}

// Anonymous positions from diagnostic run CD266B22; no real file identities.
static MirrorSnapshot measuredLargeDesktop() {
    MirrorSnapshot s;
    s.screen = {0, 0, 3456, 2234};
    s.workArea = {0, 85, 3456, 2079};
    s.singleScreen = s.fullDesktop = s.candidatePreview = true;
    s.capturedAt = 100;
    const Vec2 raw[] = {
        {3391, 78},
        {3011, 74},
        {103, 276},
        {3284, 2099},
        {3147, 302},
        {3147, 78},
        {3147, 190},
        {3269, 414},
        {3399, 2045},
        {3270, 194},
        {3269, 526},
        {3391, 1867},
        {3269, 1755},
        {3280, 1888},
        {3391, 414},
        {3391, 1755},
        {3391, 526},
        {102, 89},
    };
    for (Vec2 point : raw) {
        MirrorItem i;
        i.id = IconId{static_cast<uint32_t>(s.items.size() + 1)};
        i.texture = TextureId{static_cast<uint32_t>(s.items.size() + 1)};
        i.rawDpos = point;
        i.sceneAnchor = macGlobalPointsToScene(macTopLeftPointsToGlobal(point, s.screen), s.workArea);
        i.finderIconSize = 64;
        i.dppDrawSize = {64, 64};
        i.dppCollisionBox = {64 * .9f, 64 * .9f};
        s.items.push_back(i);
    }
    s.finderItemCount = s.items.size();
    return s;
}
TEST(Mirror, MeasuredLargeDesktopInsetsOnlyFourEdgeReplicas) {
    auto s = measuredLargeDesktop();
    auto setup = MirrorSession::scene(s, 101);
    ASSERT_EQ(setup.icons.size(), 18u);
    unsigned adjusted = 0;
    for (size_t n = 0; n < s.items.size(); ++n) {
        const auto center = setup.icons[n].center;
        if (!(center == s.items[n].sceneAnchor)) ++adjusted;
        EXPECT_EQ(center.x, s.items[n].sceneAnchor.x);
        EXPECT_GE(center.y, 32);
        EXPECT_LE(center.y, 2079 - 32);
    }
    EXPECT_EQ(adjusted, 4u);
    EXPECT_EQ(setup.icons[1].center, (Vec2{3011, 32}));
    EXPECT_EQ(s.items[1].rawDpos, (Vec2{3011, 74}));
    s.candidatePreview = false;
    for (auto& i : s.items) i.coordinateValidated = true;
    auto active = MirrorSession::scene(s, 101);
    for (size_t n = 0; n < setup.icons.size(); ++n)
        EXPECT_EQ(active.icons[n].center, setup.icons[n].center);
    MirrorSession session;
    session.start(s, 101);
    auto* gravity = dynamic_cast<const GravityMode*>(&session.game()->mode());
    ASSERT_NE(gravity, nullptr);
    EXPECT_NEAR(gravity->bodySceneCenter(1).y, 32, .001);
    session.reset();
    gravity = dynamic_cast<const GravityMode*>(&session.game()->mode());
    EXPECT_NEAR(gravity->bodySceneCenter(1).y, 32, .001);
}
TEST(Mirror, EdgeInsetDoesNotAdmitOutsideOrForgedCoordinates) {
    auto s = measuredLargeDesktop();
    s.items[0].rawDpos.y = 69;
    s.items[0].sceneAnchor = macGlobalPointsToScene(
        macTopLeftPointsToGlobal(s.items[0].rawDpos, s.screen), s.workArea);
    EXPECT_THROW(MirrorSession::scene(s, 101), std::invalid_argument);
    s = measuredLargeDesktop();
    s.items[0].sceneAnchor.y = 32;
    EXPECT_THROW(MirrorSession::scene(s, 101), std::invalid_argument);
    s = measuredLargeDesktop();
    s.workArea.width = 50;
    EXPECT_THROW(MirrorSession::scene(s, 101), std::invalid_argument);
}

TEST(Mirror, FullDesktopInsetBoundsAllEdgesWithoutMovingInterior) {
    auto s = measuredLargeDesktop();
    const Vec2 centers[] = {{4, 200}, {3452, 200}, {500, 4}, {500, 2075}, {500, 500}};
    for (size_t n = 0; n < 5; ++n) {
        s.items[n].sceneAnchor = centers[n];
        s.items[n].rawDpos = macGlobalPointsToScene(macSceneToGlobalPoints(centers[n], s.workArea), s.screen);
    }
    auto setup = MirrorSession::scene(s, 101);
    EXPECT_EQ(setup.icons[0].center, (Vec2{32, 200}));
    EXPECT_EQ(setup.icons[1].center, (Vec2{3424, 200}));
    EXPECT_EQ(setup.icons[2].center, (Vec2{500, 32}));
    EXPECT_EQ(setup.icons[3].center, (Vec2{500, 2047}));
    EXPECT_EQ(setup.icons[4].center, centers[4]);
    // Invalid membership and dimensions remain fatal even for an edge candidate.
    s.finderItemCount--;
    EXPECT_THROW(MirrorSession::scene(s, 101), std::invalid_argument);
}
