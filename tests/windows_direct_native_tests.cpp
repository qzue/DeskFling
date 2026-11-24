// Independent synthetic Win32 message fixture. No Shell, desktop hiding, OS mouse injection,
// actual target opens or protected-session creation. Foreground acquisition/rendering are stubbed.
#include <gtest/gtest.h>

#include "core/modes/gravity_mode.h"
#include "platforms/windows/mirror_preview.cpp"

using namespace dpp;
using namespace dpp::winmirror;
class DirectNative : public ::testing::Test {
   protected:
    winprobe::Report report;
    MarkerWindow window{report, {}};
    void SetUp() override {
        window.fixtureInput = true;
        window.protectedMode = true;
        window.backgroundAfterOpen = true;
        window.previewDpi = 96;
        window.contract.work = {0, 0, 800, 600};
        SceneSetup setup;
        setup.workAreaSize = {800, 600};
        setup.icons = {{IconId{1}, TextureId{1}, {150, 150}, {64, 64}},
                       {IconId{2}, TextureId{2}, {150, 150}, {64, 64}}};
        window.playback = std::make_unique<PreviewPlayback>(setup);
        WNDCLASSW wc{};
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = MarkerWindow::overlayClass;
        wc.lpfnWndProc = MarkerWindow::overlayProcedure;
        ASSERT_NE(RegisterClassW(&wc), 0);
        window.panel = CreateWindowW(L"STATIC", L"fixture", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr,
                                     wc.hInstance, nullptr);
        ASSERT_NE(window.panel, nullptr);
        SetWindowLongPtrW(window.panel, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&window));
        window.overlay =
            CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"synthetic input only",
                            WS_POPUP, 0, 0, 800, 600, nullptr, nullptr, wc.hInstance, &window);
        ASSERT_NE(window.overlay, nullptr);
        window.armDesktopReturn();
    }
    void send(UINT message, LPARAM point = MAKELPARAM(150, 150)) {
        // Post only to this fixture's own HWND; GetMessageTime is real queue metadata.
        Sleep(2);
        ASSERT_TRUE(PostMessageW(window.overlay, message, message == WM_LBUTTONDOWN ? MK_LBUTTON : 0, point));
        MSG event{};
        ASSERT_TRUE(PeekMessageW(&event, window.overlay, message, message, PM_REMOVE));
        DispatchMessageW(&event);
    }
};
TEST_F(DirectNative, FirstDownSelectsDrawOrderOnceAndReleaseDoesNotOpen) {
    EXPECT_EQ(SendMessageW(window.overlay, WM_MOUSEACTIVATE, 0, MAKELPARAM(HTCLIENT, WM_LBUTTONDOWN)),
              MA_NOACTIVATE);
    send(WM_LBUTTONDOWN);
    ASSERT_TRUE(window.playback->grabbing());
    const auto* gravity = dynamic_cast<const GravityMode*>(&window.playback->game()->mode());
    ASSERT_NE(gravity, nullptr);
    EXPECT_EQ(gravity->world().grabbedBody(), std::optional<BodyIndex>{1});
    SendMessageW(window.overlay, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(150, 150));
    EXPECT_EQ(window.pointerDowns, 1u);
    EXPECT_EQ(GetCapture(), window.overlay);
    send(WM_LBUTTONUP);
    EXPECT_FALSE(window.playback->grabbing());
    EXPECT_EQ(window.fixtureMenus, 0u);
    EXPECT_NE(GetCapture(), window.overlay);
}
TEST_F(DirectNative, FirstRightDownDispatchesCapturedMenuTargetOnce) {
    send(WM_RBUTTONDOWN);
    EXPECT_EQ(window.fixtureMenus, 1u);
    EXPECT_EQ(window.fixtureMenuTarget, TextureId{2});
    EXPECT_FALSE(window.playback->gravityStarted());
    SendMessageW(window.overlay, WM_RBUTTONDOWN, 0, MAKELPARAM(150, 150));
    EXPECT_EQ(window.fixtureMenus, 1u);
}
TEST_F(DirectNative, FocusReleaseReturnsButExplicitDisablePersists) {
    send(WM_LBUTTONDOWN);
    send(WM_KILLFOCUS);
    EXPECT_FALSE(window.playback->grabbing());
    EXPECT_TRUE(window.desktopReady);
    send(WM_LBUTTONDOWN);
    EXPECT_TRUE(window.playback->grabbing());
    MarkerWindow::panelProcedure(window.panel, WM_COMMAND, 24, 0);
    send(WM_LBUTTONDOWN);
    send(WM_RBUTTONDOWN);
    EXPECT_FALSE(window.playback->grabbing());
    EXPECT_FALSE(window.directInput.enabled());
    EXPECT_EQ(window.fixtureMenus, 0u);
    window.armDesktopReturn();
    EXPECT_FALSE(window.desktopReady);
}
TEST_F(DirectNative, PauseBlankStaleAndInvalidProtectionRejectDown) {
    send(WM_LBUTTONDOWN, MAKELPARAM(700, 500));
    EXPECT_FALSE(window.playback->gravityStarted());
    window.fixtureProtection = false;
    send(WM_LBUTTONDOWN);
    send(WM_RBUTTONDOWN);
    EXPECT_FALSE(window.playback->gravityStarted());
    EXPECT_EQ(window.fixtureMenus, 0u);
    window.fixtureProtection = true;
    window.committedInputTime = GetTickCount() + 10000;
    send(WM_LBUTTONDOWN);
    send(WM_RBUTTONDOWN);
    EXPECT_FALSE(window.playback->gravityStarted());
    window.committedInputTime = 0;
    send(WM_LBUTTONDOWN);
    window.leaveInteraction();
    window.playback->togglePause();
    send(WM_LBUTTONDOWN);
    EXPECT_TRUE(window.playback->paused());
    EXPECT_FALSE(window.playback->grabbing());
    window.migration.begin(*window.playback->game(), MarkerWindow::migrationTime());
    send(WM_LBUTTONDOWN);
    send(WM_RBUTTONDOWN);
    EXPECT_FALSE(window.playback->grabbing());
    EXPECT_EQ(window.fixtureMenus, 0u);
    EXPECT_TRUE(window.playback->paused());
    EXPECT_EQ(window.protectedSession, nullptr);
}

