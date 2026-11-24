#include <gtest/gtest.h>

#include "core/scene/coordinates.h"
#include "platforms/windows/mirror_preview.h"

using namespace dpp::winmirror;
using namespace dpp::winprobe;
namespace {
Report fixture() {
    Report r;
    r.snapshot = "fixture-only";
    r.environment.session = r.environment.shellSession = 1;
    r.environment.stationVisible = r.environment.inputDesktop = true;
    r.environment.shellWindow = reinterpret_cast<HWND>(1);
    for (int i = 0; i < 3; ++i) {
        Sample s;
        s.id = r.snapshot + "/S" + std::to_string(i + 1);
        // Minimal distinct opaque child PIDLs; never sent to the real Shell.
        auto* bytes = static_cast<unsigned char*>(CoTaskMemAlloc(5));
        if (!bytes) throw std::bad_alloc();
        bytes[0] = 3;
        bytes[1] = 0;
        bytes[2] = static_cast<unsigned char>(i + 1);
        bytes[3] = bytes[4] = 0;
        s.child.reset(reinterpret_cast<PITEMID_CHILD>(bytes));
        s.bitmap.reset(CreateBitmap(64, 64, 1, 32, nullptr));
        s.itemHr = s.imageHr = S_OK;
        setPosition(s, S_OK, {-30 + i * 50, 12});
        r.samples.push_back(std::move(s));
    }
    return r;
}
struct OwnWindow {
    HWND value = CreateWindowExW(0, L"STATIC", L"DPP test", WS_POPUP, 30, 45, 100, 80, nullptr, nullptr,
                                 GetModuleHandleW(nullptr), nullptr);
    ~OwnWindow() {
        if (value) DestroyWindow(value);
    }
};
}  // namespace
TEST(WindowsMirror, SnapshotIdentityBitmapAndPositionStayAssociated) {
    auto r = fixture();
    ASSERT_TRUE(snapshotUsable(r));
    std::swap(r.samples[0], r.samples[1]);
    EXPECT_FALSE(snapshotUsable(r));
    std::swap(r.samples[0], r.samples[1]);
    r.samples[1].bitmap.reset();
    EXPECT_FALSE(snapshotUsable(r));
}
TEST(WindowsMirror, RejectsChangedUnknownAndPartialSnapshots) {
    auto r = fixture();
    r.displayChanged = true;
    EXPECT_FALSE(snapshotUsable(r));
    r.displayChanged = false;
    r.environment.inputDesktop.reset();
    EXPECT_FALSE(snapshotUsable(r));
    r.environment.inputDesktop = true;
    setPosition(r.samples[0], E_FAIL, {1, 2});
    EXPECT_FALSE(snapshotUsable(r));
}
TEST(WindowsMirror, SnapshotReadRejectsReuseAndInvalidCountWithoutShellAccess) {
    Report r;
    DesktopView view;
    EXPECT_EQ(captureSamples(r, 2, view), 2);
    EXPECT_TRUE(r.snapshot.empty());
    r = fixture();
    EXPECT_EQ(captureSamples(r, 5, view), 2);
    EXPECT_EQ(r.samples.size(), 3u);
    EXPECT_FALSE(view.shell);
}
TEST(WindowsMirror, ZeroMapOffsetIsSuccessAndInvalidWindowIsFailure) {
    OwnWindow w;
    ASSERT_TRUE(w.value);
    auto p = mapPoint(w.value, w.value, {-30, 7});
    EXPECT_EQ(p.x, -30);
    EXPECT_EQ(p.y, 7);
    EXPECT_THROW(mapPoint(reinterpret_cast<HWND>(1), nullptr, {0, 0}), std::runtime_error);
}
TEST(WindowsMirror, NativeMappingRoundTripsAndSceneUsesExplicitDpi) {
    for (int i = 0; i < 3; ++i) {
        OwnWindow w;
        ASSERT_TRUE(w.value);
        auto screen = mapPoint(w.value, nullptr, {17, 2});
        auto back = mapPoint(nullptr, w.value, screen);
        EXPECT_EQ(back.x, 17);
        EXPECT_EQ(back.y, 2);
        dpp::PixelRect work{-1920, -200, 0, 880};
        auto scene = dpp::windowsScreenPixelsToScene(screen.x, screen.y, work, dpp::windowsDpiScale(120));
        auto pixels = dpp::windowsSceneToScreenPixels(scene, work, dpp::windowsDpiScale(120));
        EXPECT_NEAR(pixels.x, static_cast<float>(screen.x), 0.001f);
        EXPECT_NEAR(pixels.y, static_cast<float>(screen.y), 0.001f);
    }
}

