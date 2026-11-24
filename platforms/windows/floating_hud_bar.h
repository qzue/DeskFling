#pragma once
// macOS Sonoma/Sequoia VibrantDark-inspired floating capsule HUD bar,
// Direct2D + DirectWrite dark-glass popup menu, system tray controller, and
// LocalAppData preferences helper for Windows DPP.
// Uses WS_EX_LAYERED + UpdateLayeredWindow backed by ID2D1DCRenderTarget
// (32-bpp premultiplied BGRA) + DirectWrite grayscale vector typography,
// soft multi-pass Gaussian drop shadow (HTTRANSPARENT in shadow apron),
// unified 5-segment NSSegmentedControl track, and clean recessed pills.

// clang-format off
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
// clang-format on

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include "core/scene/playground.h"
#include "core/scene/user_prefs.h"

namespace dpp::winmirror {

struct WinUserPrefs {
    bool hasHudOrigin = false;
    int hudX = 0;
    int hudY = 0;
    bool soundMuted = false;
    bool showFilenames = true;
    int lastMode = 0;
    dpp::UserPreferences appPrefs{};

    static std::wstring prefsDirPath() {
        PWSTR raw = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)) || !raw) return {};
        std::wstring dir = raw;
        CoTaskMemFree(raw);
        dir += L"\\DesktopPhysicsPlayground-Prefs-v1";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir;
    }

    static std::wstring prefsFilePath() {
        const std::wstring dir = prefsDirPath();
        return dir.empty() ? std::wstring{} : (dir + L"\\prefs.ini");
    }

    static std::wstring appPrefsFilePath() {
        const std::wstring dir = prefsDirPath();
        return dir.empty() ? std::wstring{} : (dir + L"\\prefs_v2.cfg");
    }

    void load() noexcept {
        const std::wstring path = prefsFilePath();
        if (path.empty()) return;
        hasHudOrigin = GetPrivateProfileIntW(L"HUD", L"HasOrigin", 0, path.c_str()) != 0;
        hudX = static_cast<int>(GetPrivateProfileIntW(L"HUD", L"X", 0, path.c_str()));
        hudY = static_cast<int>(GetPrivateProfileIntW(L"HUD", L"Y", 0, path.c_str()));
        soundMuted = GetPrivateProfileIntW(L"Audio", L"Muted", 0, path.c_str()) != 0;
        showFilenames = GetPrivateProfileIntW(L"View", L"ShowFilenames", 1, path.c_str()) != 0;
        lastMode = std::clamp<int>(
            static_cast<int>(GetPrivateProfileIntW(L"Play", L"LastMode", 0, path.c_str())), 0, 4);

        appPrefs.soundMuted = soundMuted;
        appPrefs.showFilenames = showFilenames;
        appPrefs.lastPlayMode = static_cast<PlayMode>(lastMode);

        const std::wstring cfgPath = appPrefsFilePath();
        if (!cfgPath.empty()) {
            std::ifstream in(cfgPath, std::ios::binary);
            if (in) {
                std::ostringstream ss;
                ss << in.rdbuf();
                const std::string content = ss.str();
                if (!content.empty()) {
                    appPrefs = dpp::UserPreferences::parseIni(content, /*forColdBoot=*/true);
                    soundMuted = appPrefs.soundMuted;
                    showFilenames = appPrefs.showFilenames;
                    lastMode = static_cast<int>(appPrefs.lastPlayMode);
                }
            }
        }
    }

    void save() const noexcept {
        const std::wstring path = prefsFilePath();
        if (path.empty()) return;
        WritePrivateProfileStringW(L"HUD", L"HasOrigin", hasHudOrigin ? L"1" : L"0", path.c_str());
        wchar_t buf[32]{};
        std::swprintf(buf, 32, L"%d", hudX);
        WritePrivateProfileStringW(L"HUD", L"X", buf, path.c_str());
        std::swprintf(buf, 32, L"%d", hudY);
        WritePrivateProfileStringW(L"HUD", L"Y", buf, path.c_str());
        WritePrivateProfileStringW(L"Audio", L"Muted", soundMuted ? L"1" : L"0", path.c_str());
        WritePrivateProfileStringW(L"View", L"ShowFilenames", showFilenames ? L"1" : L"0", path.c_str());
        std::swprintf(buf, 32, L"%d", lastMode);
        WritePrivateProfileStringW(L"Play", L"LastMode", buf, path.c_str());

        const std::wstring cfgPath = appPrefsFilePath();
        if (!cfgPath.empty()) {
            dpp::UserPreferences copy = appPrefs;
            copy.soundMuted = soundMuted;
            copy.showFilenames = showFilenames;
            copy.lastPlayMode = static_cast<PlayMode>(std::clamp<int>(lastMode, 0, 4));
            const std::string text = copy.serializeIni();
            if (!text.empty()) {
                std::ofstream out(cfgPath, std::ios::binary | std::ios::trunc);
                if (out) out.write(text.data(), static_cast<std::streamsize>(text.size()));
            }
        }
    }
};

// Shared Direct2D + DirectWrite resources & macOS VibrantDark glass primitives
// for per-pixel alpha layered windows (capsule bar, settings card, popup menu).
struct MacGlassContext {
    Microsoft::WRL::ComPtr<ID2D1Factory> d2dFactory;
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> dcTarget;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> roundStroke;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite;