TEST_F(DirectNative, ManualFallbackEntersWithoutMouseAndDoesNotResumeUserPause) {
    MarkerWindow::panelProcedure(window.panel, WM_COMMAND, 24, 0);
    EXPECT_FALSE(window.directInput.enabled());
    MarkerWindow::panelProcedure(window.panel, WM_COMMAND, 23, 0);
    EXPECT_TRUE(window.directInput.enabled());
    EXPECT_TRUE(window.playback->interacting());
    EXPECT_TRUE(window.playback->gravityStarted());
    EXPECT_FALSE(window.playback->grabbing());
    EXPECT_EQ(window.pointerDowns, 0u);
    window.leaveInteraction();
    window.playback->togglePause();
    MarkerWindow::panelProcedure(window.panel, WM_COMMAND, 24, 0);
    MarkerWindow::panelProcedure(window.panel, WM_COMMAND, 23, 0);
    EXPECT_TRUE(window.playback->paused());
    EXPECT_FALSE(window.playback->interacting());
}
TEST_F(DirectNative, InputTransitionsDoNotCreateProtectionOrRefreshStartTime) {
    window.started = 123456;
    report.snapshot = "same-fixture-snapshot";
    send(WM_LBUTTONDOWN);
    send(WM_LBUTTONUP);
    send(WM_RBUTTONDOWN);
    send(WM_KILLFOCUS);
    MarkerWindow::panelProcedure(window.panel, WM_COMMAND, 24, 0);
    MarkerWindow::panelProcedure(window.panel, WM_COMMAND, 23, 0);
    EXPECT_EQ(window.started, 123456u);
    EXPECT_EQ(report.snapshot, "same-fixture-snapshot");
    EXPECT_EQ(window.protectedSession, nullptr);
    EXPECT_TRUE(window.openAttempts.empty());
    EXPECT_FALSE(window.stopping);
}
TEST_F(DirectNative, ExplicitDisableDuringMigrationRemainsDisabledAfterRearm) {
    send(WM_LBUTTONDOWN);
    send(WM_LBUTTONUP);
    window.leaveInteraction();
    window.migration.begin(*window.playback->game(), MarkerWindow::migrationTime());
    MarkerWindow::panelProcedure(window.panel, WM_COMMAND, 24, 0);
    EXPECT_FALSE(window.directInput.enabled());
    window.migration.invalidate();
    window.armDesktopReturn();
    EXPECT_FALSE(window.desktopReady);
    EXPECT_NE(GetWindowLongPtrW(window.overlay, GWL_EXSTYLE) & WS_EX_TRANSPARENT, 0);
}
TEST_F(DirectNative, BlackHoleEdgeCapturesAndDragsWithoutOpening) {
    window.playback->startGravity(true);
    window.playback->game()->setBlackHole(true);
    send(WM_LBUTTONDOWN, MAKELPARAM(442, 300));
    ASSERT_TRUE(window.playback->grabbing());
    EXPECT_EQ(GetCapture(), window.overlay);
    SendMessageW(window.overlay, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(500, 350));
    const auto& gravity = static_cast<const GravityMode&>(window.playback->game()->mode());
    EXPECT_EQ(gravity.blackHole().center, (Vec2{458, 350}));
    send(WM_LBUTTONUP, MAKELPARAM(500, 350));
    EXPECT_FALSE(window.playback->grabbing());
    EXPECT_NE(GetCapture(), window.overlay);
    EXPECT_EQ(window.fixtureMenus, 0u);
}
TEST_F(DirectNative, CinematicShadowDragsIconsWinAndAltOrbitsAngles) {
    window.playback->startGravity(true);
    window.playback->game()->setBlackHoleStyle(BlackHoleStyle::Cinematic);
    window.playback->game()->setBlackHole(true);
    // Center of shadow (400, 300) drags the cinematic black hole.
    send(WM_LBUTTONDOWN, MAKELPARAM(400, 300));
    ASSERT_TRUE(window.playback->grabbing());
    SendMessageW(window.overlay, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(460, 330));
    const auto& gravity = static_cast<const GravityMode&>(window.playback->game()->mode());
    EXPECT_EQ(gravity.blackHole().center, (Vec2{460, 330}));
    send(WM_LBUTTONUP, MAKELPARAM(460, 330));

    // Alt + drag on the outer accretion ring orbits 3D elevation & tilt without moving center.
    window.fixtureAltDown = true;
    const int ringOffset = static_cast<int>(std::lround(gravity.blackHole().radius * 2.1f));
    const LPARAM ringPt = MAKELPARAM(460 + ringOffset, 330);
    EXPECT_EQ(SendMessageW(window.overlay, WM_NCHITTEST, 0, ringPt), HTCLIENT);
    send(WM_LBUTTONDOWN, ringPt);
    ASSERT_TRUE(window.playback->grabbing());
    SendMessageW(window.overlay, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(460 + ringOffset + 40, 330 + 30));
    EXPECT_EQ(gravity.blackHole().center, (Vec2{460, 330}));
    EXPECT_NEAR(gravity.blackHole().elevation, BlackHoleState::kDefaultElevation + 30 * .012f, 1e-4f);
    EXPECT_NEAR(gravity.blackHole().tilt, 40 * .012f, 1e-4f);
    send(WM_LBUTTONUP, MAKELPARAM(460 + ringOffset + 40, 330 + 30));
    window.fixtureAltDown = false;
    EXPECT_FALSE(window.playback->grabbing());

    // Without Alt, the outer accretion ring is transparent to clicks.
    EXPECT_EQ(SendMessageW(window.overlay, WM_NCHITTEST, 0, ringPt), HTTRANSPARENT);
}

