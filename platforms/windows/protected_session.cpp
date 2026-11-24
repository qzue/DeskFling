// clang-format off
#include <windows.h>
#include <objbase.h>
// clang-format on
#include "protected_session.h"

#include <sddl.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#ifdef DPP_PROTECTED_TESTS
#include <gtest/gtest.h>
#endif
#include <iostream>
#include <stdexcept>
#include <string>

#include "probe_samples.h"

namespace dpp::winprotected {
using Microsoft::WRL::ComPtr;
namespace {
constexpr DWORD mask = 1;  // compatibility backend owns only window visibility
constexpr ULONGLONG shortExperimentMs = 20000;
constexpr ULONGLONG mirrorSessionMs = 180000;
constexpr ULONGLONG continuousMirrorMs = 43200000ULL;
enum class Phase : DWORD { Prepared = 1, Tagged, Armed, Hidden, Restored, Failed };
struct Record {
    DWORD version = 2, size = sizeof(Record);
    GUID nonce{};
    DWORD session = 0, owner = 0, host = 0;
    ULONGLONG ownerCreated = 0, hostCreated = 0, view = 0, parent = 0, root = 0;
    DWORD originalShellFlags = 0;
    LUID login{};
    std::array<wchar_t, 192> sid{};
    DWORD original = 1, expected = 0, changedMask = mask;
    Phase phase = Phase::Prepared;
    DWORD result = 0, recoveryTrigger = 0;  // 1 normal, 2 owner exited, 3 heartbeat, 4 deadline, 5 command
    ULONGLONG started = 0;
    DWORD checksum = 0;
};
// v3 retains the v2 wire layout and adds the bounded 180s mirror policy;
// v4 adds the continuous-heartbeat 12h session policy while keeping 5s pulse/crash recovery.
// All three versions remain recoverable; unknown policies fail closed.
ULONGLONG maximumMs(const Record& r) {
    return r.version == 4 ? continuousMirrorMs : (r.version == 3 ? mirrorSessionMs : shortExperimentMs);
}
ULONGLONG awakeTickMs() noexcept {
    ULONGLONG unbiased100ns = 0;
    if (QueryUnbiasedInterruptTime(&unbiased100ns) != FALSE) {
        return unbiased100ns / 10000ULL;
    }
    return GetTickCount64();
}
struct Handle {
    HANDLE h = nullptr;
    Handle() = default;
    explicit Handle(HANDLE value) : h(value) {}
    ~Handle() {
        if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
void require(bool value, const char* what) {
    if (!value) {
        std::cerr << what << "\n";
        throw std::runtime_error(what);
    }
}
void hr(HRESULT value, const char* what) {
    if (FAILED(value)) {
        std::cerr << what << " HRESULT=0x" << std::hex << value << std::dec << '\n';
        throw std::runtime_error(what);
    }
}
ULONGLONG creation(HANDLE process) {
    FILETIME a{}, b{}, c{}, d{};
    require(GetProcessTimes(process, &a, &b, &c, &d) != FALSE, "process creation time");
    return (static_cast<ULONGLONG>(a.dwHighDateTime) << 32) | a.dwLowDateTime;
}
std::wstring guid(const GUID& value) {
    wchar_t text[40]{};
    require(StringFromGUID2(value, text, 40) != 0, "GUID");
    return text;
}
struct Identity {
    DWORD session{};
    LUID login{};
    std::wstring sid;
    Identity() {
        require(ProcessIdToSessionId(GetCurrentProcessId(), &session) != FALSE, "login session");
        Handle token;
        require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.h) != FALSE, "token");
        TOKEN_STATISTICS stats{};
        DWORD bytes{};
        require(GetTokenInformation(token.h, TokenStatistics, &stats, sizeof(stats), &bytes) != FALSE,
                "authentication identity");
        login = stats.AuthenticationId;
        std::array<BYTE, 1024> buffer{};
        require(GetTokenInformation(token.h, TokenUser, buffer.data(), static_cast<DWORD>(buffer.size()),
                                    &bytes) != FALSE,
                "user identity");
        LPWSTR raw = nullptr;
        require(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &raw) != FALSE,
                "SID");
        sid = raw;
        LocalFree(raw);
        require(sid.size() < 192, "SID length");
    }
    bool matches(const Record& r) const {
        return session == r.session && login.LowPart == r.login.LowPart &&
               login.HighPart == r.login.HighPart && sid == r.sid.data();
    }
    std::wstring prefix() const {
        return L"Local\\DPP.Protected.v1." + sid + L"." + std::to_wstring(session);
    }
};
struct Security {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
    explicit Security(const Identity& id) {
        const auto sddl = L"D:P(A;;GA;;;" + id.sid + L")";
        require(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                                     &descriptor, nullptr) != FALSE,
                "private ACL");
        attributes.lpSecurityDescriptor = descriptor;
    }
    ~Security() { LocalFree(descriptor); }
};
DWORD hash(const Record& r) {
    DWORD value = 2166136261u;
    const auto* bytes = reinterpret_cast<const BYTE*>(&r);
    for (size_t i = 0; i < offsetof(Record, checksum); ++i) value = (value ^ bytes[i]) * 16777619u;
    return value;
}
struct Store {
    Identity id;
    Security security{id};
    std::wstring path;
    explicit Store(std::wstring dir = {}) {
        if (dir.empty()) {
            PWSTR root = nullptr;
            hr(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &root), "LocalAppData");
            dir = root;
            CoTaskMemFree(root);
            dir += L"\\DesktopPhysicsPlayground-Recovery-v1";
        }
        if (!CreateDirectoryW(dir.c_str(), &security.attributes))
            require(GetLastError() == ERROR_ALREADY_EXISTS, "journal directory");
        const DWORD attributes = GetFileAttributesW(dir.c_str());
        require(attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT),
                "journal directory reparse point");
        path = dir + L"\\session-" + std::to_wstring(id.session) + L".bin";
    }
    bool read(Record& r) {
        Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (file.h == INVALID_HANDLE_VALUE) {
            require(GetLastError() == ERROR_FILE_NOT_FOUND, "open journal");
            return false;
        }
        BY_HANDLE_FILE_INFORMATION info{};
        require(GetFileInformationByHandle(file.h, &info) &&
                    !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT),
                "journal reparse point");
        LARGE_INTEGER size{};
        DWORD bytes{};
        require(GetFileSizeEx(file.h, &size) && size.QuadPart == sizeof(r), "journal size/corruption");
        require(ReadFile(file.h, &r, sizeof(r), &bytes, nullptr) && bytes == sizeof(r), "read journal");
        require((r.version == 2 || r.version == 3 || r.version == 4) && r.size == sizeof(r) &&
                    r.sid.back() == 0 && r.checksum == hash(r) && r.changedMask == mask && r.expected == 0 &&
                    r.original == 1 && r.phase >= Phase::Prepared && r.phase <= Phase::Failed,
                "journal validation");
        require(id.matches(r), "journal belongs to another login; retained");
        return true;
    }
    void write(Record& r) {
        r.checksum = hash(r);
        GUID unique{};
        hr(CoCreateGuid(&unique), "temporary nonce");
        const auto temp = path + guid(unique) + L".tmp";
        {
            Handle file(CreateFileW(temp.c_str(), GENERIC_WRITE, 0, &security.attributes, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr));
            require(file.h != INVALID_HANDLE_VALUE, "create journal");
            DWORD bytes{};
            require(WriteFile(file.h, &r, sizeof(r), &bytes, nullptr) && bytes == sizeof(r) &&
                        FlushFileBuffers(file.h),
                    "flush journal");
        }
        require(MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) !=
                    FALSE,
                "commit journal");
    }
    void archive(const Record& r) {
        require(MoveFileExW(path.c_str(), (path + guid(r.nonce) + L".recovered").c_str(),
                            MOVEFILE_WRITE_THROUGH) != FALSE,
                "archive recovered journal");
    }
};
struct Guard {
    HANDLE mutex;
    explicit Guard(HANDLE value) : mutex(value) {
        const auto result = WaitForSingleObject(mutex, 3000);
        require(result == WAIT_OBJECT_0 || result == WAIT_ABANDONED, "recovery operation busy");
    }
    ~Guard() { ReleaseMutex(mutex); }
};
struct View {
    winprobe::DesktopView desktop;
    ComPtr<IFolderView2> flags;
    HWND hwnd{};
    DWORD pid{};
    ULONGLONG created{};
    View() {
        require(winprobe::interactive(winprobe::inspectEnvironment()),
                "requires interactive desktop and same-session Explorer");
        desktop = winprobe::openDesktopView();
        hr(desktop.hr, desktop.stage);
        hr(desktop.view.As(&flags), "IFolderView2");
        hr(desktop.shell->GetWindow(&hwnd), "view window");
        require(GetWindowThreadProcessId(hwnd, &pid) != 0, "host PID");
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
        require(process.h != nullptr, "host process");
        created = creation(process.h);
        DWORD shellPid{};
        if (HWND sw = GetShellWindow()) GetWindowThreadProcessId(sw, &shellPid);
        require((shellPid == 0 || pid == shellPid) && pid != GetCurrentProcessId(), "Shell host identity");
    }
    DWORD get() {
        DWORD value{};
        hr(flags->GetCurrentFolderFlags(&value), "GetCurrentFolderFlags");
        return value;
    }
    bool matches(const Record& r) const {
        return r.host == pid && r.hostCreated == created && r.view == reinterpret_cast<ULONGLONG>(hwnd);
    }
    void customPosition() {
        ComPtr<IFolderViewOptions> options;
        hr(desktop.shell.As(&options), "IFolderViewOptions QueryInterface");
        FOLDERVIEWOPTIONS value{};
        hr(options->GetFolderViewOptions(&value), "GetFolderViewOptions");
        std::cout << "folder_view_options=0x" << std::hex << value << std::dec << '\n';
        require((value & FVO_CUSTOMPOSITION) != 0,
                "FVO_CUSTOMPOSITION absent; no layout option will be changed");
    }
};
// Explorer window structure is an implementation dependency, not a public Shell contract.
std::wstring className(HWND hwnd) {
    wchar_t value[128]{};
    require(GetClassNameW(hwnd, value, 128) != 0, "window class");
    return value;
}
std::wstring marker(const Record& r) { return L"DPP.Protected.Visibility." + guid(r.nonce); }
HANDLE markerValue() { return reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(1)); }
HWND target(const Record& r, bool requireMarker) {
    const auto hwnd = reinterpret_cast<HWND>(r.view);
    const auto parent = reinterpret_cast<HWND>(r.parent);
    const auto root = reinterpret_cast<HWND>(r.root);
    require(IsWindow(hwnd) && IsWindow(parent) && IsWindow(root), "target window destroyed");
    require(className(hwnd) == L"SysListView32" && className(parent) == L"SHELLDLL_DefView",
            "target class changed");
    const auto rootClass = className(root);
    require(rootClass == L"Progman" || rootClass == L"WorkerW", "not an Explorer desktop root");
    const HWND gaRoot = GetAncestor(root, GA_ROOT);
    const bool validHierarchy =
        (gaRoot == root) || (rootClass == L"WorkerW" && gaRoot && className(gaRoot) == L"Progman");
    require(GetParent(hwnd) == parent && GetParent(parent) == root && validHierarchy,
            "desktop parent chain changed");
    require(FindWindowExW(parent, nullptr, L"SysListView32", nullptr) == hwnd &&
                FindWindowExW(parent, hwnd, L"SysListView32", nullptr) == nullptr,
            "ambiguous desktop ListView");
    DWORD shellPid{};
    if (HWND sw = GetShellWindow()) GetWindowThreadProcessId(sw, &shellPid);
    for (HWND window : {hwnd, parent, root, gaRoot}) {
        if (!window) continue;
        DWORD pid{};
        require(GetWindowThreadProcessId(window, &pid) && pid == r.host &&
                    (shellPid == 0 || pid == shellPid) && pid != GetCurrentProcessId(),
                "target process changed");
    }
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, r.host));
    require(process.h && creation(process.h) == r.hostCreated, "Explorer process identity changed");
    DWORD session{};
    require(ProcessIdToSessionId(r.host, &session) && session == r.session, "Explorer session changed");
    if (requireMarker)
        require(GetPropW(hwnd, marker(r).c_str()) == markerValue(),
                "window lifetime marker missing; stale HWND not used");
    return hwnd;
}
bool ownVisible(HWND hwnd) { return (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_VISIBLE) != 0; }
void flushShellDesktopRepaint(const Record& r) noexcept {
    const auto hwnd = reinterpret_cast<HWND>(r.view);
    const auto parent = reinterpret_cast<HWND>(r.parent);
    const auto root = reinterpret_cast<HWND>(r.root);
    constexpr UINT kFlags = RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN;
    if (IsWindow(hwnd)) RedrawWindow(hwnd, nullptr, nullptr, kFlags);
    if (IsWindow(parent)) RedrawWindow(parent, nullptr, nullptr, kFlags);
    if (IsWindow(root)) {
        RedrawWindow(root, nullptr, nullptr, kFlags);
        if (HWND ga = GetAncestor(root, GA_ROOT); ga && ga != root && IsWindow(ga)) {
            RedrawWindow(ga, nullptr, nullptr, kFlags);
        }
    }
    if (HWND progman = FindWindowW(L"Progman", nullptr); progman && IsWindow(progman)) {
        RedrawWindow(progman, nullptr, nullptr, kFlags);
    }
    EnumWindows(
        [](HWND top, LPARAM lp) -> BOOL {
            const DWORD hostPid = static_cast<DWORD>(lp);
            DWORD pid = 0;
            if (GetWindowThreadProcessId(top, &pid) && pid == hostPid && className(top) == L"WorkerW") {
                RedrawWindow(top, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
            }
            return TRUE;
        },
        static_cast<LPARAM>(r.host));
}
void setVisible(const Record& r, bool visible) {
    const HWND hwnd = target(r, true);
    require(ShowWindowAsync(hwnd, visible ? SW_SHOWNOACTIVATE : SW_HIDE) != FALSE,
            "visibility request rejected");
    const auto started = GetTickCount64();
    // API acceptance is not completion. Observe the target's own style and effective visibility.
    do {
        Sleep(25);
        target(r, true);
        if (ownVisible(hwnd) == visible && (IsWindowVisible(hwnd) != FALSE) == visible) {
            flushShellDesktopRepaint(r);
            return;
        }
    } while (GetTickCount64() - started < 1500);
    throw std::runtime_error("visibility readback timeout; intent retained");
}
Record inspectTarget() {
    View view;
    Record r{};
    r.originalShellFlags = view.get();
    require(!(r.originalShellFlags & FWF_NOICONS), "Shell icons already hidden");
    require(className(view.hwnd) == L"SHELLDLL_DefView", "unsupported Shell view window");
    HWND hwnd = FindWindowExW(view.hwnd, nullptr, L"SysListView32", nullptr);
    r.view = reinterpret_cast<ULONGLONG>(hwnd);
    r.parent = reinterpret_cast<ULONGLONG>(view.hwnd);
    r.root = reinterpret_cast<ULONGLONG>(GetParent(view.hwnd));
    r.host = view.pid;
    r.hostCreated = view.created;
    require(ProcessIdToSessionId(GetCurrentProcessId(), &r.session) != FALSE, "session identity");
    target(r, false);
    require(ownVisible(hwnd) && IsWindowVisible(hwnd), "desktop ListView already hidden");
    return r;
}
struct Backend : SessionBackend {
    Store store;
    Handle instance, operation, ready, pulse, sleepEvent, wakeEvent, helper;
    Record record{};
    std::uintptr_t expectedList = 0;
    DWORD requestedRecordVersion = 2;
    bool startupExperiment = false;
    bool own = false, persisted = false, preparationOnly = false;
#ifdef DPP_RECOVERY_TEST_HOOKS
    bool failHelperLaunch = false, failAfterHide = false, injectedHidden = false;
    DWORD injectedHelperLaunchError = 0;
#endif
    Backend() {
        operation.h =
            CreateMutexW(&store.security.attributes, FALSE, (store.id.prefix() + L".operation").c_str());
        require(operation.h != nullptr, "recovery mutex");
    }
    bool lock() override {
        instance.h = CreateMutexW(&store.security.attributes, FALSE, (store.id.prefix() + L".owner").c_str());
        require(instance.h != nullptr, "owner mutex");
        const DWORD wait = WaitForSingleObject(instance.h, 0);
        own = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
        return own;
    }
    bool recover(const GUID* expected = nullptr, DWORD trigger = 5) {
        Guard guard(operation.h);
        Record r{};
        if (!store.read(r)) return true;
        if (expected && !IsEqualGUID(*expected, r.nonce))
            return true;  // stale helper cannot touch newer session
        if (r.phase == Phase::Restored) {
            // State restoration is already durable; remove only our surviving marker.
            const auto hwnd = reinterpret_cast<HWND>(r.view);
            if (GetPropW(hwnd, marker(r).c_str()) == markerValue()) {
                target(r, true);
                require(RemovePropW(hwnd, marker(r).c_str()) == markerValue(), "remove recovered marker");
            }
            return true;
        }
        const Phase previous = r.phase;
        r.recoveryTrigger = trigger;
        HWND marked = nullptr;
        try {
            if (r.phase != Phase::Prepared) {
                const HWND hwnd = target(r, r.phase != Phase::Tagged);
                const bool tagged = GetPropW(hwnd, marker(r).c_str()) == markerValue();
                if (r.phase == Phase::Armed || r.phase == Phase::Hidden || r.phase == Phase::Failed) {
                    const bool wasHidden = !ownVisible(hwnd);
                    setVisible(r, true);
                    // Armed covers an uncertain queued hide. Never claim completion from an
                    // already-visible target when no hidden readback was durably observed.
                    require(r.phase != Phase::Armed || wasHidden,
                            "uncertain hide completion; show requested, retain intent for inspection");
                }
                if (tagged) marked = hwnd;
            }
            r.phase = Phase::Restored;
            r.result = ERROR_SUCCESS;
            store.write(r);
            if (marked)
                require(RemovePropW(marked, marker(r).c_str()) == markerValue(),
                        "remove owned lifetime marker");
            std::cout << "visibility_api_restored=true visual_confirmation=required\n";
            return true;
        } catch (...) {
            r.phase = previous;  // never turn a non-mutating Prepared record into an armed intent
            r.result = ERROR_RECOVERY_FAILURE;
            store.write(r);
            throw;
        }
    }
    bool recoverResidual() override {
        if (!recover()) return false;
        Guard guard(operation.h);
        Record r{};
        if (store.read(r)) store.archive(r);
        return true;
    }
    bool probe() override {
        View view;
        wchar_t build[32]{};
        DWORD bytes = sizeof(build);
        require(RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                             L"CurrentBuildNumber", RRF_RT_REG_SZ, nullptr, build, &bytes) == ERROR_SUCCESS,
                "unvalidated OS build");
        wchar_t* buildEnd = nullptr;
        const unsigned long buildNum = std::wcstoul(build, &buildEnd, 10);
        if (startupExperiment) {
            require(GetSystemMetrics(SM_CMONITORS) >= 1, "no display monitor detected");
            require(buildEnd != build && !*buildEnd && buildNum >= 19041UL,
                    "unsupported Windows build (requires Windows 10 2004+ or Windows 11)");
            MONITORINFO info{sizeof(info), {}, {}, 0};
            const auto monitor = MonitorFromWindow(view.hwnd, MONITOR_DEFAULTTONEAREST);
            require(monitor && GetMonitorInfoW(monitor, &info) && GetDpiForWindow(view.hwnd) > 0 &&
                        info.rcWork.right > info.rcWork.left && info.rcWork.bottom > info.rcWork.top &&
                        info.rcWork.left >= info.rcMonitor.left && info.rcWork.top >= info.rcMonitor.top &&
                        info.rcWork.right <= info.rcMonitor.right &&
                        info.rcWork.bottom <= info.rcMonitor.bottom,
                    "unknown experimental display configuration");
        } else {
            require(GetSystemMetrics(SM_CMONITORS) == 1, "single monitor only");
            DWORD buildRevision{};
            bytes = sizeof(buildRevision);
            require(RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                                 L"UBR", RRF_RT_REG_DWORD, nullptr, &buildRevision, &bytes) == ERROR_SUCCESS &&
                        buildRevision == 6466,
                    "unvalidated OS revision");
            require(std::wstring(build) == L"19045", "unvalidated OS build");
            RECT work{};
            require(SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0) && work.left == 0 && work.top == 0 &&
                        work.right == 1920 && work.bottom == 1030,
                    "unvalidated work area");
            require(GetSystemMetrics(SM_CXSCREEN) == 1920 && GetSystemMetrics(SM_CYSCREEN) == 1080 &&
                        GetDpiForWindow(view.hwnd) == 120,
                    "unvalidated display configuration");
        }
        inspectTarget();
        return true;
    }
    bool snapshot() override {
        record = inspectTarget();
        record.version = requestedRecordVersion;
        require(!expectedList || expectedList == record.view, "mirror snapshot target changed");
        record.owner = GetCurrentProcessId();
        record.ownerCreated = creation(GetCurrentProcess());
        record.session = store.id.session;
        record.login = store.id.login;
        std::copy(store.id.sid.begin(), store.id.sid.end(), record.sid.begin());
        hr(CoCreateGuid(&record.nonce), "session nonce");
        record.started = GetTickCount64();
        return true;
    }
    bool persistIntent() override {
        Guard guard(operation.h);
        store.write(record);
        persisted = true;
        return true;
    }
    bool prepareScene() override { return true; }  // deliberately no rendering in first experiment
    std::wstring eventName(const wchar_t* suffix) const {
        return store.id.prefix() + guid(record.nonce) + suffix;
    }
    bool recoveryReady() override {
        ready.h = CreateEventW(&store.security.attributes, TRUE, FALSE, eventName(L".ready").c_str());
        pulse.h = CreateEventW(&store.security.attributes, FALSE, FALSE, eventName(L".pulse").c_str());
        if (record.version == 4) {
            sleepEvent.h = CreateEventW(&store.security.attributes, FALSE, FALSE, eventName(L".sleep").c_str());
            wakeEvent.h = CreateEventW(&store.security.attributes, FALSE, FALSE, eventName(L".wake").c_str());
        }
        require(ready.h && pulse.h && (record.version != 4 || (sleepEvent.h && wakeEvent.h)),
                "helper handshake events");
        wchar_t exe[32768]{};
        DWORD length = GetModuleFileNameW(nullptr, exe, 32768);
        require(length && length < 32768, "helper executable");
        std::wstring command = L"\"" + std::wstring(exe) + L"\" --helper " + guid(record.nonce);
#ifdef DPP_RECOVERY_TEST_HOOKS
        if (failHelperLaunch) {
            const auto absent = std::wstring(exe) + guid(record.nonce) + L".absent";
            require(GetFileAttributesW(absent.c_str()) == INVALID_FILE_ATTRIBUTES,
                    "test launch path unexpectedly exists");
            require(absent.size() < 32768, "test path length");
            std::copy(absent.begin(), absent.end(), exe);
            exe[absent.size()] = 0;
        }
#endif
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        const BOOL launched = CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                                             nullptr, nullptr, &startup, &process);
        if (!launched) {
            const DWORD error = GetLastError();
            std::cerr << "helper_launch_win32=" << error << "\n";
#ifdef DPP_RECOVERY_TEST_HOOKS
            injectedHelperLaunchError = error;
#endif
        }
        require(launched != FALSE, "start recovery helper");
        helper.h = process.hProcess;
        CloseHandle(process.hThread);
        HANDLE handles[]{ready.h, helper.h};
        return WaitForMultipleObjects(2, handles, FALSE, 5000) == WAIT_OBJECT_0;
    }
    bool activate() override {
        Guard guard(operation.h);
        Record current{};
        require(store.read(current) && IsEqualGUID(current.nonce, record.nonce) &&
                    current.phase == Phase::Prepared,
                "activation journal changed");
        require(WaitForSingleObject(helper.h, 0) == WAIT_TIMEOUT &&
                    GetTickCount64() - record.started < maximumMs(record),
                "helper unavailable or session expired");
        if (preparationOnly) {
            std::cout << "helper_ready=true no_visibility_change=true\n";
            return true;
        }
        const HWND hwnd = target(record, false);
        require(ownVisible(hwnd) && IsWindowVisible(hwnd), "visibility changed before hide");
        require(!GetPropW(hwnd, marker(record).c_str()), "lifetime marker collision");
        record.phase = Phase::Tagged;
        store.write(record);
        require(SetPropW(hwnd, marker(record).c_str(), markerValue()) != FALSE, "install lifetime marker");
        target(record, true);
        record.phase = Phase::Armed;
        store.write(record);
        setVisible(record, false);
        record.phase = Phase::Hidden;
        store.write(record);
#ifdef DPP_RECOVERY_TEST_HOOKS
        if (failAfterHide) {
            injectedHidden = true;
            std::cout << "injected_failure_after_durable_hidden=true\n" << std::flush;
            throw std::runtime_error("injected initialization failure after durable hidden state");
        }
#endif
        std::cout << "window_hidden_readback=true backend=listview_compatibility "
                     "visual_confirmation=required maximum_seconds="
                  << maximumMs(record) / 1000 << "\n";
        return true;
    }
    bool restore() override { return !persisted || recover(&record.nonce, 1); }
    void unlock() noexcept override {
        if (own) {
            ReleaseMutex(instance.h);
            own = false;
        }
    }
};
int helperMain(Backend& backend, const wchar_t* nonce) {
    GUID expected{};
    hr(CLSIDFromString(nonce, &expected), "helper nonce");
    {
        Guard guard(backend.operation.h);
        require(backend.store.read(backend.record) && IsEqualGUID(expected, backend.record.nonce) &&
                    backend.record.phase == Phase::Prepared,
                "helper journal mismatch");
        target(backend.record, false);
    }
    Handle owner(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, backend.record.owner));
    require(owner.h && creation(owner.h) == backend.record.ownerCreated, "owner identity");
    Handle ready(OpenEventW(EVENT_MODIFY_STATE, FALSE, backend.eventName(L".ready").c_str()));
    Handle pulse(OpenEventW(SYNCHRONIZE, FALSE, backend.eventName(L".pulse").c_str()));
    require(ready.h && pulse.h, "helper handshake");
    require(SetEvent(ready.h) != FALSE, "helper ready");
    ULONGLONG heartbeat = GetTickCount64();
    const bool isContinuous = (backend.record.version == 4);
    dpp::ContinuousWatchdogClock continuousClock{};
    continuousClock.reset(heartbeat, awakeTickMs());
    Handle sleepEvt(isContinuous ? OpenEventW(SYNCHRONIZE, FALSE, backend.eventName(L".sleep").c_str())
                                 : nullptr);
    Handle wakeEvt(isContinuous ? OpenEventW(SYNCHRONIZE, FALSE, backend.eventName(L".wake").c_str())
                                : nullptr);
    for (;;) {
        HANDLE handles[]{owner.h, pulse.h, sleepEvt.h, wakeEvt.h};
        const DWORD handleCount = (isContinuous && sleepEvt.h && wakeEvt.h) ? 4u : 2u;
        const DWORD wait = WaitForMultipleObjects(handleCount, handles, FALSE, 500);
        const auto now = GetTickCount64();
        const auto awakeNow = awakeTickMs();
        if (isContinuous) {
            continuousClock.observeLoop(now, awakeNow, 500ULL);
        }
        if (wait == WAIT_OBJECT_0 + 1) {
            heartbeat = now;
            if (isContinuous) continuousClock.recordPulse(awakeNow);
        } else if (isContinuous && wait == WAIT_OBJECT_0 + 2) {
            heartbeat = now;
            continuousClock.recordSuspendState(true, now, awakeNow);
        } else if (isContinuous && wait == WAIT_OBJECT_0 + 3) {
            heartbeat = now;
            continuousClock.recordSuspendState(false, now, awakeNow);
        }
        {
            Guard guard(backend.operation.h);
            Record current{};
            require(backend.store.read(current), "helper journal missing");
            if (!IsEqualGUID(current.nonce, expected) || current.phase == Phase::Restored) return 0;
        }
        const bool pulseTimedOut = isContinuous ? continuousClock.heartbeatTimedOut(now, awakeNow)
                                                : (now - heartbeat >= 5000);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_FAILED || pulseTimedOut ||
            now - backend.record.started >= maximumMs(backend.record)) {
            const DWORD trigger = wait == WAIT_OBJECT_0 ? 2u : (pulseTimedOut ? 3u : 4u);
            return backend.recover(&expected, trigger) ? 0 : 1;
        }
    }
}
#ifdef DPP_PROTECTED_TESTS
struct JournalTest : testing::Test {
    std::wstring directory;
    std::unique_ptr<Store> store;
    Record record{};
    void SetUp() override {
        wchar_t temp[MAX_PATH]{};
        ASSERT_NE(GetTempPathW(MAX_PATH, temp), 0u);
        GUID nonce{};
        ASSERT_TRUE(SUCCEEDED(CoCreateGuid(&nonce)));
        directory = std::wstring(temp) + L"DPP-journal-test-" + guid(nonce);
        store = std::make_unique<Store>(directory);
        record.session = store->id.session;
        record.login = store->id.login;
        std::copy(store->id.sid.begin(), store->id.sid.end(), record.sid.begin());
        record.nonce = nonce;
    }
    void TearDown() override {
        DeleteFileW(store->path.c_str());
        DeleteFileW((store->path + guid(record.nonce) + L".recovered").c_str());
        RemoveDirectoryW(directory.c_str());
    }
};
TEST_F(JournalTest, DurableRoundtripAndArchive) {
    Record read{};
    EXPECT_FALSE(store->read(read));
    store->write(record);
    ASSERT_TRUE(store->read(read));
    EXPECT_TRUE(IsEqualGUID(record.nonce, read.nonce));
    record.phase = Phase::Armed;
    store->write(record);
    ASSERT_TRUE(store->read(read));
    EXPECT_EQ(read.phase, Phase::Armed);
    record.phase = Phase::Restored;
    store->write(record);
    store->archive(record);
    EXPECT_FALSE(store->read(read));
}
TEST_F(JournalTest, BothBoundedPoliciesRecoverableUnknownPolicyRejected) {
    Record read{};
    store->write(record);
    ASSERT_TRUE(store->read(read));
    EXPECT_EQ(maximumMs(read), 20000u);
    record.version = 3;
    store->write(record);
    ASSERT_TRUE(store->read(read));
    EXPECT_EQ(maximumMs(read), 180000u);
    record.version = 4;
    store->write(record);
    ASSERT_TRUE(store->read(read));
    EXPECT_EQ(maximumMs(read), 43200000u);
    record.version = 5;
    store->write(record);
    EXPECT_THROW(store->read(read), std::runtime_error);
}
TEST_F(JournalTest, CorruptOrTruncatedJournalRetained) {
    store->write(record);
    {
        Handle file(
            CreateFileW(store->path.c_str(), GENERIC_WRITE, 0, nullptr, TRUNCATE_EXISTING, 0, nullptr));
        ASSERT_NE(file.h, INVALID_HANDLE_VALUE);
    }
    Record read{};
    EXPECT_THROW(store->read(read), std::runtime_error);
    EXPECT_NE(GetFileAttributesW(store->path.c_str()), INVALID_FILE_ATTRIBUTES);
}
TEST_F(JournalTest, RejectOtherLoginAndArbitraryMask) {
    record.login.LowPart ^= 1;
    store->write(record);
    Record read{};
    EXPECT_THROW(store->read(read), std::runtime_error);
    record.login = store->id.login;
    record.changedMask |= 0x10;
    store->write(record);
    EXPECT_THROW(store->read(read), std::runtime_error);
}
TEST_F(JournalTest, RejectAlreadyHiddenOriginal) {
    record.original = 0;
    store->write(record);
    Record read{};
    EXPECT_THROW(store->read(read), std::runtime_error);
}
TEST_F(JournalTest, CommitFailureDoesNotReplaceIntent) {
    store->write(record);
    Handle held(CreateFileW(store->path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr));
    ASSERT_NE(held.h, INVALID_HANDLE_VALUE);
    record.phase = Phase::Armed;
    EXPECT_THROW(store->write(record), std::runtime_error);
    // Failed temporary records are retained; no visibility call is part of this test.
}
#endif
}  // namespace
struct ProtectedSession::Impl {
    Backend backend;
    SessionCoordinator coordinator{backend};
};
ProtectedSession::ProtectedSession() : impl_(std::make_unique<Impl>()) {}
ProtectedSession::~ProtectedSession() = default;
bool ProtectedSession::begin(std::uintptr_t expectedList, bool startupExperiment, bool continuousSession) {
    impl_->backend.startupExperiment = startupExperiment;
    impl_->backend.expectedList = expectedList;
    impl_->backend.requestedRecordVersion = continuousSession ? 4 : 3;
    return impl_->coordinator.start();
}
bool ProtectedSession::isContinuous() const noexcept { return impl_ && impl_->backend.record.version == 4; }
bool ProtectedSession::active() const noexcept { return impl_->coordinator.state() == SessionState::Active; }
std::optional<SessionTiming> ProtectedSession::timing() const {
    const auto& r = impl_->backend.record;
    if (!r.started || IsEqualGUID(r.nonce, GUID{})) return std::nullopt;
    const auto id = guid(r.nonce);
    std::string ascii;
    for (const wchar_t ch : id) ascii.push_back(static_cast<char>(ch));  // GUID is ASCII.
    return SessionTiming{ascii, r.started, r.started + maximumMs(r) - 2000, r.started + maximumMs(r),
                         static_cast<int>(impl_->coordinator.state())};
}
bool ProtectedSession::heartbeat() noexcept {
    if (!active()) return false;
    try {
        auto& b = impl_->backend;
        // Stop before the independently enforced helper deadline. Mode changes never extend it.
        if (GetTickCount64() - b.record.started >= maximumMs(b.record) - 2000 ||
            WaitForSingleObject(b.helper.h, 0) != WAIT_TIMEOUT)
            return false;
        const HWND hwnd = target(b.record, true);
        if (b.record.phase == Phase::Hidden && (ownVisible(hwnd) || IsWindowVisible(hwnd) != FALSE)) {
            (void)ShowWindowAsync(hwnd, SW_HIDE);
            flushShellDesktopRepaint(b.record);
        }
        return SetEvent(b.pulse.h) != FALSE;
    } catch (...) {
        return false;
    }
}
void ProtectedSession::notifyPowerSuspend(bool suspending) noexcept {
    if (!active() || !isContinuous()) return;
    auto& b = impl_->backend;
    if (suspending) {
        if (b.sleepEvent.h) (void)SetEvent(b.sleepEvent.h);
    } else {
        if (b.wakeEvent.h) (void)SetEvent(b.wakeEvent.h);
        if (b.pulse.h) (void)SetEvent(b.pulse.h);
    }
}
unsigned ProtectedSession::remainingSeconds() const noexcept {
    if (!active()) return 0;
    const auto& r = impl_->backend.record;
    const auto elapsed = GetTickCount64() - r.started;
    return elapsed >= maximumMs(r) ? 0 : static_cast<unsigned>((maximumMs(r) - elapsed + 999) / 1000);
}
bool ProtectedSession::stop() noexcept { return impl_->coordinator.stop(); }

