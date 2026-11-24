#pragma once
// Opt-in Windows Explorer desktop icon position write-back & undo backup helper.
// Uses public Shell COM IFolderView::SelectAndPositionItems (SVSI_POSITIONITEM).
// Never modifies Explorer folder flags (refuses cleanly if FWF_AUTOARRANGE is enabled)
// and is never invoked automatically on exit, Esc, recovery, or unit tests.

// clang-format off
#include <windows.h>
#include <shlobj.h>
// clang-format on

#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "core/scene/coordinates.h"
#include "platforms/windows/probe_samples.h"

namespace dpp::winmirror {

struct WinLayoutWritebackPlan {
    std::vector<POINT> originalViewPoints;
    std::vector<POINT> targetViewPoints;
};

[[nodiscard]] inline bool buildWinLayoutWritebackPlan(const winprobe::Report& report,
                                                      const std::vector<Vec2>& currentSceneCenters,
                                                      const PixelRect& workAreaPx, int viewIconPixels,
                                                      unsigned dpi, int manualOffsetYPixels,
                                                      WinLayoutWritebackPlan& outPlan) {
    outPlan.originalViewPoints.clear();
    outPlan.targetViewPoints.clear();
    if (dpi == 0 || viewIconPixels <= 0 || !report.viewWindowScreenRect.has_value() ||
        report.samples.empty() || report.samples.size() != currentSceneCenters.size()) {
        return false;
    }
    const float scale = windowsDpiScale(dpi);
    const RECT viewScreen = *report.viewWindowScreenRect;
    outPlan.originalViewPoints.reserve(report.samples.size());
    outPlan.targetViewPoints.reserve(report.samples.size());

    for (std::size_t i = 0; i < report.samples.size(); ++i) {
        const auto& sample = report.samples[i];
        if (!sample.child || !sample.position.has_value() || !currentSceneCenters[i].isFinite()) {
            outPlan.originalViewPoints.clear();
            outPlan.targetViewPoints.clear();
            return false;
        }
        outPlan.originalViewPoints.push_back({sample.position->x, sample.position->y});
        const Vec2 anchorDip =
            windowsSceneCenterToIconAnchor(currentSceneCenters[i], viewIconPixels, scale, manualOffsetYPixels);
        const Vec2 screenPx = windowsSceneToScreenPixels(anchorDip, workAreaPx, scale);
        const LONG vx = static_cast<LONG>(std::lround(screenPx.x)) - viewScreen.left;
        const LONG vy = static_cast<LONG>(std::lround(screenPx.y)) - viewScreen.top;
        outPlan.targetViewPoints.push_back({vx, vy});
    }
    return true;
}

[[nodiscard]] inline bool buildWinLayoutWritebackPlanSubset(const winprobe::Report& report,
                                                            const std::vector<std::size_t>& activeOrigIndices,
                                                            const std::vector<Vec2>& currentSceneCenters,
                                                            const PixelRect& workAreaPx, int viewIconPixels,
                                                            unsigned dpi, int manualOffsetYPixels,
                                                            WinLayoutWritebackPlan& outPlan) {
    outPlan.originalViewPoints.clear();
    outPlan.targetViewPoints.clear();
    if (dpi == 0 || viewIconPixels <= 0 || !report.viewWindowScreenRect.has_value() ||
        report.samples.empty() || activeOrigIndices.empty() ||
        activeOrigIndices.size() != currentSceneCenters.size()) {
        return false;
    }
    const float scale = windowsDpiScale(dpi);
    const RECT viewScreen = *report.viewWindowScreenRect;
    outPlan.originalViewPoints.reserve(report.samples.size());
    outPlan.targetViewPoints.reserve(report.samples.size());
    for (const auto& sample : report.samples) {
        if (!sample.child || !sample.position.has_value()) {
            outPlan.originalViewPoints.clear();
            outPlan.targetViewPoints.clear();
            return false;
        }
        const POINT orig{sample.position->x, sample.position->y};
        outPlan.originalViewPoints.push_back(orig);
        outPlan.targetViewPoints.push_back(orig);
    }
    for (std::size_t k = 0; k < activeOrigIndices.size(); ++k) {
        const std::size_t idx = activeOrigIndices[k];
        if (idx >= report.samples.size() || !currentSceneCenters[k].isFinite()) {
            outPlan.originalViewPoints.clear();
            outPlan.targetViewPoints.clear();
            return false;
        }
        const Vec2 anchorDip =
            windowsSceneCenterToIconAnchor(currentSceneCenters[k], viewIconPixels, scale, manualOffsetYPixels);
        const Vec2 screenPx = windowsSceneToScreenPixels(anchorDip, workAreaPx, scale);
        const LONG vx = static_cast<LONG>(std::lround(screenPx.x)) - viewScreen.left;
        const LONG vy = static_cast<LONG>(std::lround(screenPx.y)) - viewScreen.top;
        outPlan.targetViewPoints[idx] = {vx, vy};
    }
    return true;
}

using WinShellPositionWriter =
    std::function<HRESULT(const std::vector<PCUITEMID_CHILD>& pidls, const std::vector<POINT>& points)>;

[[nodiscard]] inline bool applyWinLayoutWriteback(const winprobe::Report& report,
                                                  const std::vector<POINT>& desiredPoints,
                                                  const WinShellPositionWriter& writer,
                                                  std::wstring* outUserError = nullptr) {
    if (report.samples.empty() || report.samples.size() != desiredPoints.size()) {
        if (outUserError) *outUserError = L"无可写回的桌面图标坐标记录。";
        return false;
    }
    std::vector<PCUITEMID_CHILD> pidls;
    pidls.reserve(report.samples.size());
    for (const auto& s : report.samples) {
        if (!s.child) {
            if (outUserError) *outUserError = L"桌面项目标识缺失，已拒绝修改图标位置。";
            return false;
        }
        pidls.push_back(s.child.get());
    }
    const HRESULT hr = writer(pidls, desiredPoints);
    if (FAILED(hr)) {
        if (outUserError && outUserError->empty()) {
            *outUserError = L"Explorer 拒绝更新图标坐标，未完成写回。";
        }
        return false;
    }
    return true;
}

[[nodiscard]] inline bool applyWinLayoutToLiveExplorer(const winprobe::Report& report,
                                                       const std::vector<POINT>& desiredPoints,
                                                       std::wstring* outUserError = nullptr) {
    auto desktop = winprobe::openDesktopView();
    if (FAILED(desktop.hr) || !desktop.view || !desktop.shell) {
        if (outUserError) *outUserError = L"无法连接当前桌面 Explorer 视图。";
        return false;
    }
    HWND liveViewHwnd = nullptr;
    if (FAILED(desktop.shell->GetWindow(&liveViewHwnd)) || !report.viewWindow.has_value()) {
        if (outUserError) *outUserError = L"桌面视图句柄校验失败，已安全取消。";
        return false;
    }
    Microsoft::WRL::ComPtr<IFolderView2> fv2;
    if (SUCCEEDED(desktop.view.As(&fv2)) && fv2) {
        DWORD folderFlags = 0;
        if (SUCCEEDED(fv2->GetCurrentFolderFlags(&folderFlags)) && (folderFlags & FWF_AUTOARRANGE) != 0) {
            if (outUserError) {
                *outUserError = L"系统桌面当前开启了「自动排列图标」，请先在桌面右键 → 查看 → 取消勾选「自动排列图标」后再保存布局。";
            }
            return false;
        }
    }
    return applyWinLayoutWriteback(
        report, desiredPoints,
        [&](const std::vector<PCUITEMID_CHILD>& pidls, const std::vector<POINT>& pts) -> HRESULT {
            return desktop.view->SelectAndPositionItems(
                static_cast<UINT>(pidls.size()), const_cast<PCUITEMID_CHILD*>(pidls.data()),
                const_cast<POINT*>(pts.data()), SVSI_POSITIONITEM);
        },
        outUserError);
}

[[nodiscard]] inline std::string layoutWideToUtf8(const std::wstring& ws) {
    if (ws.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), nullptr, 0, nullptr,
                                      nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), out.data(), n, nullptr, nullptr);
    return out;
}