TEST(BlackHoleD3D11Offscreen, CompilesHlslAndRendersShadowAndAccretionGlow) {
    ComScope com;
    ASSERT_TRUE(SUCCEEDED(com.hr));
    D3D_FEATURE_LEVEL level{};
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    ComPtr<ID3D11Device> d3d;
    ComPtr<ID3D11DeviceContext> ctx;
    HRESULT hr =
        D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                          levels, 2, D3D11_SDK_VERSION, &d3d, &level, &ctx);
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                               levels, 2, D3D11_SDK_VERSION, &d3d, &level, &ctx);
    }
    ASSERT_TRUE(SUCCEEDED(hr));
    ComPtr<IDXGIDevice> dxgi;
    ASSERT_TRUE(SUCCEEDED(d3d.As(&dxgi)));
    ComPtr<ID2D1Factory1> d2dFactory;
    ASSERT_TRUE(SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                                            nullptr, reinterpret_cast<void**>(d2dFactory.GetAddressOf()))));
    ComPtr<ID2D1Device> d2dDevice;
    ASSERT_TRUE(SUCCEEDED(d2dFactory->CreateDevice(dxgi.Get(), &d2dDevice)));
    ComPtr<ID2D1DeviceContext> d2d;
    ASSERT_TRUE(SUCCEEDED(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2d)));

    ComPtr<IWICImagingFactory> wic;
    ASSERT_TRUE(SUCCEEDED(
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))));
    ComPtr<IWICBitmap> bmp;
    ASSERT_TRUE(
        SUCCEEDED(wic->CreateBitmap(32, 32, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp)));
    {
        ComPtr<IWICBitmapLock> lock;
        WICRect rc{0, 0, 32, 32};
        ASSERT_TRUE(SUCCEEDED(bmp->Lock(&rc, WICBitmapLockWrite, &lock)));
        UINT cb = 0;
        BYTE* pv = nullptr;
        ASSERT_TRUE(SUCCEEDED(lock->GetDataPointer(&cb, &pv)));
        for (UINT i = 0; i < cb; i += 4) {
            pv[i + 0] = 200;
            pv[i + 1] = 150;
            pv[i + 2] = 80;
            pv[i + 3] = 255;
        }
    }
    ComPtr<IWICFormatConverter> conv;
    ASSERT_TRUE(SUCCEEDED(wic->CreateFormatConverter(&conv)));
    ASSERT_TRUE(SUCCEEDED(conv->Initialize(bmp.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                           nullptr, 0, WICBitmapPaletteTypeCustom)));

    BlackHoleD3D11 hole;
    hole.cacheIcon(d3d.Get(), conv.Get());
    BlackHoleItem item{{200, 200}, 42.0f, 0.5f, 2.3f, 0.056f, 0.25f, BlackHoleItem::Layer::Back};
    ASSERT_TRUE(hole.begin(d3d.Get(), d2d.Get(), item, 1.0f));
    EXPECT_TRUE(hole.active());

    SpriteItem bgIcon{TextureId{1}, {275, 200}, {48, 48}, 0.0f, 1.0f, true, false, 0.35f, 0.0f};
    SpriteItem fgIcon{TextureId{1}, {215, 205}, {48, 48}, 0.2f, 1.0f, false, false, 1.0f, 0.42f};
    EXPECT_EQ(hole.claim(bgIcon), BlackHoleD3D11::Claim::Background);
    EXPECT_EQ(hole.claim(fgIcon), BlackHoleD3D11::Claim::Foreground);
    hole.renderToTarget(d3d.Get());

    // Render into a tiny D2D target bitmap via DrawBitmap to verify end-to-end DXGI->D2D interop.
    D3D11_TEXTURE2D_DESC td{};
    td.Width = 400;
    td.Height = 400;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> canvas;
    ASSERT_TRUE(SUCCEEDED(d3d->CreateTexture2D(&td, nullptr, &canvas)));
    ComPtr<IDXGISurface> surface;
    ASSERT_TRUE(SUCCEEDED(canvas.As(&surface)));
    const auto props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    ComPtr<ID2D1Bitmap1> targetBmp;
    ASSERT_TRUE(SUCCEEDED(d2d->CreateBitmapFromDxgiSurface(surface.Get(), &props, &targetBmp)));
    d2d->SetTarget(targetBmp.Get());
    d2d->SetUnitMode(D2D1_UNIT_MODE_DIPS);
    d2d->BeginDraw();
    d2d->Clear(D2D1::ColorF(0, 0, 0, 0));
    d2d->DrawBitmap(hole.bitmap(), hole.destRect(), 1.0f, D2D1_INTERPOLATION_MODE_LINEAR);
    ASSERT_TRUE(SUCCEEDED(d2d->EndDraw()));
    d2d->SetTarget(nullptr);

    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    ASSERT_TRUE(SUCCEEDED(d3d->CreateTexture2D(&td, nullptr, &staging)));
    ctx->CopyResource(staging.Get(), canvas.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    ASSERT_TRUE(SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)));
    const auto pixel = [&](UINT x, UINT y) {
        const auto* row = static_cast<const std::uint8_t*>(mapped.pData) + y * mapped.RowPitch;
        return row + x * 4;  // B, G, R, A
    };
    // Event-horizon shadow just above the equatorial plane (200, 192) is opaque dark.
    const auto* centerPx = pixel(200, 192);
    EXPECT_GT(centerPx[3], 235u);
    EXPECT_LT(centerPx[2], 45u);
    // Max accretion disk emission across the region is warm orange-gold (R > G > B).
    unsigned maxRed = 0, redAtMax = 0, greenAtMax = 0, blueAtMax = 0;
    for (UINT y = 120; y < 280; y += 2) {
        for (UINT x = 80; x < 320; x += 2) {
            const auto* p = pixel(x, y);
            if (p[2] > maxRed) {
                maxRed = p[2];
                redAtMax = p[2];
                greenAtMax = p[1];
                blueAtMax = p[0];
            }
        }
    }
    ctx->Unmap(staging.Get(), 0);
    EXPECT_GT(maxRed, 150u);
    EXPECT_GT(redAtMax, greenAtMax);
    EXPECT_GT(greenAtMax, blueAtMax);
}

