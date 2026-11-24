#pragma once
// macOS Sonoma/Sequoia VibrantDark-inspired floating Mode-Exclusive Settings
// Popover Card & Quick-Start Launcher Card for Windows DPP.
// Rendered with Direct2D + DirectWrite + UpdateLayeredWindow (32-bpp premultiplied
// ARGB, Per-Monitor-V2 DPI scaled) while reading/dispatching directly through
// MarkerWindow's backing Win32 control IDs on `panel` so zero core/session logic
// is duplicated.

#include <commctrl.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "floating_hud_bar.h"

namespace dpp::winmirror {

class WinModeSettingsCard {
   public:
    static constexpr const wchar_t* kWindowClass = L"DPPWinMacSettingsCardV1";
    static constexpr float kShadowMarginDip = 14.0f;
    static constexpr float kCardCornerDip = 16.0f;
    static constexpr float kLauncherCornerDip = 20.0f;
    static constexpr float kLauncherWidthDip = 540.0f;
    static constexpr float kSettingsWidthDip = 428.0f;

    std::function<void()> onCloseCard;
    std::function<void()> onDidChangeVisibility;
    std::function<bool()> queryModalDialogActive;

    WinModeSettingsCard() = default;
    ~WinModeSettingsCard() { destroy(); }
    WinModeSettingsCard(const WinModeSettingsCard&) = delete;
    WinModeSettingsCard& operator=(const WinModeSettingsCard&) = delete;

    HWND window() const noexcept { return hwnd_; }
    HWND popupWindow() const noexcept { return dropdown_.window(); }
    bool popupActive() const noexcept { return dropdown_.active() || closingDropdown_; }
    void cancelPopup() noexcept { dropdown_.cancel(); }

    bool visible() const noexcept {
        return IsWindow(hwnd_) && (GetWindowLongPtrW(hwnd_, GWL_STYLE) & WS_VISIBLE) != 0;
    }

    void destroy() noexcept {
        dropdown_.destroy();
        if (IsWindow(hwnd_)) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
        titleFmt_.Reset();
        brandTitleFmt_.Reset();
        brandSubFmt_.Reset();
        ctaFmt_.Reset();
        labelFmt_.Reset();
        ctrlFmt_.Reset();
        ctrlCenterFmt_.Reset();
        monoFmt_.Reset();
        hintFmt_.Reset();
        appIconBmp_.Reset();
        appIconLoaded_ = false;
    }

    void hide() noexcept {
        dropdown_.cancel();
        editFocused_ = false;
        activeSliderIdx_ = -1;
        pressedIdx_ = -1;
        hoverIdx_ = -1;
        if (IsWindow(hwnd_)) {
            if (GetCapture() == hwnd_) ReleaseCapture();
            ShowWindow(hwnd_, SW_HIDE);
        }
        if (onDidChangeVisibility) onDidChangeVisibility();
    }

    // Shows or updates the Quick-Start Launcher Card (centered on workArea).
    void showLauncher(HWND backingPanel, HWND phaseLabelHwnd, RECT workArea, UINT dpi,
                      bool hasLayoutBackup) noexcept {
        if (!IsWindow(backingPanel)) return;
        backingPanel_ = backingPanel;
        phaseLabelHwnd_ = phaseLabelHwnd;
        workArea_ = workArea;
        dpi_ = dpi ? dpi : 96u;
        launcherMode_ = true;
        hasLayoutBackup_ = hasLayoutBackup;
        editFocused_ = false;

        ensureWindowCreated();
        if (!IsWindow(hwnd_)) return;

        rebuildLayout();
        const float s = static_cast<float>(dpi_) / 96.0f;
        const int marginPx = shadowMarginPx();
        const int bodyPxW = static_cast<int>(std::lround(bodyW_ * s));
        const int bodyPxH = static_cast<int>(std::lround(bodyH_ * s));
        const int pxW = static_cast<int>(std::lround(totalW_ * s));
        const int pxH = static_cast<int>(std::lround(totalH_ * s));
        const int bodyX = workArea_.left + ((workArea_.right - workArea_.left) - bodyPxW) / 2;
        const int bodyY = workArea_.top + ((workArea_.bottom - workArea_.top) - bodyPxH) / 2;
        winX_ = bodyX - marginPx;
        winY_ = bodyY - marginPx;

        render();
        SetWindowPos(hwnd_, HWND_TOP, winX_, winY_, pxW, pxH, SWP_SHOWWINDOW);
        if (onDidChangeVisibility) onDidChangeVisibility();
    }