    bool ensure() noexcept {
        if (!d2dFactory) {
            if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr,
                                         reinterpret_cast<void**>(d2dFactory.GetAddressOf())))) {
                return false;
            }
        }
        if (!roundStroke) {
            const auto sp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                        D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
            (void)d2dFactory->CreateStrokeStyle(sp, nullptr, 0, &roundStroke);
        }
        if (!dcTarget) {
            const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f,
                D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT);
            if (FAILED(d2dFactory->CreateDCRenderTarget(&props, &dcTarget))) return false;
            dcTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            dcTarget->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        }
        if (!brush) {
            if (FAILED(dcTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &brush))) {
                return false;
            }
        }
        if (!dwrite) {
            if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                           reinterpret_cast<IUnknown**>(dwrite.GetAddressOf())))) {
                return false;
            }
        }
        return true;
    }

    Microsoft::WRL::ComPtr<IDWriteTextFormat> makeFormat(float dipSize, DWRITE_FONT_WEIGHT weight,
                                                         bool centerH, bool wrap = false) noexcept {
        Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt;
        if (!dwrite) return fmt;
        if (FAILED(dwrite->CreateTextFormat(L"Microsoft YaHei UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                            DWRITE_FONT_STRETCH_NORMAL, dipSize, L"zh-CN", &fmt))) {
            (void)dwrite->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                           DWRITE_FONT_STRETCH_NORMAL, dipSize, L"en-US", &fmt);
        }
        if (fmt) {
            fmt->SetTextAlignment(centerH ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_LEADING);
            fmt->SetParagraphAlignment(wrap ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR
                                            : DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            fmt->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
            if (!wrap) {
                DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                Microsoft::WRL::ComPtr<IDWriteInlineObject> ellipsis;
                if (SUCCEEDED(dwrite->CreateEllipsisTrimmingSign(fmt.Get(), &ellipsis))) {
                    (void)fmt->SetTrimming(&trim, ellipsis.Get());
                }
            }
        }
        return fmt;
    }

    void fillVerticalGradientRounded(const D2D1_ROUNDED_RECT& rr, D2D1_COLOR_F topCol,
                                     D2D1_COLOR_F botCol) noexcept {
        if (!dcTarget) return;
        const D2D1_GRADIENT_STOP stops[2] = {{0.0f, topCol}, {1.0f, botCol}};
        Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> coll;
        if (FAILED(dcTarget->CreateGradientStopCollection(stops, 2, &coll))) return;
        Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> grad;
        const auto lp =
            D2D1::LinearGradientBrushProperties({rr.rect.left, rr.rect.top}, {rr.rect.left, rr.rect.bottom});
        if (SUCCEEDED(dcTarget->CreateLinearGradientBrush(lp, coll.Get(), &grad))) {
            dcTarget->FillRoundedRectangle(rr, grad.Get());
        }
    }

    void strokeVerticalGradientRounded(const D2D1_ROUNDED_RECT& rr, D2D1_COLOR_F topCol, D2D1_COLOR_F botCol,
                                       float strokeWidth = 1.0f) noexcept {
        if (!dcTarget) return;
        const D2D1_GRADIENT_STOP stops[2] = {{0.0f, topCol}, {1.0f, botCol}};
        Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> coll;
        if (FAILED(dcTarget->CreateGradientStopCollection(stops, 2, &coll))) return;
        Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> grad;
        const auto lp =
            D2D1::LinearGradientBrushProperties({rr.rect.left, rr.rect.top}, {rr.rect.left, rr.rect.bottom});
        if (SUCCEEDED(dcTarget->CreateLinearGradientBrush(lp, coll.Get(), &grad))) {
            dcTarget->DrawRoundedRectangle(rr, grad.Get(), strokeWidth);
        }
    }

    // Draws macOS Sonoma/Sequoia VibrantDark HUD frosted dark-charcoal glass surface
    // with soft multi-layer drop shadow, subtle inner sheen, and crisp double-bevel rim.
    void drawMacHudGlass(D2D1_RECT_F body, float radius, float surfaceAlpha = 0.91f) noexcept {
        if (!dcTarget || !brush) return;

        // 1. Multi-layer soft downward + ambient Gaussian shadow
        constexpr int kShadowPasses = 8;
        for (int i = kShadowPasses; i >= 1; --i) {
            const float t = static_cast<float>(i) / static_cast<float>(kShadowPasses);
            const float spread = t * 10.5f;
            const float yShift = 1.0f + t * 3.2f;
            const float a = 0.052f * (1.0f - t * 0.72f);
            brush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, a));
            const D2D1_RECT_F sr{body.left - spread, body.top - spread * 0.65f + yShift, body.right + spread,
                                 body.bottom + spread + yShift};
            dcTarget->FillRoundedRectangle(D2D1::RoundedRect(sr, radius + spread, radius + spread),
                                           brush.Get());
        }

        // 2. Crisp dark outer hairline for separation on bright wallpapers
        const D2D1_RECT_F outerRc{body.left - 0.5f, body.top - 0.5f, body.right + 0.5f, body.bottom + 0.5f};
        brush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.44f));
        dcTarget->DrawRoundedRectangle(D2D1::RoundedRect(outerRc, radius + 0.5f, radius + 0.5f), brush.Get(),
                                       1.0f);

        // 3. Neutral macOS VibrantDark HUD graphite body
        const D2D1_ROUNDED_RECT mainRr = D2D1::RoundedRect(body, radius, radius);
        fillVerticalGradientRounded(
            mainRr, D2D1::ColorF(0.145f, 0.145f, 0.165f, surfaceAlpha),
            D2D1::ColorF(0.094f, 0.094f, 0.110f, std::min(0.96f, surfaceAlpha + 0.03f)));

        // 4. Subtle top-half frosted specular sheen
        const D2D1_RECT_F sheenRc{body.left + 1.0f, body.top + 1.0f, body.right - 1.0f,
                                  body.top + (body.bottom - body.top) * 0.52f};
        fillVerticalGradientRounded(
            D2D1::RoundedRect(sheenRc, std::max(2.0f, radius - 1.0f), std::max(2.0f, radius - 1.0f)),
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.055f), D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f));

        // 5. Crisp 1 DIP inner specular glass rim
        const D2D1_RECT_F rimRc{body.left + 0.5f, body.top + 0.5f, body.right - 0.5f, body.bottom - 0.5f};
        strokeVerticalGradientRounded(
            D2D1::RoundedRect(rimRc, std::max(1.0f, radius - 0.5f), std::max(1.0f, radius - 0.5f)),
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.23f), D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f), 1.0f);
    }

    template <typename DrawFn>
    bool commitLayered(HWND hwnd, int screenX, int screenY, float dipW, float dipH, UINT dpi,
                       DrawFn&& drawFn) noexcept {
        if (!IsWindow(hwnd) || dipW <= 1.0f || dipH <= 1.0f) return false;
        const UINT safeDpi = dpi ? dpi : 96u;
        const float scale = static_cast<float>(safeDpi) / 96.0f;
        const int pxW = std::max(2, static_cast<int>(std::lround(dipW * scale)));
        const int pxH = std::max(2, static_cast<int>(std::lround(dipH * scale)));
        if (!ensure()) return false;

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = pxW;
        bmi.bmiHeader.biHeight = -pxH;  // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        HDC screenDC = GetDC(nullptr);
        HDC memDC = CreateCompatibleDC(screenDC);
        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(screenDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp || !bits || !memDC) {
            if (bmp) DeleteObject(bmp);
            if (memDC) DeleteDC(memDC);
            if (screenDC) ReleaseDC(nullptr, screenDC);
            return false;
        }

        HGDIOBJ oldBmp = SelectObject(memDC, bmp);
        std::memset(bits, 0, static_cast<std::size_t>(pxW) * static_cast<std::size_t>(pxH) * 4u);

        const RECT subRc{0, 0, pxW, pxH};
        bool ok = false;
        if (SUCCEEDED(dcTarget->BindDC(memDC, &subRc))) {
            dcTarget->SetDpi(static_cast<float>(safeDpi), static_cast<float>(safeDpi));
            dcTarget->BeginDraw();
            dcTarget->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
            drawFn(*this);
            if (dcTarget->EndDraw() == D2DERR_RECREATE_TARGET) {
                dcTarget.Reset();
                brush.Reset();
            } else {
                POINT ptWin{screenX, screenY};
                SIZE szWin{pxW, pxH};
                POINT ptSrc{0, 0};
                BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
                ok = (UpdateLayeredWindow(hwnd, screenDC, &ptWin, &szWin, memDC, &ptSrc, 0, &bf, ULW_ALPHA) !=
                      FALSE);
            }
        }

        SelectObject(memDC, oldBmp);
        DeleteObject(bmp);
        DeleteDC(memDC);
        if (screenDC) ReleaseDC(nullptr, screenDC);
        return ok;
    }
};

inline bool pointInsideRoundedDip(float px, float py, D2D1_RECT_F r, float rad) noexcept {
    if (px < r.left || px > r.right || py < r.top || py > r.bottom) return false;
    const float cx = (r.left + r.right) * 0.5f;
    const float cy = (r.top + r.bottom) * 0.5f;
    const float hw = (r.right - r.left) * 0.5f;
    const float hh = (r.bottom - r.top) * 0.5f;
    const float cr = std::min(rad, std::min(hw, hh));
    const float qx = std::max(std::abs(px - cx) - (hw - cr), 0.0f);
    const float qy = std::max(std::abs(py - cy) - (hh - cr), 0.0f);
    return (qx * qx + qy * qy) <= (cr + 0.5f) * (cr + 0.5f);
}

// macOS-style dark frosted-glass popup menu (replaces classic white Win32 CreatePopupMenu
// for both PopUpButton dropdowns and System Tray right-click menus).
struct DarkMenuEntry {
    int id = -1;
    std::wstring label;
    bool checked = false;
    bool enabled = true;
    bool separator = false;
    bool danger = false;
};