TEST_F(DirectNative, BilliardsHoverAimPullBackCaptureAimToggleAndKeepSessionMenu) {
    HWND aimCheck =
        CreateWindowW(L"BUTTON", L"aim", WS_CHILD | BS_AUTOCHECKBOX, 0, 0, 10, 10, window.panel,
                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(46)), GetModuleHandleW(nullptr), nullptr);
    ASSERT_NE(aimCheck, nullptr);
    SendMessageW(aimCheck, BM_SETCHECK, BST_CHECKED, 0);

    window.pendingBilliardsConfig.rule = BilliardsRule::EightBall;
    window.pendingBilliardsConfig.matchMode = BilliardsMatchMode::Hotseat;
    window.setPlayModeSelection(PlayMode::Billiards);
    ASSERT_NE(window.playback->game(), nullptr);
    ASSERT_TRUE(window.playback->game()->billiardsEnabled());
    auto* bm = window.playback->game()->mutableBilliardsMode();
    ASSERT_NE(bm, nullptr);

    // 1. WM_NCHITTEST: table, spin widget, and aim-guide toggle pill hit HTCLIENT; blank desktop outside hits
    // HTTRANSPARENT.
    const Vec2 wc = bm->whiteSceneCenter();
    const LPARAM whitePt =
        MAKELPARAM(static_cast<int>(std::lround(wc.x)), static_cast<int>(std::lround(wc.y)));
    const Vec2 aimPill = bm->aimToggleCenterScene();
    const LPARAM aimPillPt =
        MAKELPARAM(static_cast<int>(std::lround(aimPill.x)), static_cast<int>(std::lround(aimPill.y)));
    EXPECT_EQ(SendMessageW(window.overlay, WM_NCHITTEST, 0, whitePt), HTCLIENT);
    EXPECT_EQ(SendMessageW(window.overlay, WM_NCHITTEST, 0, aimPillPt), HTCLIENT);
    EXPECT_EQ(SendMessageW(window.overlay, WM_NCHITTEST, 0, MAKELPARAM(8, 592)), HTTRANSPARENT);

    // 2. Hover WM_MOUSEMOVE without button updates cue aim without grabbing or capturing.
    SendMessageW(
        window.overlay, WM_MOUSEMOVE, 0,
        MAKELPARAM(static_cast<int>(std::lround(wc.x)), static_cast<int>(std::lround(wc.y - 60.0f))));
    EXPECT_NE(GetCapture(), window.overlay);
    EXPECT_FALSE(window.playback->grabbing());

    // 3. Clicking on-canvas Aim Guide pill toggles showAimGuide and syncs panel checkbox 46!
    ASSERT_TRUE(bm->config().showAimGuide);
    send(WM_LBUTTONDOWN, aimPillPt);
    send(WM_LBUTTONUP, aimPillPt);
    EXPECT_FALSE(bm->config().showAimGuide);
    EXPECT_EQ(SendMessageW(aimCheck, BM_GETCHECK, 0, 0), BST_UNCHECKED);
    send(WM_LBUTTONDOWN, aimPillPt);
    send(WM_LBUTTONUP, aimPillPt);
    EXPECT_TRUE(bm->config().showAimGuide);
    EXPECT_EQ(SendMessageW(aimCheck, BM_GETCHECK, 0, 0), BST_CHECKED);

    // 4. Aim right, press inside table -> SetCapture acquired; drag back outside table -> shoots on release!
    const int ax = static_cast<int>(std::lround(wc.x + 80.0f));
    const int ay = static_cast<int>(std::lround(wc.y));
    SendMessageW(window.overlay, WM_MOUSEMOVE, 0, MAKELPARAM(ax, ay));
    send(WM_LBUTTONDOWN, MAKELPARAM(ax, ay));
    ASSERT_EQ(bm->state(), BilliardsState::Charging);
    ASSERT_TRUE(window.playback->grabbing());
    EXPECT_EQ(GetCapture(), window.overlay);

    // Pull back 110pt (even beyond left table edge) and release.
    SendMessageW(window.overlay, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(ax - 110, ay));
    EXPECT_GT(bm->pullDistancePoints(), 100.0f);
    send(WM_LBUTTONUP, MAKELPARAM(ax - 110, ay));
    EXPECT_NE(GetCapture(), window.overlay);
    EXPECT_EQ(bm->stats().shots, 1);
    EXPECT_GT(window.billiardsAudio.playedCount(), 0u);

    // 5. Right-click on an icon ball dispatches keep-session menu; right-click on non-icon filler ball does
    // not.
    std::size_t iconBallIdx = 0, fillerBallIdx = 0;
    for (std::size_t i = 0; i < bm->objectBallCount(); ++i) {
        if (bm->objectBallTexture(i) != kNoTexture) iconBallIdx = i;
        if (bm->objectBallTexture(i) == kNoTexture) fillerBallIdx = i;
    }
    const Vec2 iconPos = bm->objectBallScene(iconBallIdx);
    send(WM_RBUTTONDOWN,
         MAKELPARAM(static_cast<int>(std::lround(iconPos.x)), static_cast<int>(std::lround(iconPos.y))));
    EXPECT_EQ(window.fixtureMenus, 1u);
    EXPECT_EQ(window.fixtureMenuTarget, bm->objectBallTexture(iconBallIdx));

    const Vec2 fillerPos = bm->objectBallScene(fillerBallIdx);
    send(WM_RBUTTONDOWN,
         MAKELPARAM(static_cast<int>(std::lround(fillerPos.x)), static_cast<int>(std::lround(fillerPos.y))));
    EXPECT_EQ(window.fixtureMenus, 1u);
}

TEST(BilliardsD2DOffscreen, RendersClippedIconBallsRailsDashedGuideAndChineseHud) {
    ComScope com;
    ASSERT_TRUE(SUCCEEDED(com.hr));
    D3D_FEATURE_LEVEL level{};
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    Graphics gfx;
    HRESULT hr =
        D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                          levels, 2, D3D11_SDK_VERSION, &gfx.d3d, &level, nullptr);
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                               levels, 2, D3D11_SDK_VERSION, &gfx.d3d, &level, nullptr);
    }
    ASSERT_TRUE(SUCCEEDED(hr));
    ComPtr<ID3D11DeviceContext> ctx;
    gfx.d3d->GetImmediateContext(&ctx);
    ComPtr<IDXGIDevice> dxgi;
    ASSERT_TRUE(SUCCEEDED(gfx.d3d.As(&dxgi)));
    ASSERT_TRUE(
        SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr,
                                    reinterpret_cast<void**>(gfx.d2dFactory.GetAddressOf()))));
    ComPtr<ID2D1Device> d2dDevice;
    ASSERT_TRUE(SUCCEEDED(gfx.d2dFactory->CreateDevice(dxgi.Get(), &d2dDevice)));
    ASSERT_TRUE(SUCCEEDED(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &gfx.draw)));

    D3D11_TEXTURE2D_DESC td{};
    td.Width = 960;
    td.Height = 600;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> canvas;
    ASSERT_TRUE(SUCCEEDED(gfx.d3d->CreateTexture2D(&td, nullptr, &canvas)));
    ComPtr<IDXGISurface> surface;
    ASSERT_TRUE(SUCCEEDED(canvas.As(&surface)));
    const auto props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    ASSERT_TRUE(SUCCEEDED(gfx.draw->CreateBitmapFromDxgiSurface(surface.Get(), &props, &gfx.targetBitmap)));
    gfx.draw->SetTarget(gfx.targetBitmap.Get());
    gfx.initSharedDrawingResources();

    winprobe::Report rep;
    rep.snapshot = "d2d-billiards";
    for (int i = 0; i < 6; ++i) {
        winprobe::Sample s;
        s.id = rep.snapshot + "/S" + std::to_string(i + 1);
        s.localName = L"App" + std::to_wstring(i + 1);
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = 48;
        bmi.bmiHeader.biHeight = -48;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        s.bitmap.reset(CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0));
        ASSERT_TRUE(s.bitmap && bits);
        auto* px = static_cast<std::uint8_t*>(bits);
        for (int p = 0; p < 48 * 48 * 4; p += 4) {
            px[p + 0] = 220;
            px[p + 1] = 140;
            px[p + 2] = 60;
            px[p + 3] = 255;
        }
        rep.samples.push_back(std::move(s));
    }
    gfx.cacheImages(rep);

    Playground pg(syntheticScene(6, {960, 600}));
    BilliardsConfig cfg = BilliardsConfig::desktop();
    cfg.rule = BilliardsRule::EightBall;
    cfg.matchMode = BilliardsMatchMode::Hotseat;
    cfg.showBallNumbers = true;
    pg.setBilliardsConfig(cfg);
    pg.setBilliards(true);
    ASSERT_TRUE(pg.billiardsEnabled());

    ASSERT_NO_THROW(gfx.drawCopiesToTarget(pg.drawList(), 96, false, false, {960, 600}));
    gfx.draw->SetTarget(nullptr);

    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    ASSERT_TRUE(SUCCEEDED(gfx.d3d->CreateTexture2D(&td, nullptr, &staging)));
    ctx->CopyResource(staging.Get(), canvas.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    ASSERT_TRUE(SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)));
    const auto pixel = [&](UINT x, UINT y) {
        const auto* row = static_cast<const std::uint8_t*>(mapped.pData) + y * mapped.RowPitch;
        return row + x * 4;  // B, G, R, A
    };

    // Extreme corner outside table & HUD remains completely transparent (alpha == 0).
    EXPECT_EQ(pixel(4, 595)[3], 0u);

    // Cue ball center is bright opaque ivory-white.
    const Vec2 wc = pg.billiardsMode()->whiteSceneCenter();
    const auto* whitePx = pixel(static_cast<UINT>(std::lround(wc.x)), static_cast<UINT>(std::lround(wc.y)));
    EXPECT_GT(whitePx[3], 230u);
    EXPECT_GT(whitePx[0], 200u);
    EXPECT_GT(whitePx[1], 200u);
    EXPECT_GT(whitePx[2], 200u);

    // Circular-clipped icon ball center is opaque.
    const Vec2 b0 = pg.billiardsMode()->objectBallScene(0);
    const auto* ballPx = pixel(static_cast<UINT>(std::lround(b0.x)), static_cast<UINT>(std::lround(b0.y)));
    EXPECT_GT(ballPx[3], 220u);
    ctx->Unmap(staging.Get(), 0);
}