TEST(WindowsMirror, CustomCopiesUseSnapshotTextureAndExplicitTopLeftSize) {
    auto r = fixture();
    const std::vector<dpp::Vec2> anchors{{17, 2}, {17, 125}, {112, 2}};
    const auto setup = makePlaybackScene(r, anchors, {1000, 800}, 48, 96);
    ASSERT_EQ(setup.icons.size(), 3u);
    for (std::size_t i = 0; i < anchors.size(); ++i) {
        EXPECT_EQ(static_cast<unsigned>(setup.icons[i].texture), i + 1);
        EXPECT_FLOAT_EQ(setup.icons[i].center.x, anchors[i].x + 24);
        EXPECT_FLOAT_EQ(setup.icons[i].center.y, anchors[i].y + 24);
        EXPECT_FLOAT_EQ(setup.icons[i].iconSize.x, 48);
    }
    const auto scaled = makePlaybackScene(r, anchors, {1000, 800}, 48, 120);
    EXPECT_FLOAT_EQ(scaled.icons[0].iconSize.x, 38.4f);
    EXPECT_FLOAT_EQ(scaled.icons[0].center.x, anchors[0].x + 19.2f);
    const auto shifted = makePlaybackScene(r, anchors, {1000, 800}, 48, 120, 4);
    for (std::size_t i = 0; i < anchors.size(); ++i) {
        EXPECT_FLOAT_EQ(shifted.icons[i].center.x, scaled.icons[i].center.x);
        EXPECT_NEAR(shifted.icons[i].center.y - scaled.icons[i].center.y, 3.2f, .0001f);
        EXPECT_EQ(shifted.icons[i].iconSize, scaled.icons[i].iconSize);
        EXPECT_EQ(shifted.icons[i].texture, scaled.icons[i].texture);
    }
    EXPECT_THROW(makePlaybackScene(r, anchors, {1000, 800}, 0, 120), std::invalid_argument);
    EXPECT_THROW(makePlaybackScene(r, anchors, {1000, 800}, 48, 0), std::runtime_error);
    EXPECT_THROW(makePlaybackScene(r, {{1, 2}}, {1000, 800}, 48, 96), std::runtime_error);
    r.samples[0].bitmap.reset();
    EXPECT_THROW(makePlaybackScene(r, anchors, {1000, 800}, 48, 96), std::runtime_error);
}