class WinDarkPopupMenu {
   public:
    static constexpr const wchar_t* kWindowClass = L"DPPWinMacDarkPopupMenuV1";
    static constexpr float kShadowMarginDip = 12.0f;
    static constexpr float kRowHeightDip = 27.0f;
    static constexpr float kSepHeightDip = 9.0f;
    static constexpr float kCornerRadiusDip = 11.0f;

    WinDarkPopupMenu() = default;
    ~WinDarkPopupMenu() { destroy(); }
    WinDarkPopupMenu(const WinDarkPopupMenu&) = delete;
    WinDarkPopupMenu& operator=(const WinDarkPopupMenu&) = delete;

    HWND window() const noexcept { return hwnd_; }
    bool active() const noexcept { return tracking_; }
    void cancel() noexcept { tracking_ = false; }

    void destroy() noexcept {
        tracking_ = false;
        if (IsWindow(hwnd_)) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
    }

    // Displays the dark-glass popup menu at screen anchor (anchorScreenX, anchorScreenY),
    // runs a lightweight local message loop that keeps WM_TIMER / Winsock alive, and returns
    // the chosen entry's `id` (or -1 if dismissed).
    int showModal(HWND owner, int anchorScreenX, int anchorScreenY, UINT dpi,
                  const std::vector<DarkMenuEntry>& items, float minWidthDip = 168.0f) noexcept {
        if (items.empty() || tracking_) return -1;
        ensureClassRegistered();
        dpi_ = dpi ? dpi : 96u;
        items_ = items;
        hoverIdx_ = -1;
        chosenId_ = -1;

        if (!glass_.ensure()) return -1;
        font_ = glass_.makeFormat(12.5f, DWRITE_FONT_WEIGHT_MEDIUM, false, false);

        // Measure natural text width via DirectWrite
        float maxTextW = minWidthDip - 52.0f;
        bool anyCheck = false;
        for (const auto& it : items_) {
            if (it.separator) continue;
            if (it.checked) anyCheck = true;
            if (glass_.dwrite && font_ && !it.label.empty()) {
                Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
                if (SUCCEEDED(glass_.dwrite->CreateTextLayout(it.label.c_str(),
                                                              static_cast<UINT32>(it.label.size()),
                                                              font_.Get(), 600.0f, kRowHeightDip, &layout))) {
                    DWRITE_TEXT_METRICS tm{};
                    if (SUCCEEDED(layout->GetMetrics(&tm))) {
                        maxTextW = std::max(maxTextW, tm.widthIncludingTrailingWhitespace);
                    }
                }
            }
        }
        hasChecks_ = anyCheck;
        const float leftGutter = hasChecks_ ? 26.0f : 14.0f;
        bodyW_ = std::clamp(leftGutter + maxTextW + 22.0f, minWidthDip, 380.0f);
        float totalRowsH = 10.0f;
        for (const auto& it : items_) {
            totalRowsH += it.separator ? kSepHeightDip : kRowHeightDip;
        }
        bodyH_ = totalRowsH;
        totalW_ = bodyW_ + kShadowMarginDip * 2.0f;
        totalH_ = bodyH_ + kShadowMarginDip * 2.0f;

        const float scale = static_cast<float>(dpi_) / 96.0f;
        const int pxW = static_cast<int>(std::lround(totalW_ * scale));
        const int pxH = static_cast<int>(std::lround(totalH_ * scale));
        const int marginPx = static_cast<int>(std::lround(kShadowMarginDip * scale));

        // Clamp inside monitor work area
        POINT ptAnchor{anchorScreenX, anchorScreenY};
        HMONITOR mon = MonitorFromPoint(ptAnchor, MONITOR_DEFAULTTONEAREST);
        RECT work{0, 0, 1920, 1080};
        if (mon) {
            MONITORINFO mi{sizeof(mi), {}, {}, 0};
            if (GetMonitorInfoW(mon, &mi)) work = mi.rcWork;
        }
        int winX = anchorScreenX - marginPx;
        int winY = anchorScreenY - marginPx;
        const int bodyPxW = pxW - marginPx * 2;
        const int bodyPxH = pxH - marginPx * 2;
        if (winX + marginPx + bodyPxW > work.right - 6) {
            winX = std::max<int>(work.left + 6 - marginPx, work.right - 6 - bodyPxW - marginPx);
        }
        if (winX + marginPx < work.left + 6) {
            winX = work.left + 6 - marginPx;
        }
        if (winY + marginPx + bodyPxH > work.bottom - 6) {
            winY = std::max<int>(work.top + 6 - marginPx, anchorScreenY - bodyPxH - marginPx - 4);
        }

        if (!IsWindow(hwnd_)) {
            hwnd_ =
                CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kWindowClass, L"", WS_POPUP,
                                winX, winY, pxW, pxH, owner, nullptr, GetModuleHandleW(nullptr), this);
        } else {
            SetWindowPos(hwnd_, HWND_TOPMOST, winX, winY, pxW, pxH, SWP_NOACTIVATE);
        }
        if (!IsWindow(hwnd_)) return -1;

        winX_ = winX;
        winY_ = winY;
        render();
        SetWindowPos(hwnd_, HWND_TOPMOST, winX, winY, pxW, pxH, SWP_SHOWWINDOW);
        (void)SetForegroundWindow(hwnd_);