TEST(WindowsBilliardsLanLoopback, HostAndGuestSyncRulesActionsSnapshotsAndDisconnect) {
    BilliardsLan host;
    BilliardsLan guest;

    BilliardsRule guestRule = BilliardsRule::Free;
    bool gotAction = false;
    BilliardsRemoteAction hostReceivedAct{};
    bool gotSnapshot = false;
    BilliardsLanSnapshot guestReceivedSnap{};
    std::vector<SoundEvent> guestReceivedSounds;

    guest.onRuleSync = [&](BilliardsRule r) { guestRule = r; };
    host.onAction = [&](const BilliardsRemoteAction& a) {
        gotAction = true;
        hostReceivedAct = a;
    };
    guest.onSnapshot = [&](const BilliardsLanSnapshot& s, const std::vector<SoundEvent>& snd) {
        gotSnapshot = true;
        guestReceivedSnap = s;
        guestReceivedSounds = snd;
    };

    ASSERT_TRUE(host.host(0, true));
    ASSERT_GT(host.boundPort(), 0u);
    ASSERT_TRUE(guest.join("127.0.0.1:" + std::to_string(host.boundPort())));

    for (int i = 0; i < 100 && (!host.connected() || !guest.connected()); ++i) {
        host.poll();
        guest.poll();
        Sleep(5);
    }
    ASSERT_EQ(host.state(), BilliardsLan::State::ConnectedHost);
    ASSERT_EQ(guest.state(), BilliardsLan::State::ConnectedGuest);

    // 1. Host -> Guest rule sync.
    ASSERT_TRUE(host.sendRuleSync(BilliardsRule::Snooker));
    for (int i = 0; i < 50 && guestRule != BilliardsRule::Snooker; ++i) {
        host.poll();
        guest.poll();
        Sleep(2);
    }
    EXPECT_EQ(guestRule, BilliardsRule::Snooker);

    // 2. Guest -> Host shot action.
    BilliardsRemoteAction act;
    act.kind = BilliardsRemoteAction::Kind::Shoot;
    act.aimDir = {0.6f, 0.8f};
    act.power = 0.75f;
    ASSERT_TRUE(guest.sendAction(act));
    for (int i = 0; i < 50 && !gotAction; ++i) {
        host.poll();
        guest.poll();
        Sleep(2);
    }
    ASSERT_TRUE(gotAction);
    EXPECT_EQ(hostReceivedAct.kind, BilliardsRemoteAction::Kind::Shoot);
    EXPECT_NEAR(hostReceivedAct.power, 0.75f, 1e-4f);

    // 3. Host -> Guest snapshot + sounds.
    BilliardsLanSnapshot snap;
    snap.rule = BilliardsRule::Snooker;
    snap.currentPlayer = 1;
    snap.scores = {14, 7};
    std::snprintf(snap.statusNote.data(), snap.statusNote.size(), "%s", "轮到 P2");
    const SoundEvent se{SoundKind::CueStrike, 0.85f, -0.2f, 1.0f};
    ASSERT_TRUE(host.sendSnapshot(snap, std::span<const SoundEvent>(&se, 1)));
    for (int i = 0; i < 50 && !gotSnapshot; ++i) {
        host.poll();
        guest.poll();
        Sleep(2);
    }
    ASSERT_TRUE(gotSnapshot);
    EXPECT_EQ(guestReceivedSnap.scores[0], 14);
    EXPECT_EQ(guestReceivedSnap.scores[1], 7);
    EXPECT_STREQ(guestReceivedSnap.statusNote.data(), "轮到 P2");
    ASSERT_EQ(guestReceivedSounds.size(), 1u);
    EXPECT_EQ(guestReceivedSounds[0].kind, SoundKind::CueStrike);

    // 4. Guest disconnect returns Host cleanly to HostingWaiting.
    guest.stop();
    for (int i = 0; i < 50 && host.state() == BilliardsLan::State::ConnectedHost; ++i) {
        host.poll();
        Sleep(2);
    }
    EXPECT_EQ(host.state(), BilliardsLan::State::HostingWaiting);
    host.stop();
    EXPECT_EQ(host.state(), BilliardsLan::State::Disconnected);
}