TEST(WindowsMirror, PlaybackRequiresStaticConfirmationAndNeverGrabs) {
    auto r = fixture();
    auto setup = makePlaybackScene(r, {{20, 20}, {120, 20}, {220, 20}}, {1000, 800}, 48, 96);
    PreviewPlayback playback(setup);
    EXPECT_FALSE(playback.gravityStarted());
    EXPECT_FALSE(playback.tick(1));
    EXPECT_THROW(playback.startGravity(false), std::logic_error);
    EXPECT_FALSE(playback.gravityStarted());
    const auto start = std::get<dpp::SpriteItem>(playback.drawList().commands()[0]).center;
    playback.startGravity(true);
    for (int i = 0; i < 30; ++i) playback.tick(1.0 / 60);
    auto dropped = std::get<dpp::SpriteItem>(playback.drawList().commands()[0]).center;
    EXPECT_GT(dropped.y, start.y);
    EXPECT_FALSE(playback.grabbing());
    playback.togglePause();
    EXPECT_TRUE(playback.paused());
    for (int i = 0; i < 30; ++i) playback.tick(1.0 / 60);
    EXPECT_FLOAT_EQ(std::get<dpp::SpriteItem>(playback.drawList().commands()[0]).center.y, dropped.y);
    playback.reset();
    EXPECT_NEAR(std::get<dpp::SpriteItem>(playback.drawList().commands()[0]).center.y, start.y, .001f);
    EXPECT_TRUE(playback.paused());
    playback.togglePause();
    EXPECT_FALSE(playback.paused());
    playback.stop();
    EXPECT_FALSE(playback.gravityStarted());
    EXPECT_FALSE(playback.grabbing());
    for (int i = 0; i < 3; ++i) {
        playback.startGravity(true);
        playback.tick(.02);
        playback.stop();
    }
    EXPECT_FALSE(playback.gravityStarted());
}

TEST(WindowsMirror, UnknownOrDifferentDisplayProfileNeverEnablesCopiesAutomatically) {
    Report r;
    ViewContract c;
    EXPECT_FALSE(matchesValidatedMarkerConfiguration(r, c, 120));
    r.osMajor = 10;
    r.osMinor = 0;
    r.osBuild = 19045;
    r.osRevision = 6466;
    r.nativeArchitecture = r.programArchitecture = "x64";
    r.threadDpi = r.viewDpiContext = "per_monitor_v2";
    r.monitors.emplace_back();
    r.monitors[0].bounds = RECT{0, 0, 1920, 1080};
    c.work = {0, 0, 1920, 1030};
    c.listScreen = {0, 0, 1920, 1080};
    c.dpi = 120;
    EXPECT_TRUE(matchesValidatedMarkerConfiguration(r, c, 120));
    EXPECT_EQ(startupCalibrationPixels(r, c, 120), 5);
    EXPECT_EQ(startupCalibrationPixels(r, c, 144), 0);
    c.work.left = -20;
    EXPECT_EQ(startupCalibrationPixels(r, c, 120), 0);
    c.work.left = 0;
    EXPECT_FALSE(matchesValidatedMarkerConfiguration(r, c, 144));
    r.osBuild = 22631;
    EXPECT_FALSE(matchesValidatedMarkerConfiguration(r, c, 120));
}

TEST(WindowsMirror, PointerInputRequiresExplicitInteractionAfterPlayback) {
    auto r = fixture();
    auto setup = makePlaybackScene(r, {{20, 20}, {120, 20}, {220, 20}}, {1000, 800}, 48, 96);
    PreviewPlayback playback(setup);
    EXPECT_THROW(playback.enterInteraction(true), std::logic_error);
    playback.startGravity(true);
    playback.pointer({dpp::InputType::PointerDown, {44, 44}, 1});
    EXPECT_FALSE(playback.grabbing());
    EXPECT_THROW(playback.enterInteraction(false), std::logic_error);
    playback.enterInteraction(true);
    playback.pointer({dpp::InputType::PointerDown, {44, 44}, 2});
    ASSERT_TRUE(playback.grabbing());
    for (int i = 0; i < 20; ++i) {
        playback.pointer(
            {dpp::InputType::PointerMove, {static_cast<float>(i * 100 - 300), 80}, 2.01 + .01 * i});
        playback.tick(.01);
        ASSERT_TRUE(playback.grabbing());
    }
    playback.pointer({dpp::InputType::PointerUp, {500, 100}, 2.3});
    EXPECT_FALSE(playback.grabbing());
    EXPECT_TRUE(playback.interacting());
    playback.leaveInteraction();
    EXPECT_FALSE(playback.interacting());
    playback.pointer({dpp::InputType::PointerDown, {144, 44}, 3});
    EXPECT_FALSE(playback.grabbing());
}