        tracking_ = true;
        MSG msg{};
        while (tracking_ && IsWindow(hwnd_)) {
            const BOOL gm = GetMessageW(&msg, nullptr, 0, 0);
            if (gm <= 0) {
                if (gm == 0) PostQuitMessage(static_cast<int>(msg.wParam));
                tracking_ = false;
                break;
            }
            if (msg.message == WM_HOTKEY) {
                // Pressing Esc while a popup/dropdown menu is open dismisses only the popup menu,
                // never terminating the underlying playground session.
                tracking_ = false;
                break;
            }
            if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
                tracking_ = false;
                break;
            }
            if ((msg.message == WM_LBUTTONDOWN || msg.message == WM_RBUTTONDOWN ||
                 msg.message == WM_MBUTTONDOWN || msg.message == WM_NCLBUTTONDOWN ||
                 msg.message == WM_NCRBUTTONDOWN) &&
                msg.hwnd != hwnd_) {
                tracking_ = false;
                // Swallow click-outside dismiss if it hit owner/card, or let popup close cleanly
                if (msg.hwnd == owner) {
                    break;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (IsWindow(hwnd_)) ShowWindow(hwnd_, SW_HIDE);
        tracking_ = false;
        return chosenId_;
    }

   private:
    static void ensureClassRegistered() noexcept {
        static bool registered = false;
        if (registered) return;
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = wndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kWindowClass;
        if (RegisterClassExW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
            registered = true;
        }
    }

    D2D1_RECT_F bodyRectDip() const noexcept {
        return {kShadowMarginDip, kShadowMarginDip, kShadowMarginDip + bodyW_, kShadowMarginDip + bodyH_};
    }

    int hitRow(float dipX, float dipY) const noexcept {
        const auto body = bodyRectDip();
        if (!pointInsideRoundedDip(dipX, dipY, body, kCornerRadiusDip)) return -1;
        float y = body.top + 5.0f;
        for (std::size_t i = 0; i < items_.size(); ++i) {
            const float rh = items_[i].separator ? kSepHeightDip : kRowHeightDip;
            if (dipY >= y && dipY < y + rh) {
                if (items_[i].separator || !items_[i].enabled) return -1;
                return static_cast<int>(i);
            }
            y += rh;
        }
        return -1;
    }

    void render() noexcept {
        glass_.commitLayered(hwnd_, winX_, winY_, totalW_, totalH_, dpi_, [&](MacGlassContext& g) {
            const auto body = bodyRectDip();
            g.drawMacHudGlass(body, kCornerRadiusDip, 0.95f);

            const float leftGutter = hasChecks_ ? 26.0f : 14.0f;
            float y = body.top + 5.0f;
            for (std::size_t i = 0; i < items_.size(); ++i) {
                const auto& it = items_[i];
                if (it.separator) {
                    const float sy = std::floor(y + kSepHeightDip * 0.5f) + 0.5f;
                    g.brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.11f));
                    g.dcTarget->DrawLine({body.left + 10.0f, sy}, {body.right - 10.0f, sy}, g.brush.Get(),
                                         1.0f);
                    y += kSepHeightDip;
                    continue;
                }

                const D2D1_RECT_F rowRc{body.left + 5.0f, y + 1.0f, body.right - 5.0f,
                                        y + kRowHeightDip - 1.0f};
                const bool hovered = (static_cast<int>(i) == hoverIdx_) && it.enabled;
                if (hovered) {
                    if (it.danger) {
                        g.brush->SetColor(D2D1::ColorF(0.90f, 0.27f, 0.26f, 0.88f));
                    } else {
                        // macOS Dark Mode menu selection accent (#0A84FF)
                        g.brush->SetColor(D2D1::ColorF(0.039f, 0.518f, 1.0f, 0.92f));
                    }
                    g.dcTarget->FillRoundedRectangle(D2D1::RoundedRect(rowRc, 6.0f, 6.0f), g.brush.Get());
                }

                D2D1_COLOR_F txtCol = D2D1::ColorF(0.95f, 0.96f, 0.98f, 1.0f);
                if (!it.enabled) {
                    txtCol = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.32f);
                } else if (it.danger && !hovered) {
                    txtCol = D2D1::ColorF(1.0f, 0.48f, 0.45f, 1.0f);
                }

                if (it.checked) {
                    const float cx = body.left + 14.5f;
                    const float cy = y + kRowHeightDip * 0.5f + 0.5f;
                    g.brush->SetColor(txtCol);
                    g.dcTarget->DrawLine({cx - 4.2f, cy + 0.2f}, {cx - 1.2f, cy + 3.4f}, g.brush.Get(), 1.85f,
                                         g.roundStroke.Get());
                    g.dcTarget->DrawLine({cx - 1.2f, cy + 3.4f}, {cx + 4.5f, cy - 3.6f}, g.brush.Get(), 1.85f,
                                         g.roundStroke.Get());
                }

                if (font_ && !it.label.empty()) {
                    g.brush->SetColor(txtCol);
                    const D2D1_RECT_F trc{body.left + leftGutter, y, body.right - 12.0f, y + kRowHeightDip};
                    g.dcTarget->DrawTextW(it.label.c_str(), static_cast<UINT32>(it.label.size()), font_.Get(),
                                          trc, g.brush.Get());
                }
                y += kRowHeightDip;
            }
        });
    }

    static LRESULT CALLBACK wndProc(HWND w, UINT m, WPARAM wp, LPARAM lp) noexcept {
        auto* self = reinterpret_cast<WinDarkPopupMenu*>(GetWindowLongPtrW(w, GWLP_USERDATA));
        if (m == WM_NCCREATE) {
            self = static_cast<WinDarkPopupMenu*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(w, m, wp, lp);

        if (m == WM_ERASEBKGND) return 1;
        if (m == WM_NCHITTEST) {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(w, &pt);
            const float inv = 96.0f / static_cast<float>(self->dpi_ ? self->dpi_ : 96u);
            if (!pointInsideRoundedDip(static_cast<float>(pt.x) * inv, static_cast<float>(pt.y) * inv,
                                       self->bodyRectDip(), kCornerRadiusDip)) {
                return HTTRANSPARENT;
            }
            return HTCLIENT;
        }
        if (m == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE) {
            self->tracking_ = false;
            return 0;
        }
        if (m == WM_MOUSEMOVE) {
            const float inv = 96.0f / static_cast<float>(self->dpi_ ? self->dpi_ : 96u);
            const int next = self->hitRow(static_cast<float>(GET_X_LPARAM(lp)) * inv,
                                          static_cast<float>(GET_Y_LPARAM(lp)) * inv);
            if (next != self->hoverIdx_) {
                self->hoverIdx_ = next;
                self->render();
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, w, 0};
                TrackMouseEvent(&tme);
            }
            return 0;
        }
        if (m == WM_MOUSELEAVE) {
            if (self->hoverIdx_ != -1) {
                self->hoverIdx_ = -1;
                self->render();
            }
            return 0;
        }
        if (m == WM_LBUTTONDOWN) {
            const float inv = 96.0f / static_cast<float>(self->dpi_ ? self->dpi_ : 96u);
            self->hoverIdx_ = self->hitRow(static_cast<float>(GET_X_LPARAM(lp)) * inv,
                                           static_cast<float>(GET_Y_LPARAM(lp)) * inv);
            self->render();
            return 0;
        }
        if (m == WM_LBUTTONUP) {
            const float inv = 96.0f / static_cast<float>(self->dpi_ ? self->dpi_ : 96u);
            const int hit = self->hitRow(static_cast<float>(GET_X_LPARAM(lp)) * inv,
                                         static_cast<float>(GET_Y_LPARAM(lp)) * inv);
            if (hit >= 0 && hit < static_cast<int>(self->items_.size()) &&
                self->items_[static_cast<std::size_t>(hit)].enabled) {
                self->chosenId_ = self->items_[static_cast<std::size_t>(hit)].id;
            }
            self->tracking_ = false;
            return 0;
        }
        return DefWindowProcW(w, m, wp, lp);
    }

    HWND hwnd_ = nullptr;
    MacGlassContext glass_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> font_;
    std::vector<DarkMenuEntry> items_;
    UINT dpi_ = 96u;
    float bodyW_ = 180.0f;
    float bodyH_ = 100.0f;
    float totalW_ = 204.0f;
    float totalH_ = 124.0f;
    int winX_ = 0;
    int winY_ = 0;
    int hoverIdx_ = -1;
    int chosenId_ = -1;
    bool hasChecks_ = false;
    bool tracking_ = false;
};

class WinFloatingHudBar {
   public:
    static constexpr UINT kTrayCallbackMsg = WM_APP + 24;
    static constexpr const wchar_t* kWindowClass = L"DPPWinFloatingCapsuleV2";
    static constexpr float kPillWidthDip = 596.0f;
    static constexpr float kPillHeightDip = 44.0f;
    static constexpr float kShadowMarginDip = 12.0f;
    static constexpr float kTotalWidthDip = kPillWidthDip + kShadowMarginDip * 2.0f;
    static constexpr float kTotalHeightDip = kPillHeightDip + kShadowMarginDip * 2.0f;

    std::function<void(PlayMode)> onSelectMode;
    std::function<void()> onQuickStart;
    std::function<void()> onTogglePause;
    std::function<void()> onReset;
    std::function<void()> onToggleMute;
    std::function<void()> onToggleSettings;
    std::function<void()> onHideCapsule;
    std::function<void()> onStopSession;
    std::function<void()> onDidMove;
    std::function<void()> onSaveDesktopLayout;
    std::function<void()> onUndoDesktopLayout;
    std::function<void()> onRestorePlaygroundFocus;

    void setHasLayoutBackup(bool v) noexcept { hasLayoutBackup_ = v; }

    WinFloatingHudBar() = default;
    ~WinFloatingHudBar() { destroy(); }
    WinFloatingHudBar(const WinFloatingHudBar&) = delete;
    WinFloatingHudBar& operator=(const WinFloatingHudBar&) = delete;