TEST_F(DirectNative, SlingshotPullLaunchAndAirHockeyHoverKeyboardAndHudBar) {
    // 1. Slingshot: pouch hits HTCLIENT, pull-back acquires capture, release launches shot + plays audio.
    window.setPlayModeSelection(PlayMode::Slingshot);
    ASSERT_NE(window.playback->game(), nullptr);
    ASSERT_TRUE(window.playback->game()->slingshotEnabled());
    auto* sm = window.playback->game()->mutableSlingshotMode();
    ASSERT_NE(sm, nullptr);
    const int initialAmmo = sm->remainingAmmo();
    const Vec2 pouch = sm->pouchRestScene();
    const LPARAM pouchPt =
        MAKELPARAM(static_cast<int>(std::lround(pouch.x)), static_cast<int>(std::lround(pouch.y)));
    EXPECT_EQ(SendMessageW(window.overlay, WM_NCHITTEST, 0, pouchPt), HTCLIENT);
    EXPECT_EQ(SendMessageW(window.overlay, WM_NCHITTEST, 0, MAKELPARAM(8, 8)), HTTRANSPARENT);

    send(WM_LBUTTONDOWN, pouchPt);
    ASSERT_TRUE(window.playback->grabbing());
    EXPECT_EQ(GetCapture(), window.overlay);
    const LPARAM pullPt = MAKELPARAM(static_cast<int>(std::lround(pouch.x - 68.0f)),
                                     static_cast<int>(std::lround(pouch.y + 18.0f)));
    SendMessageW(window.overlay, WM_MOUSEMOVE, MK_LBUTTON, pullPt);
    send(WM_LBUTTONUP, pullPt);
    EXPECT_NE(GetCapture(), window.overlay);
    EXPECT_EQ(sm->remainingAmmo(), initialAmmo - 1);
    EXPECT_GT(window.billiardsAudio.playedCount(), 0u);

    // 2. Air Hockey: hover moves left mallet, WASD moves right mallet in Hotseat2P.
    window.pendingAirHockeyConfig.matchMode = AirHockeyMatchMode::Hotseat2P;
    window.setPlayModeSelection(PlayMode::AirHockey);
    ASSERT_TRUE(window.playback->game()->airHockeyEnabled());
    auto* ah = window.playback->game()->mutableAirHockeyMode();
    ASSERT_NE(ah, nullptr);
    const Vec2 rMin = ah->rinkMinScene();
    const Vec2 rSz = ah->rinkSizeScene();
    const LPARAM hoverPt = MAKELPARAM(static_cast<int>(std::lround(rMin.x + rSz.x * 0.25f)),
                                      static_cast<int>(std::lround(rMin.y + rSz.y * 0.35f)));
    EXPECT_EQ(SendMessageW(window.overlay, WM_NCHITTEST, 0, hoverPt), HTCLIENT);
    SendMessageW(window.overlay, WM_MOUSEMOVE, 0, hoverPt);

    SendMessageW(window.overlay, WM_KEYDOWN, 'W', 0);
    EXPECT_NE(window.p2KeyMask, 0u);
    SendMessageW(window.overlay, WM_KEYUP, 'W', 0);
    EXPECT_EQ(window.p2KeyMask, 0u);

    // 3. Floating Capsule HUD Bar creates, paints, syncs 5 modes, hides, and destroys cleanly.
    WinUserPrefs prefs{};
    window.hudBar.createAndShow({0, 0, 1280, 720}, 96, prefs);
    ASSERT_NE(window.hudBar.window(), nullptr);
    EXPECT_TRUE(window.hudBar.visible());
    window.syncHudBar();
    SendMessageW(window.hudBar.window(), WM_PAINT, 0, 0);
    window.hudBar.hide();
    EXPECT_FALSE(window.hudBar.visible());
    window.hudBar.destroy();
    EXPECT_EQ(window.hudBar.window(), nullptr);

    // 4. Gravity & BlackHole procedural audio waveforms synthesize & dispatch cleanly.
    const std::uint64_t beforeCount = window.billiardsAudio.playedCount();
    const SoundEvent extraSounds[] = {
        {SoundKind::IconTap, 0.55f, -0.3f, 1.02f},          {SoundKind::IconThud, 0.72f, 0.2f, 0.94f},
        {SoundKind::IconGrab, 0.40f, 0.0f, 1.06f},          {SoundKind::BlackHoleHum, 0.28f, 0.1f, 0.96f},
        {SoundKind::BlackHoleSwallow, 0.62f, -0.1f, 1.04f},
    };
    window.billiardsAudio.play(extraSounds, false);
    EXPECT_EQ(window.billiardsAudio.playedCount(), beforeCount + 5u);
}

TEST(SlingshotAndAirHockeyD2DOffscreen, RendersRotatedBoxPlanksAndAirHockeyRink) {
    ComScope com;
    ASSERT_TRUE(SUCCEEDED(com.hr));
    D3D_FEATURE_LEVEL level{};
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    Graphics gfx;
    HRESULT hr =
        D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                          levels, 2, D3D11_SDK_VERSION, &gfx.d3d, &level, nullptr);
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                               levels, 2, D3D11_SDK_VERSION, &gfx.d3d, &level, nullptr);
    }
    ASSERT_TRUE(SUCCEEDED(hr));
    ComPtr<IDXGIDevice> dxgi;
    ASSERT_TRUE(SUCCEEDED(gfx.d3d.As(&dxgi)));
    ASSERT_TRUE(
        SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr,
                                    reinterpret_cast<void**>(gfx.d2dFactory.GetAddressOf()))));
    ComPtr<ID2D1Device> d2dDevice;
    ASSERT_TRUE(SUCCEEDED(gfx.d2dFactory->CreateDevice(dxgi.Get(), &d2dDevice)));
    ASSERT_TRUE(SUCCEEDED(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &gfx.draw)));

    D3D11_TEXTURE2D_DESC td{};
    td.Width = 1024;
    td.Height = 640;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> canvas;
    ASSERT_TRUE(SUCCEEDED(gfx.d3d->CreateTexture2D(&td, nullptr, &canvas)));
    ComPtr<IDXGISurface> surface;
    ASSERT_TRUE(SUCCEEDED(canvas.As(&surface)));
    const auto props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    ASSERT_TRUE(SUCCEEDED(gfx.draw->CreateBitmapFromDxgiSurface(surface.Get(), &props, &gfx.targetBitmap)));
    gfx.draw->SetTarget(gfx.targetBitmap.Get());
    gfx.initSharedDrawingResources();

    winprobe::Report rep;
    rep.snapshot = "d2d-slingshot-hockey";
    for (int i = 0; i < 3; ++i) {
        winprobe::Sample s;
        s.id = rep.snapshot + "/S" + std::to_string(i + 1);
        s.localName = L"Icon" + std::to_wstring(i + 1);
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = 48;
        bmi.bmiHeader.biHeight = -48;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        s.bitmap.reset(CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0));
        ASSERT_TRUE(s.bitmap && bits);
        auto* px = static_cast<std::uint8_t*>(bits);
        for (int p = 0; p < 48 * 48 * 4; p += 4) {
            px[p + 0] = 230;
            px[p + 1] = 160;
            px[p + 2] = 70;
            px[p + 3] = 255;
        }
        rep.samples.push_back(std::move(s));
    }
    gfx.cacheImages(rep);

    // 3 icons (< 4 ammo + defenders) tests both cached icon sprites AND synthetic emblem fallback!
    Playground pg(syntheticScene(3, {1024, 640}));
    pg.setPlayMode(PlayMode::Slingshot);
    ASSERT_TRUE(pg.slingshotEnabled());
    ASSERT_NO_THROW(gfx.drawCopiesToTarget(pg.drawList(), 96, false, false, {1024, 640}));

    pg.setPlayMode(PlayMode::AirHockey);
    ASSERT_TRUE(pg.airHockeyEnabled());
    ASSERT_NO_THROW(gfx.drawCopiesToTarget(pg.drawList(), 96, false, false, {1024, 640}));
    gfx.draw->SetTarget(nullptr);
}