TEST(WindowsMirror, FocusPauseResetAndStopCancelAnActiveGrab) {
    auto r = fixture();
    auto setup = makePlaybackScene(r, {{20, 20}, {120, 20}, {220, 20}}, {1000, 800}, 48, 96);
    PreviewPlayback playback(setup);
    playback.startGravity(true);
    auto grab = [&] {
        playback.enterInteraction(true);
        playback.pointer({dpp::InputType::PointerDown, {44, 44}, 1});
        ASSERT_TRUE(playback.grabbing());
    };
    grab();
    playback.leaveInteraction();
    EXPECT_FALSE(playback.grabbing());
    grab();
    playback.togglePause();
    EXPECT_FALSE(playback.grabbing());
    EXPECT_FALSE(playback.interacting());
    EXPECT_THROW(playback.enterInteraction(true), std::logic_error);
    playback.togglePause();
    grab();
    playback.reset();
    EXPECT_FALSE(playback.grabbing());
    EXPECT_FALSE(playback.interacting());
    grab();
    playback.stop();
    EXPECT_FALSE(playback.grabbing());
    EXPECT_FALSE(playback.interacting());
    EXPECT_FALSE(playback.gravityStarted());
}

TEST(WindowsMirror, SystemIconCopyKeepsPixelDimensionsAndOwnsItsBitmap) {
    struct Com {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        ~Com() {
            if (SUCCEEDED(hr)) CoUninitialize();
        }
    } com;
    ASSERT_TRUE(SUCCEEDED(com.hr));
    EXPECT_THROW(bitmapFromIcon(nullptr), std::runtime_error);
    // Synthetic owned icon only. No real Shell or desktop access.
    std::vector<BYTE> mask(8 * 60, 0);
    std::vector<DWORD> pixels(60 * 60, 0xff336699u);
    struct Icon {
        HICON value;
        ~Icon() {
            if (value) DestroyIcon(value);
        }
    } icon{CreateIcon(nullptr, 60, 60, 1, 32, mask.data(), reinterpret_cast<const BYTE*>(pixels.data()))};
    ASSERT_TRUE(icon.value);
    auto bitmap = bitmapFromIcon(icon.value);
    BITMAP info{};
    ASSERT_EQ(GetObjectW(bitmap.get(), sizeof(info), &info), static_cast<int>(sizeof(info)));
    EXPECT_EQ(info.bmWidth, 60);
    EXPECT_EQ(info.bmHeight, 60);
    ASSERT_TRUE(info.bmBits);
    EXPECT_EQ(static_cast<const DWORD*>(info.bmBits)[30 * 60 + 30], 0xff336699u);
    ASSERT_TRUE(DestroyIcon(icon.value));
    icon.value = nullptr;
    // The bitmap remains alive after the supplied icon is destroyed.
    ASSERT_EQ(GetObjectW(bitmap.get(), sizeof(info), &info), static_cast<int>(sizeof(info)));
    auto r = fixture();
    auto scene = makePlaybackScene(r, {{0, 0}, {100, 0}, {200, 0}}, {1000, 800}, 60, 120);
    EXPECT_FLOAT_EQ(scene.icons[0].iconSize.x, 48);
}

TEST(WindowsMirror, ProtectedSnapshotRequiresAllItemsAndEveryResource) {
    auto r = fixture();
    r.fullSnapshot = true;
    r.itemCount = 15;
    EXPECT_FALSE(snapshotUsable(r));  // three valid samples cannot replace fifteen originals
    r.itemCount = 3;
    ASSERT_TRUE(snapshotUsable(r));
    r.samples[2].bitmap.reset();
    EXPECT_FALSE(snapshotUsable(r));
    r = fixture();
    r.fullSnapshot = true;
    r.samples.resize(1);
    r.itemCount = 1;
    EXPECT_TRUE(snapshotUsable(r));  // explicit small test desktop
    r.itemCount.reset();
    EXPECT_FALSE(snapshotUsable(r));
}