    void installTray(HWND notifyWindow) noexcept {
        if (trayAdded_ || !IsWindow(notifyWindow)) return;
        trayOwner_ = notifyWindow;
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = notifyWindow;
        nid.uID = 1001u;
        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        nid.uCallbackMessage = kTrayCallbackMsg;
        HICON customIcon = static_cast<HICON>(
            LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101), IMAGE_ICON,
                       GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
        nid.hIcon = customIcon ? customIcon : LoadIconW(nullptr, IDI_APPLICATION);
        wcscpy_s(nid.szTip, L"桌面游乐场 · DeskFling (右键菜单 / Esc 恢复桌面)");
        trayAdded_ = (Shell_NotifyIconW(NIM_ADD, &nid) != FALSE);
    }

    void removeTray() noexcept {
        if (!trayAdded_) return;
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = trayOwner_;
        nid.uID = 1001u;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        trayAdded_ = false;
        trayOwner_ = nullptr;
    }

    // Returns the visible capsule body bounds in screen pixels (excluding the transparent drop-shadow apron).
    RECT pillScreenRect() const noexcept {
        RECT wr{};
        if (!IsWindow(hwnd_) || !GetWindowRect(hwnd_, &wr)) return wr;
        const int m = shadowMarginPx();
        return {wr.left + m, wr.top + m, wr.right - m, wr.bottom - m};
    }

    HWND popupWindow() const noexcept { return trayPopup_.window(); }
    bool popupActive() const noexcept { return trayPopup_.active(); }

    void createAndShow(RECT workArea, UINT dpi, WinUserPrefs& prefs) noexcept {
        workArea_ = workArea;
        dpi_ = dpi ? dpi : 96u;
        prefs_ = &prefs;
        ensureClassRegistered();

        const float s = static_cast<float>(dpi_) / 96.0f;
        const int pillPxW = static_cast<int>(std::lround(kPillWidthDip * s));
        const int pillPxH = static_cast<int>(std::lround(kPillHeightDip * s));
        const int marginPx = shadowMarginPx();
        width_ = static_cast<int>(std::lround(kTotalWidthDip * s));
        height_ = static_cast<int>(std::lround(kTotalHeightDip * s));

        int pillX = workArea.left + ((workArea.right - workArea.left) - pillPxW) / 2;
        int pillY = workArea.top + static_cast<int>(std::lround(10.0f * s));
        if (prefs.hasHudOrigin) {
            pillX = std::clamp<int>(prefs.hudX, static_cast<int>(workArea.left + 6),
                                    std::max<int>(static_cast<int>(workArea.left + 6),
                                                  static_cast<int>(workArea.right - pillPxW - 6)));
            pillY = std::clamp<int>(prefs.hudY, static_cast<int>(workArea.top + 6),
                                    std::max<int>(static_cast<int>(workArea.top + 6),
                                                  static_cast<int>(workArea.bottom - pillPxH - 6)));
        }
        const int winX = pillX - marginPx;
        const int winY = pillY - marginPx;

        if (!IsWindow(hwnd_)) {
            hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
                                    kWindowClass, L"DPP Capsule", WS_POPUP, winX, winY, width_, height_,
                                    nullptr, nullptr, GetModuleHandleW(nullptr), this);
        } else {
            SetWindowPos(hwnd_, HWND_TOPMOST, winX, winY, width_, height_, SWP_NOACTIVATE);
        }
        if (!IsWindow(hwnd_)) return;

        rebuildFormats();
        renderLayered(winX, winY);
        SetWindowPos(hwnd_, HWND_TOPMOST, winX, winY, width_, height_, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    void hide() noexcept {
        if (IsWindow(hwnd_)) ShowWindow(hwnd_, SW_HIDE);
    }

    bool visible() const noexcept {
        return IsWindow(hwnd_) && (GetWindowLongPtrW(hwnd_, GWL_STYLE) & WS_VISIBLE) != 0;
    }
    HWND window() const noexcept { return hwnd_; }

    void destroy() noexcept {
        trayPopup_.destroy();
        removeTray();
        if (IsWindow(hwnd_)) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
        font_.Reset();
    }

    void syncState(bool sessionActive, PlayMode mode, bool paused, bool muted, bool canBilliards,
                   bool settingsVisible) noexcept {
        sessionActive_ = sessionActive;
        currentMode_ = mode;
        paused_ = paused;
        muted_ = muted;
        canBilliards_ = canBilliards;
        settingsVisible_ = settingsVisible;
        if (IsWindow(hwnd_)) {
            RECT wr{};
            GetWindowRect(hwnd_, &wr);
            renderLayered(wr.left, wr.top);
        }
    }

    void handleTrayMessage(HWND menuOwner, LPARAM lp) noexcept {
        const UINT ev = LOWORD(lp);
        if (ev == WM_LBUTTONUP || ev == WM_LBUTTONDBLCLK) {
            if (sessionActive_) {
                if (visible()) {
                    hide();
                    if (onHideCapsule) onHideCapsule();
                } else if (prefs_) {
                    createAndShow(workArea_, dpi_, *prefs_);
                }
            } else if (onToggleSettings) {
                onToggleSettings();
            }
            return;
        }
        if (ev == WM_RBUTTONUP || ev == WM_CONTEXTMENU) {
            showTrayMenu(menuOwner);
        }
    }

   private:
    enum class SlotId : int {
        None = -1,
        Grip = 0,
        ModeGravity = 1,
        ModeBlackHole = 2,
        ModeBilliards = 3,
        ModeSlingshot = 4,
        ModeHockey = 5,
        Pause = 6,
        Reset = 7,
        Mute = 8,
        Settings = 9,
        Collapse = 10,
        Restore = 11,
    };

    struct DipSlot {
        SlotId id = SlotId::None;
        D2D1_RECT_F rc{};
        const wchar_t* label = L"";
        bool active = false;
        bool enabled = true;
        bool danger = false;
        bool isSegment = false;
    };

    int shadowMarginPx() const noexcept {
        const float s = static_cast<float>(dpi_ ? dpi_ : 96u) / 96.0f;
        return static_cast<int>(std::lround(kShadowMarginDip * s));
    }

    static D2D1_RECT_F pillBodyDip() noexcept {
        return {kShadowMarginDip, kShadowMarginDip, kShadowMarginDip + kPillWidthDip,
                kShadowMarginDip + kPillHeightDip};
    }

    static D2D1_RECT_F segmentTrackDip() noexcept {
        const auto b = pillBodyDip();
        return {b.left + 32.0f, b.top + 8.0f, b.left + 318.0f, b.top + 36.0f};
    }

    static void ensureClassRegistered() noexcept {
        static bool registered = false;
        if (registered) return;
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = wndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kWindowClass;
        if (RegisterClassExW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
            registered = true;
        }
    }

    void rebuildFormats() noexcept {
        if (!glass_.ensure()) return;
        font_ = glass_.makeFormat(12.2f, DWRITE_FONT_WEIGHT_SEMI_BOLD, true, false);
    }

    std::array<DipSlot, 12> buildSlots() const noexcept {
        const auto b = pillBodyDip();
        const float top = b.top + 8.0f;
        const float bot = b.top + 36.0f;

        // 1. Unified 5-segment NSSegmentedControl inside [b.left + 32, b.left + 318] (width = 286 DIP)
        const float segLeft = b.left + 32.0f;
        const std::array<float, 5> segWidths = {54.0f, 54.0f, 54.0f, 54.0f, 70.0f};
        std::array<D2D1_RECT_F, 5> segRc{};
        float sx = segLeft;
        for (std::size_t i = 0; i < 5; ++i) {
            segRc[i] = {sx, top, sx + segWidths[i], bot};
            sx += segWidths[i];
        }

        // 2. Clean recessed macOS action buttons on the right of the hairline divider (x = b.left + 325)
        float ax = b.left + 332.0f;
        auto nextAct = [&](float w, float gap = 2.0f) -> D2D1_RECT_F {
            const D2D1_RECT_F r{ax, top, ax + w, bot};
            ax += w + gap;
            return r;
        };

        return {{
            {SlotId::Grip,
             {b.left + 6.0f, b.top + 2.0f, b.left + 30.0f, b.bottom - 2.0f},
             L"",
             false,
             true,
             false,
             false},
            {SlotId::ModeGravity, segRc[0], L"重力", currentMode_ == PlayMode::Gravity, true, false, true},
            {SlotId::ModeBlackHole, segRc[1], L"黑洞", currentMode_ == PlayMode::BlackHole, true, false,
             true},
            {SlotId::ModeBilliards, segRc[2], L"台球", currentMode_ == PlayMode::Billiards,
             currentMode_ == PlayMode::Billiards || canBilliards_, false, true},
            {SlotId::ModeSlingshot, segRc[3], L"弹弓", currentMode_ == PlayMode::Slingshot, true, false,
             true},
            {SlotId::ModeHockey, segRc[4], L"曲棍球", currentMode_ == PlayMode::AirHockey, true, false, true},
            {SlotId::Pause, nextAct(43.0f), paused_ ? L"继续" : L"暂停", paused_, true, false, false},
            {SlotId::Reset, nextAct(43.0f), L"重置", false, true, false, false},
            {SlotId::Mute, nextAct(43.0f), muted_ ? L"静音" : L"音效", muted_, true, false, false},
            {SlotId::Settings, nextAct(45.0f), settingsVisible_ ? L"收起" : L"设置", settingsVisible_, true,
             false, false},
            {SlotId::Collapse, nextAct(26.0f), L"—", false, true, false, false},
            {SlotId::Restore, nextAct(46.0f, 0.0f), L"还原", false, true, true, false},
        }};
    }

    SlotId hitSlotDip(float dipX, float dipY) const noexcept {
        if (!pointInsideRoundedDip(dipX, dipY, pillBodyDip(), kPillHeightDip * 0.5f)) return SlotId::None;
        const auto slots = buildSlots();
        for (const auto& sl : slots) {
            if (dipX >= sl.rc.left && dipX < sl.rc.right && dipY >= sl.rc.top && dipY < sl.rc.bottom) {
                return sl.enabled ? sl.id : SlotId::None;
            }
        }
        // Any empty space inside the capsule drags the capsule smoothly just like the grip!
        return SlotId::Grip;
    }

    void renderLayered(int screenX, int screenY) noexcept {
        if (!IsWindow(hwnd_)) return;
        if (!font_) rebuildFormats();

        glass_.commitLayered(
            hwnd_, screenX, screenY, kTotalWidthDip, kTotalHeightDip, dpi_, [&](MacGlassContext& g) {
                const auto body = pillBodyDip();
                // 1. Frosted dark charcoal macOS HUDWindow capsule + soft Gaussian drop shadow
                g.drawMacHudGlass(body, kPillHeightDip * 0.5f, 0.90f);

                // 2. Unified macOS NSSegmentedControl trough for the 5 modes
                const auto track = segmentTrackDip();
                g.brush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.30f));
                g.dcTarget->FillRoundedRectangle(D2D1::RoundedRect(track, 8.5f, 8.5f), g.brush.Get());
                g.brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f));
                g.dcTarget->DrawRoundedRectangle(D2D1::RoundedRect({track.left + 0.5f, track.top + 0.5f,
                                                                    track.right - 0.5f, track.bottom - 0.5f},
                                                                   8.0f, 8.0f),
                                                 g.brush.Get(), 1.0f);