[[nodiscard]] inline std::wstring layoutUtf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

inline bool saveWinLayoutBackupFile(const std::wstring& path, const winprobe::Report& report,
                                    const std::vector<POINT>& originalPoints) noexcept {
    if (path.empty() || report.samples.empty() || report.samples.size() != originalPoints.size()) {
        return false;
    }
    try {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << "DPP_WIN_LAYOUT_BACKUP_V1\n";
        for (std::size_t i = 0; i < report.samples.size(); ++i) {
            const std::string nameUtf8 = layoutWideToUtf8(report.samples[i].localName);
            if (nameUtf8.empty() || nameUtf8.find('\n') != std::string::npos ||
                nameUtf8.find('\r') != std::string::npos || nameUtf8.find('\t') != std::string::npos) {
                continue;
            }
            out << originalPoints[i].x << '\t' << originalPoints[i].y << '\t' << nameUtf8 << '\n';
        }
        return out.good();
    } catch (...) {
        return false;
    }
}

[[nodiscard]] inline bool loadWinLayoutBackupFile(const std::wstring& path,
                                                  std::map<std::wstring, POINT>& outByName) noexcept {
    outByName.clear();
    if (path.empty()) return false;
    try {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        std::string header;
        if (!std::getline(in, header) || header != "DPP_WIN_LAYOUT_BACKUP_V1") return false;
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const auto t1 = line.find('\t');
            if (t1 == std::string::npos) continue;
            const auto t2 = line.find('\t', t1 + 1);
            if (t2 == std::string::npos) continue;
            const long px = std::strtol(line.substr(0, t1).c_str(), nullptr, 10);
            const long py = std::strtol(line.substr(t1 + 1, t2 - t1 - 1).c_str(), nullptr, 10);
            const std::wstring wname = layoutUtf8ToWide(line.substr(t2 + 1));
            if (!wname.empty()) {
                outByName[wname] = POINT{static_cast<LONG>(px), static_cast<LONG>(py)};
            }
        }
        return !outByName.empty();
    } catch (...) {
        outByName.clear();
        return false;
    }
}

