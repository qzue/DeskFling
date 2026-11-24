#include <d2d1.h>
#include <dwrite.h>
#include <windows.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <stdexcept>
#include <type_traits>

#include "core/scene/playground.h"
#include "platforms/windows/frame_timer.h"
using namespace dpp;
using Microsoft::WRL::ComPtr;
namespace {
struct Host {
    Playground game{syntheticScene()};
    ComPtr<ID2D1Factory> factory;
    ComPtr<ID2D1HwndRenderTarget> target;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<IDWriteFactory> textFactory;
    ComPtr<IDWriteTextFormat> format;
    SteadyClock::time_point previous = SteadyClock::now();
    float scale = 1;
    bool active = true;
    FrameTimer timer;
    void wake(HWND window) {
        if (active) timer.set(window, FrameTimer::activeInterval);
    }
    void check(HRESULT hr) {
        if (FAILED(hr)) throw std::runtime_error("Direct2D resource failure");
    }
    void create(HWND window) {
        if (target) return;
        RECT r;
        GetClientRect(window, &r);
        check(factory->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(
                window, D2D1::SizeU(static_cast<UINT32>(r.right), static_cast<UINT32>(r.bottom))),
            &target));
        target->SetDpi(96 * scale, 96 * scale);
        check(target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brush));
    }
    void draw(HWND window) {
        create(window);
        auto start = SteadyClock::now();
        target->BeginDraw();
        target->Clear(D2D1::ColorF(.055f, .085f, .12f));
        for (const auto& cmd : game.drawList().commands())
            std::visit(
                [&](const auto& item) {
                    using T = std::decay_t<decltype(item)>;
                    if constexpr (std::is_same_v<T, SpriteItem>) {
                        const auto id = static_cast<unsigned>(item.texture);
                        brush->SetColor(
                            D2D1::ColorF(.16f + .07f * static_cast<float>(id % 4), .46f, .77f, item.opacity));
                        target->SetTransform(D2D1::Matrix3x2F::Rotation(
                            item.rotation * 57.29578f, D2D1::Point2F(item.center.x, item.center.y)));
                        auto rect =
                            D2D1::RectF(item.center.x - item.size.x / 2, item.center.y - item.size.y / 2,
                                        item.center.x + item.size.x / 2, item.center.y + item.size.y / 2);
                        target->FillRoundedRectangle(
                            D2D1::RoundedRect(rect, item.clipCircle ? item.size.x / 2 : 8,
                                              item.clipCircle ? item.size.y / 2 : 8),
                            brush.Get());
                        target->SetTransform(D2D1::Matrix3x2F::Identity());
                    } else if constexpr (std::is_same_v<T, CircleItem>) {
                        brush->SetColor(D2D1::ColorF(item.fill.r / 255.f, item.fill.g / 255.f,
                                                     item.fill.b / 255.f, item.fill.a / 255.f));
                        target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(item.center.x, item.center.y),
                                                          item.radius, item.radius),
                                            brush.Get());
                    } else if constexpr (std::is_same_v<T, LineItem>) {
                        brush->SetColor(D2D1::ColorF(item.color.r / 255.f, item.color.g / 255.f,
                                                     item.color.b / 255.f, item.color.a / 255.f));
                        target->DrawLine(D2D1::Point2F(item.a.x, item.a.y), D2D1::Point2F(item.b.x, item.b.y),
                                         brush.Get(), item.width);
                    } else if constexpr (std::is_same_v<T, TextItem>) {
                        wchar_t text[96]{};
                        MultiByteToWideChar(CP_UTF8, 0, item.text.data(), -1, text, 96);
                        brush->SetColor(D2D1::ColorF(D2D1::ColorF::White));
                        target->DrawText(
                            text, static_cast<UINT32>(wcslen(text)), format.Get(),
                            D2D1::RectF(item.position.x, item.position.y, 1100, item.position.y + 30),
                            brush.Get());
                    }
                },
                cmd);
        const auto hr = target->EndDraw();
        game.perf.submit.add(elapsedMs(start));
        if (hr == D2DERR_RECREATE_TARGET) {
            brush.Reset();
            target.Reset();
        } else
            check(hr);
    }
    void input(HWND w, InputType type, LPARAM lp = 0) {
        auto time = std::chrono::duration<double>(SteadyClock::now().time_since_epoch()).count();
        if (game.idle()) previous = SteadyClock::now();
        game.input(
            {type,
             {static_cast<float>(GET_X_LPARAM(lp)) / scale, static_cast<float>(GET_Y_LPARAM(lp)) / scale},
             time});
        wake(w);
    }
};
LRESULT CALLBACK procedure(HWND w, UINT msg, WPARAM wp, LPARAM lp) noexcept {
    auto* h = reinterpret_cast<Host*>(GetWindowLongPtrW(w, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        h = static_cast<Host*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(h));
    }
    if (!h) return DefWindowProcW(w, msg, wp, lp);
    try {
        switch (msg) {
            case WM_TIMER: {
                // KillTimer does not remove messages already queued before focus loss.
                if (!h->active || !h->timer.accepts(wp)) return 0;
                auto now = SteadyClock::now();
                double dt = std::chrono::duration<double>(now - h->previous).count();
                h->previous = now;
                if (h->game.tick(dt)) InvalidateRect(w, nullptr, FALSE);
                h->timer.set(w, h->game.idle() ? FrameTimer::idleInterval : FrameTimer::activeInterval);
                return 0;
            }
            case WM_PAINT: {
                PAINTSTRUCT ps;
                BeginPaint(w, &ps);
                try {
                    h->draw(w);
                } catch (...) {
                    EndPaint(w, &ps);
                    throw;
                }
                EndPaint(w, &ps);
                return 0;
            }
            case WM_ERASEBKGND:
                return 1;
            case WM_LBUTTONDOWN:
                SetCapture(w);
                h->input(w, InputType::PointerDown, lp);
                return 0;
            case WM_MOUSEMOVE:
                h->input(w, InputType::PointerMove, lp);
                return 0;
            case WM_LBUTTONUP:
                h->input(w, InputType::PointerUp, lp);
                ReleaseCapture();
                return 0;
            case WM_CAPTURECHANGED:
                h->input(w, InputType::PointerCancel);
                return 0;
            case WM_ACTIVATE:
                h->active = LOWORD(wp) != WA_INACTIVE;
                h->input(w, InputType::PointerCancel);
                h->previous = SteadyClock::now();
                if (!h->active) {
                    ReleaseCapture();
                    h->timer.stop(w);
                }
                return 0;
            case WM_KEYDOWN:
                if (lp & (1LL << 30)) return 0;
                if (wp == VK_ESCAPE)
                    DestroyWindow(w);
                else if (wp == VK_SPACE)
                    h->input(w, InputType::TogglePause);
                else if (wp == 'R')
                    h->input(w, InputType::Reset);
                else if (wp == 'B') {
                    h->game.switchMode(h->game.mode().kind() == ModeKind::Gravity ? ModeKind::Billiards
                                                                                  : ModeKind::Gravity);
                    h->wake(w);
                }
                return 0;
            case WM_COMMAND:
                if (LOWORD(wp) == 1) h->input(w, InputType::TogglePause);
                if (LOWORD(wp) == 2) h->input(w, InputType::Reset);
                if (LOWORD(wp) == 3) {
                    h->game.switchMode(h->game.mode().kind() == ModeKind::Gravity ? ModeKind::Billiards
                                                                                  : ModeKind::Gravity);
                    h->wake(w);
                }
                if (LOWORD(wp) == 4) DestroyWindow(w);
                return 0;
            case WM_DISPLAYCHANGE:
            case WM_DPICHANGED:
                DestroyWindow(w);
                return 0;  // conservative sandbox stop
            case WM_DESTROY:
                h->timer.stop(w);
                PostQuitMessage(0);
                return 0;
        }
    } catch (...) {
        MessageBoxW(w, L"Sandbox stopped after an error. No desktop state was changed.",
                    L"Desktop Physics Sandbox", MB_OK | MB_ICONERROR);
        DestroyWindow(w);
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}
}  // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    try {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        Host host;
        host.scale = static_cast<float>(GetDpiForSystem()) / 96.f;
        host.check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, host.factory.GetAddressOf()));
        host.check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(host.textFactory.GetAddressOf())));
        host.check(host.textFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                                      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                      14, L"en-us", &host.format));
        WNDCLASSW wc{};
        wc.lpfnWndProc = procedure;
        wc.hInstance = instance;
        wc.lpszClassName = L"DPPSandbox";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        if (!RegisterClassW(&wc)) return 1;
        HMENU menu = CreateMenu();
        AppendMenuW(menu, MF_STRING, 1, L"Pause / Resume");
        AppendMenuW(menu, MF_STRING, 2, L"Reset");
        AppendMenuW(menu, MF_STRING, 3, L"Switch mode");
        AppendMenuW(menu, MF_STRING, 4, L"Exit");
        constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        RECT rect{0, 0, static_cast<LONG>(1100 * host.scale), static_cast<LONG>(700 * host.scale)};
        AdjustWindowRectExForDpi(&rect, style, TRUE, 0, GetDpiForSystem());
        HWND window = CreateWindowExW(
            0, wc.lpszClassName,
            L"DEVELOPMENT SANDBOX - synthetic shapes - Space pause / R reset / B mode / Esc exit", style,
            CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, nullptr, menu,
            instance, &host);
        if (!window) {
            DestroyMenu(menu);
            return 1;
        }
        host.wake(window);
        ShowWindow(window, show);
        MSG msg{};
        BOOL result;
        while ((result = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return result < 0 ? 1 : 0;
    } catch (...) {
        MessageBoxW(nullptr, L"Sandbox initialization failed", L"DPP", MB_OK | MB_ICONERROR);
        return 1;
    }
}