void tryAutoRecoverStaleSession() noexcept {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    try {
        Backend backend;
        if (backend.lock()) {
            try {
                (void)backend.recoverResidual();
            } catch (...) {
                // If Explorer restarted/rebooted since a prior crash and current desktop icons are
                // already visible, archive the stale record so a fresh session is not blocked.
                try {
                    const Record live = inspectTarget();
                    (void)live;
                    Guard guard(backend.operation.h);
                    Record stale{};
                    if (backend.store.read(stale)) backend.store.archive(stale);
                } catch (...) {
                }
            }
            ReleaseMutex(backend.instance.h);
        }
    } catch (...) {
    }
    if (SUCCEEDED(com)) CoUninitialize();
}

int run(int argc, wchar_t** argv) {
    if (argc == 1 || (argc == 2 && std::wstring(argv[1]) == L"--help")) {
        std::cout << "DPP protected visibility experiment (no mirrors/physics)\n"
                     "--audit (read-only public Shell preconditions)\n"
                     "--compat-audit (read-only Explorer target inspection)\n"
                     "--prepare (verify helper lifecycle without hiding)\n"
                     "--compat-experiment (explicit local experiment, Esc restores)\n"
                     "--recover (independent recovery) --status (local record)\n";
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) return 1;
    int result = 1;
    try {
        const std::wstring mode(argv[1]);
        if (argc == 2 && mode == L"--audit") {
            View view;
            std::cout << "current_folder_flags=0x" << std::hex << view.get() << std::dec << '\n';
            view.customPosition();
            result = 0;
        } else if (argc == 2 && mode == L"--compat-audit") {
            const auto r = inspectTarget();
            std::cout << "compatibility_target_verified=true original_visible=true "
                         "backend=listview_compatibility\n";
            std::cout << "shell_flags_unchanged=0x" << std::hex << r.originalShellFlags << std::dec << "\n";
            result = 0;
        } else if (argc == 2 && mode == L"--prepare") {
            Backend backend;
            backend.preparationOnly = true;
            SessionCoordinator coordinator(backend);
            if (coordinator.start() && coordinator.stop()) {
                require(WaitForSingleObject(backend.helper.h, 3000) == WAIT_OBJECT_0,
                        "helper did not finish prepared session");
                DWORD code{};
                require(GetExitCodeProcess(backend.helper.h, &code) && code == 0,
                        "helper preparation failed");
                std::cout << "helper_clean_exit=true no_visibility_change=true\n";
                result = 0;
            }
        } else if (argc == 2 && mode == L"--status") {
            Backend backend;
            Guard guard(backend.operation.h);
            Record r{};
            if (backend.store.read(r)) {
                std::wcout << L"session_id=" << guid(r.nonce) << L" phase=" << static_cast<DWORD>(r.phase)
                           << L" result=" << r.result << L" trigger=" << r.recoveryTrigger << L"\n";
            } else
                std::cout << "no journal\n";
            result = 0;
        } else if (argc == 2 && mode == L"--recover") {
            Backend backend;
            result = backend.recover() ? 0 : 1;
        } else if (argc == 3 && mode == L"--helper") {
            Backend backend;
            result = helperMain(backend, argv[2]);
#ifdef DPP_RECOVERY_TEST_HOOKS
        } else if (argc == 2 && mode == L"--test-identity-guards") {
            const Record good = inspectTarget();
            for (int i = 0; i < 3; ++i) {
                Record changed = good;
                if (i == 0) changed.hostCreated ^= 1;
                if (i == 1) changed.parent = changed.root;
                if (i == 2) changed.view = changed.parent;
                bool rejected = false;
                try {
                    target(changed, false);
                } catch (...) {
                    rejected = true;
                }
                require(rejected, "identity guard accepted changed host/parent/class");
            }
            require(ownVisible(target(good, false)), "original visibility changed during read-only guards");
            std::cout << "identity_guards_passed=true visibility_mutated=false\n";
            result = 0;
        } else if (argc == 2 && mode == L"--test-stale-session") {
            Backend backend;
            backend.preparationOnly = true;
            SessionCoordinator coordinator(backend);
            require(coordinator.start(), "stale session test preparation failed");
            Record before{}, after{};
            {
                Guard guard(backend.operation.h);
                require(backend.store.read(before), "prepared record missing");
            }
            GUID stale{};
            hr(CoCreateGuid(&stale), "stale nonce");
            require(backend.recover(&stale), "stale recovery guard failed");
            {
                Guard guard(backend.operation.h);
                require(backend.store.read(after), "prepared record lost");
            }
            require(before.checksum == after.checksum && after.phase == Phase::Prepared,
                    "stale recovery changed new session");
            require(coordinator.stop(), "prepared session cleanup failed");
            require(WaitForSingleObject(backend.helper.h, 3000) == WAIT_OBJECT_0,
                    "prepared helper not closed");
            std::cout << "stale_session_rejected=true new_record_unchanged=true visibility_mutated=false\n";
            result = 0;
        } else if (argc == 2 && (mode == L"--test-hang" || mode == L"--test-deadline" ||
                                 mode == L"--test-init-failure" || mode == L"--test-helper-failure")) {
            Backend backend;
            backend.failHelperLaunch = mode == L"--test-helper-failure";
            backend.failAfterHide = mode == L"--test-init-failure";
            SessionCoordinator coordinator(backend);
            const bool started = coordinator.start();
            if (mode == L"--test-deadline") {
                require(started, "deadline test did not enter hidden state");
                const auto limit = GetTickCount64() + 25000;
                while (GetTickCount64() < limit && WaitForSingleObject(backend.helper.h, 200) == WAIT_TIMEOUT)
                    SetEvent(backend.pulse.h);
                require(coordinator.stop(), "deadline test recovery failed");
            } else if (mode == L"--test-hang") {
                require(started, "hang test did not enter hidden state");
                std::cout << "test_owner_alive_no_heartbeat=true\n" << std::flush;
                Sleep(8000);  // deliberately stall only this test owner's UI, outside the recovery lock
                require(coordinator.stop(), "hang test recovery failed");
            } else
                require(!started && coordinator.state() == SessionState::Idle,
                        "injected failure was not rolled back");
            if (mode == L"--test-init-failure")
                require(backend.injectedHidden, "initialization fault was not reached");
            if (mode == L"--test-helper-failure")
                require(backend.injectedHelperLaunchError == ERROR_FILE_NOT_FOUND,
                        "expected helper CreateProcess failure was not reached");
            Record r{};
            require(backend.store.read(r) && r.phase == Phase::Restored && r.result == 0,
                    "test recovery record missing");
            if (mode == L"--test-deadline")
                require(r.recoveryTrigger == 4, "independent helper deadline not observed");
            if (mode == L"--test-hang")
                require(r.recoveryTrigger == 3, "hang test did not recover through heartbeat timeout");
            require(ownVisible(target(r, false)), "test original visibility not restored");
            std::cout << "test_passed=true restored_record=true trigger=" << r.recoveryTrigger << "\n";
            result = 0;
#endif
        } else if (argc == 2 && mode == L"--compat-experiment") {
            require(RegisterHotKey(nullptr, 1, MOD_NOREPEAT, VK_ESCAPE) != FALSE,
                    "emergency Escape hotkey unavailable");
            Backend backend;
            SessionCoordinator coordinator(backend);
            const HWND initialForeground = GetForegroundWindow();
            if (coordinator.start()) {
                const auto started = GetTickCount64();
                bool stop = false;
                while (!stop && GetTickCount64() - started < 15000) {
                    SetEvent(backend.pulse.h);
                    MsgWaitForMultipleObjects(0, nullptr, FALSE, 200, QS_ALLINPUT);
                    MSG message{};
                    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                        if (message.message == WM_HOTKEY || message.message == WM_QUIT ||
                            message.message == WM_DISPLAYCHANGE)
                            stop = true;
                        TranslateMessage(&message);
                        DispatchMessageW(&message);
                    }
                    if (WaitForSingleObject(backend.helper.h, 0) != WAIT_TIMEOUT) stop = true;
                    RECT work{};
                    if (GetForegroundWindow() != initialForeground || GetSystemMetrics(SM_CMONITORS) != 1 ||
                        GetSystemMetrics(SM_CXSCREEN) != 1920 || GetSystemMetrics(SM_CYSCREEN) != 1080 ||
                        GetDpiForWindow(reinterpret_cast<HWND>(backend.record.parent)) != 120 ||
                        !SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0) || work.left != 0 ||
                        work.top != 0 || work.right != 1920 || work.bottom != 1030)
                        stop = true;
                }
                result = coordinator.stop() ? 0 : 1;
            } else {
                std::cerr << "start failed: " << coordinator.failure() << '\n';
            }
            UnregisterHotKey(nullptr, 1);
        } else {
            std::cerr << "invalid arguments\n";
            result = 2;
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << "; no recovery success claimed\n";
    }
    CoUninitialize();
    return result;
}
}  // namespace dpp::winprotected