    // Shows or updates the Mode-Exclusive Floating Settings Popover Card anchored beneath
    // `hudPillScreenRect`.
    void showModeSettings(HWND backingPanel, HWND phaseLabelHwnd, RECT workArea, RECT hudPillScreenRect,
                          bool hudVisible, UINT dpi, PlayMode mode, bool hasLayoutBackup,
                          bool repositionToHud) noexcept {
        if (!IsWindow(backingPanel)) return;
        backingPanel_ = backingPanel;
        phaseLabelHwnd_ = phaseLabelHwnd;
        workArea_ = workArea;
        dpi_ = dpi ? dpi : 96u;
        launcherMode_ = false;
        currentMode_ = mode;
        hasLayoutBackup_ = hasLayoutBackup;

        ensureWindowCreated();
        if (!IsWindow(hwnd_)) return;

        const bool wasVisible = visible();
        rebuildLayout();

        const float s = static_cast<float>(dpi_) / 96.0f;
        const int pxW = static_cast<int>(std::lround(totalW_ * s));
        const int pxH = static_cast<int>(std::lround(totalH_ * s));
        if (repositionToHud || !wasVisible) {
            computeSettingsPosition(hudPillScreenRect, hudVisible);
        } else {
            // Keep current top-left of card body anchored while height morphs dynamically
            RECT wr{};
            if (GetWindowRect(hwnd_, &wr)) {
                winX_ = wr.left;
                winY_ = wr.top;
                const int marginPx = shadowMarginPx();
                const int bodyPxH = pxH - marginPx * 2;
                if (winY_ + marginPx + bodyPxH > workArea_.bottom - 6) {
                    winY_ = std::max<int>(workArea_.top + 6 - marginPx,
                                          workArea_.bottom - 6 - bodyPxH - marginPx);
                }
            }
        }

        render();
        SetWindowPos(hwnd_, HWND_TOPMOST, winX_, winY_, pxW, pxH, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        if (!wasVisible && onDidChangeVisibility) onDidChangeVisibility();
    }

    void followHud(RECT workArea, RECT hudPillScreenRect, bool hudVisible) noexcept {
        if (!visible() || launcherMode_) return;
        workArea_ = workArea;
        computeSettingsPosition(hudPillScreenRect, hudVisible);
        SetWindowPos(hwnd_, HWND_TOPMOST, winX_, winY_, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
    }

    void refresh(bool hasLayoutBackup) noexcept {
        hasLayoutBackup_ = hasLayoutBackup;
        if (!visible() || !IsWindow(backingPanel_)) return;
        const float prevBodyH = bodyH_;
        rebuildLayout();
        if (std::abs(bodyH_ - prevBodyH) > 0.5f) {
            const float s = static_cast<float>(dpi_) / 96.0f;
            const int pxW = static_cast<int>(std::lround(totalW_ * s));
            const int pxH = static_cast<int>(std::lround(totalH_ * s));
            SetWindowPos(hwnd_, launcherMode_ ? HWND_TOP : HWND_TOPMOST, winX_, winY_, pxW, pxH,
                         SWP_NOACTIVATE);
        }
        render();
    }

   private:
    enum class WidgetKind {
        CloseBtn,
        DragHeader,
        PrimaryButton,
        ModePill,
        Button,
        DangerButton,
        AccentButton,
        Popup,
        Checkbox,
        Slider,
        IconList,
        TextEdit,
        StaticLabel,
        ValueLabel,
        StatusLabel,
        HintLabel,
    };

    struct Widget {
        WidgetKind kind = WidgetKind::StaticLabel;
        int ctrlId = 0;
        D2D1_RECT_F rc{};
        std::wstring text;
        int sliderMin = 0;
        int sliderMax = 100;
        bool enabled = true;
    };

    int shadowMarginPx() const noexcept {
        const float s = static_cast<float>(dpi_ ? dpi_ : 96u) / 96.0f;
        return static_cast<int>(std::lround(kShadowMarginDip * s));
    }

    D2D1_RECT_F bodyRectDip() const noexcept {
        return {kShadowMarginDip, kShadowMarginDip, kShadowMarginDip + bodyW_, kShadowMarginDip + bodyH_};
    }

    void computeSettingsPosition(RECT hudPillScreenRect, bool hudVisible) noexcept {
        const float s = static_cast<float>(dpi_) / 96.0f;
        const int marginPx = shadowMarginPx();
        const int bodyPxW = static_cast<int>(std::lround(bodyW_ * s));
        const int bodyPxH = static_cast<int>(std::lround(bodyH_ * s));
        const int gapPx = static_cast<int>(std::lround(8.0f * s));

        int bodyX = workArea_.left + ((workArea_.right - workArea_.left) - bodyPxW) / 2;
        int bodyY = workArea_.top + static_cast<int>(std::lround(62.0f * s));
        if (hudVisible && hudPillScreenRect.right > hudPillScreenRect.left) {
            bodyX = std::clamp<int>(
                static_cast<int>((hudPillScreenRect.left + hudPillScreenRect.right - bodyPxW) / 2),
                static_cast<int>(workArea_.left + 8),
                std::max<int>(static_cast<int>(workArea_.left + 8),
                              static_cast<int>(workArea_.right - bodyPxW - 8)));
            bodyY = static_cast<int>(hudPillScreenRect.bottom + gapPx);
            if (bodyY + bodyPxH > workArea_.bottom - 8) {
                bodyY = std::max<int>(static_cast<int>(workArea_.top + 8),
                                      static_cast<int>(hudPillScreenRect.top - bodyPxH - gapPx));
            }
        }
        winX_ = bodyX - marginPx;
        winY_ = bodyY - marginPx;
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

    void ensureWindowCreated() noexcept {
        ensureClassRegistered();
        if (glass_.ensure() && !titleFmt_) {
            titleFmt_ = glass_.makeFormat(13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, false);
            brandTitleFmt_ = glass_.makeFormat(18.5f, DWRITE_FONT_WEIGHT_BOLD, false, false);
            brandSubFmt_ = glass_.makeFormat(12.0f, DWRITE_FONT_WEIGHT_REGULAR, false, false);
            ctaFmt_ = glass_.makeFormat(15.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, true, false);
            labelFmt_ = glass_.makeFormat(12.5f, DWRITE_FONT_WEIGHT_MEDIUM, false, false);
            ctrlFmt_ = glass_.makeFormat(12.2f, DWRITE_FONT_WEIGHT_MEDIUM, false, false);
            ctrlCenterFmt_ = glass_.makeFormat(12.4f, DWRITE_FONT_WEIGHT_SEMI_BOLD, true, false);
            monoFmt_ = glass_.makeFormat(11.6f, DWRITE_FONT_WEIGHT_REGULAR, false, false);
            hintFmt_ = glass_.makeFormat(11.4f, DWRITE_FONT_WEIGHT_REGULAR, false, true);
        }
        if (glass_.dcTarget && !appIconLoaded_) {
            appIconLoaded_ = true;
            HICON hIcon = static_cast<HICON>(LoadImageW(
                GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101), IMAGE_ICON, 128, 128, LR_DEFAULTCOLOR));
            if (hIcon) {
                Microsoft::WRL::ComPtr<IWICImagingFactory> wic;
                if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                               IID_PPV_ARGS(&wic)))) {
                    Microsoft::WRL::ComPtr<IWICBitmap> wb;
                    if (SUCCEEDED(wic->CreateBitmapFromHICON(hIcon, &wb))) {
                        Microsoft::WRL::ComPtr<IWICFormatConverter> conv;
                        if (SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
                            SUCCEEDED(conv->Initialize(wb.Get(), GUID_WICPixelFormat32bppPBGRA,
                                                       WICBitmapDitherTypeNone, nullptr, 0.0,
                                                       WICBitmapPaletteTypeCustom))) {
                            (void)glass_.dcTarget->CreateBitmapFromWicBitmap(conv.Get(), nullptr,
                                                                             &appIconBmp_);
                        }
                    }
                }
                DestroyIcon(hIcon);
            }
        }
        if (!IsWindow(hwnd_)) {
            hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kWindowClass,
                                    L"桌面游乐场 · DeskFling", WS_POPUP, 0, 0, 100, 100, nullptr, nullptr,
                                    GetModuleHandleW(nullptr), this);
        }
    }

    std::wstring readCtrlText(int id) const noexcept {
        if (!IsWindow(backingPanel_)) return {};
        HWND c = GetDlgItem(backingPanel_, id);
        if (!c) return {};
        wchar_t buf[512]{};
        GetWindowTextW(c, buf, 511);
        return buf;
    }

    std::wstring readHwndText(HWND h) const noexcept {
        if (!IsWindow(h)) return {};
        wchar_t buf[512]{};
        GetWindowTextW(h, buf, 511);
        return buf;
    }

    int readComboSel(int id) const noexcept {
        if (!IsWindow(backingPanel_)) return 0;
        HWND c = GetDlgItem(backingPanel_, id);
        if (!c) return 0;
        const LRESULT r = SendMessageW(c, CB_GETCURSEL, 0, 0);
        return r == CB_ERR ? 0 : static_cast<int>(r);
    }

    std::wstring readComboCurrentText(int id) const noexcept {
        if (!IsWindow(backingPanel_)) return {};
        HWND c = GetDlgItem(backingPanel_, id);
        if (!c) return {};
        const int sel = readComboSel(id);
        const LRESULT len = SendMessageW(c, CB_GETLBTEXTLEN, static_cast<WPARAM>(sel), 0);
        if (len <= 0 || len > 256) return {};
        std::wstring out(static_cast<std::size_t>(len) + 1u, L'\0');
        SendMessageW(c, CB_GETLBTEXT, static_cast<WPARAM>(sel), reinterpret_cast<LPARAM>(out.data()));
        out.resize(std::wcslen(out.c_str()));
        return out;
    }

    bool readCheckbox(int id) const noexcept {
        if (!IsWindow(backingPanel_)) return false;
        HWND c = GetDlgItem(backingPanel_, id);
        return c && SendMessageW(c, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }

    int readSliderPos(int id) const noexcept {
        if (!IsWindow(backingPanel_)) return 0;
        HWND c = GetDlgItem(backingPanel_, id);
        return c ? static_cast<int>(SendMessageW(c, TBM_GETPOS, 0, 0)) : 0;
    }

    bool isCtrlEnabled(int id) const noexcept {
        if (!IsWindow(backingPanel_)) return false;
        HWND c = GetDlgItem(backingPanel_, id);
        return c && IsWindowEnabled(c) != FALSE;
    }

    void syncEditBufferFromBacking() noexcept {
        if (!editFocused_) {
            editBuffer_ = readCtrlText(51);
        }
    }

    void pushEditBufferToBacking() noexcept {
        if (!IsWindow(backingPanel_)) return;
        if (HWND ed = GetDlgItem(backingPanel_, 51)) {
            SetWindowTextW(ed, editBuffer_.c_str());
        }
    }

    void rebuildLayout() noexcept {
        widgets_.clear();
        syncEditBufferFromBacking();

        if (launcherMode_) {
            bodyW_ = kLauncherWidthDip;
            const float L = kShadowMarginDip;
            const float T = kShadowMarginDip;
            const float W = bodyW_;

            titleText_.clear();
            // Whole header is draggable; top-right circular close button exits cleanly
            widgets_.push_back(
                {WidgetKind::DragHeader, 0, {L, T, L + W - 48.0f, T + 76.0f}, {}, 0, 0, true});
            widgets_.push_back({WidgetKind::CloseBtn,
                                11,
                                {L + W - 40.0f, T + 14.0f, L + W - 14.0f, T + 40.0f},
                                L"✕",
                                0,
                                0,
                                true});

            float y = T + 94.0f;
            // Row 1: Display selection (Combo ID 78)
            widgets_.push_back(
                {WidgetKind::StaticLabel, 0, {L + 26.0f, y, L + 98.0f, y + 28.0f}, L"游玩屏幕", 0, 0, true});
            widgets_.push_back(
                {WidgetKind::Popup, 78, {L + 102.0f, y, L + W - 26.0f, y + 28.0f}, {}, 0, 0, true});
            y += 44.0f;

            // Row 2: Initial PlayMode pills (Combo ID 40, mode index in sliderMin)
            widgets_.push_back(
                {WidgetKind::StaticLabel, 0, {L + 26.0f, y, L + 98.0f, y + 34.0f}, L"初始玩法", 0, 0, true});
            const wchar_t* pillNames[5] = {L"重力", L"3D 黑洞", L"台球", L"弹弓", L"曲棍球"};
            const float pillStartX = L + 102.0f;
            const float pillGap = 7.0f;
            const float pillW = std::floor(((L + W - 26.0f) - pillStartX - pillGap * 4.0f) / 5.0f);
            for (int i = 0; i < 5; ++i) {
                const float px = pillStartX + static_cast<float>(i) * (pillW + pillGap);
                widgets_.push_back(
                    {WidgetKind::ModePill, 40, {px, y, px + pillW, y + 34.0f}, pillNames[i], i, 4, true});
            }
            y += 50.0f;

            // Primary CTA Button
            widgets_.push_back({WidgetKind::PrimaryButton,
                                20,
                                {L + 26.0f, y, L + W - 26.0f, y + 48.0f},
                                L"▶  开始游玩",
                                0,
                                0,
                                isCtrlEnabled(20)});
            y += 48.0f;

            // Show footer row ONLY when a layout backup exists or an explicit warning/error is active
            std::wstring statusStr = readHwndText(phaseLabelHwnd_);
            const bool isAlertStatus =
                !statusStr.empty() &&
                (statusStr.find(L"失败") != std::wstring::npos || statusStr.find(L"异常") != std::wstring::npos ||
                 statusStr.find(L"未") != std::wstring::npos || statusStr.find(L"请先") != std::wstring::npos ||
                 statusStr.find(L"撤销") != std::wstring::npos);
            if (hasLayoutBackup_ || isAlertStatus) {
                y += 12.0f;
                if (hasLayoutBackup_) {
                    if (isAlertStatus) {
                        widgets_.push_back({WidgetKind::StatusLabel,
                                            0,
                                            {L + 26.0f, y, L + W - 144.0f, y + 30.0f},
                                            statusStr,
                                            0,
                                            0,
                                            true});
                    }
                    widgets_.push_back({WidgetKind::Button,
                                        72,
                                        {L + W - 134.0f, y + 1.0f, L + W - 26.0f, y + 29.0f},
                                        L"↺ 撤销上次写回",
                                        0,
                                        0,
                                        true});
                } else {
                    widgets_.push_back({WidgetKind::StatusLabel,
                                        0,
                                        {L + 26.0f, y, L + W - 26.0f, y + 30.0f},
                                        statusStr,
                                        0,
                                        0,
                                        true});
                }
                y += 38.0f;
            } else {
                y += 22.0f;
            }
            bodyH_ = y - T;
            totalW_ = bodyW_ + kShadowMarginDip * 2.0f;
            totalH_ = bodyH_ + kShadowMarginDip * 2.0f;
            return;
        }

        bodyW_ = kSettingsWidthDip;
        const float L = kShadowMarginDip;
        const float T = kShadowMarginDip;
        const float W = bodyW_;

        widgets_.push_back({WidgetKind::DragHeader, 0, {L, T, L + W - 46.0f, T + 40.0f}, {}, 0, 0, true});
        widgets_.push_back(
            {WidgetKind::CloseBtn, 0, {L + W - 38.0f, T + 8.0f, L + W - 14.0f, T + 32.0f}, L"✕", 0, 0, true});

        float y = T + 50.0f;
        if (HWND c78 = IsWindow(backingPanel_) ? GetDlgItem(backingPanel_, 78) : nullptr;
            c78 && SendMessageW(c78, CB_GETCOUNT, 0, 0) > 1) {
            widgets_.push_back(
                {WidgetKind::StaticLabel, 0, {L + 18.0f, y, L + 82.0f, y + 26.0f}, L"显示屏幕", 0, 0, true});
            widgets_.push_back(
                {WidgetKind::Popup, 78, {L + 86.0f, y, L + W - 18.0f, y + 26.0f}, {}, 0, 0, true});
            y += 34.0f;
        }

        if (currentMode_ == PlayMode::Gravity) {
            titleText_ = L"普通重力 · 科学物理设置";
            widgets_.push_back(
                {WidgetKind::StaticLabel, 0, {L + 18.0f, y, L + 82.0f, y + 26.0f}, L"天体环境", 0, 0, true});
            widgets_.push_back(
                {WidgetKind::Popup, 73, {L + 86.0f, y, L + 312.0f, y + 26.0f}, {}, 0, 0, true});
            widgets_.push_back(
                {WidgetKind::Popup, 74, {L + 318.0f, y, L + W - 18.0f, y + 26.0f}, {}, 0, 0, true});
            y += 34.0f;

            widgets_.push_back(
                {WidgetKind::StaticLabel, 0, {L + 18.0f, y, L + 82.0f, y + 24.0f}, L"重力 g", 0, 0, true});
            widgets_.push_back(
                {WidgetKind::Slider, 75, {L + 86.0f, y, L + 288.0f, y + 24.0f}, {}, 0, 4000, true});
            widgets_.push_back(
                {WidgetKind::ValueLabel, 223, {L + 296.0f, y, L + W - 14.0f, y + 24.0f}, {}, 0, 0, true});
            y += 32.0f;

            widgets_.push_back(
                {WidgetKind::StaticLabel, 0, {L + 18.0f, y, L + 82.0f, y + 24.0f}, L"碰撞弹性", 0, 0, true});
            widgets_.push_back(
                {WidgetKind::Slider, 76, {L + 86.0f, y, L + 326.0f, y + 24.0f}, {}, 0, 95, true});
            widgets_.push_back(
                {WidgetKind::ValueLabel, 225, {L + 334.0f, y, L + W - 14.0f, y + 24.0f}, {}, 0, 0, true});
            y += 32.0f;

            widgets_.push_back({WidgetKind::Checkbox,
                                29,
                                {L + 18.0f, y, L + 212.0f, y + 24.0f},
                                L"显示桌面图标文件名",
                                0,
                                0,
                                true});
            widgets_.push_back({WidgetKind::Button,
                                77,
                                {L + W - 124.0f, y - 1.0f, L + W - 18.0f, y + 25.0f},
                                L"标准地球重力",
                                0,
                                0,
                                true});
            y += 33.0f;

            widgets_.push_back({WidgetKind::Button,
                                71,
                                {L + 18.0f, y, L + 206.0f, y + 27.0f},
                                L"保存当前位置到桌面…",
                                0,
                                0,
                                true});
            widgets_.push_back({WidgetKind::Button,
                                72,
                                {L + 214.0f, y, L + 314.0f, y + 27.0f},
                                L"撤销写回",
                                0,
                                0,
                                hasLayoutBackup_ || isCtrlEnabled(72)});
            y += 35.0f;
        } else if (currentMode_ == PlayMode::BlackHole) {
            titleText_ = L"3D 拟真黑洞 · 专属设置";
            widgets_.push_back({WidgetKind::Checkbox,
                                31,
                                {L + 18.0f, y, L + 224.0f, y + 24.0f},
                                L"黑洞吞噬全部图标副本",
                                0,
                                0,
                                true});
            widgets_.push_back({WidgetKind::Button,
                                35,
                                {L + W - 124.0f, y - 1.0f, L + W - 18.0f, y + 25.0f},
                                L"默认 3D 角度",
                                0,
                                0,
                                true});
            y += 34.0f;

            widgets_.push_back(
                {WidgetKind::StaticLabel, 0, {L + 18.0f, y, L + 82.0f, y + 24.0f}, L"黑洞大小", 0, 0, true});
            widgets_.push_back(
                {WidgetKind::Slider, 32, {L + 86.0f, y, L + W - 18.0f, y + 24.0f}, {}, 50, 200, true});
            y += 32.0f;

            widgets_.push_back(
                {WidgetKind::StaticLabel, 0, {L + 18.0f, y, L + 82.0f, y + 24.0f}, L"俯仰角度", 0, 0, true});
            widgets_.push_back(
                {WidgetKind::Slider, 33, {L + 86.0f, y, L + W - 68.0f, y + 24.0f}, {}, 0, 170, true});
            widgets_.push_back(
                {WidgetKind::ValueLabel, 36, {L + W - 60.0f, y, L + W - 14.0f, y + 24.0f}, {}, 0, 0, true});
            y += 32.0f;

            widgets_.push_back(
                {WidgetKind::StaticLabel, 0, {L + 18.0f, y, L + 82.0f, y + 24.0f}, L"倾斜角度", 0, 0, true});
            widgets_.push_back(
                {WidgetKind::Slider, 34, {L + 86.0f, y, L + W - 68.0f, y + 24.0f}, {}, 0, 360, true});
            widgets_.push_back(
                {WidgetKind::ValueLabel, 37, {L + W - 60.0f, y, L + W - 14.0f, y + 24.0f}, {}, 0, 0, true});
            y += 32.0f;
        } else if (currentMode_ == PlayMode::Billiards) {
            titleText_ = L"桌面台球 · 专属设置";
            widgets_.push_back(
                {WidgetKind::StaticLabel, 0, {L + 18.0f, y, L + 82.0f, y + 26.0f}, L"台球规则", 0, 0, true});
            widgets_.push_back(
                {WidgetKind::Popup, 41, {L + 86.0f, y, L + W - 18.0f, y + 26.0f}, {}, 0, 0, true});
            y += 34.0f;

            const int mIdx = readComboSel(42);
            widgets_.push_back(
                {WidgetKind::StaticLabel, 0, {L + 18.0f, y, L + 82.0f, y + 26.0f}, L"对局模式", 0, 0, true});
            if (mIdx == 1) {
                widgets_.push_back(
                    {WidgetKind::Popup, 42, {L + 86.0f, y, L + 278.0f, y + 26.0f}, {}, 0, 0, true});
                widgets_.push_back(
                    {WidgetKind::Popup, 60, {L + 286.0f, y, L + W - 18.0f, y + 26.0f}, {}, 0, 0, true});
            } else {
                widgets_.push_back(
                    {WidgetKind::Popup, 42, {L + 86.0f, y, L + W - 18.0f, y + 26.0f}, {}, 0, 0, true});
            }
            y += 34.0f;

            widgets_.push_back(
                {WidgetKind::StaticLabel, 0, {L + 18.0f, y, L + 82.0f, y + 26.0f}, L"球桌外观", 0, 0, true});
            widgets_.push_back(
                {WidgetKind::Popup, 45, {L + 86.0f, y, L + W - 18.0f, y + 26.0f}, {}, 0, 0, true});
            y += 33.0f;

            widgets_.push_back({WidgetKind::Checkbox,
                                43,
                                {L + 18.0f, y, L + 202.0f, y + 22.0f},
                                L"显示球边框/彩环",
                                0,
                                0,
                                true});
            widgets_.push_back({WidgetKind::Checkbox,
                                44,
                                {L + 210.0f, y, L + W - 18.0f, y + 22.0f},
                                L"球心显示球号",
                                0,
                                0,
                                true});
            y += 27.0f;

            widgets_.push_back({WidgetKind::Checkbox,
                                46,
                                {L + 18.0f, y, L + 202.0f, y + 22.0f},
                                L"瞄准折射辅助线",
                                0,
                                0,
                                true});
            if (mIdx == 1) {
                widgets_.push_back({WidgetKind::Checkbox,
                                    68,
                                    {L + 210.0f, y, L + W - 14.0f, y + 22.0f},
                                    L"显示人机战术意图提示",
                                    0,
                                    0,
                                    true});
            }
            y += 32.0f;

            const int iIdx = readComboSel(47);
            widgets_.push_back(
                {WidgetKind::StaticLabel, 0, {L + 18.0f, y, L + 82.0f, y + 26.0f}, L"出场图标", 0, 0, true});
            widgets_.push_back(
                {WidgetKind::Popup, 47, {L + 86.0f, y, L + W - 18.0f, y + 26.0f}, {}, 0, 0, true});
            y += 34.0f;

            widgets_.push_back(
                {WidgetKind::StaticLabel, 0, {L + 18.0f, y, L + 82.0f, y + 26.0f}, L"其余图标", 0, 0, true});
            widgets_.push_back(
                {WidgetKind::Popup, 49, {L + 86.0f, y, L + W - 18.0f, y + 26.0f}, {}, 0, 0, true});
            y += 34.0f;

            if (iIdx == 2) {
                appendIconPickerSection(L, W, y);
            }
            if (mIdx == 3) {
                appendLanSection(L, W, y);
            }
        } else if (currentMode_ == PlayMode::Slingshot) {
            titleText_ = L"弹弓攻城 · 专属设置";
            const std::wstring editTxt = readCtrlText(69);
            widgets_.push_back(
                {WidgetKind::StaticLabel, 0, {L + 18.0f, y, L + 82.0f, y + 26.0f}, L"攻城关卡", 0, 0, true});
            widgets_.push_back(
                {WidgetKind::Popup, 61, {L + 86.0f, y, L + 304.0f, y + 26.0f}, {}, 0, 0, true});
            widgets_.push_back({WidgetKind::AccentButton,
                                69,
                                {L + 312.0f, y, L + W - 18.0f, y + 26.0f},
                                editTxt.empty() ? L"关卡工坊" : editTxt,
                                0,
                                0,
                                true});
            y += 34.0f;

            widgets_.push_back(
                {WidgetKind::StaticLabel, 0, {L + 18.0f, y, L + 82.0f, y + 26.0f}, L"每关弹药", 0, 0, true});
            widgets_.push_back(
                {WidgetKind::Popup, 62, {L + 86.0f, y, L + 216.0f, y + 26.0f}, {}, 0, 0, true});
            widgets_.push_back({WidgetKind::Checkbox,
                                63,
                                {L + 226.0f, y + 2.0f, L + W - 14.0f, y + 24.0f},
                                L"显示抛物线弹道预测点",
                                0,
                                0,
                                true});
            y += 34.0f;

            const int iIdx = readComboSel(47);
            widgets_.push_back(
                {WidgetKind::StaticLabel, 0, {L + 18.0f, y, L + 82.0f, y + 26.0f}, L"出场图标", 0, 0, true});
            widgets_.push_back(
                {WidgetKind::Popup, 47, {L + 86.0f, y, L + W - 18.0f, y + 26.0f}, {}, 0, 0, true});
            y += 34.0f;

            if (iIdx == 2) {
                appendIconPickerSection(L, W, y);
            }
        } else {
            titleText_ = L"空气曲棍球 · 专属设置";
            const int hmIdx = readComboSel(64);
            widgets_.push_back(
                {WidgetKind::StaticLabel, 0, {L + 18.0f, y, L + 82.0f, y + 26.0f}, L"对局模式", 0, 0, true});
            if (hmIdx == 0) {
                widgets_.push_back(
                    {WidgetKind::Popup, 64, {L + 86.0f, y, L + 278.0f, y + 26.0f}, {}, 0, 0, true});
                widgets_.push_back(
                    {WidgetKind::Popup, 65, {L + 286.0f, y, L + W - 18.0f, y + 26.0f}, {}, 0, 0, true});
            } else {
                widgets_.push_back(
                    {WidgetKind::Popup, 64, {L + 86.0f, y, L + W - 18.0f, y + 26.0f}, {}, 0, 0, true});
            }
            y += 34.0f;

            widgets_.push_back(
                {WidgetKind::StaticLabel, 0, {L + 18.0f, y, L + 82.0f, y + 26.0f}, L"获胜条件", 0, 0, true});
            widgets_.push_back(
                {WidgetKind::Popup, 66, {L + 86.0f, y, L + 226.0f, y + 26.0f}, {}, 0, 0, true});
            widgets_.push_back({WidgetKind::Checkbox,
                                67,
                                {L + 236.0f, y + 2.0f, L + W - 14.0f, y + 24.0f},
                                L"显示霓虹圆角球台外框",
                                0,
                                0,
                                true});
            y += 34.0f;

            widgets_.push_back(
                {WidgetKind::StaticLabel, 0, {L + 18.0f, y, L + 82.0f, y + 26.0f}, L"其余图标", 0, 0, true});
            widgets_.push_back(
                {WidgetKind::Popup, 49, {L + 86.0f, y, L + W - 18.0f, y + 26.0f}, {}, 0, 0, true});
            y += 34.0f;

            if (hmIdx == 2) {
                appendLanSection(L, W, y);
            }
        }

        bodyH_ = (y - T) + 6.0f;
        totalW_ = bodyW_ + kShadowMarginDip * 2.0f;
        totalH_ = bodyH_ + kShadowMarginDip * 2.0f;
    }

    void appendIconPickerSection(float L, float W, float& y) noexcept {
        widgets_.push_back({WidgetKind::StaticLabel,
                            0,
                            {L + 18.0f, y, L + W - 18.0f, y + 18.0f},
                            L"自选出场图标（勾选后点“应用自选”）：",
                            0,
                            0,
                            true});
        y += 22.0f;
        widgets_.push_back(
            {WidgetKind::IconList, 56, {L + 18.0f, y, L + W - 18.0f, y + 116.0f}, {}, 0, 0, true});
        y += 122.0f;
        widgets_.push_back(
            {WidgetKind::Button, 57, {L + 18.0f, y, L + 106.0f, y + 26.0f}, L"全选", 0, 0, true});
        widgets_.push_back(
            {WidgetKind::Button, 58, {L + 114.0f, y, L + 202.0f, y + 26.0f}, L"清空", 0, 0, true});
        widgets_.push_back({WidgetKind::AccentButton,
                            59,
                            {L + 210.0f, y, L + W - 18.0f, y + 26.0f},
                            L"应用自选 / 换阵",
                            0,
                            0,
                            true});
        y += 34.0f;
    }

    void appendLanSection(float L, float W, float& y) noexcept {
        widgets_.push_back(
            {WidgetKind::StaticLabel, 0, {L + 18.0f, y, L + 82.0f, y + 26.0f}, L"联机地址", 0, 0, true});
        widgets_.push_back({WidgetKind::TextEdit, 51, {L + 86.0f, y, L + 212.0f, y + 26.0f}, {}, 0, 0, true});
        widgets_.push_back(
            {WidgetKind::Button, 52, {L + 216.0f, y, L + 262.0f, y + 26.0f}, L"建房", 0, 0, true});
        widgets_.push_back(
            {WidgetKind::AccentButton, 70, {L + 266.0f, y, L + 330.0f, y + 26.0f}, L"搜房加入", 0, 0, true});
        widgets_.push_back(
            {WidgetKind::Button, 53, {L + 334.0f, y, L + 372.0f, y + 26.0f}, L"连IP", 0, 0, true});
        widgets_.push_back(
            {WidgetKind::Button, 54, {L + 376.0f, y, L + W - 14.0f, y + 26.0f}, L"断开", 0, 0, true});
        y += 31.0f;

        const std::wstring lanSt = readCtrlText(55);
        widgets_.push_back({WidgetKind::StatusLabel,
                            55,
                            {L + 86.0f, y, L + W - 16.0f, y + 20.0f},
                            lanSt.empty() ? L"局域网未连接" : lanSt,
                            0,
                            0,
                            true});
        y += 24.0f;
    }

    int hitWidget(float dipX, float dipY) const noexcept {
        const float corner = launcherMode_ ? kLauncherCornerDip : kCardCornerDip;
        if (!pointInsideRoundedDip(dipX, dipY, bodyRectDip(), corner)) return -1;
        for (int i = static_cast<int>(widgets_.size()) - 1; i >= 0; --i) {
            const auto& wg = widgets_[static_cast<std::size_t>(i)];
            if (wg.kind == WidgetKind::StaticLabel || wg.kind == WidgetKind::ValueLabel ||
                wg.kind == WidgetKind::StatusLabel || wg.kind == WidgetKind::HintLabel) {
                continue;
            }
            if (dipX >= wg.rc.left && dipX < wg.rc.right && dipY >= wg.rc.top && dipY < wg.rc.bottom) {
                return wg.enabled ? i : -1;
            }
        }
        return -1;
    }

    void render() noexcept {
        if (!IsWindow(hwnd_)) return;
        glass_.commitLayered(hwnd_, winX_, winY_, totalW_, totalH_, dpi_, [&](MacGlassContext& g) {
            const auto body = bodyRectDip();
            const float corner = launcherMode_ ? kLauncherCornerDip : kCardCornerDip;
            g.drawMacHudGlass(body, corner, 0.93f);

            if (launcherMode_) {
                // Clean Commercial Launcher Header: 50x50 AppIcon + Vertically Centered Brand Title + Divider
                const D2D1_RECT_F iconRc{body.left + 26.0f, body.top + 14.0f, body.left + 76.0f,
                                         body.top + 64.0f};
                if (appIconBmp_) {
                    g.dcTarget->DrawBitmap(appIconBmp_.Get(), iconRc, 1.0f,
                                           D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                } else {
                    g.fillVerticalGradientRounded(D2D1::RoundedRect(iconRc, 12.0f, 12.0f),
                                                  D2D1::ColorF(0.10f, 0.22f, 0.46f, 0.96f),
                                                  D2D1::ColorF(0.04f, 0.10f, 0.24f, 0.98f));
                    const float icx = (iconRc.left + iconRc.right) * 0.5f;
                    const float icy = (iconRc.top + iconRc.bottom) * 0.5f;
                    g.brush->SetColor(D2D1::ColorF(0.28f, 0.72f, 1.0f, 0.95f));
                    g.dcTarget->DrawEllipse(D2D1::Ellipse({icx, icy}, 16.0f, 9.0f), g.brush.Get(), 2.0f);
                    g.dcTarget->FillEllipse(D2D1::Ellipse({icx, icy}, 5.5f, 5.5f), g.brush.Get());
                }
                if (brandTitleFmt_) {
                    const wchar_t* bt = L"桌面游乐场 · DeskFling";
                    g.brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.98f));
                    g.dcTarget->DrawTextW(bt, static_cast<UINT32>(std::wcslen(bt)), brandTitleFmt_.Get(),
                                          {body.left + 90.0f, body.top + 25.0f, body.right - 52.0f,
                                           body.top + 55.0f},
                                          g.brush.Get());
                }
                const float hDivY = std::floor(body.top + 76.0f) + 0.5f;
                g.brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f));
                g.dcTarget->DrawLine({body.left + 24.0f, hDivY}, {body.right - 24.0f, hDivY}, g.brush.Get(),
                                     1.0f);
            } else {
                // Header title & hairline divider (matching macOS modeSettingsPanel)
                if (titleFmt_ && !titleText_.empty()) {
                    g.brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.96f));
                    const D2D1_RECT_F trc{body.left + 18.0f, body.top + 10.0f, body.right - 46.0f,
                                          body.top + 32.0f};
                    g.dcTarget->DrawTextW(titleText_.c_str(), static_cast<UINT32>(titleText_.size()),
                                          titleFmt_.Get(), trc, g.brush.Get());
                }
                const float divY = std::floor(body.top + 40.0f) + 0.5f;
                g.brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f));
                g.dcTarget->DrawLine({body.left + 16.0f, divY}, {body.right - 16.0f, divY}, g.brush.Get(),
                                     1.0f);
            }

            for (std::size_t idx = 0; idx < widgets_.size(); ++idx) {
                const auto& wg = widgets_[idx];
                const bool hovered = (static_cast<int>(idx) == hoverIdx_) && wg.enabled;
                const bool pressed = (static_cast<int>(idx) == pressedIdx_) && hovered;

                switch (wg.kind) {
                    case WidgetKind::DragHeader:
                        break;

                    case WidgetKind::CloseBtn: {
                        if (hovered || pressed) {
                            g.brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, pressed ? 0.22f : 0.13f));
                            g.dcTarget->FillRoundedRectangle(D2D1::RoundedRect(wg.rc, 6.0f, 6.0f),
                                                             g.brush.Get());
                        }
                        const float cx = (wg.rc.left + wg.rc.right) * 0.5f;
                        const float cy = (wg.rc.top + wg.rc.bottom) * 0.5f;
                        g.brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, hovered ? 0.95f : 0.68f));
                        g.dcTarget->DrawLine({cx - 4.3f, cy - 4.3f}, {cx + 4.3f, cy + 4.3f}, g.brush.Get(),
                                             1.65f, g.roundStroke.Get());
                        g.dcTarget->DrawLine({cx + 4.3f, cy - 4.3f}, {cx - 4.3f, cy + 4.3f}, g.brush.Get(),
                                             1.65f, g.roundStroke.Get());
                        break;
                    }

                    case WidgetKind::ModePill: {
                        const bool selected = (readComboSel(wg.ctrlId) == wg.sliderMin);
                        const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(wg.rc, 9.5f, 9.5f);
                        if (selected) {
                            g.fillVerticalGradientRounded(rr, D2D1::ColorF(0.10f, 0.56f, 1.0f, 0.94f),
                                                          D2D1::ColorF(0.03f, 0.42f, 0.92f, 0.96f));
                            g.strokeVerticalGradientRounded(
                                D2D1::RoundedRect({wg.rc.left + 0.5f, wg.rc.top + 0.5f, wg.rc.right - 0.5f,
                                                   wg.rc.bottom - 0.5f},
                                                  9.0f, 9.0f),
                                D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.36f), D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f),
                                1.0f);
                        } else {
                            g.brush->SetColor(
                                D2D1::ColorF(1.0f, 1.0f, 1.0f, pressed ? 0.20f : (hovered ? 0.14f : 0.08f)));
                            g.dcTarget->FillRoundedRectangle(rr, g.brush.Get());
                            g.brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f));
                            g.dcTarget->DrawRoundedRectangle(
                                D2D1::RoundedRect({wg.rc.left + 0.5f, wg.rc.top + 0.5f, wg.rc.right - 0.5f,
                                                   wg.rc.bottom - 0.5f},
                                                  9.0f, 9.0f),
                                g.brush.Get(), 1.0f);
                        }
                        if (ctrlCenterFmt_ && !wg.text.empty()) {
                            g.brush->SetColor(selected ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f)
                                                       : D2D1::ColorF(0.86f, 0.88f, 0.92f, 1.0f));
                            g.dcTarget->DrawTextW(wg.text.c_str(), static_cast<UINT32>(wg.text.size()),
                                                  ctrlCenterFmt_.Get(), wg.rc, g.brush.Get());
                        }
                        break;
                    }

                    case WidgetKind::PrimaryButton:
                    case WidgetKind::AccentButton: {
                        const float rad = (wg.kind == WidgetKind::PrimaryButton) ? 12.0f : 6.5f;
                        const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(wg.rc, rad, rad);
                        if (!wg.enabled) {
                            g.brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f));
                            g.dcTarget->FillRoundedRectangle(rr, g.brush.Get());
                        } else {
                            const float boost = pressed ? -0.07f : (hovered ? 0.06f : 0.0f);
                            g.fillVerticalGradientRounded(
                                rr,
                                D2D1::ColorF(std::clamp(0.11f + boost, 0.0f, 1.0f),
                                             std::clamp(0.56f + boost, 0.0f, 1.0f), 1.0f, 0.96f),
                                D2D1::ColorF(std::clamp(0.02f + boost, 0.0f, 1.0f),
                                             std::clamp(0.42f + boost, 0.0f, 1.0f),
                                             std::clamp(0.92f + boost, 0.0f, 1.0f), 0.98f));
                            g.strokeVerticalGradientRounded(
                                D2D1::RoundedRect({wg.rc.left + 0.5f, wg.rc.top + 0.5f, wg.rc.right - 0.5f,
                                                   wg.rc.bottom - 0.5f},
                                                  rad - 0.5f, rad - 0.5f),
                                D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.34f), D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f),
                                1.0f);
                        }
                        IDWriteTextFormat* btnFmt =
                            (wg.kind == WidgetKind::PrimaryButton && ctaFmt_) ? ctaFmt_.Get()
                                                                              : ctrlCenterFmt_.Get();
                        if (btnFmt && !wg.text.empty()) {
                            g.brush->SetColor(wg.enabled ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f)
                                                         : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.35f));
                            g.dcTarget->DrawTextW(wg.text.c_str(), static_cast<UINT32>(wg.text.size()),
                                                  btnFmt, wg.rc, g.brush.Get());
                        }
                        break;
                    }

                    case WidgetKind::Button:
                    case WidgetKind::DangerButton: {
                        const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(wg.rc, 6.5f, 6.5f);
                        if (!wg.enabled) {
                            g.brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.05f));
                            g.dcTarget->FillRoundedRectangle(rr, g.brush.Get());
                        } else if (wg.kind == WidgetKind::DangerButton) {
                            g.brush->SetColor(D2D1::ColorF(1.0f, 0.34f, 0.31f,
                                                           pressed ? 0.30f : (hovered ? 0.22f : 0.14f)));
                            g.dcTarget->FillRoundedRectangle(rr, g.brush.Get());
                            g.brush->SetColor(D2D1::ColorF(1.0f, 0.48f, 0.45f, 0.26f));
                            g.dcTarget->DrawRoundedRectangle(
                                D2D1::RoundedRect({wg.rc.left + 0.5f, wg.rc.top + 0.5f, wg.rc.right - 0.5f,
                                                   wg.rc.bottom - 0.5f},
                                                  6.0f, 6.0f),
                                g.brush.Get(), 1.0f);
                        } else {
                            g.brush->SetColor(
                                D2D1::ColorF(1.0f, 1.0f, 1.0f, pressed ? 0.24f : (hovered ? 0.17f : 0.11f)));
                            g.dcTarget->FillRoundedRectangle(rr, g.brush.Get());
                            g.strokeVerticalGradientRounded(
                                D2D1::RoundedRect({wg.rc.left + 0.5f, wg.rc.top + 0.5f, wg.rc.right - 0.5f,
                                                   wg.rc.bottom - 0.5f},
                                                  6.0f, 6.0f),
                                D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.20f), D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f),
                                1.0f);
                        }
                        if (ctrlCenterFmt_ && !wg.text.empty()) {
                            D2D1_COLOR_F tc = D2D1::ColorF(0.95f, 0.96f, 0.98f, 1.0f);
                            if (!wg.enabled) {
                                tc = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.30f);
                            } else if (wg.kind == WidgetKind::DangerButton) {
                                tc = D2D1::ColorF(1.0f, 0.55f, 0.52f, 1.0f);
                            }
                            g.brush->SetColor(tc);
                            g.dcTarget->DrawTextW(wg.text.c_str(), static_cast<UINT32>(wg.text.size()),
                                                  ctrlCenterFmt_.Get(), wg.rc, g.brush.Get());
                        }
                        break;
                    }

                    case WidgetKind::Popup: {
                        // macOS NSPopUpButton look: dark rounded body + blue dual-chevron badge on the right
                        const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(wg.rc, 6.5f, 6.5f);
                        g.brush->SetColor(
                            D2D1::ColorF(1.0f, 1.0f, 1.0f, pressed ? 0.20f : (hovered ? 0.15f : 0.10f)));
                        g.dcTarget->FillRoundedRectangle(rr, g.brush.Get());
                        g.strokeVerticalGradientRounded(
                            D2D1::RoundedRect({wg.rc.left + 0.5f, wg.rc.top + 0.5f, wg.rc.right - 0.5f,
                                               wg.rc.bottom - 0.5f},
                                              6.0f, 6.0f),
                            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.20f), D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.09f),
                            1.0f);

                        // Right-side macOS #0A84FF dual-chevron pill badge
                        const D2D1_RECT_F badgeRc{wg.rc.right - 21.0f, wg.rc.top + 3.0f, wg.rc.right - 3.0f,
                                                  wg.rc.bottom - 3.0f};
                        g.fillVerticalGradientRounded(D2D1::RoundedRect(badgeRc, 4.5f, 4.5f),
                                                      D2D1::ColorF(0.10f, 0.56f, 1.0f, 0.95f),
                                                      D2D1::ColorF(0.02f, 0.44f, 0.94f, 0.95f));
                        const float bcx = (badgeRc.left + badgeRc.right) * 0.5f;
                        const float bcy = (badgeRc.top + badgeRc.bottom) * 0.5f;
                        g.brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.96f));
                        // Up chevron
                        g.dcTarget->DrawLine({bcx - 3.0f, bcy - 1.4f}, {bcx, bcy - 4.2f}, g.brush.Get(), 1.4f,
                                             g.roundStroke.Get());
                        g.dcTarget->DrawLine({bcx, bcy - 4.2f}, {bcx + 3.0f, bcy - 1.4f}, g.brush.Get(), 1.4f,
                                             g.roundStroke.Get());
                        // Down chevron
                        g.dcTarget->DrawLine({bcx - 3.0f, bcy + 1.4f}, {bcx, bcy + 4.2f}, g.brush.Get(), 1.4f,
                                             g.roundStroke.Get());
                        g.dcTarget->DrawLine({bcx, bcy + 4.2f}, {bcx + 3.0f, bcy + 1.4f}, g.brush.Get(), 1.4f,
                                             g.roundStroke.Get());

                        const std::wstring curTxt = readComboCurrentText(wg.ctrlId);
                        if (ctrlFmt_ && !curTxt.empty()) {
                            g.brush->SetColor(D2D1::ColorF(0.96f, 0.97f, 0.99f, 1.0f));
                            const D2D1_RECT_F trc{wg.rc.left + 10.0f, wg.rc.top, badgeRc.left - 5.0f,
                                                  wg.rc.bottom};
                            g.dcTarget->DrawTextW(curTxt.c_str(), static_cast<UINT32>(curTxt.size()),
                                                  ctrlFmt_.Get(), trc, g.brush.Get());
                        }
                        break;
                    }

                    case WidgetKind::Checkbox: {
                        const bool checked = readCheckbox(wg.ctrlId);
                        const float cy = (wg.rc.top + wg.rc.bottom) * 0.5f;
                        const D2D1_RECT_F box{wg.rc.left, cy - 8.0f, wg.rc.left + 16.0f, cy + 8.0f};
                        const D2D1_ROUNDED_RECT boxRr = D2D1::RoundedRect(box, 4.5f, 4.5f);
                        if (checked) {
                            g.fillVerticalGradientRounded(boxRr, D2D1::ColorF(0.12f, 0.58f, 1.0f, 0.98f),
                                                          D2D1::ColorF(0.02f, 0.44f, 0.94f, 0.98f));
                            const float bcx = (box.left + box.right) * 0.5f;
                            const float bcy = (box.top + box.bottom) * 0.5f + 0.4f;
                            g.brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));
                            g.dcTarget->DrawLine({bcx - 4.0f, bcy + 0.1f}, {bcx - 1.1f, bcy + 3.2f},
                                                 g.brush.Get(), 1.85f, g.roundStroke.Get());
                            g.dcTarget->DrawLine({bcx - 1.1f, bcy + 3.2f}, {bcx + 4.3f, bcy - 3.4f},
                                                 g.brush.Get(), 1.85f, g.roundStroke.Get());
                        } else {
                            g.brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, hovered ? 0.14f : 0.08f));
                            g.dcTarget->FillRoundedRectangle(boxRr, g.brush.Get());
                            g.brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, hovered ? 0.36f : 0.24f));
                            g.dcTarget->DrawRoundedRectangle(
                                D2D1::RoundedRect(
                                    {box.left + 0.5f, box.top + 0.5f, box.right - 0.5f, box.bottom - 0.5f},
                                    4.0f, 4.0f),
                                g.brush.Get(), 1.0f);
                        }
                        if (ctrlFmt_ && !wg.text.empty()) {
                            g.brush->SetColor(D2D1::ColorF(0.93f, 0.94f, 0.97f, hovered ? 1.0f : 0.92f));
                            const D2D1_RECT_F trc{box.right + 7.0f, wg.rc.top, wg.rc.right, wg.rc.bottom};
                            g.dcTarget->DrawTextW(wg.text.c_str(), static_cast<UINT32>(wg.text.size()),
                                                  ctrlFmt_.Get(), trc, g.brush.Get());
                        }
                        break;
                    }

                    case WidgetKind::Slider: {
                        const int pos = std::clamp(readSliderPos(wg.ctrlId), wg.sliderMin, wg.sliderMax);
                        const float span = static_cast<float>(std::max(1, wg.sliderMax - wg.sliderMin));
                        const float frac =
                            std::clamp(static_cast<float>(pos - wg.sliderMin) / span, 0.0f, 1.0f);
                        const float cy = (wg.rc.top + wg.rc.bottom) * 0.5f;
                        const float trackL = wg.rc.left + 8.0f;
                        const float trackR = wg.rc.right - 8.0f;
                        const float thumbX = trackL + frac * (trackR - trackL);

                        // Background groove
                        const D2D1_RECT_F groove{trackL, cy - 2.2f, trackR, cy + 2.2f};
                        g.brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f));
                        g.dcTarget->FillRoundedRectangle(D2D1::RoundedRect(groove, 2.2f, 2.2f),
                                                         g.brush.Get());

                        // Active macOS blue fill
                        if (thumbX > trackL + 0.5f) {
                            const D2D1_RECT_F fill{trackL, cy - 2.2f, thumbX, cy + 2.2f};
                            g.brush->SetColor(D2D1::ColorF(0.06f, 0.53f, 1.0f, 0.96f));
                            g.dcTarget->FillRoundedRectangle(D2D1::RoundedRect(fill, 2.2f, 2.2f),
                                                             g.brush.Get());
                        }

                        // Circular white macOS slider knob + soft shadow
                        const float kr = (activeSliderIdx_ == static_cast<int>(idx) || hovered) ? 8.2f : 7.5f;
                        g.brush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.36f));
                        g.dcTarget->FillEllipse(D2D1::Ellipse({thumbX, cy + 1.2f}, kr, kr), g.brush.Get());
                        g.brush->SetColor(D2D1::ColorF(0.99f, 0.99f, 1.0f, 1.0f));
                        g.dcTarget->FillEllipse(D2D1::Ellipse({thumbX, cy}, kr, kr), g.brush.Get());
                        break;
                    }

                    case WidgetKind::IconList: {
                        drawIconListWidget(g, wg);
                        break;
                    }

                    case WidgetKind::TextEdit: {
                        const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(wg.rc, 6.0f, 6.0f);
                        g.brush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.36f));
                        g.dcTarget->FillRoundedRectangle(rr, g.brush.Get());
                        if (editFocused_) {
                            g.brush->SetColor(D2D1::ColorF(0.08f, 0.54f, 1.0f, 0.92f));
                            g.dcTarget->DrawRoundedRectangle(
                                D2D1::RoundedRect({wg.rc.left + 0.75f, wg.rc.top + 0.75f, wg.rc.right - 0.75f,
                                                   wg.rc.bottom - 0.75f},
                                                  5.5f, 5.5f),
                                g.brush.Get(), 1.5f);
                        } else {
                            g.brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, hovered ? 0.28f : 0.16f));
                            g.dcTarget->DrawRoundedRectangle(
                                D2D1::RoundedRect({wg.rc.left + 0.5f, wg.rc.top + 0.5f, wg.rc.right - 0.5f,
                                                   wg.rc.bottom - 0.5f},
                                                  5.5f, 5.5f),
                                g.brush.Get(), 1.0f);
                        }

                        const std::wstring shown = editBuffer_.empty()
                                                       ? std::wstring(L"房主IP:39457")
                                                       : (editFocused_ ? (editBuffer_ + L"|") : editBuffer_);
                        if (monoFmt_) {
                            g.brush->SetColor(editBuffer_.empty() && !editFocused_
                                                  ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.35f)
                                                  : D2D1::ColorF(0.97f, 0.98f, 1.0f, 1.0f));
                            const D2D1_RECT_F trc{wg.rc.left + 8.0f, wg.rc.top, wg.rc.right - 6.0f,
                                                  wg.rc.bottom};
                            g.dcTarget->DrawTextW(shown.c_str(), static_cast<UINT32>(shown.size()),
                                                  monoFmt_.Get(), trc, g.brush.Get());
                        }
                        break;
                    }

                    case WidgetKind::StaticLabel: {
                        if (labelFmt_ && !wg.text.empty()) {
                            g.brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.88f));
                            g.dcTarget->DrawTextW(wg.text.c_str(), static_cast<UINT32>(wg.text.size()),
                                                  labelFmt_.Get(), wg.rc, g.brush.Get());
                        }
                        break;
                    }

                    case WidgetKind::ValueLabel: {
                        const std::wstring val = readCtrlText(wg.ctrlId);
                        if (monoFmt_ && !val.empty()) {
                            g.brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.86f));
                            g.dcTarget->DrawTextW(val.c_str(), static_cast<UINT32>(val.size()),
                                                  monoFmt_.Get(), wg.rc, g.brush.Get());
                        }
                        break;
                    }

                    case WidgetKind::StatusLabel: {
                        const std::wstring st = wg.ctrlId ? readCtrlText(wg.ctrlId) : wg.text;
                        if (hintFmt_ && !st.empty()) {
                            g.brush->SetColor(launcherMode_ ? D2D1::ColorF(0.62f, 0.94f, 0.76f, 0.94f)
                                                            : D2D1::ColorF(0.76f, 0.82f, 0.92f, 0.88f));
                            g.dcTarget->DrawTextW(st.c_str(), static_cast<UINT32>(st.size()), hintFmt_.Get(),
                                                  wg.rc, g.brush.Get());
                        }
                        break;
                    }

                    case WidgetKind::HintLabel: {
                        if (hintFmt_ && !wg.text.empty()) {
                            g.brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.66f));
                            g.dcTarget->DrawTextW(wg.text.c_str(), static_cast<UINT32>(wg.text.size()),
                                                  hintFmt_.Get(), wg.rc, g.brush.Get());
                        }
                        break;
                    }
                }
            }
        });
    }

    void drawIconListWidget(MacGlassContext& g, const Widget& wg) noexcept {
        const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(wg.rc, 8.0f, 8.0f);
        g.brush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.30f));
        g.dcTarget->FillRoundedRectangle(rr, g.brush.Get());
        g.brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f));
        g.dcTarget->DrawRoundedRectangle(
            D2D1::RoundedRect({wg.rc.left + 0.5f, wg.rc.top + 0.5f, wg.rc.right - 0.5f, wg.rc.bottom - 0.5f},
                              7.5f, 7.5f),
            g.brush.Get(), 1.0f);

        if (!IsWindow(backingPanel_)) return;
        HWND lb = GetDlgItem(backingPanel_, wg.ctrlId);
        if (!lb) return;
        const int count = static_cast<int>(SendMessageW(lb, LB_GETCOUNT, 0, 0));
        if (count <= 0) return;

        constexpr float kRowH = 23.0f;
        const float innerH = (wg.rc.bottom - wg.rc.top) - 6.0f;
        const int visibleRows = std::max(1, static_cast<int>(innerH / kRowH));
        const int maxScroll = std::max(0, count - visibleRows);
        iconListScroll_ = std::clamp(iconListScroll_, 0, maxScroll);

        g.dcTarget->PushAxisAlignedClip(
            {wg.rc.left + 2.0f, wg.rc.top + 3.0f, wg.rc.right - 2.0f, wg.rc.bottom - 3.0f},
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        for (int vis = 0; vis < visibleRows + 1; ++vis) {
            const int itemIdx = iconListScroll_ + vis;
            if (itemIdx >= count) break;
            const float ry = wg.rc.top + 3.0f + static_cast<float>(vis) * kRowH;
            const bool selected = SendMessageW(lb, LB_GETSEL, static_cast<WPARAM>(itemIdx), 0) > 0;

            if (itemIdx == hoverListRow_) {
                g.brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f));
                g.dcTarget->FillRoundedRectangle(
                    D2D1::RoundedRect({wg.rc.left + 4.0f, ry + 1.0f, wg.rc.right - 12.0f, ry + kRowH - 1.0f},
                                      4.5f, 4.5f),
                    g.brush.Get());
            }

            const float cy = ry + kRowH * 0.5f;
            const D2D1_RECT_F box{wg.rc.left + 10.0f, cy - 6.5f, wg.rc.left + 23.0f, cy + 6.5f};
            if (selected) {
                g.brush->SetColor(D2D1::ColorF(0.08f, 0.54f, 1.0f, 0.96f));
                g.dcTarget->FillRoundedRectangle(D2D1::RoundedRect(box, 3.5f, 3.5f), g.brush.Get());
                const float bcx = (box.left + box.right) * 0.5f;
                const float bcy = (box.top + box.bottom) * 0.5f + 0.3f;
                g.brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));
                g.dcTarget->DrawLine({bcx - 3.2f, bcy}, {bcx - 0.9f, bcy + 2.5f}, g.brush.Get(), 1.6f,
                                     g.roundStroke.Get());
                g.dcTarget->DrawLine({bcx - 0.9f, bcy + 2.5f}, {bcx + 3.4f, bcy - 2.6f}, g.brush.Get(), 1.6f,
                                     g.roundStroke.Get());
            } else {
                g.brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.22f));
                g.dcTarget->DrawRoundedRectangle(D2D1::RoundedRect(box, 3.5f, 3.5f), g.brush.Get(), 1.0f);
            }

            const LRESULT tlen = SendMessageW(lb, LB_GETTEXTLEN, static_cast<WPARAM>(itemIdx), 0);
            if (tlen > 0 && tlen <= 256 && ctrlFmt_) {
                std::wstring rowStr(static_cast<std::size_t>(tlen) + 1u, L'\0');
                SendMessageW(lb, LB_GETTEXT, static_cast<WPARAM>(itemIdx),
                             reinterpret_cast<LPARAM>(rowStr.data()));
                rowStr.resize(std::wcslen(rowStr.c_str()));
                g.brush->SetColor(selected ? D2D1::ColorF(0.98f, 0.99f, 1.0f, 1.0f)
                                           : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.78f));
                g.dcTarget->DrawTextW(rowStr.c_str(), static_cast<UINT32>(rowStr.size()), ctrlFmt_.Get(),
                                      {box.right + 7.0f, ry, wg.rc.right - 14.0f, ry + kRowH}, g.brush.Get());
            }
        }

        g.dcTarget->PopAxisAlignedClip();

        // Thin macOS dark scrollbar thumb
        if (count > visibleRows) {
            const float trackTop = wg.rc.top + 5.0f;
            const float trackBot = wg.rc.bottom - 5.0f;
            const float trackLen = trackBot - trackTop;
            const float thumbLen =
                std::max(18.0f, trackLen * (static_cast<float>(visibleRows) / static_cast<float>(count)));
            const float thumbTop = trackTop + (trackLen - thumbLen) * (static_cast<float>(iconListScroll_) /
                                                                       static_cast<float>(maxScroll));
            g.brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.28f));
            g.dcTarget->FillRoundedRectangle(
                D2D1::RoundedRect({wg.rc.right - 7.5f, thumbTop, wg.rc.right - 3.5f, thumbTop + thumbLen},
                                  2.0f, 2.0f),
                g.brush.Get());
        }
    }

    void updateSliderAt(int widgetIdx, float dipX) noexcept {
        if (widgetIdx < 0 || widgetIdx >= static_cast<int>(widgets_.size()) || !IsWindow(backingPanel_))
            return;
        const auto& wg = widgets_[static_cast<std::size_t>(widgetIdx)];
        if (wg.kind != WidgetKind::Slider) return;
        HWND tb = GetDlgItem(backingPanel_, wg.ctrlId);
        if (!tb) return;

        const float trackL = wg.rc.left + 8.0f;
        const float trackR = std::max(trackL + 1.0f, wg.rc.right - 8.0f);
        const float frac = std::clamp((dipX - trackL) / (trackR - trackL), 0.0f, 1.0f);
        const int newPos =
            wg.sliderMin +
            static_cast<int>(std::lround(frac * static_cast<float>(wg.sliderMax - wg.sliderMin)));
        SendMessageW(tb, TBM_SETPOS, TRUE, newPos);
        SendMessageW(backingPanel_, WM_HSCROLL, MAKEWPARAM(TB_THUMBTRACK, newPos),
                     reinterpret_cast<LPARAM>(tb));
        render();
    }

    void openComboDropdown(const Widget& wg) noexcept {
        if (!IsWindow(backingPanel_)) return;
        HWND cb = GetDlgItem(backingPanel_, wg.ctrlId);
        if (!cb) return;
        const int cnt = static_cast<int>(SendMessageW(cb, CB_GETCOUNT, 0, 0));
        if (cnt <= 0) return;
        const int curSel = readComboSel(wg.ctrlId);

        std::vector<DarkMenuEntry> entries;
        entries.reserve(static_cast<std::size_t>(cnt));
        for (int i = 0; i < cnt; ++i) {
            const LRESULT len = SendMessageW(cb, CB_GETLBTEXTLEN, static_cast<WPARAM>(i), 0);
            std::wstring txt;
            if (len > 0 && len <= 256) {
                txt.assign(static_cast<std::size_t>(len) + 1u, L'\0');
                SendMessageW(cb, CB_GETLBTEXT, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(txt.data()));
                txt.resize(std::wcslen(txt.c_str()));
            }
            entries.push_back({i, std::move(txt), i == curSel, true, false, false});
        }

        const float s = static_cast<float>(dpi_) / 96.0f;
        const int anchorX = winX_ + static_cast<int>(std::lround(wg.rc.left * s));
        const int anchorY = winY_ + static_cast<int>(std::lround((wg.rc.bottom + 3.0f) * s));
        const float minW = wg.rc.right - wg.rc.left;

        closingDropdown_ = true;
        const int picked = dropdown_.showModal(hwnd_, anchorX, anchorY, dpi_, entries, minW);
        if (picked >= 0 && picked < cnt && picked != curSel && IsWindow(backingPanel_)) {
            SendMessageW(cb, CB_SETCURSEL, static_cast<WPARAM>(picked), 0);
            SendMessageW(backingPanel_, WM_COMMAND, MAKEWPARAM(wg.ctrlId, CBN_SELCHANGE),
                         reinterpret_cast<LPARAM>(cb));
            refresh(hasLayoutBackup_);
        } else {
            render();
        }
        closingDropdown_ = false;
    }

    void activateWidget(int widgetIdx, float dipX, float dipY) noexcept {
        if (widgetIdx < 0 || widgetIdx >= static_cast<int>(widgets_.size())) return;
        const Widget wg = widgets_[static_cast<std::size_t>(widgetIdx)];
        if (!wg.enabled) return;

        if (wg.kind == WidgetKind::CloseBtn) {
            if (wg.ctrlId == 11 && IsWindow(backingPanel_)) {
                PostMessageW(backingPanel_, WM_COMMAND, 11, 0);
            } else {
                hide();
                if (onCloseCard) onCloseCard();
            }
            return;
        }

        if (wg.kind == WidgetKind::Popup) {
            openComboDropdown(wg);
            return;
        }

        if (wg.kind == WidgetKind::ModePill && IsWindow(backingPanel_)) {
            if (HWND cb = GetDlgItem(backingPanel_, wg.ctrlId)) {
                SendMessageW(cb, CB_SETCURSEL, static_cast<WPARAM>(wg.sliderMin), 0);
                SendMessageW(backingPanel_, WM_COMMAND, MAKEWPARAM(wg.ctrlId, CBN_SELCHANGE),
                             reinterpret_cast<LPARAM>(cb));
                render();
            }
            return;
        }

        if (wg.kind == WidgetKind::Checkbox && IsWindow(backingPanel_)) {
            if (HWND cb = GetDlgItem(backingPanel_, wg.ctrlId)) {
                const bool next = !readCheckbox(wg.ctrlId);
                SendMessageW(cb, BM_SETCHECK, next ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(backingPanel_, WM_COMMAND, MAKEWPARAM(wg.ctrlId, BN_CLICKED),
                             reinterpret_cast<LPARAM>(cb));
                refresh(hasLayoutBackup_);
            }
            return;
        }

        if ((wg.kind == WidgetKind::PrimaryButton || wg.kind == WidgetKind::AccentButton ||
             wg.kind == WidgetKind::Button || wg.kind == WidgetKind::DangerButton) &&
            IsWindow(backingPanel_)) {
            if (wg.ctrlId == 52 || wg.ctrlId == 53 || wg.ctrlId == 70) {
                pushEditBufferToBacking();
            }
            HWND btn = GetDlgItem(backingPanel_, wg.ctrlId);
            if (wg.ctrlId == 20 || wg.ctrlId == 11 || wg.ctrlId == 71 || wg.ctrlId == 72) {
                // Post potentially modal or session-transition commands cleanly after mouse-up
                PostMessageW(backingPanel_, WM_COMMAND, MAKEWPARAM(wg.ctrlId, BN_CLICKED),
                             reinterpret_cast<LPARAM>(btn));
            } else {
                SendMessageW(backingPanel_, WM_COMMAND, MAKEWPARAM(wg.ctrlId, BN_CLICKED),
                             reinterpret_cast<LPARAM>(btn));
                refresh(hasLayoutBackup_);
            }
            return;
        }

        if (wg.kind == WidgetKind::IconList && IsWindow(backingPanel_)) {
            HWND lb = GetDlgItem(backingPanel_, wg.ctrlId);
            if (!lb) return;
            const int count = static_cast<int>(SendMessageW(lb, LB_GETCOUNT, 0, 0));
            constexpr float kRowH = 23.0f;
            const int row = iconListScroll_ + static_cast<int>((dipY - (wg.rc.top + 3.0f)) / kRowH);
            if (dipX < wg.rc.right - 10.0f && row >= 0 && row < count) {
                const bool cur = SendMessageW(lb, LB_GETSEL, static_cast<WPARAM>(row), 0) > 0;
                SendMessageW(lb, LB_SETSEL, cur ? FALSE : TRUE, static_cast<LPARAM>(row));
                render();
            }
        }
    }

    static LRESULT CALLBACK wndProc(HWND w, UINT m, WPARAM wp, LPARAM lp) noexcept {
        auto* self = reinterpret_cast<WinModeSettingsCard*>(GetWindowLongPtrW(w, GWLP_USERDATA));
        if (m == WM_NCCREATE) {
            self = static_cast<WinModeSettingsCard*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(w, m, wp, lp);

        if (m == WM_ERASEBKGND) return 1;
        if (m == WM_CLOSE) {
            if (self->launcherMode_ && IsWindow(self->backingPanel_)) {
                PostMessageW(self->backingPanel_, WM_COMMAND, 11, 0);
            } else {
                self->hide();
                if (self->onCloseCard) self->onCloseCard();
            }
            return 0;
        }
        if (m == WM_NCHITTEST) {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(w, &pt);
            const float inv = 96.0f / static_cast<float>(self->dpi_ ? self->dpi_ : 96u);
            const float corner = self->launcherMode_ ? kLauncherCornerDip : kCardCornerDip;
            if (!pointInsideRoundedDip(static_cast<float>(pt.x) * inv, static_cast<float>(pt.y) * inv,
                                       self->bodyRectDip(), corner)) {
                return HTTRANSPARENT;
            }
            return HTCLIENT;
        }
        if (m == WM_MOUSEACTIVATE) {
            if (self->launcherMode_) return MA_ACTIVATE;
            // Only activate key focus when clicking the inline LAN IP text box;
            // all other clicks stay non-activating so desktop/game focus stays effortless!
            POINT pt{};
            GetCursorPos(&pt);
            ScreenToClient(w, &pt);
            const float inv = 96.0f / static_cast<float>(self->dpi_ ? self->dpi_ : 96u);
            const int hit = self->hitWidget(static_cast<float>(pt.x) * inv, static_cast<float>(pt.y) * inv);
            if (hit >= 0 && self->widgets_[static_cast<std::size_t>(hit)].kind == WidgetKind::TextEdit) {
                return MA_ACTIVATE;
            }
            return MA_NOACTIVATE;
        }
        if (m == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE) {
            if (self->editFocused_) {
                self->editFocused_ = false;
                self->pushEditBufferToBacking();
                self->render();
            }
            // Do NOT auto-hide the card on focus loss (e.g. Windows Firewall popup when enabling LAN,
            // dropdown menu close, or copying an IP address from another app). Clicking the playground
            // canvas, pressing Esc, or clicking Close / Settings cleanly dismisses the card.
            return 0;
        }
        if (m == WM_PAINT) {
            PAINTSTRUCT ps{};
            BeginPaint(w, &ps);
            EndPaint(w, &ps);
            self->render();
            return 0;
        }
        if (m == WM_MOUSEWHEEL) {
            const int delta = GET_WHEEL_DELTA_WPARAM(wp);
            for (const auto& wg : self->widgets_) {
                if (wg.kind == WidgetKind::IconList) {
                    self->iconListScroll_ = std::max(0, self->iconListScroll_ - (delta > 0 ? 2 : -2));
                    self->render();
                    return 0;
                }
            }
            return 0;
        }
        if (m == WM_KEYDOWN) {
            if (wp == VK_ESCAPE) {
                if (self->launcherMode_ && IsWindow(self->backingPanel_)) {
                    PostMessageW(self->backingPanel_, WM_COMMAND, 11, 0);
                } else {
                    self->hide();
                    if (self->onCloseCard) self->onCloseCard();
                }
                return 0;
            }
            if (wp == VK_RETURN && self->launcherMode_ && !self->editFocused_ &&
                IsWindow(self->backingPanel_) && self->isCtrlEnabled(20)) {
                HWND btn = GetDlgItem(self->backingPanel_, 20);
                PostMessageW(self->backingPanel_, WM_COMMAND, MAKEWPARAM(20, BN_CLICKED),
                             reinterpret_cast<LPARAM>(btn));
                return 0;
            }
            if (self->editFocused_) {
                if (wp == VK_BACK) {
                    if (!self->editBuffer_.empty()) self->editBuffer_.pop_back();
                    self->pushEditBufferToBacking();
                    self->render();
                    return 0;
                }
                if (wp == VK_DELETE) {
                    self->editBuffer_.clear();
                    self->pushEditBufferToBacking();
                    self->render();
                    return 0;
                }
                if (wp == VK_RETURN && IsWindow(self->backingPanel_)) {
                    self->pushEditBufferToBacking();
                    HWND btn = GetDlgItem(self->backingPanel_, 53);
                    SendMessageW(self->backingPanel_, WM_COMMAND, MAKEWPARAM(53, BN_CLICKED),
                                 reinterpret_cast<LPARAM>(btn));
                    self->refresh(self->hasLayoutBackup_);
                    return 0;
                }
                if (wp == 'V' && (GetKeyState(VK_CONTROL) & 0x8000) != 0) {
                    if (OpenClipboard(w)) {
                        if (HANDLE hData = GetClipboardData(CF_UNICODETEXT)) {
                            if (const auto* wch = static_cast<const wchar_t*>(GlobalLock(hData))) {
                                for (std::size_t i = 0; wch[i] != L'\0' && self->editBuffer_.size() < 96;
                                     ++i) {
                                    if (wch[i] >= 32 && wch[i] != 127) self->editBuffer_.push_back(wch[i]);
                                }
                                GlobalUnlock(hData);
                            }
                        }
                        CloseClipboard();
                        self->pushEditBufferToBacking();
                        self->render();
                    }
                    return 0;
                }
            }
            return 0;
        }
        if (m == WM_CHAR && self->editFocused_) {
            const wchar_t ch = static_cast<wchar_t>(wp);
            if (ch >= 32 && ch != 127 && !(GetKeyState(VK_CONTROL) & 0x8000) &&
                self->editBuffer_.size() < 96) {
                self->editBuffer_.push_back(ch);
                self->pushEditBufferToBacking();
                self->render();
            }
            return 0;
        }
        if (m == WM_MOUSEMOVE) {
            const float inv = 96.0f / static_cast<float>(self->dpi_ ? self->dpi_ : 96u);
            const float dipX = static_cast<float>(GET_X_LPARAM(lp)) * inv;
            const float dipY = static_cast<float>(GET_Y_LPARAM(lp)) * inv;

            if (self->draggingCard_) {
                POINT cur{};
                GetCursorPos(&cur);
                const float s = static_cast<float>(self->dpi_ ? self->dpi_ : 96u) / 96.0f;
                const int marginPx = self->shadowMarginPx();
                const int bodyPxW = static_cast<int>(std::lround(self->bodyW_ * s));
                const int bodyPxH = static_cast<int>(std::lround(self->bodyH_ * s));
                const int rawBodyX =
                    static_cast<int>(self->dragStartWin_.x + marginPx + (cur.x - self->dragStartCursor_.x));
                const int rawBodyY =
                    static_cast<int>(self->dragStartWin_.y + marginPx + (cur.y - self->dragStartCursor_.y));
                const int clampedX =
                    std::clamp<int>(rawBodyX, static_cast<int>(self->workArea_.left + 6),
                                    std::max<int>(static_cast<int>(self->workArea_.left + 6),
                                                  static_cast<int>(self->workArea_.right - bodyPxW - 6)));
                const int clampedY =
                    std::clamp<int>(rawBodyY, static_cast<int>(self->workArea_.top + 6),
                                    std::max<int>(static_cast<int>(self->workArea_.top + 6),
                                                  static_cast<int>(self->workArea_.bottom - bodyPxH - 6)));
                self->winX_ = clampedX - marginPx;
                self->winY_ = clampedY - marginPx;
                SetWindowPos(w, self->launcherMode_ ? HWND_TOP : HWND_TOPMOST, self->winX_, self->winY_, 0, 0,
                             SWP_NOSIZE | SWP_NOACTIVATE);
                return 0;
            }
            if (self->activeSliderIdx_ >= 0) {
                self->updateSliderAt(self->activeSliderIdx_, dipX);
                return 0;
            }

            const int nextHover = self->hitWidget(dipX, dipY);
            int nextListRow = -1;
            if (nextHover >= 0 &&
                self->widgets_[static_cast<std::size_t>(nextHover)].kind == WidgetKind::IconList) {
                const auto& lwg = self->widgets_[static_cast<std::size_t>(nextHover)];
                if (dipX < lwg.rc.right - 10.0f) {
                    nextListRow =
                        self->iconListScroll_ + static_cast<int>((dipY - (lwg.rc.top + 3.0f)) / 23.0f);
                }
            }
            if (nextHover != self->hoverIdx_ || nextListRow != self->hoverListRow_) {
                self->hoverIdx_ = nextHover;
                self->hoverListRow_ = nextListRow;
                self->render();
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, w, 0};
                TrackMouseEvent(&tme);
            }
            return 0;
        }
        if (m == WM_MOUSELEAVE) {
            if (self->hoverIdx_ != -1 || self->hoverListRow_ != -1) {
                self->hoverIdx_ = -1;
                self->hoverListRow_ = -1;
                self->render();
            }
            return 0;
        }
        if (m == WM_LBUTTONDOWN) {
            const float inv = 96.0f / static_cast<float>(self->dpi_ ? self->dpi_ : 96u);
            const float dipX = static_cast<float>(GET_X_LPARAM(lp)) * inv;
            const float dipY = static_cast<float>(GET_Y_LPARAM(lp)) * inv;
            const int hit = self->hitWidget(dipX, dipY);

            const bool hitEdit =
                (hit >= 0 && self->widgets_[static_cast<std::size_t>(hit)].kind == WidgetKind::TextEdit);
            if (self->editFocused_ && !hitEdit) {
                self->editFocused_ = false;
                self->pushEditBufferToBacking();
            }
            if (hitEdit) {
                self->editFocused_ = true;
                (void)SetForegroundWindow(w);
                SetFocus(w);
                self->render();
                return 0;
            }

            if ((hit >= 0 && self->widgets_[static_cast<std::size_t>(hit)].kind == WidgetKind::DragHeader) ||
                (self->launcherMode_ && hit < 0 &&
                 pointInsideRoundedDip(dipX, dipY, self->bodyRectDip(), kLauncherCornerDip))) {
                self->draggingCard_ = true;
                GetCursorPos(&self->dragStartCursor_);
                self->dragStartWin_ = {self->winX_, self->winY_};
                SetCapture(w);
                return 0;
            }
            if (hit >= 0 && self->widgets_[static_cast<std::size_t>(hit)].kind == WidgetKind::Slider) {
                self->activeSliderIdx_ = hit;
                SetCapture(w);
                self->updateSliderAt(hit, dipX);
                return 0;
            }

            self->pressedIdx_ = hit;
            SetCapture(w);
            self->render();
            return 0;
        }
        if (m == WM_LBUTTONUP) {
            if (self->draggingCard_) {
                self->draggingCard_ = false;
                if (GetCapture() == w) ReleaseCapture();
                return 0;
            }
            if (self->activeSliderIdx_ >= 0) {
                self->activeSliderIdx_ = -1;
                if (GetCapture() == w) ReleaseCapture();
                self->render();
                return 0;
            }
            if (GetCapture() == w) ReleaseCapture();
            const float inv = 96.0f / static_cast<float>(self->dpi_ ? self->dpi_ : 96u);
            const float dipX = static_cast<float>(GET_X_LPARAM(lp)) * inv;
            const float dipY = static_cast<float>(GET_Y_LPARAM(lp)) * inv;
            const int hit = self->hitWidget(dipX, dipY);
            const int pressed = self->pressedIdx_;
            self->pressedIdx_ = -1;
            self->render();
            if (hit >= 0 && hit == pressed) {
                self->activateWidget(hit, dipX, dipY);
            }
            return 0;
        }
        return DefWindowProcW(w, m, wp, lp);
    }

    HWND hwnd_ = nullptr;
    HWND backingPanel_ = nullptr;
    HWND phaseLabelHwnd_ = nullptr;
    MacGlassContext glass_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> titleFmt_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> brandTitleFmt_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> brandSubFmt_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> ctaFmt_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> labelFmt_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> ctrlFmt_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> ctrlCenterFmt_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> monoFmt_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> hintFmt_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> appIconBmp_;
    bool appIconLoaded_ = false;
    WinDarkPopupMenu dropdown_;

    RECT workArea_{};
    UINT dpi_ = 96u;
    bool launcherMode_ = true;
    PlayMode currentMode_ = PlayMode::Gravity;
    bool hasLayoutBackup_ = false;
    std::wstring titleText_;
    std::vector<Widget> widgets_;

    float bodyW_ = kSettingsWidthDip;
    float bodyH_ = 220.0f;
    float totalW_ = kSettingsWidthDip + kShadowMarginDip * 2.0f;
    float totalH_ = 248.0f;
    int winX_ = 0;
    int winY_ = 0;

    int hoverIdx_ = -1;
    int pressedIdx_ = -1;
    int activeSliderIdx_ = -1;
    int hoverListRow_ = -1;
    int iconListScroll_ = 0;
    bool draggingCard_ = false;
    POINT dragStartCursor_{};
    POINT dragStartWin_{};
    bool editFocused_ = false;
    bool closingDropdown_ = false;
    std::wstring editBuffer_;
};

}  // namespace dpp::winmirror
