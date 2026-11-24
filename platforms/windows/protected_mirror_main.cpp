// clang-format off
#include <windows.h>
#include <objbase.h>
// clang-format on

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <iostream>
#include <string>
#include <vector>

#include "mirror_preview.h"
#include "protected_session.h"

namespace {

std::wstring defaultGuiLogDir() {
    wchar_t buf[MAX_PATH]{};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        n = GetTempPathW(MAX_PATH, buf);
    }
    std::wstring dir = (n > 0 && n < MAX_PATH) ? std::wstring(buf, n) : L".";
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
    dir += L"\\DesktopPhysicsPlayground-Logs";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

void pruneOldLogs(const std::wstring& dir) {
    struct Entry {
        std::wstring path;
        ULONGLONG writeTime = 0;
    };
    std::vector<Entry> entries;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\playground-live-*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
        ULARGE_INTEGER ui{};
        ui.LowPart = fd.ftLastWriteTime.dwLowDateTime;
        ui.HighPart = fd.ftLastWriteTime.dwHighDateTime;
        entries.push_back({dir + L"\\" + fd.cFileName, ui.QuadPart});
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    constexpr std::size_t kMaxRetainedFiles = 40;
    if (entries.size() <= kMaxRetainedFiles) return;
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.writeTime < b.writeTime; });
    const std::size_t toDelete = entries.size() - kMaxRetainedFiles;
    for (std::size_t i = 0; i < toDelete; ++i) {
        DeleteFileW(entries[i].path.c_str());
    }
}

std::wstring makeRunUuidString() {
    GUID g{};
    if (FAILED(CoCreateGuid(&g))) {
        wchar_t fallback[64]{};
        std::swprintf(fallback, 64, L"%llu-%lu", static_cast<unsigned long long>(GetTickCount64()),
                      static_cast<unsigned long>(GetCurrentProcessId()));
        return fallback;
    }
    wchar_t out[64]{};
    std::swprintf(out, 64, L"%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", g.Data1, g.Data2, g.Data3,
                  g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6],
                  g.Data4[7]);
    return out;
}

void attachParentConsoleIfInteractiveCli() {
    const HANDLE outHandle = GetStdHandle(STD_OUTPUT_HANDLE);
    const DWORD outType = outHandle ? GetFileType(outHandle) : FILE_TYPE_UNKNOWN;
    if (outType == FILE_TYPE_UNKNOWN) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            FILE* fpOut = nullptr;
            FILE* fpErr = nullptr;
            (void)freopen_s(&fpOut, "CONOUT$", "w", stdout);
            (void)freopen_s(&fpErr, "CONOUT$", "w", stderr);
            std::ios::sync_with_stdio(true);
        }
    }
}

bool isProcessElevated() noexcept {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION elev{};
    DWORD cb = 0;
    const BOOL ok = GetTokenInformation(tok, TokenElevation, &elev, sizeof(elev), &cb);
    CloseHandle(tok);
    return ok && elev.TokenIsElevated != 0;
}

bool relaunchViaExplorerDesktopToken() noexcept {
    HWND shellWnd = GetShellWindow();
    if (!shellWnd) shellWnd = FindWindowW(L"Progman", nullptr);
    if (!shellWnd) return false;
    DWORD shellPid = 0;
    GetWindowThreadProcessId(shellWnd, &shellPid);
    if (!shellPid || shellPid == GetCurrentProcessId()) return false;

    HANDLE shellProc = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, shellPid);
    if (!shellProc) return false;
    HANDLE shellTok = nullptr;
    if (!OpenProcessToken(shellProc, TOKEN_DUPLICATE, &shellTok)) {
        CloseHandle(shellProc);
        return false;
    }
    HANDLE primaryTok = nullptr;
    const BOOL dupOk = DuplicateTokenEx(shellTok, TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY | TOKEN_DUPLICATE |
                                                      TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID,
                                        nullptr, SecurityImpersonation, TokenPrimary, &primaryTok);
    CloseHandle(shellTok);
    CloseHandle(shellProc);
    if (!dupOk || !primaryTok) return false;

    wchar_t exePath[32768]{};
    const DWORD len = GetModuleFileNameW(nullptr, exePath, 32768);
    if (!len || len >= 32768) {
        CloseHandle(primaryTok);
        return false;
    }
    std::wstring cmdLine = L"\"" + std::wstring(exePath) + L"\" --deelevated";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const BOOL launched = CreateProcessWithTokenW(primaryTok, 0, exePath, cmdLine.data(), 0, nullptr, nullptr,
                                                  &si, &pi);
    CloseHandle(primaryTok);
    if (!launched) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