inline void clearWinLayoutBackupFile(const std::wstring& path) noexcept {
    if (!path.empty()) DeleteFileW(path.c_str());
}

[[nodiscard]] inline bool applyWinNamedLayoutToLiveExplorer(const winprobe::Report& report,
                                                            const std::map<std::wstring, POINT>& byName,
                                                            std::wstring* outUserError = nullptr) {
    if (report.samples.empty() || byName.empty()) {
        if (outUserError) *outUserError = L"无可还原的桌面图标备份记录。";
        return false;
    }
    auto desktop = winprobe::openDesktopView();
    if (FAILED(desktop.hr) || !desktop.view || !desktop.shell) {
        if (outUserError) *outUserError = L"无法连接当前桌面 Explorer 视图。";
        return false;
    }
    Microsoft::WRL::ComPtr<IFolderView2> fv2;
    if (SUCCEEDED(desktop.view.As(&fv2)) && fv2) {
        DWORD folderFlags = 0;
        if (SUCCEEDED(fv2->GetCurrentFolderFlags(&folderFlags)) && (folderFlags & FWF_AUTOARRANGE) != 0) {
            if (outUserError) {
                *outUserError = L"系统桌面当前开启了「自动排列图标」，请先取消勾选「自动排列图标」后再撤销写回。";
            }
            return false;
        }
    }
    std::vector<PCUITEMID_CHILD> pidls;
    std::vector<POINT> pts;
    for (const auto& s : report.samples) {
        if (!s.child || s.localName.empty()) continue;
        const auto it = byName.find(s.localName);
        if (it != byName.end()) {
            pidls.push_back(s.child.get());
            pts.push_back(it->second);
        }
    }
    if (pidls.empty()) {
        if (outUserError) *outUserError = L"当前桌面未找到与备份匹配的图标项。";
        return false;
    }
    const HRESULT hr = desktop.view->SelectAndPositionItems(
        static_cast<UINT>(pidls.size()), pidls.data(), pts.data(), SVSI_POSITIONITEM);
    if (FAILED(hr)) {
        if (outUserError) *outUserError = L"Explorer 拒绝恢复备份图标坐标。";
        return false;
    }
    return true;
}

}  // namespace dpp::winmirror