TEST(WindowsMirror, OpenHandoffReleasesGrabWithoutResettingMovedMirrors) {
    auto r = fixture();
    PreviewPlayback playback(makePlaybackScene(r, {{20, 20}, {120, 20}, {220, 20}}, {1000, 800}, 48, 96));
    playback.startGravity(true);
    playback.enterInteraction(true);
    playback.pointer({dpp::InputType::PointerDown, {44, 44}, 1});
    ASSERT_TRUE(playback.grabbing());
    playback.pointer({dpp::InputType::PointerMove, {450, 350}, 1.1});
    playback.tick(.02);
    const auto before = playback.drawList();
    playback.leaveInteraction();
    EXPECT_FALSE(playback.grabbing());
    EXPECT_TRUE(playback.gravityStarted());
    EXPECT_EQ(playback.drawList().commands().size(), before.commands().size());
    for (size_t i = 0; i < before.commands().size(); ++i) {
        const auto* a = std::get_if<dpp::SpriteItem>(&before.commands()[i]);
        const auto* b = std::get_if<dpp::SpriteItem>(&playback.drawList().commands()[i]);
        if (!a) continue;
        ASSERT_TRUE(b);
        EXPECT_EQ(a->texture, b->texture);
        EXPECT_FLOAT_EQ(a->center.x, b->center.x);
        EXPECT_FLOAT_EQ(a->center.y, b->center.y);
        EXPECT_FLOAT_EQ(a->rotation, b->rotation);
    }
    playback.enterInteraction(true);
    EXPECT_TRUE(playback.interacting());
}

#include "platforms/mirror_hit.h"
#include "platforms/windows/filename_labels.h"
TEST(WindowsMirror, FilenameLayoutsHandleUnicodeAndTrimLongNamesWithoutChangingIdentity) {
    auto report = fixture();
    report.samples[0].localName = L"文件 🗂 image.json";
    report.samples[1].localName = std::wstring(250, L'长') + L".png";
    report.samples[2].localName = L"folder";
    FilenameLabels labels;
    labels.cache(report);
    ASSERT_EQ(labels.size(), 3u);
    EXPECT_EQ(labels.at(dpp::kNoTexture), nullptr);
    EXPECT_EQ(labels.at(dpp::TextureId{4}), nullptr);
    for (unsigned i = 1; i <= 3; ++i) {
        auto* layout = labels.at(dpp::TextureId{i});
        ASSERT_NE(layout, nullptr);
        DWRITE_TEXT_METRICS metrics{};
        ASSERT_TRUE(SUCCEEDED(layout->GetMetrics(&metrics)));
        EXPECT_GT(metrics.width, 0);
        EXPECT_LE(metrics.height, filenameHeight);
        // DirectWrite includes pre-trimming wrapped lines in lineCount. The effective
        // layout height and the second line's ellipsis determine the visible result.
        EXPECT_FLOAT_EQ(layout->GetMaxWidth(), filenameWidth);
        EXPECT_FLOAT_EQ(layout->GetMaxHeight(), filenameHeight);
    }
    UINT32 count = 0;
    labels.at(dpp::TextureId{2})->GetLineMetrics(nullptr, 0, &count);
    ASSERT_GT(count, 1u);
    std::vector<DWRITE_LINE_METRICS> lines(count);
    ASSERT_TRUE(SUCCEEDED(labels.at(dpp::TextureId{2})->GetLineMetrics(lines.data(), count, &count)));
    EXPECT_TRUE(lines[1].isTrimmed);
    auto* cached = labels.at(dpp::TextureId{1});
    EXPECT_EQ(labels.fit(dpp::TextureId{1}, 52), cached);
    EXPECT_FLOAT_EQ(cached->GetMaxWidth(), 52);
    EXPECT_EQ(labels.fit(dpp::TextureId{1}, 52), cached);
    EXPECT_EQ(labels.size(), 3u);
    EXPECT_TRUE(snapshotUsable(report));
}