TEST(WindowsAirHockeyLanLoopback, HostAndGuestSyncMalletInputAndRinkState) {
    BilliardsLan host;
    BilliardsLan guest;
    bool gotIn = false;
    AirHockeyRemoteInput recvIn{};
    bool gotSt = false;
    AirHockeyLanState recvSt{};
    std::vector<SoundEvent> recvSounds;

    host.onHockeyInput = [&](const AirHockeyRemoteInput& in) {
        gotIn = true;
        recvIn = in;
    };
    guest.onHockeyState = [&](const AirHockeyLanState& st, const std::vector<SoundEvent>& snd) {
        gotSt = true;
        recvSt = st;
        recvSounds = snd;
    };

    ASSERT_TRUE(host.host(0, true));
    ASSERT_TRUE(guest.join("127.0.0.1:" + std::to_string(host.boundPort())));
    for (int i = 0; i < 100 && (!host.connected() || !guest.connected()); ++i) {
        host.poll();
        guest.poll();
        Sleep(5);
    }
    ASSERT_TRUE(host.connected() && guest.connected());

    ASSERT_TRUE(guest.sendHockeyInput({{0.84f, 0.36f}, true}));
    for (int i = 0; i < 50 && !gotIn; ++i) {
        host.poll();
        guest.poll();
        Sleep(2);
    }
    ASSERT_TRUE(gotIn);
    EXPECT_NEAR(recvIn.normTarget.x, 0.84f, 1e-4f);
    EXPECT_TRUE(recvIn.requestRematch);

    AirHockeyLanState st{};
    st.puckNorm = {0.42f, 0.58f};
    st.scores = {4, 3};
    st.targetGoals = 7;
    const SoundEvent goalSnd{SoundKind::HockeyGoal, 0.9f, 0.1f, 1.0f};
    ASSERT_TRUE(host.sendHockeyState(st, std::span<const SoundEvent>(&goalSnd, 1)));
    for (int i = 0; i < 50 && !gotSt; ++i) {
        host.poll();
        guest.poll();
        Sleep(2);
    }
    ASSERT_TRUE(gotSt);
    EXPECT_EQ(recvSt.scores[0], 4);
    EXPECT_EQ(recvSt.scores[1], 3);
    ASSERT_EQ(recvSounds.size(), 1u);
    EXPECT_EQ(recvSounds[0].kind, SoundKind::HockeyGoal);

    guest.stop();
    host.stop();
}

TEST(WindowsBilliardsLanLoopback, UdpRoomDiscoveryQuickScanJoinAndAutoReconnect) {
    BilliardsLan host;
    BilliardsLan guest;
    guest.setReconnectDelayMs(40);
    host.setAdvertisedGame(lan_codec::LanGameKind::Billiards, static_cast<int>(BilliardsRule::Snooker));

    ASSERT_TRUE(host.host(/*port=*/0, /*loopbackOnly=*/true, /*discoveryPort=*/0));
    ASSERT_GT(host.boundPort(), 0u);
    ASSERT_GT(host.boundDiscoveryPort(), 0u);

    ASSERT_TRUE(guest.quickScanAndJoin(lan_codec::LanGameKind::Billiards, host.boundDiscoveryPort(),
                                       /*loopbackOnly=*/true));
    for (int i = 0; i < 120 && (!host.connected() || !guest.connected() || guest.sessionToken() == 0u); ++i) {
        host.poll();
        guest.poll();
        Sleep(5);
    }
    ASSERT_TRUE(host.connected() && guest.connected());
    EXPECT_FALSE(guest.discoveredRooms().empty());
    EXPECT_NE(guest.sessionToken(), 0u);
    EXPECT_EQ(host.sessionToken(), guest.sessionToken());

    // Simulate transient peer disconnect and verify automatic Guest reconnect:
    const std::uint16_t samePort = host.boundPort();
    host.stop();
    for (int i = 0; i < 60 && !guest.reconnecting(); ++i) {
        guest.poll();
        Sleep(5);
    }
    EXPECT_TRUE(guest.reconnecting());
    ASSERT_TRUE(host.host(samePort, /*loopbackOnly=*/true, /*discoveryPort=*/0));
    for (int i = 0; i < 150 && (!host.connected() || !guest.connected()); ++i) {
        host.poll();
        guest.poll();
        Sleep(10);
    }
    EXPECT_TRUE(host.connected());
    EXPECT_TRUE(guest.connected());
    EXPECT_FALSE(guest.reconnecting());
    guest.stop();
    host.stop();
}

