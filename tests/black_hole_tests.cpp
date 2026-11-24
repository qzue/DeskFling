#include <gtest/gtest.h>

#include <cmath>
#include <variant>

#include "core/scene/playground.h"
#include "platforms/mirror_hit.h"
using namespace dpp;
namespace {
SceneSetup scene() {
    return {
        {800, 600},
        {{IconId{1}, TextureId{1}, {400, 300}, {48, 48}}, {IconId{2}, TextureId{2}, {530, 300}, {48, 48}}}};
}
const GravityMode& gravity(const Playground& p) { return static_cast<const GravityMode&>(p.mode()); }
void advance(Playground& p, int n = 100) {
    for (int i = 0; i < n; ++i) p.tick(1. / 120);
}
}  // namespace
TEST(BlackHole, DefaultOffAndExplicitEdgeDrag) {
    Playground p(scene());
    EXPECT_FALSE(gravity(p).blackHole().enabled);
    p.setBlackHole(true);
    EXPECT_FALSE(p.blackHoleEdge({400, 300}));
    EXPECT_TRUE(p.blackHoleEdge({442, 300}));
    p.input({InputType::PointerDown, {442, 300}, 0});
    p.input({InputType::PointerMove, {500, 350}, .1});
    EXPECT_EQ(gravity(p).blackHole().center, (Vec2{458, 350}));
    p.input({InputType::PointerCancel, {}, .2});
    EXPECT_FALSE(p.mode().stats().grabbing);
}
TEST(BlackHole, SwallowRemovesHitButRetainsIdentityAndReset) {
    Playground p(scene());
    p.setBlackHole(true);
    advance(p, 3);
    EXPECT_EQ(gravity(p).blackHole().objects[0].phase, SwallowPhase::Swallowing);
    EXPECT_FALSE(p.mode().world().state(0).enabled);
    EXPECT_EQ(mirrorHit(p.drawList(), {400, 300}), kNoTexture);
    advance(p);
    EXPECT_EQ(gravity(p).blackHole().objects[0].phase, SwallowPhase::Swallowed);
    EXPECT_EQ(p.mode().world().bodyCount(), 2u);
    p.reset();
    p.tick(0);
    EXPECT_TRUE(gravity(p).blackHole().enabled);
    EXPECT_TRUE(p.mode().world().state(0).enabled);
    EXPECT_EQ(mirrorHit(p.drawList(), {400, 300}), TextureId{1});
}
TEST(BlackHole, GrabWinsAndReleaseCanSwallow) {
    Playground p(scene());
    p.setBlackHole(true);
    p.input({InputType::PointerDown, {400, 300}, 0, TextureId{1}});
    advance(p);
    EXPECT_EQ(gravity(p).blackHole().objects[0].phase, SwallowPhase::Normal);
    EXPECT_TRUE(p.mode().world().state(0).enabled);
    p.input({InputType::PointerUp, {400, 300}, 1});
    advance(p);
    EXPECT_EQ(gravity(p).blackHole().objects[0].phase, SwallowPhase::Swallowed);
}
TEST(BlackHole, PauseFreezesAndDisableRestores) {
    Playground p(scene());
    p.setBlackHole(true);
    advance(p, 3);
    const auto before = gravity(p).blackHole();
    p.input({InputType::TogglePause, {}, 0});
    advance(p);
    EXPECT_EQ(gravity(p).blackHole().objects[0].elapsed, before.objects[0].elapsed);
    EXPECT_EQ(gravity(p).blackHole().angle, before.angle);
    p.setBlackHole(false);
    EXPECT_TRUE(p.paused());
    EXPECT_FALSE(gravity(p).blackHole().enabled);
    EXPECT_TRUE(p.mode().world().state(0).enabled);
    EXPECT_EQ(mirrorHit(p.drawList(), {400, 300}), TextureId{1});
}
TEST(BlackHole, FiniteAndBoundedMotion) {
    Playground p(scene());
    p.setBlackHole(true);
    for (int i = 0; i < 2400; ++i) {
        p.tick(1. / 120);
        for (const auto& b : gravity(p).bodyStates()) {
            EXPECT_TRUE(b.position.isFinite());
            EXPECT_TRUE(b.linearVelocity.isFinite());
            EXPECT_LE(b.linearVelocity.length(), 8.5f);
        }
    }
}
TEST(BlackHole, MigrationPreservesSwallowedAndCurrentResetLayout) {
    Playground p(scene());
    p.setBlackHole(true);
    advance(p);
    auto f = p.freezeForMigration();
    const auto before = gravity(p).blackHole();
    advance(p);
    EXPECT_EQ(gravity(p).blackHole().angle, before.angle);
    auto next = Playground::prepareMigration(f, {1000, 800});
    next->commitMigration();
    EXPECT_EQ(gravity(*next).blackHole().objects[0].phase, SwallowPhase::Swallowed);
    EXPECT_FALSE(next->mode().world().state(0).enabled);
    EXPECT_EQ(gravity(*next).blackHole().center, (Vec2{500, 400}));
    next->reset();
    next->tick(0);
    EXPECT_TRUE(next->mode().world().state(0).enabled);
    EXPECT_TRUE(gravity(*next).blackHole().enabled);
}
TEST(BlackHole, ExitAndMigrationRejectLateEnable) {
    Playground p(scene());
    p.freezeForMigration();
    p.setBlackHole(true);
    EXPECT_FALSE(gravity(p).blackHole().enabled);
    Playground q(scene());
    q.input({InputType::RequestExit, {}, 0});
    q.setBlackHole(true);
    EXPECT_FALSE(gravity(q).blackHole().enabled);
}
TEST(BlackHole, NearbyIconAttractsWithTangentialMotion) {
    auto s = scene();
    s.icons.erase(s.icons.begin());
    Playground p(s);
    p.setBlackHole(true);
    advance(p, 12);
    const auto b = p.mode().world().state(0);
    EXPECT_LT(gravity(p).bodySceneCenter(0).x, 530);
    EXPECT_LT(b.linearVelocity.x, 0);
    EXPECT_TRUE(b.linearVelocity.isFinite());
}
TEST(BlackHole, GrabbedBodyMatchesOrdinaryGravity) {
    Playground normal(scene()), hole(scene());
    hole.setBlackHole(true);
    for (auto* p : {&normal, &hole}) p->input({InputType::PointerDown, {530, 300}, 0, TextureId{2}});
    advance(normal, 12);
    advance(hole, 12);
    const auto a = normal.mode().world().state(1), b = hole.mode().world().state(1);
    EXPECT_NEAR(a.position.x, b.position.x, 1e-5);
    EXPECT_NEAR(a.position.y, b.position.y, 1e-5);
}
TEST(BlackHole, ResetPreservesMovedPositionAndPausedMigrationAnimation) {
    Playground p(scene());
    p.setBlackHole(true);
    advance(p, 3);
    p.input({InputType::TogglePause, {}, 0});
    const auto frozen = p.freezeForMigration();
    auto next = Playground::prepareMigration(frozen, {900, 700});
    next->commitMigration();
    advance(*next);
    EXPECT_TRUE(next->paused());
    EXPECT_EQ(gravity(*next).blackHole().objects[0].elapsed, frozen.blackHole.objects[0].elapsed);
    const auto center = gravity(*next).blackHole().center;
    next->reset();
    EXPECT_EQ(gravity(*next).blackHole().center, center);
    next->setBlackHole(false);
    EXPECT_FALSE(gravity(*next).blackHole().enabled);
    EXPECT_EQ(gravity(*next).blackHole().objects[0].phase, SwallowPhase::Normal);
}
namespace {
template <class T>
std::vector<size_t> indicesOf(const DrawList& d) {
    std::vector<size_t> r;
    for (size_t i = 0; i < d.commands().size(); ++i)
        if (std::holds_alternative<T>(d.commands()[i])) r.push_back(i);
    return r;
}
}  // namespace
TEST(BlackHole, ClassicStyleIsUnchangedDefault) {
    Playground p(scene());
    p.setBlackHole(true);
    EXPECT_EQ(gravity(p).blackHole().style, BlackHoleStyle::Classic);
    EXPECT_TRUE(indicesOf<BlackHoleItem>(p.drawList()).empty());
    EXPECT_EQ(indicesOf<CircleItem>(p.drawList()).size(), 1u);
}
TEST(BlackHole, CinematicLayersSwallowingCopiesBetweenDiskHalves) {
    Playground p(scene());
    p.setBlackHoleStyle(BlackHoleStyle::Cinematic);
    p.setBlackHole(true);
    EXPECT_TRUE(indicesOf<CircleItem>(p.drawList()).empty());
    advance(p, 3);  // icon 1 sits at the centre and starts swallowing
    ASSERT_EQ(gravity(p).blackHole().objects[0].phase, SwallowPhase::Swallowing);
    const auto holes = indicesOf<BlackHoleItem>(p.drawList());
    ASSERT_EQ(holes.size(), 2u);
    const auto& cmds = p.drawList().commands();
    EXPECT_EQ(std::get<BlackHoleItem>(cmds[holes[0]]).layer, BlackHoleItem::Layer::Back);
    EXPECT_EQ(std::get<BlackHoleItem>(cmds[holes[1]]).layer, BlackHoleItem::Layer::Front);
    for (size_t i = 0; i < cmds.size(); ++i) {
        const auto* s = std::get_if<SpriteItem>(&cmds[i]);
        if (!s) continue;
        const bool swallowing = s->texture == TextureId{1};
        EXPECT_EQ(i > holes[0] && i < holes[1], swallowing);
        EXPECT_EQ(s->interactive, !swallowing);
        EXPECT_TRUE(s->center.isFinite() && s->size.isFinite());
        if (swallowing) {
            EXPECT_GT(s->consume, 0.0f);
        }
    }
    EXPECT_EQ(mirrorHit(p.drawList(), {400, 300}), kNoTexture);
    advance(p);  // 0.83 s: cinematic dissolve (1.5 s) is still running
    EXPECT_EQ(gravity(p).blackHole().objects[0].phase, SwallowPhase::Swallowing);
    advance(p, 100);
    EXPECT_EQ(gravity(p).blackHole().objects[0].phase, SwallowPhase::Swallowed);
    for (const auto& c : p.drawList().commands())
        if (const auto* s = std::get_if<SpriteItem>(&c); s && s->texture == TextureId{1}) {
            EXPECT_LE(s->opacity, 0.0f);
            EXPECT_FLOAT_EQ(s->consume, 1.0f);
        }
}
TEST(BlackHole, CinematicTidalProgressAndGrabFlag) {
    Playground p({{800, 600},
                  {{IconId{1}, TextureId{1}, {400 + 42 * 2, 300}, {48, 48}},
                   {IconId{2}, TextureId{2}, {60, 60}, {48, 48}}}});
    p.setBlackHoleStyle(BlackHoleStyle::Cinematic);
    p.setBlackHole(true);
    float nearTidal = -1, farTidal = -1;
    for (const auto& c : p.drawList().commands())
        if (const auto* s = std::get_if<SpriteItem>(&c))
            (s->texture == TextureId{1} ? nearTidal : farTidal) = s->tidal;
    EXPECT_GT(nearTidal, 0.3f);
    EXPECT_LT(nearTidal, 1.0f);
    EXPECT_EQ(farTidal, 0.0f);
    p.input({InputType::PointerDown, {60, 60}, 0, TextureId{2}});
    p.input({InputType::PointerMove, {420, 300}, .05});
    advance(p, 30);
    for (const auto& c : p.drawList().commands())
        if (const auto* s = std::get_if<SpriteItem>(&c); s && s->texture == TextureId{2}) {
            EXPECT_TRUE(s->grabbed);
            EXPECT_GT(s->tidal, 0.0f);  // grabbed copies shed particles but are not swallowed
            EXPECT_EQ(s->consume, 0.0f);
        }
    p.input({InputType::PointerCancel, {}, 1});
}
TEST(BlackHole, ClassicSpriteHasNoCinematicFields) {
    Playground p(scene());
    p.setBlackHole(true);
    advance(p, 3);
    for (const auto& c : p.drawList().commands())
        if (const auto* s = std::get_if<SpriteItem>(&c)) {
            EXPECT_EQ(s->tidal, 0.0f);
            EXPECT_EQ(s->consume, 0.0f);
        }
}
TEST(BlackHole, DevourAllEventuallySwallowsEveryCopy) {
    SceneSetup s{{1000, 700}, {}};
    for (uint32_t i = 0; i < 8; ++i)
        s.icons.push_back({IconId{i + 1},
                           TextureId{i + 1},
                           {60.0f + 120.0f * static_cast<float>(i), i % 2 ? 640.0f : 80.0f},
                           {48, 48}});
    Playground p(s);
    p.setBlackHoleStyle(BlackHoleStyle::Cinematic);
    p.setBlackHole(true);
    advance(p, 120 * 6);
    size_t swallowedWithout = 0;
    for (const auto& o : gravity(p).blackHole().objects) swallowedWithout += o.phase != SwallowPhase::Normal;
    EXPECT_LT(swallowedWithout, 8u);  // base range does not reach the corners
    p.setBlackHoleDevour(true);
    EXPECT_EQ(gravity(p).blackHole().devourElapsed, 0.0f);
    bool all = false;
    for (int second = 0; second < 90 && !all; ++second) {
        advance(p, 120);
        all = true;
        for (const auto& o : gravity(p).blackHole().objects) all = all && o.phase == SwallowPhase::Swallowed;
        for (BodyIndex i = 0; i < 8; ++i) {
            const auto b = p.mode().world().state(i);
            ASSERT_TRUE(b.position.isFinite() && b.linearVelocity.isFinite());
        }
    }
    EXPECT_TRUE(all);
    EXPECT_GT(gravity(p).blackHole().effectiveRange({1000, 700}), gravity(p).blackHole().range);
    p.reset();  // restores every copy and restarts the ramp, devour stays selected
    EXPECT_TRUE(gravity(p).blackHole().devourAll);
    EXPECT_EQ(gravity(p).blackHole().devourElapsed, 0.0f);
    EXPECT_TRUE(p.mode().world().state(0).enabled);
    p.setBlackHoleDevour(false);
    EXPECT_FLOAT_EQ(gravity(p).blackHole().effectiveRange({1000, 700}), gravity(p).blackHole().range);
    const auto frozen = p.freezeForMigration();
    p.setBlackHoleDevour(true);  // refused while migration is pending
    EXPECT_FALSE(gravity(p).blackHole().devourAll);
    (void)frozen;
}
TEST(BlackHole, CinematicDragsFromShadowButIconsWin) {
    Playground p({{800, 600}, {{IconId{1}, TextureId{1}, {100, 100}, {48, 48}}}});
    p.setBlackHoleStyle(BlackHoleStyle::Cinematic);
    p.setBlackHole(true);
    const float r0 = gravity(p).blackHole().radius;
    EXPECT_FLOAT_EQ(r0, 600.0f * 0.115f);
    EXPECT_TRUE(p.blackHoleEdge({400, 300}));
    EXPECT_TRUE(p.blackHoleEdge({400.0f + r0 * 1.2f, 300}));
    EXPECT_FALSE(p.blackHoleEdge({400.0f + r0 * 1.6f + 10.0f, 300}));
    p.input({InputType::PointerDown, {400, 300}, 0});
    p.input({InputType::PointerMove, {450, 320}, .1});
    EXPECT_EQ(gravity(p).blackHole().center, (Vec2{450, 320}));
    p.input({InputType::PointerUp, {450, 320}, .2});
    // An icon reported under the pointer is grabbed instead of the hole.
    p.input({InputType::PointerDown, {450, 320}, .3, TextureId{1}});
    p.input({InputType::PointerMove, {500, 320}, .4});
    EXPECT_EQ(gravity(p).blackHole().center, (Vec2{450, 320}));
    EXPECT_TRUE(p.mode().world().grabbedBody().has_value());
    p.input({InputType::PointerCancel, {}, .5});
}
TEST(BlackHole, ScaleIsClampedAndSurvivesResetAndMigration) {
    Playground p(scene());
    p.setBlackHoleStyle(BlackHoleStyle::Cinematic);
    p.setBlackHoleScale(1.5f);
    p.setBlackHole(true);
    EXPECT_FLOAT_EQ(gravity(p).blackHole().radius, 600.0f * 0.115f * 1.5f);
    EXPECT_FLOAT_EQ(gravity(p).blackHole().range, 600.0f * 0.45f * 1.5f);
    p.setBlackHoleScale(99);
    EXPECT_FLOAT_EQ(gravity(p).blackHole().scale, BlackHoleState::kMaxScale);
    p.setBlackHoleScale(std::nanf(""));
    EXPECT_FLOAT_EQ(gravity(p).blackHole().scale, BlackHoleState::kMaxScale);
    p.setBlackHoleScale(.1f);
    EXPECT_FLOAT_EQ(gravity(p).blackHole().scale, BlackHoleState::kMinScale);
    EXPECT_FLOAT_EQ(gravity(p).blackHole().radius, 600.0f * 0.115f * BlackHoleState::kMinScale);
    p.setBlackHoleScale(1.5f);
    p.reset();
    EXPECT_FLOAT_EQ(gravity(p).blackHole().radius, 600.0f * 0.115f * 1.5f);
    const auto frozen = p.freezeForMigration();
    p.setBlackHoleScale(1);  // refused while migration is pending
    EXPECT_FLOAT_EQ(gravity(p).blackHole().scale, 1.5f);
    auto next = Playground::prepareMigration(frozen, {1000, 800});
    EXPECT_FLOAT_EQ(gravity(*next).blackHole().radius, 800.0f * 0.115f * 1.5f);
    EXPECT_EQ(gravity(*next).blackHole().style, BlackHoleStyle::Cinematic);
}
TEST(BlackHole, AnglesClampOptionOrbitAndSurviveResetAndMigration) {
    Playground p({{800, 600}, {{IconId{1}, TextureId{1}, {100, 100}, {48, 48}}}});
    p.setBlackHoleStyle(BlackHoleStyle::Cinematic);
    p.setBlackHole(true);
    EXPECT_FLOAT_EQ(gravity(p).blackHole().elevation, BlackHoleState::kDefaultElevation);
    EXPECT_FLOAT_EQ(gravity(p).blackHole().tilt, 0.0f);

    p.setBlackHoleAngles(99.0f, -99.0f);
    EXPECT_FLOAT_EQ(gravity(p).blackHole().elevation, BlackHoleState::kMaxElevation);
    EXPECT_FLOAT_EQ(gravity(p).blackHole().tilt, BlackHoleState::kMinTilt);
    p.setBlackHoleAngles(std::nanf(""), std::nanf(""));
    EXPECT_FLOAT_EQ(gravity(p).blackHole().elevation, BlackHoleState::kMaxElevation);
    EXPECT_FLOAT_EQ(gravity(p).blackHole().tilt, BlackHoleState::kMinTilt);

    p.setBlackHoleAngles(.35f, -.45f);
    const auto holes = indicesOf<BlackHoleItem>(p.drawList());
    ASSERT_EQ(holes.size(), 2u);
    EXPECT_FLOAT_EQ(std::get<BlackHoleItem>(p.drawList().commands()[holes[0]]).elevation, .35f);
    EXPECT_FLOAT_EQ(std::get<BlackHoleItem>(p.drawList().commands()[holes[0]]).tilt, -.45f);

    // Option-drag accepts the accretion ring (outside normal shadow drag) and orbits without moving center.
    const Vec2 ringHit{400 + gravity(p).blackHole().radius * 2.2f, 300};
    EXPECT_FALSE(p.blackHoleEdge(ringHit, false));
    EXPECT_TRUE(p.blackHoleEdge(ringHit, true));
    p.input({InputType::PointerDown, ringHit, 0, kNoTexture, true});
    EXPECT_TRUE(p.mode().stats().grabbing);
    p.input({InputType::PointerMove, {ringHit.x + 50, ringHit.y + 40}, .1, kNoTexture, true});
    EXPECT_EQ(gravity(p).blackHole().center, (Vec2{400, 300}));
    EXPECT_NEAR(gravity(p).blackHole().elevation, .35f + 40 * .012f, 1e-4f);
    EXPECT_NEAR(gravity(p).blackHole().tilt, -.45f + 50 * .012f, 1e-4f);
    p.input({InputType::PointerUp, {ringHit.x + 50, ringHit.y + 40}, .2});
    EXPECT_FALSE(p.mode().stats().grabbing);

    const float savedEl = gravity(p).blackHole().elevation, savedTilt = gravity(p).blackHole().tilt;
    p.reset();
    EXPECT_FLOAT_EQ(gravity(p).blackHole().elevation, savedEl);
    EXPECT_FLOAT_EQ(gravity(p).blackHole().tilt, savedTilt);
    const auto frozen = p.freezeForMigration();
    p.setBlackHoleAngles(0, 0);  // refused while migration is pending
    EXPECT_FLOAT_EQ(gravity(p).blackHole().elevation, savedEl);
    auto next = Playground::prepareMigration(frozen, {1000, 800});
    EXPECT_FLOAT_EQ(gravity(*next).blackHole().elevation, savedEl);
    EXPECT_FLOAT_EQ(gravity(*next).blackHole().tilt, savedTilt);
}

TEST(BlackHole, EmitsOpeningHumPeriodicPulseAndSwallowChirp) {
    Playground p(scene());
    std::vector<SoundEvent> sounds;
    p.setBlackHoleStyle(BlackHoleStyle::Cinematic);
    p.setBlackHole(true);
    p.drainSounds(sounds);

    bool sawHum = false;
    for (const auto& s : sounds) {
        if (s.kind == SoundKind::BlackHoleHum) sawHum = true;
    }
    EXPECT_TRUE(sawHum);

    bool sawSwallow = false;
    int humPulses = 0;
    for (int i = 0; i < 80; ++i) {
        p.tick(1.0 / 60.0);
        p.drainSounds(sounds);
        for (const auto& s : sounds) {
            if (s.kind == SoundKind::BlackHoleSwallow) sawSwallow = true;
            if (s.kind == SoundKind::BlackHoleHum) ++humPulses;
        }
    }
    EXPECT_TRUE(sawSwallow);
    EXPECT_GE(humPulses, 3);
}