std::wstring asciiToWide(const std::string& s) {
    std::wstring out;
    out.reserve(s.size());
    for (unsigned char ch : s) out.push_back(static_cast<wchar_t>(ch));
    return out;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    // Out-of-process watchdog helper / recovery / status CLI modes:
    if (argc >= 2 && (std::wstring(argv[1]) == L"--helper" || std::wstring(argv[1]) == L"--recover" ||
                      std::wstring(argv[1]) == L"--status")) {
        if (std::wstring(argv[1]) != L"--helper") attachParentConsoleIfInteractiveCli();
        return dpp::winprotected::run(argc, argv);
    }
    if (argc == 2 && std::wstring(argv[1]) == L"--help") {
        attachParentConsoleIfInteractiveCli();
        std::cout
            << "DeskFling - 桌面游乐场 (GUI double-click or CLI)\n"
               "Zero arguments: launches GUI directly with rotating logs in %LOCALAPPDATA%\\DesktopPhysicsPlayground-Logs.\n"
               "Experimental protected desktop mirror --report <NEW.json>\n"
               "--startup-display-experiment --report <NEW.json> for single-display geometry.\n"
               "One-click Quick Start collapses to Tray + Floating Capsule HUD; independent out-of-process\n"
               "heartbeat/crash watchdog (5s hang / instant owner-exit / Esc / 12h fallback).\n";
        return 0;
    }

    const bool deelevatedChild = (argc == 2 && std::wstring(argv[1]) == L"--deelevated");
    // Zero-argument native GUI double-click launch (no console window!):
    if (argc == 1 || deelevatedChild) {
        const std::wstring logDir = defaultGuiLogDir();
        pruneOldLogs(logDir);
        const std::wstring runId = makeRunUuidString();
        const std::wstring logPath = logDir + L"\\playground-live-" + runId + L".log";
        const std::wstring reportPath = logDir + L"\\playground-live-" + runId + L".json";
        FILE* fpOut = nullptr;
        FILE* fpErr = nullptr;
        (void)_wfreopen_s(&fpOut, logPath.c_str(), L"w", stdout);
        (void)_wfreopen_s(&fpErr, (logDir + L"\\playground-live-" + runId + L".err.log").c_str(), L"w",
                          stderr);
        try {
            dpp::winprotected::tryAutoRecoverStaleSession();
            std::string failStage;
            HRESULT failHr = S_OK;
            const int rc = dpp::winmirror::runMarkers(reportPath, /*protectedMode=*/true,
                                                      /*startupExperiment=*/true, &failStage, &failHr);
            if (rc != 0) {
                if (!deelevatedChild && isProcessElevated() &&
                    (failStage.find("FindWindowSW") != std::string::npos ||
                     failStage == "fresh_snapshot_read_failed")) {
                    if (relaunchViaExplorerDesktopToken()) return 0;
                }
                wchar_t hrBuf[32]{};
                std::swprintf(hrBuf, 32, L"0x%08lX", static_cast<unsigned long>(failHr));
                const std::wstring diagLine =
                    L"诊断阶段：" + (failStage.empty() ? std::wstring(L"unknown") : asciiToWide(failStage)) +
                    L" (" + hrBuf + L")\n";
                const std::wstring msg =
                    L"桌面游乐场 (DeskFling) 启动前置检查未通过。\n\n" + diagLine +
                    L"\n常见原因：\n"
                    L"1. 请勿以「管理员身份」运行（管理员权限与普通桌面 Explorer 隔离，直接普通双击即可）；\n"
                    L"2. 当前桌面若开启了「隐藏桌面图标」，请在桌面右键 → 查看 → 勾选「显示桌面图标」；\n"
                    L"3. 若已有另一个桌面游乐场实例正在运行，请先按 Esc 或从右下角托盘退出。\n\n"
                    L"详细日志已保存至 %LOCALAPPDATA%\\DesktopPhysicsPlayground-Logs。";
                MessageBoxW(nullptr, msg.c_str(), L"桌面游乐场 · DeskFling", MB_OK | MB_ICONWARNING);
            }
            return rc;
        } catch (...) {
            MessageBoxW(nullptr,
                        L"桌面游乐场 (DeskFling) 遇到异常退出；若桌面图标未显示，请双击同目录「紧急恢复桌面.cmd」恢复。",
                        L"桌面游乐场 · DeskFling", MB_OK | MB_ICONERROR);
            return 1;
        }
    }

    const bool experiment = argc == 4 && std::wstring(argv[1]) == L"--startup-display-experiment";
    if (experiment) {
        --argc;
        ++argv;
    }
    if (argc != 3 || std::wstring(argv[1]) != L"--report" || !*argv[2]) return 2;
    try {
        return dpp::winmirror::runMarkers(argv[2], true, experiment);
    } catch (...) {
        std::cerr << "protected mirror failed; inspect recovery journal\n";
        return 1;
    }
}