TEST(WindowsLayoutWriteback, OfflineMockWriterAppliesRejectsFailureAndUndoes) {
    winprobe::Report rep;
    rep.snapshot = "win-writeback";
    rep.viewWindowScreenRect = RECT{0, 0, 1920, 1080};
    for (int i = 0; i < 3; ++i) {
        winprobe::Sample s;
        s.id = rep.snapshot + "/S" + std::to_string(i + 1);
        s.localName = L"Item" + std::to_wstring(i + 1);
        // Minimal distinct opaque child PIDLs; never sent to the real Shell or disk.
        auto* bytes = static_cast<unsigned char*>(CoTaskMemAlloc(5));
        ASSERT_NE(bytes, nullptr);
        bytes[0] = 3;
        bytes[1] = 0;
        bytes[2] = static_cast<unsigned char>(i + 1);
        bytes[3] = bytes[4] = 0;
        s.child.reset(reinterpret_cast<PITEMID_CHILD>(bytes));
        s.position = winprobe::ShellItemPosition{40, 50 + i * 100};
        rep.samples.push_back(std::move(s));
    }
    const std::vector<Vec2> centers = {{300.0f, 400.0f}, {450.0f, 400.0f}, {600.0f, 400.0f}};
    const PixelRect work{0, 0, 1920, 1030};
    WinLayoutWritebackPlan plan{};
    ASSERT_TRUE(buildWinLayoutWritebackPlan(rep, centers, work, /*viewIconPixels=*/60, /*dpi=*/120,
                                            /*manualOffsetYPixels=*/5, plan));
    ASSERT_EQ(plan.originalViewPoints.size(), 3u);
    ASSERT_EQ(plan.targetViewPoints.size(), 3u);

    std::vector<POINT> livePts = plan.originalViewPoints;
    std::wstring errText;
    // Writer failure leaves livePts untouched and returns false with Chinese error text:
    EXPECT_FALSE(applyWinLayoutWriteback(
        rep, plan.targetViewPoints,
        [&](const std::vector<PCUITEMID_CHILD>&, const std::vector<POINT>&) -> HRESULT {
            return E_ACCESSDENIED;
        },
        &errText));
    EXPECT_FALSE(errText.empty());
    EXPECT_EQ(livePts[0].x, 40);

    // Full write-back + Undo restoration:
    ASSERT_TRUE(applyWinLayoutWriteback(
        rep, plan.targetViewPoints,
        [&](const std::vector<PCUITEMID_CHILD>&, const std::vector<POINT>& pts) -> HRESULT {
            livePts = pts;
            return S_OK;
        },
        &errText));
    EXPECT_GT(livePts[0].x, 300);

    ASSERT_TRUE(applyWinLayoutWriteback(
        rep, plan.originalViewPoints,
        [&](const std::vector<PCUITEMID_CHILD>&, const std::vector<POINT>& pts) -> HRESULT {
            livePts = pts;
            return S_OK;
        },
        &errText));
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(livePts[static_cast<std::size_t>(i)].x, 40);
        EXPECT_EQ(livePts[static_cast<std::size_t>(i)].y, 50 + i * 100);
    }
}

TEST_F(DirectNative, MacStyleHudCapsuleAndSettingsCardRenderScaleAndHitTest) {
    ComScope com;
    ASSERT_TRUE(SUCCEEDED(com.hr));

    WinUserPrefs prefs{};
    const RECT work{0, 0, 1920, 1030};
    window.hudBar.createAndShow(work, /*dpi=*/120, prefs);
    ASSERT_NE(window.hudBar.window(), nullptr);
    EXPECT_TRUE(window.hudBar.visible());

    RECT hudWinRc{};
    ASSERT_TRUE(GetWindowRect(window.hudBar.window(), &hudWinRc));
    const RECT pillRc = window.hudBar.pillScreenRect();
    EXPECT_EQ(pillRc.right - pillRc.left, 745);  // 596 DIP * 1.25
    EXPECT_EQ(pillRc.bottom - pillRc.top, 55);   // 44 DIP * 1.25
    EXPECT_GT(pillRc.left, hudWinRc.left);
    EXPECT_LT(pillRc.right, hudWinRc.right);

    // Shadow apron passes clicks through; capsule center intercepts without stealing focus.
    EXPECT_EQ(SendMessageW(window.hudBar.window(), WM_NCHITTEST, 0,
                           MAKELPARAM(hudWinRc.left + 2, hudWinRc.top + 2)),
              HTTRANSPARENT);
    EXPECT_EQ(SendMessageW(window.hudBar.window(), WM_NCHITTEST, 0,
                           MAKELPARAM((pillRc.left + pillRc.right) / 2, (pillRc.top + pillRc.bottom) / 2)),
              HTCLIENT);
    EXPECT_EQ(SendMessageW(window.hudBar.window(), WM_MOUSEACTIVATE, 0, 0), MA_NOACTIVATE);

    // Clicking the BlackHole segment (2nd segment inside the unified segmented control) dispatches cleanly.
    std::optional<PlayMode> selectedMode;
    window.hudBar.onSelectMode = [&](PlayMode pm) { selectedMode = pm; };
    const int segClickClientX = static_cast<int>(std::lround((12.0f + 32.0f + 54.0f + 27.0f) * 1.25f));
    const int segClickClientY = static_cast<int>(std::lround((12.0f + 22.0f) * 1.25f));
    SendMessageW(window.hudBar.window(), WM_LBUTTONDOWN, MK_LBUTTON,
                 MAKELPARAM(segClickClientX, segClickClientY));
    SendMessageW(window.hudBar.window(), WM_LBUTTONUP, 0, MAKELPARAM(segClickClientX, segClickClientY));
    EXPECT_EQ(selectedMode, std::optional<PlayMode>{PlayMode::BlackHole});

    // Quick-Start Launcher card & all 5 Mode-Exclusive Settings cards render and anchor cleanly at 125% DPI.
    window.settingsCard.showLauncher(window.panel, nullptr, work, /*dpi=*/120, /*hasLayoutBackup=*/true);
    ASSERT_NE(window.settingsCard.window(), nullptr);
    EXPECT_TRUE(window.settingsCard.visible());

    for (PlayMode pm : {PlayMode::Gravity, PlayMode::BlackHole, PlayMode::Billiards, PlayMode::Slingshot,
                        PlayMode::AirHockey}) {
        window.settingsCard.showModeSettings(window.panel, nullptr, work, pillRc, /*hudVisible=*/true,
                                             /*dpi=*/120, pm, /*hasLayoutBackup=*/false,
                                             /*repositionToHud=*/true);
        EXPECT_TRUE(window.settingsCard.visible());
        RECT cardWinRc{};
        ASSERT_TRUE(GetWindowRect(window.settingsCard.window(), &cardWinRc));
        EXPECT_EQ(SendMessageW(window.settingsCard.window(), WM_NCHITTEST, 0,
                               MAKELPARAM(cardWinRc.left + 2, cardWinRc.top + 2)),
                  HTTRANSPARENT);
        EXPECT_EQ(SendMessageW(window.settingsCard.window(), WM_NCHITTEST, 0,
                               MAKELPARAM((cardWinRc.left + cardWinRc.right) / 2,
                                          (cardWinRc.top + cardWinRc.bottom) / 2)),
                  HTCLIENT);
    }

    window.settingsCard.destroy();
    window.hudBar.destroy();
}