                const auto slots = buildSlots();

                // Hairline segment separators (only between adjacent unselected, unhovered segments)
                for (std::size_t si = 1; si <= 4; ++si) {
                    const auto& leftSeg = slots[si];
                    const auto& rightSeg = slots[si + 1];
                    const bool leftHighlighted = leftSeg.active || hoverSlot_ == leftSeg.id;
                    const bool rightHighlighted = rightSeg.active || hoverSlot_ == rightSeg.id;
                    if (!leftHighlighted && !rightHighlighted) {
                        const float dx = std::floor(rightSeg.rc.left) + 0.5f;
                        g.brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f));
                        g.dcTarget->DrawLine({dx, track.top + 6.5f}, {dx, track.bottom - 6.5f}, g.brush.Get(),
                                             1.0f);
                    }
                }

                // Subtle vertical hairline between mode segmented control and action buttons
                const float divX = std::floor(body.left + 325.0f) + 0.5f;
                g.brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f));
                g.dcTarget->DrawLine({divX, body.top + 13.0f}, {divX, body.bottom - 13.0f}, g.brush.Get(),
                                     1.0f);

                // 3. Draw grip dots, segments, and recessed action pills
                for (const auto& sl : slots) {
                    if (sl.id == SlotId::Grip) {
                        const float dotAlpha = (hoverSlot_ == SlotId::Grip || dragging_) ? 0.72f : 0.45f;
                        g.brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, dotAlpha));
                        for (int r = 0; r < 3; ++r) {
                            for (int c = 0; c < 2; ++c) {
                                const float cx = body.left + 15.0f + static_cast<float>(c) * 6.0f;
                                const float cy = body.top + 15.0f + static_cast<float>(r) * 7.0f;
                                g.dcTarget->FillEllipse(D2D1::Ellipse({cx, cy}, 1.55f, 1.55f), g.brush.Get());
                            }
                        }
                        continue;
                    }

                    const bool hovered = (hoverSlot_ == sl.id) && sl.enabled;
                    const bool pressed = (pressedSlot_ == sl.id) && hovered;

                    if (sl.isSegment) {
                        const D2D1_RECT_F pill{sl.rc.left + 1.5f, sl.rc.top + 1.5f, sl.rc.right - 1.5f,
                                               sl.rc.bottom - 1.5f};
                        const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(pill, 7.0f, 7.0f);
                        if (sl.active) {
                            // Subtle shadow under active segment
                            g.brush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.30f));
                            g.dcTarget->FillRoundedRectangle(
                                D2D1::RoundedRect(
                                    {pill.left, pill.top + 1.0f, pill.right, pill.bottom + 1.0f}, 7.0f, 7.0f),
                                g.brush.Get());
                            // Elevated macOS VibrantDark active segment pill
                            g.fillVerticalGradientRounded(rr, D2D1::ColorF(0.43f, 0.43f, 0.47f, 0.92f),
                                                          D2D1::ColorF(0.33f, 0.33f, 0.37f, 0.92f));
                            g.strokeVerticalGradientRounded(
                                D2D1::RoundedRect({pill.left + 0.5f, pill.top + 0.5f, pill.right - 0.5f,
                                                   pill.bottom - 0.5f},
                                                  6.5f, 6.5f),
                                D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.28f), D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.09f),
                                1.0f);
                        } else if (pressed) {
                            g.brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.15f));
                            g.dcTarget->FillRoundedRectangle(rr, g.brush.Get());
                        } else if (hovered) {
                            g.brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.09f));
                            g.dcTarget->FillRoundedRectangle(rr, g.brush.Get());
                        }
                    } else {
                        // macOS NSBezelStyleRecessed action button: clean & borderless when idle!
                        const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(sl.rc, 7.0f, 7.0f);
                        if (sl.danger) {
                            if (pressed) {
                                g.brush->SetColor(D2D1::ColorF(1.0f, 0.35f, 0.32f, 0.32f));
                                g.dcTarget->FillRoundedRectangle(rr, g.brush.Get());
                            } else if (hovered) {
                                g.brush->SetColor(D2D1::ColorF(1.0f, 0.35f, 0.32f, 0.22f));
                                g.dcTarget->FillRoundedRectangle(rr, g.brush.Get());
                            }
                        } else if (sl.active) {
                            g.brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, pressed ? 0.27f : 0.21f));
                            g.dcTarget->FillRoundedRectangle(rr, g.brush.Get());
                            g.brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f));
                            g.dcTarget->DrawRoundedRectangle(
                                D2D1::RoundedRect({sl.rc.left + 0.5f, sl.rc.top + 0.5f, sl.rc.right - 0.5f,
                                                   sl.rc.bottom - 0.5f},
                                                  6.5f, 6.5f),
                                g.brush.Get(), 1.0f);
                        } else if (pressed) {
                            g.brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.20f));
                            g.dcTarget->FillRoundedRectangle(rr, g.brush.Get());
                        } else if (hovered) {
                            g.brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f));
                            g.dcTarget->FillRoundedRectangle(rr, g.brush.Get());
                        }
                    }

                    D2D1_COLOR_F txtCol = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.88f);
                    if (!sl.enabled) {
                        txtCol = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.30f);
                    } else if (sl.danger) {
                        txtCol = hovered ? D2D1::ColorF(1.0f, 0.62f, 0.58f, 1.0f)
                                         : D2D1::ColorF(1.0f, 0.48f, 0.45f, 1.0f);
                    } else if (sl.active || hovered) {
                        txtCol = D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
                    }

                    if (font_ && sl.label && sl.label[0] != L'\0') {
                        g.brush->SetColor(txtCol);
                        g.dcTarget->DrawTextW(sl.label, static_cast<UINT32>(std::wcslen(sl.label)),
                                              font_.Get(), sl.rc, g.brush.Get());
                    }
                }
            });
    }

    void dispatchSlotClick(SlotId id) noexcept {
        switch (id) {
            case SlotId::ModeGravity:
                if (onSelectMode) onSelectMode(PlayMode::Gravity);
                if (onRestorePlaygroundFocus) onRestorePlaygroundFocus();
                break;
            case SlotId::ModeBlackHole:
                if (onSelectMode) onSelectMode(PlayMode::BlackHole);
                if (onRestorePlaygroundFocus) onRestorePlaygroundFocus();
                break;
            case SlotId::ModeBilliards:
                if (onSelectMode) onSelectMode(PlayMode::Billiards);
                if (onRestorePlaygroundFocus) onRestorePlaygroundFocus();
                break;
            case SlotId::ModeSlingshot:
                if (onSelectMode) onSelectMode(PlayMode::Slingshot);
                if (onRestorePlaygroundFocus) onRestorePlaygroundFocus();
                break;
            case SlotId::ModeHockey:
                if (onSelectMode) onSelectMode(PlayMode::AirHockey);
                if (onRestorePlaygroundFocus) onRestorePlaygroundFocus();
                break;
            case SlotId::Pause:
                if (onTogglePause) onTogglePause();
                if (onRestorePlaygroundFocus) onRestorePlaygroundFocus();
                break;
            case SlotId::Reset:
                if (onReset) onReset();
                if (onRestorePlaygroundFocus) onRestorePlaygroundFocus();
                break;
            case SlotId::Mute:
                if (onToggleMute) onToggleMute();
                if (onRestorePlaygroundFocus) onRestorePlaygroundFocus();
                break;
            case SlotId::Settings:
                if (onToggleSettings) onToggleSettings();
                break;
            case SlotId::Collapse:
                hide();
                if (onHideCapsule) onHideCapsule();
                if (onRestorePlaygroundFocus) onRestorePlaygroundFocus();
                break;
            case SlotId::Restore:
                if (onStopSession) onStopSession();
                break;
            default:
                break;
        }
    }

    void showTrayMenu(HWND owner) noexcept {
        if (!IsWindow(owner)) return;
        std::vector<DarkMenuEntry> items;
        if (!sessionActive_) {
            items.push_back({1, L"⚡ 一键进入桌面游乐场", false, true, false, false});
            items.push_back({2, L"打开快速启动窗口…", false, true, false, false});
            items.push_back({-1, L"", false, false, true, false});
            items.push_back({40, L"打开运行日志目录…", false, true, false, false});
            items.push_back({99, L"退出程序", false, true, false, true});
        } else {
            items.push_back(
                {3, visible() ? L"隐藏顶部悬浮胶囊条" : L"显示顶部悬浮胶囊条", false, true, false, false});
            items.push_back({2, settingsVisible_ ? L"收起当前玩法设置" : L"打开当前玩法专属设置…", false,
                             true, false, false});
            items.push_back({-1, L"", false, false, true, false});
            items.push_back({10, L"普通重力", currentMode_ == PlayMode::Gravity, true, false, false});
            items.push_back({11, L"玩具黑洞 (3D)", currentMode_ == PlayMode::BlackHole, true, false, false});
            items.push_back({12, L"桌面台球", currentMode_ == PlayMode::Billiards,
                             currentMode_ == PlayMode::Billiards || canBilliards_, false, false});
            items.push_back({13, L"弹弓攻城", currentMode_ == PlayMode::Slingshot, true, false, false});
            items.push_back({14, L"空气曲棍球", currentMode_ == PlayMode::AirHockey, true, false, false});
            items.push_back({-1, L"", false, false, true, false});
            items.push_back({20, paused_ ? L"继续物理运行" : L"暂停物理运行", false, true, false, false});
            items.push_back({21, L"重置当前场景", false, true, false, false});
            items.push_back({22, L"静音合成音效", muted_, true, false, false});
            items.push_back({-1, L"", false, false, true, false});
            items.push_back({30, L"保存当前图标位置到桌面…", false, true, false, false});
            if (hasLayoutBackup_) {
                items.push_back({31, L"撤销上次位置写回（恢复原坐标）", false, true, false, false});
            }
            items.push_back({40, L"打开运行日志目录…", false, true, false, false});
            items.push_back({-1, L"", false, false, true, false});
            items.push_back({99, L"立即恢复原桌面并退出 (Esc)", false, true, false, true});
        }

        POINT cur{};
        GetCursorPos(&cur);
        const int cmd = trayPopup_.showModal(owner, cur.x, cur.y, dpi_, items, 224.0f);

        switch (cmd) {
            case 1:
                if (onQuickStart) onQuickStart();
                break;
            case 2:
                if (onToggleSettings) onToggleSettings();
                break;
            case 3:
                if (visible())
                    hide();
                else if (prefs_)
                    createAndShow(workArea_, dpi_, *prefs_);
                break;
            case 10:
            case 11:
            case 12:
            case 13:
            case 14:
                if (onSelectMode) onSelectMode(static_cast<PlayMode>(cmd - 10));
                if (onRestorePlaygroundFocus) onRestorePlaygroundFocus();
                break;
            case 20:
                if (onTogglePause) onTogglePause();
                if (onRestorePlaygroundFocus) onRestorePlaygroundFocus();
                break;
            case 21:
                if (onReset) onReset();
                if (onRestorePlaygroundFocus) onRestorePlaygroundFocus();
                break;
            case 22:
                if (onToggleMute) onToggleMute();
                if (onRestorePlaygroundFocus) onRestorePlaygroundFocus();
                break;
            case 30:
                if (onSaveDesktopLayout) onSaveDesktopLayout();
                break;
            case 31:
                if (onUndoDesktopLayout) onUndoDesktopLayout();
                break;
            case 40: {
                wchar_t buf[MAX_PATH]{};
                DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
                if (n == 0 || n >= MAX_PATH) n = GetTempPathW(MAX_PATH, buf);
                if (n > 0 && n < MAX_PATH) {
                    std::wstring dir(buf, n);
                    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
                    dir += L"\\DesktopPhysicsPlayground-Logs";
                    CreateDirectoryW(dir.c_str(), nullptr);
                    ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
                break;
            }
            case 99:
                if (onStopSession) onStopSession();
                break;
            default:
                if (sessionActive_ && onRestorePlaygroundFocus) onRestorePlaygroundFocus();
                break;
        }
    }

    static LRESULT CALLBACK wndProc(HWND w, UINT m, WPARAM wp, LPARAM lp) noexcept {
        auto* self = reinterpret_cast<WinFloatingHudBar*>(GetWindowLongPtrW(w, GWLP_USERDATA));
        if (m == WM_NCCREATE) {
            self = static_cast<WinFloatingHudBar*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(w, m, wp, lp);

        if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
        if (m == WM_ERASEBKGND) return 1;
        if (m == WM_CLOSE) {
            self->hide();
            if (self->onHideCapsule) self->onHideCapsule();
            return 0;
        }
        if (m == WM_NCHITTEST) {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(w, &pt);
            const float inv = 96.0f / static_cast<float>(self->dpi_ ? self->dpi_ : 96u);
            if (!pointInsideRoundedDip(static_cast<float>(pt.x) * inv, static_cast<float>(pt.y) * inv,
                                       pillBodyDip(), kPillHeightDip * 0.5f)) {
                return HTTRANSPARENT;
            }
            return HTCLIENT;
        }
        if (m == WM_SETCURSOR) {
            if (LOWORD(lp) == HTCLIENT) {
                SetCursor(LoadCursorW(nullptr, (self->hoverSlot_ == SlotId::Grip || self->dragging_)
                                                   ? IDC_SIZEALL
                                                   : IDC_ARROW));
                return TRUE;
            }
        }
        if (m == WM_PAINT) {
            PAINTSTRUCT ps{};
            BeginPaint(w, &ps);
            EndPaint(w, &ps);
            RECT wr{};
            if (GetWindowRect(w, &wr)) self->renderLayered(wr.left, wr.top);
            return 0;
        }
        if (m == WM_MOUSEMOVE) {
            const float inv = 96.0f / static_cast<float>(self->dpi_ ? self->dpi_ : 96u);
            if (self->dragging_) {
                POINT cur{};
                GetCursorPos(&cur);
                const int marginPx = self->shadowMarginPx();
                const int pillPxW = self->width_ - marginPx * 2;
                const int pillPxH = self->height_ - marginPx * 2;
                const int rawPillX = static_cast<int>(self->dragStartOrigin_.x + marginPx +
                                                      (cur.x - self->dragStartCursor_.x));
                const int rawPillY = static_cast<int>(self->dragStartOrigin_.y + marginPx +
                                                      (cur.y - self->dragStartCursor_.y));
                const int pillX =
                    std::clamp<int>(rawPillX, static_cast<int>(self->workArea_.left + 6),
                                    std::max<int>(static_cast<int>(self->workArea_.left + 6),
                                                  static_cast<int>(self->workArea_.right - pillPxW - 6)));
                const int pillY =
                    std::clamp<int>(rawPillY, static_cast<int>(self->workArea_.top + 6),
                                    std::max<int>(static_cast<int>(self->workArea_.top + 6),
                                                  static_cast<int>(self->workArea_.bottom - pillPxH - 6)));
                SetWindowPos(w, HWND_TOPMOST, pillX - marginPx, pillY - marginPx, 0, 0,
                             SWP_NOSIZE | SWP_NOACTIVATE);
                if (self->prefs_) {
                    self->prefs_->hasHudOrigin = true;
                    self->prefs_->hudX = pillX;
                    self->prefs_->hudY = pillY;
                }
                if (self->onDidMove) self->onDidMove();
                return 0;
            }
            const SlotId nextHover = self->hitSlotDip(static_cast<float>(GET_X_LPARAM(lp)) * inv,
                                                      static_cast<float>(GET_Y_LPARAM(lp)) * inv);
            if (nextHover != self->hoverSlot_) {
                self->hoverSlot_ = nextHover;
                RECT wr{};
                if (GetWindowRect(w, &wr)) self->renderLayered(wr.left, wr.top);
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, w, 0};
                TrackMouseEvent(&tme);
            }
            return 0;
        }
        if (m == WM_MOUSELEAVE) {
            if (self->hoverSlot_ != SlotId::None || self->pressedSlot_ != SlotId::None) {
                self->hoverSlot_ = SlotId::None;
                self->pressedSlot_ = SlotId::None;
                RECT wr{};
                if (GetWindowRect(w, &wr)) self->renderLayered(wr.left, wr.top);
            }
            return 0;
        }
        if (m == WM_LBUTTONDOWN) {
            const float inv = 96.0f / static_cast<float>(self->dpi_ ? self->dpi_ : 96u);
            const SlotId hit = self->hitSlotDip(static_cast<float>(GET_X_LPARAM(lp)) * inv,
                                                static_cast<float>(GET_Y_LPARAM(lp)) * inv);
            if (hit == SlotId::Grip) {
                self->dragging_ = true;
                GetCursorPos(&self->dragStartCursor_);
                RECT wr{};
                GetWindowRect(w, &wr);
                self->dragStartOrigin_ = {wr.left, wr.top};
                SetCapture(w);
                RECT r2{};
                if (GetWindowRect(w, &r2)) self->renderLayered(r2.left, r2.top);
                return 0;
            }
            self->pressedSlot_ = hit;
            SetCapture(w);
            RECT wr{};
            if (GetWindowRect(w, &wr)) self->renderLayered(wr.left, wr.top);
            return 0;
        }
        if (m == WM_LBUTTONUP) {
            if (self->dragging_) {
                self->dragging_ = false;
                if (GetCapture() == w) ReleaseCapture();
                if (self->prefs_) self->prefs_->save();
                RECT wr{};
                if (GetWindowRect(w, &wr)) self->renderLayered(wr.left, wr.top);
                return 0;
            }
            if (GetCapture() == w) ReleaseCapture();
            const float inv = 96.0f / static_cast<float>(self->dpi_ ? self->dpi_ : 96u);
            const SlotId hit = self->hitSlotDip(static_cast<float>(GET_X_LPARAM(lp)) * inv,
                                                static_cast<float>(GET_Y_LPARAM(lp)) * inv);
            const SlotId pressed = self->pressedSlot_;
            self->pressedSlot_ = SlotId::None;
            RECT wr{};
            if (GetWindowRect(w, &wr)) self->renderLayered(wr.left, wr.top);
            if (hit != SlotId::None && hit == pressed) {
                self->dispatchSlotClick(hit);
            }
            return 0;
        }
        return DefWindowProcW(w, m, wp, lp);
    }

    HWND hwnd_ = nullptr;
    HWND trayOwner_ = nullptr;
    MacGlassContext glass_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> font_;
    WinDarkPopupMenu trayPopup_;
    RECT workArea_{};
    UINT dpi_ = 96u;
    int width_ = 620;
    int height_ = 68;
    WinUserPrefs* prefs_ = nullptr;

    bool trayAdded_ = false;
    bool sessionActive_ = false;
    PlayMode currentMode_ = PlayMode::Gravity;
    bool paused_ = false;
    bool muted_ = false;
    bool canBilliards_ = true;
    bool settingsVisible_ = false;
    bool hasLayoutBackup_ = false;

    SlotId hoverSlot_ = SlotId::None;
    SlotId pressedSlot_ = SlotId::None;
    bool dragging_ = false;
    POINT dragStartCursor_{};
    POINT dragStartOrigin_{};
};

}  // namespace dpp::winmirror