TEST(WindowsMirror, FilenameGeometryStaysInWorkAreaAndDoesNotBecomeAnOpenTarget) {
    const dpp::Vec2 work{800, 600};
    dpp::SpriteItem sprite{dpp::TextureId{7}, {400, 570}, {80, 40}, 1.5707963f};
    const auto atFloor = filenameBounds(sprite, work);
    EXPECT_LT(atFloor.y + atFloor.height, sprite.center.y);
    dpp::DrawList draw;
    draw.add(sprite);
    EXPECT_EQ(dpp::mirrorHit(draw, {atFloor.x + 60, atFloor.y + 16}), dpp::kNoTexture);
    EXPECT_EQ(dpp::mirrorHit(draw, sprite.center), sprite.texture);
    for (const auto position : {dpp::Vec2{10, 10}, dpp::Vec2{790, 590}}) {
        sprite.center = position;
        const auto bounds = filenameBounds(sprite, work);
        EXPECT_GE(bounds.x, 0);
        EXPECT_GE(bounds.y, 0);
        EXPECT_LE(bounds.x + bounds.width, work.x);
        EXPECT_LE(bounds.y + bounds.height, work.y);
    }
}

TEST(WindowsMirror, MigrationListBoundsWaitPreservesFrozenSceneAndOriginalDeadline) {
    for (const bool paused : {false, true}) {
        auto r = fixture();
        PreviewPlayback playback(
            makePlaybackScene(r, {{20, 20}, {120, 20}, {220, 20}}, {1000, 800}, 48, 96));
        playback.startGravity(true);
        if (paused) playback.togglePause();
        dpp::DisplayMigration migration;
        migration.begin(*playback.game(), 10.0);
        const auto* frozen = migration.frozenScene();
        migration.prepare({900, 700});
        const auto stale = migration.revision();
        ViewContract sample;
        sample.bounds = {0, 0, 1680, 1050};
        sample.listScreen = {0, 0, 1920, 1080};
        EXPECT_FALSE(migrationListBoundsReady(migration, sample, 10.1));
        EXPECT_EQ(migration.candidate(), nullptr);
        EXPECT_EQ(migration.frozenScene(), frozen);
        EXPECT_FALSE(migration.commit(stale, 10.2, true));
        sample.listScreen = sample.bounds;
        EXPECT_TRUE(migrationListBoundsReady(migration, sample, 10.2));
        EXPECT_FALSE(migration.settled(10.2));
        EXPECT_TRUE(migration.settled(10.3));
        migration.prepare({900, 700});
        auto candidate = migration.commit(migration.revision(), 10.3, true);
        ASSERT_TRUE(candidate);
        playback.commitMigration(std::move(candidate));
        EXPECT_EQ(playback.paused(), paused);
    }
}

TEST(WindowsMirror, MigrationListBoundsNeverSettlesStillExpiresAndCancelsPreparedCandidate) {
    auto r = fixture();
    PreviewPlayback playback(makePlaybackScene(r, {{20, 20}, {120, 20}, {220, 20}}, {1000, 800}, 48, 96));
    playback.startGravity(true);
    dpp::DisplayMigration migration;
    migration.begin(*playback.game(), 20.0);
    ViewContract sample;
    sample.bounds = {0, 0, 1680, 1050};
    sample.listScreen = sample.bounds;
    EXPECT_TRUE(migrationListBoundsReady(migration, sample, 20.2));
    migration.prepare({900, 700});
    const auto token = migration.revision();
    sample.listScreen = {0, 0, 1920, 1080};
    EXPECT_FALSE(migrationListBoundsReady(migration, sample, 20.3));
    EXPECT_EQ(migration.candidate(), nullptr);
    EXPECT_FALSE(migration.commit(token, 20.4, true));
    EXPECT_FALSE(migrationListBoundsReady(migration, sample, 22.99));
    EXPECT_TRUE(migration.expired(23.0));
    EXPECT_FALSE(migration.commit(migration.revision(), 23.0, true));
}
