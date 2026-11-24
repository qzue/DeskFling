#include "probe_samples.h"

#include <exdisp.h>
#include <servprov.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <shlguid.h>

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
namespace dpp::winprobe {
namespace {
std::string quote(const std::string& s) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : s) {
        if (c == '"' || c == '\\')
            out << '\\' << c;
        else if (c < 32)
            out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c);
        else
            out << c;
    }
    return out.str() + '"';
}
std::string hexCode(unsigned long value) {
    std::ostringstream out;
    out << "0x" << std::hex << std::setw(8) << std::setfill('0') << value;
    return out.str();
}
template <class T>
std::string number(const std::optional<T>& value) {
    return value ? std::to_string(*value) : "null";
}
std::string boolean(std::optional<bool> value) { return value ? (*value ? "true" : "false") : "null"; }
std::string result(std::optional<HRESULT> hr) {
    return hr ? quote(hexCode(static_cast<unsigned long>(*hr))) : "null";
}
std::string rect(std::optional<RECT> r) {
    if (!r) return "null";
    return "[" + std::to_string(r->left) + "," + std::to_string(r->top) + "," + std::to_string(r->right) +
           "," + std::to_string(r->bottom) + "]";
}
std::string hwnd(std::optional<HWND> w) {
    if (!w) return "null";
    std::ostringstream out;
    out << "0x" << std::hex << reinterpret_cast<std::uintptr_t>(*w);
    return quote(out.str());
}
const char* confirmation(Confirmation c) {
    switch (c) {
        case Confirmation::Matches:
            return "user_reports_match";
        case Confirmation::Differs:
            return "user_reports_mismatch";
        default:
            return "not_checked";
    }
}
bool succeeded(std::optional<HRESULT> hr) { return hr && SUCCEEDED(*hr); }
std::string awareness(DPI_AWARENESS_CONTEXT context) {
    if (!context) return "unknown";
    if (AreDpiAwarenessContextsEqual(context, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        return "per_monitor_v2";
    switch (GetAwarenessFromDpiAwarenessContext(context)) {
        case DPI_AWARENESS_UNAWARE:
            return "unaware";
        case DPI_AWARENESS_SYSTEM_AWARE:
            return "system_aware";
        case DPI_AWARENESS_PER_MONITOR_AWARE:
            return "per_monitor_aware";
        default:
            return "unknown";
    }
}
const char* architecture(WORD value) {
    switch (value) {
        case PROCESSOR_ARCHITECTURE_AMD64:
            return "x64";
        case PROCESSOR_ARCHITECTURE_INTEL:
            return "x86";
        case PROCESSOR_ARCHITECTURE_ARM64:
            return "arm64";
        default:
            return "unknown";
    }
}
void registryNumber(Report& r, const wchar_t* name, const char* stage, std::optional<DWORD>& target) {
    DWORD value = 0, size = sizeof(value);
    LSTATUS code = RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", name,
                                RRF_RT_REG_DWORD, nullptr, &value, &size);
    if (code == ERROR_SUCCESS)
        target = value;
    else
        r.environment.errors.emplace_back(stage, static_cast<DWORD>(code));
}
BOOL CALLBACK monitorCallback(HMONITOR monitor, HDC, LPRECT, LPARAM parameter) noexcept {
    auto& report = *reinterpret_cast<Report*>(parameter);
    try {
        Monitor record;
        MONITORINFO info{sizeof(info), {}, {}, 0};
        if (GetMonitorInfoW(monitor, &info)) {
            record.bounds = info.rcMonitor;
            record.work = info.rcWork;
            record.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
        } else
            record.infoError = GetLastError();
        DEVICE_SCALE_FACTOR scale{};
        record.scaleHr = GetScaleFactorForMonitor(monitor, &scale);
        // The API supplies a fallback even on failure; do NOT record that as measured.
        if (SUCCEEDED(*record.scaleHr)) record.scalePercent = static_cast<unsigned>(scale);
        report.monitors.push_back(record);
        return TRUE;
    } catch (...) {
        report.monitorError = ERROR_NOT_ENOUGH_MEMORY;
        return FALSE;
    }
}
void collectDisplay(Report& r, HWND viewWindow) {
    SYSTEM_INFO info{};
    GetNativeSystemInfo(&info);
    r.nativeArchitecture = architecture(info.wProcessorArchitecture);
#if defined(_M_X64)
    r.programArchitecture = "x64";
#elif defined(_M_ARM64)
    r.programArchitecture = "arm64";
#else
    r.programArchitecture = "unknown";
#endif
    registryNumber(r, L"CurrentMajorVersionNumber", "RegGetValue.major", r.osMajor);
    registryNumber(r, L"CurrentMinorVersionNumber", "RegGetValue.minor", r.osMinor);
    registryNumber(r, L"UBR", "RegGetValue.revision", r.osRevision);
    wchar_t build[32]{};
    DWORD length = sizeof(build);
    LSTATUS code = RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                                L"CurrentBuildNumber", RRF_RT_REG_SZ, nullptr, build, &length);
    if (code == ERROR_SUCCESS) {
        wchar_t* end = nullptr;
        const auto value = wcstoul(build, &end, 10);
        if (end != build && !*end)
            r.osBuild = value;
        else
            r.environment.errors.emplace_back("RegGetValue.build.parse", ERROR_INVALID_DATA);
    } else
        r.environment.errors.emplace_back("RegGetValue.build", static_cast<DWORD>(code));
    r.remoteSession = GetSystemMetrics(SM_REMOTESESSION) != 0;
    r.threadDpi = awareness(GetThreadDpiAwarenessContext());
    if (viewWindow) {
        r.viewWindow = viewWindow;
        r.viewDpiContext = awareness(GetWindowDpiAwarenessContext(viewWindow));
        const UINT dpi = GetDpiForWindow(viewWindow);
        if (dpi) r.viewDpi = dpi;
        RECT bounds{};
        if (GetClientRect(viewWindow, &bounds))
            r.viewClientRect = bounds;
        else
            r.environment.errors.emplace_back("GetClientRect.shell_view", GetLastError());
        if (GetWindowRect(viewWindow, &bounds))
            r.viewWindowScreenRect = bounds;
        else
            r.environment.errors.emplace_back("GetWindowRect.shell_view", GetLastError());
    }
    SetLastError(ERROR_SUCCESS);
    if (!EnumDisplayMonitors(nullptr, nullptr, monitorCallback, reinterpret_cast<LPARAM>(&r)) &&
        !r.monitorError)
        r.monitorError = GetLastError();
}
void readSample(DesktopView& desktop, int index, Sample& sample) {
    PITEMID_CHILD raw = nullptr;
    sample.itemHr = desktop.view->Item(index, &raw);
    sample.child.reset(raw);
    if (!succeeded(sample.itemHr)) return;
    if (!raw) {
        sample.itemHr = E_UNEXPECTED;
        return;
    }
    POINT position{};
    const HRESULT positionHr = desktop.view->GetItemPosition(sample.child.get(), &position);
    setPosition(sample, positionHr, position);
    SFGAOF attributes = SFGAO_FILESYSTEM;
    PCUITEMID_CHILD children[] = {sample.child.get()};
    sample.attributesHr = desktop.folder->GetAttributesOf(1, children, &attributes);
    if (succeeded(sample.attributesHr)) sample.filesystemItem = (attributes & SFGAO_FILESYSTEM) != 0;
    // Everything below is resolved from the SAME retained child and parent.
    // A filesystem path is never required, including for virtual Shell items.
    ComPtr<IShellItem> item;
    HRESULT hr =
        SHCreateItemWithParent(nullptr, desktop.folder.Get(), sample.child.get(), IID_PPV_ARGS(&item));
    sample.createHr = hr;
    if (SUCCEEDED(hr)) {
        PWSTR text = nullptr;
        sample.nameHr = item->GetDisplayName(SIGDN_NORMALDISPLAY, &text);
        std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> ownedText(text, &CoTaskMemFree);
        if (succeeded(sample.nameHr) && text)
            sample.localName = text;
        else if (succeeded(sample.nameHr))
            sample.nameHr = E_UNEXPECTED;
    }
    ComPtr<IShellItemImageFactory> image;
    if (SUCCEEDED(hr)) sample.factoryHr = item.As(&image);
    if (succeeded(sample.factoryHr)) {
        HBITMAP bitmap = nullptr;
        sample.imageHr = image->GetImage(SIZE{64, 64}, SIIGBF_ICONONLY, &bitmap);
        sample.bitmap.reset(bitmap);
        if (succeeded(sample.imageHr) && !bitmap) sample.imageHr = E_UNEXPECTED;
        if (!succeeded(sample.imageHr)) sample.bitmap.reset();
    }
}

Bitmap createFallbackIconDib(IShellFolder* folder, PCUITEMID_CHILD child) {
    HICON hIcon = nullptr;
    if (folder && child) {
        ComPtr<IPersistFolder2> persist;
        PIDLIST_ABSOLUTE parent = nullptr;
        if (SUCCEEDED(folder->QueryInterface(IID_PPV_ARGS(&persist))) &&
            SUCCEEDED(persist->GetCurFolder(&parent)) && parent) {
            PIDLIST_ABSOLUTE absPidl = ILCombine(parent, child);
            CoTaskMemFree(parent);
            if (absPidl) {
                SHFILEINFOW sfi{};
                if (SHGetFileInfoW(reinterpret_cast<LPCWSTR>(absPidl), 0, &sfi, sizeof(sfi),
                                   SHGFI_PIDL | SHGFI_ICON | SHGFI_LARGEICON) &&
                    sfi.hIcon) {
                    hIcon = sfi.hIcon;
                }
                CoTaskMemFree(absPidl);
            }
        }
    }
    bool destroyIconAfter = (hIcon != nullptr);
    if (!hIcon) {
        hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        destroyIconAfter = false;
    }
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = 64;
    bmi.bmiHeader.biHeight = -64;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    Bitmap dib(CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0));
    if (dib && bits) {
        std::memset(bits, 0, 64 * 64 * 4);
        HDC memDc = CreateCompatibleDC(nullptr);
        if (memDc) {
            HGDIOBJ oldBmp = SelectObject(memDc, dib.get());
            if (hIcon) {
                DrawIconEx(memDc, 0, 0, hIcon, 64, 64, 0, nullptr, DI_NORMAL);
            }
            SelectObject(memDc, oldBmp);
            DeleteDC(memDc);
        }
        // Ensure non-zero alpha if legacy 24-bit icon left alpha channel 0
        auto* px = static_cast<unsigned char*>(bits);
        bool anyAlpha = false;
        bool anyRgb = false;
        for (int i = 0; i < 64 * 64; ++i) {
            if (px[i * 4 + 3] != 0) anyAlpha = true;
            if (px[i * 4 + 0] != 0 || px[i * 4 + 1] != 0 || px[i * 4 + 2] != 0) anyRgb = true;
        }
        if (!anyAlpha) {
            for (int i = 0; i < 64 * 64; ++i) {
                if (anyRgb) {
                    if (px[i * 4 + 0] || px[i * 4 + 1] || px[i * 4 + 2]) px[i * 4 + 3] = 255;
                } else {
                    const int x = i % 64, y = i / 64;
                    if (x >= 10 && x < 54 && y >= 6 && y < 58) {
                        px[i * 4 + 0] = 235;
                        px[i * 4 + 1] = 238;
                        px[i * 4 + 2] = 245;
                        px[i * 4 + 3] = 240;
                    }
                }
            }
        }
    }
    if (destroyIconAfter && hIcon) DestroyIcon(hIcon);
    return dib;
}
}  // namespace

Options parseOptions(const std::vector<std::wstring>& args) {
    Options options;
    if (args.size() == 1 && args[0] == L"--help") {
        options.help = true;
        return options;
    }
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == L"--samples" && options.samples == 0 && i + 1 < args.size()) {
            const auto value = args[++i];
            if (value.size() != 1 || value[0] < L'3' || value[0] > L'5')
                throw std::invalid_argument("samples must be 3..5");
            options.samples = value[0] - L'0';
        } else if (args[i] == L"--report" && options.report.empty() && i + 1 < args.size()) {
            options.report = args[++i];
            if (options.report.empty()) throw std::invalid_argument("empty report path");
        } else
            throw std::invalid_argument("unknown, duplicate or missing argument");
    }
    if ((options.samples != 0) != !options.report.empty())
        throw std::invalid_argument("samples requires report");
    return options;
}
const char* helpText() {
    return "desktop_probe [--help]\n"
           "desktop_probe --samples 3|4|5 --report <NEW.json>\n"
           "No arguments: original read-only aggregate counts, no report or sample window.\n"
           "Samples: one snapshot of the first N Shell view entries; local names/icons and per-ID checks.\n"
           "Run in the normal logged-in desktop, never an SSH/service desktop.\n"
           "Report uses snapshot IDs, raw view positions and explicit human answers; no names, paths, PIDLs "
           "or images.\n"
           "Existing reports are never overwritten. Close without Save leaves answers unconfirmed.\n"
           "Exit: 0 snapshot/UI completed (NOT acceptance), 1 API/partial-read failure, 2 arguments,\n"
           "      3 noninteractive environment, 4 report write failure, 5 UI/display change.\n";
}
void setPosition(Sample& sample, HRESULT hr, POINT value) {
    sample.positionHr = hr;
    sample.position.reset();
    if (SUCCEEDED(hr)) sample.position = ShellItemPosition{value.x, value.y};
}
Environment inspectEnvironment() {
    Environment e;
    DWORD session = 0;
    if (ProcessIdToSessionId(GetCurrentProcessId(), &session))
        e.session = session;
    else
        e.errors.emplace_back("ProcessIdToSessionId.self", GetLastError());
    USEROBJECTFLAGS flags{};
    DWORD needed = 0;
    if (GetUserObjectInformationW(GetProcessWindowStation(), UOI_FLAGS, &flags, sizeof(flags), &needed))
        e.stationVisible = (flags.dwFlags & WSF_VISIBLE) != 0;
    else
        e.errors.emplace_back("GetUserObjectInformation.station", GetLastError());
    BOOL input = FALSE;
    if (GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_IO, &input, sizeof(input),
                                  &needed))
        e.inputDesktop = input != FALSE;
    else
        e.errors.emplace_back("GetUserObjectInformation.input_desktop", GetLastError());
    HWND shell = GetShellWindow();
    if (!shell) shell = FindWindowW(L"Progman", nullptr);
    if (shell) {
        e.shellWindow = shell;
        DWORD pid = 0;
        if (!GetWindowThreadProcessId(shell, &pid))
            e.errors.emplace_back("GetWindowThreadProcessId.shell", GetLastError());
        else if (ProcessIdToSessionId(pid, &session))
            e.shellSession = session;
        else
            e.errors.emplace_back("ProcessIdToSessionId.shell", GetLastError());
    } else
        e.errors.emplace_back("GetShellWindow", ERROR_NOT_FOUND);
    return e;
}
bool interactive(const Environment& e) {
    return e.stationVisible == true && e.inputDesktop == true && e.session && e.shellSession &&
           e.session == e.shellSession && e.shellWindow && *e.shellWindow;
}
DesktopView openDesktopView() {
    DesktopView d;
    ComPtr<IShellWindows> windows;
    ComPtr<IDispatch> dispatch;
    ComPtr<IServiceProvider> service;
    ComPtr<IShellBrowser> browser;
    auto step = [&](const char* stage, HRESULT hr) {
        d.stage = stage;
        d.hr = hr;
        return SUCCEEDED(hr);
    };
    if (!step("CoCreateInstance.ShellWindows",
              CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows))))
        return d;
    VARIANT location{}, root{};
    location.vt = VT_I4;
    location.lVal = 0;
    long desktop = 0;
    HRESULT findHr =
        windows->FindWindowSW(&location, &root, SWC_DESKTOP, &desktop, SWFO_NEEDDISPATCH, &dispatch);
    if ((!SUCCEEDED(findHr) || !dispatch) && windows) {
        // Retry once after nudging Progman on Win11 multi-monitor setups where Shell automation
        // initializes lazily on first desktop query.
        if (HWND progman = FindWindowW(L"Progman", nullptr)) {
            DWORD_PTR ignored = 0;
            SendMessageTimeoutW(progman, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 100, &ignored);
        }
        dispatch.Reset();
        findHr = windows->FindWindowSW(&location, &root, SWC_DESKTOP, &desktop, SWFO_NEEDDISPATCH, &dispatch);
    }
    if (!step("FindWindowSW.desktop", findHr)) return d;
    if (!dispatch) {
        step("FindWindowSW.null_dispatch", E_NOINTERFACE);
        return d;
    }
    if (!step("QueryInterface.service", dispatch.As(&service))) return d;
    if (!step("QueryService.ShellBrowser",
              service->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))))
        return d;
    if (!step("QueryActiveShellView", browser->QueryActiveShellView(&d.shell))) return d;
    if (!step("QueryInterface.FolderView", d.shell.As(&d.view))) return d;
    step("GetFolder", d.view->GetFolder(IID_PPV_ARGS(&d.folder)));
    return d;
}
std::string encodeReport(const Report& r) {
    std::ostringstream o;
    o << "{\n\"schema\":1,\"snapshot_id\":" << quote(r.snapshot)
      << ",\"captured_at_utc\":" << quote(r.capturedAtUtc)
      << ",\"identity_scope\":\"this_snapshot_only\",\"identity_basis\":\"retained_child_PIDL_in_same_"
         "IShellFolder\""
      << ",\"selection\":\"first_N_entries_of_actual_IFolderView\",\"user_saved\":" << boolean(r.userSaved)
      << ",\"display_changed\":" << boolean(r.displayChanged)
      << ",\"session_id\":" << number(r.environment.session)
      << ",\"shell_session_id\":" << number(r.environment.shellSession)
      << ",\"station_visible\":" << boolean(r.environment.stationVisible)
      << ",\"input_desktop\":" << boolean(r.environment.inputDesktop)
      << ",\"interactive_preconditions\":" << boolean(interactive(r.environment))
      << ",\"shell_host_hwnd\":" << hwnd(r.environment.shellWindow)
      << ",\"os\":{\"major\":" << number(r.osMajor) << ",\"minor\":" << number(r.osMinor)
      << ",\"build\":" << number(r.osBuild) << ",\"revision\":" << number(r.osRevision) << "}"
      << ",\"native_architecture\":" << quote(r.nativeArchitecture)
      << ",\"program_architecture\":" << quote(r.programArchitecture)
      << ",\"remote_session_metric\":" << boolean(r.remoteSession) << ",\"physical_or_virtual_machine\":null"
      << ",\"dpi_set_succeeded\":" << boolean(r.dpiSet) << ",\"dpi_set_win32_error\":" << r.dpiSetError
      << ",\"thread_dpi_awareness\":" << quote(r.threadDpi)
      << ",\"shell_view_dpi_awareness\":" << quote(r.viewDpiContext)
      << ",\"shell_view_dpi\":" << number(r.viewDpi)
      << ",\"diagnostic_window_dpi\":" << number(r.diagnosticDpi)
      << ",\"shell_view_hwnd\":" << hwnd(r.viewWindow)
      << ",\"shell_view_client_rect\":" << rect(r.viewClientRect)
      << ",\"shell_view_outer_screen_rect\":" << rect(r.viewWindowScreenRect)
      << ",\"rect_units\":\"Win32_API_coordinates_under_recorded_thread_DPI_context\""
      << ",\"monitors\":[";
    for (std::size_t i = 0; i < r.monitors.size(); ++i) {
        if (i) o << ',';
        const auto& m = r.monitors[i];
        o << "{\"monitor_id\":" << quote("M" + std::to_string(i + 1)) << ",\"screen_rect\":" << rect(m.bounds)
          << ",\"work_rect\":" << rect(m.work) << ",\"primary\":" << boolean(m.primary)
          << ",\"scale_percent\":" << number(m.scalePercent) << ",\"scale_hresult\":" << result(m.scaleHr)
          << ",\"monitor_info_win32_error\":" << number(m.infoError) << ",\"per_monitor_dpi\":null}";
    }
    o << "],\"monitor_enumeration_win32_error\":" << number(r.monitorError)
      << ",\"item_count\":" << number(r.itemCount) << ",\"shell_stage\":" << quote(r.shellStage)
      << ",\"shell_hresult\":" << result(r.shellHr) << ",\"environment_errors\":[";
    for (std::size_t i = 0; i < r.environment.errors.size(); ++i) {
        if (i) o << ',';
        o << "{\"stage\":" << quote(r.environment.errors[i].first)
          << ",\"win32_error\":" << r.environment.errors[i].second << "}";
    }
    o << "],\"samples\":[";
    for (std::size_t i = 0; i < r.samples.size(); ++i) {
        if (i) o << ',';
        const auto& s = r.samples[i];
        o << "{\"sample_id\":" << quote(s.id) << ",\"item_hresult\":" << result(s.itemHr)
          << ",\"create_shell_item_hresult\":" << result(s.createHr)
          << ",\"name_hresult\":" << result(s.nameHr) << ",\"image_factory_hresult\":" << result(s.factoryHr)
          << ",\"image_hresult\":" << result(s.imageHr) << ",\"position_hresult\":" << result(s.positionHr)
          << ",\"attributes_hresult\":" << result(s.attributesHr)
          << ",\"filesystem_item\":" << boolean(s.filesystemItem) << ",\"image_read_success\":"
          << boolean(s.imageHr ? std::optional<bool>(succeeded(s.imageHr) && bool(s.bitmap)) : std::nullopt)
          << ",\"image_source\":\"IShellItemImageFactory.GetImage_SIIGBF_ICONONLY\""
          << ",\"app_placeholder_used\":false,\"shell_generic_icon_status\":\"unknown\",\"raw_position\":";
        if (succeeded(s.positionHr) && s.position)
            o << '[' << s.position->x << ',' << s.position->y << ']';
        else
            o << "null";
        o << ",\"position_source\":\"IFolderView.GetItemPosition\",\"coordinate_space\":\"Shell_folder_view\""
          << ",\"units\":\"view_units_physical_vs_logical_unverified\",\"anchor\":\"item_upper_left_not_"
             "image_center\""
          << ",\"client_position\":null,\"screen_position\":null,\"scene_position\":null"
          << ",\"transforms_applied\":[],\"screen_alignment_verified\":false,\"image_bounds\":null,\"label_"
             "bounds\":null"
          << ",\"identity_confirmation\":"
          << quote(confirmation(r.userSaved && !r.displayChanged && succeeded(s.itemHr) && succeeded(s.nameHr)
                                    ? s.identity
                                    : Confirmation::Unchecked))
          << ",\"icon_confirmation\":"
          << quote(confirmation(r.userSaved && !r.displayChanged && succeeded(s.imageHr) && s.bitmap
                                    ? s.image
                                    : Confirmation::Unchecked))
          << ",\"relative_layout_confirmation\":"
          << quote(confirmation(r.userSaved && !r.displayChanged && succeeded(s.positionHr) && s.position
                                    ? s.layout
                                    : Confirmation::Unchecked))
          << "}";
    }
    const auto successfulItems = std::count_if(r.samples.begin(), r.samples.end(),
                                               [](const Sample& s) { return succeeded(s.itemHr); });
    const auto successfulPositions = std::count_if(r.samples.begin(), r.samples.end(), [](const Sample& s) {
        return succeeded(s.positionHr) && s.position;
    });
    const auto successfulImages = std::count_if(
        r.samples.begin(), r.samples.end(), [](const Sample& s) { return succeeded(s.imageHr) && s.bitmap; });
    o << "],\"sample_attempts\":" << r.samples.size() << ",\"sample_item_successes\":" << successfulItems
      << ",\"sample_item_failures\":" << (r.samples.size() - static_cast<std::size_t>(successfulItems))
      << ",\"sample_position_successes\":" << successfulPositions
      << ",\"sample_image_successes\":" << successfulImages
      << ",\"machine_verified_human_correspondence\":false,\"real_desktop_mode\":\"DISABLED\"}\n";
    return o.str();
}
DWORD writeNewReport(const std::wstring& path, const std::string& content) {
    if (content.size() > MAXDWORD) return ERROR_FILE_TOO_LARGE;
    HANDLE file =
        CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD written = 0;
    DWORD error = ERROR_SUCCESS;
    if (!WriteFile(file, content.data(), static_cast<DWORD>(content.size()), &written, nullptr))
        error = GetLastError();
    else if (written != content.size())
        error = ERROR_WRITE_FAULT;
    if (!error && !FlushFileBuffers(file)) error = GetLastError();
    if (!CloseHandle(file) && !error) error = GetLastError();
    return error;
}
namespace {
struct SampleWindow {
    Report& report;
    std::size_t current = 0;
    HWND name = nullptr, details = nullptr, heading = nullptr;
    HWND choices[3]{};
    HFONT font = nullptr;
    bool failed = false;
    void store() {
        auto& s = report.samples.at(current);
        Confirmation* values[] = {&s.identity, &s.image, &s.layout};
        for (int i = 0; i < 3; ++i) {
            const auto selected = SendMessageW(choices[i], CB_GETCURSEL, 0, 0);
            *values[i] = selected == 1   ? Confirmation::Matches
                         : selected == 2 ? Confirmation::Differs
                                         : Confirmation::Unchecked;
        }
    }
    void show(HWND w) {
        auto& s = report.samples.at(current);
        const std::wstring title =
            L"S" + std::to_wstring(current + 1) + L" / " + std::to_wstring(report.samples.size());
        SetWindowTextW(heading, title.c_str());
        SetWindowTextW(name,
                       s.localName.empty() ? L"名称读取失败（不使用替代名称猜测对应）" : s.localName.c_str());
        std::wstring text = L"原始位置：";
        if (s.position)
            text += L"(" + std::to_wstring(s.position->x) + L", " + std::to_wstring(s.position->y) + L")";
        else
            text += L"未知 / 读取失败";
        text +=
            L"\r\n来源：IFolderView::GetItemPosition；Shell "
            L"视图内项目左上锚点。\r\n单位及屏幕映射未验证，不是图像中心。";
        text += s.bitmap ? L"\r\n图像：Shell 图标；未使用应用占位图，通用类型图标状态未知。"
                         : L"\r\n图像读取失败，无占位图。";
        if (s.positionHr) {
            auto code = hexCode(static_cast<unsigned long>(*s.positionHr));
            text += L"\r\n位置 HRESULT：" + std::wstring(code.begin(), code.end());
        }
        if (s.imageHr) {
            auto code = hexCode(static_cast<unsigned long>(*s.imageHr));
            text += L"；图像 HRESULT：" + std::wstring(code.begin(), code.end());
        }
        SetWindowTextW(details, text.c_str());
        const Confirmation values[] = {s.identity, s.image, s.layout};
        const bool available[] = {succeeded(s.itemHr) && succeeded(s.nameHr), bool(s.bitmap),
                                  s.position.has_value()};
        for (int i = 0; i < 3; ++i) {
            SendMessageW(choices[i], CB_SETCURSEL, static_cast<WPARAM>(values[i]), 0);
            EnableWindow(choices[i], available[i]);
        }
        EnableWindow(GetDlgItem(w, 10), current > 0);
        EnableWindow(GetDlgItem(w, 11), current + 1 < report.samples.size());
        InvalidateRect(w, nullptr, TRUE);
    }
    HWND control(HWND parent, const wchar_t* kind, const wchar_t* text, DWORD style, int x, int y, int width,
                 int height, int id = 0) {
        HWND child = CreateWindowExW(0, kind, text, WS_CHILD | WS_VISIBLE | style, x, y, width, height,
                                     parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                     GetModuleHandleW(nullptr), nullptr);
        if (!child) throw std::runtime_error("sample control creation failed");
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return child;
    }
    void create(HWND w) {
        font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        if (!font) throw std::runtime_error("sample font creation failed");
        control(w, L"STATIC", L"只读样本核对：名称和图像仅在本机显示。请逐项对照现有桌面，勿移动文件。", 0,
                15, 12, 680, 25);
        heading = control(w, L"STATIC", L"", 0, 15, 45, 90, 25);
        name = control(w, L"STATIC", L"", SS_NOPREFIX, 115, 45, 565, 45);
        details = control(w, L"STATIC", L"", SS_NOPREFIX, 115, 95, 565, 155);
        const wchar_t* labels[] = {L"该编号与桌面项目对应", L"图像与该项目图标相符",
                                   L"原始位置的相对布局无明显矛盾"};
        for (int i = 0; i < 3; ++i) {
            control(w, L"STATIC", labels[i], 0, 15, 260 + 38 * i, 350, 25);
            choices[i] = control(w, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 380, 255 + 38 * i, 260,
                                 160, 20 + i);
            for (const auto* label : {L"未核对 / 无法确定", L"是（人工确认）", L"否（不一致）"})
                SendMessageW(choices[i], CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
        }
        control(w, L"STATIC",
                L"报告只保存 S 编号、原始坐标、API "
                L"结果和你的选择；不保存名称、路径或图像。\r\n请勿在核对期间调整桌面布局。关闭叉号不确认，保"
                L"存也不代表屏幕精确对齐。",
                0, 15, 380, 680, 48);
        control(w, L"BUTTON", L"上一项", WS_TABSTOP, 15, 440, 110, 32, 10);
        control(w, L"BUTTON", L"下一项", WS_TABSTOP, 140, 440, 110, 32, 11);
        control(w, L"BUTTON", L"保存结果并关闭", WS_TABSTOP, 440, 440, 200, 32, 12);
        const auto dpi = GetDpiForWindow(w);
        if (dpi) report.diagnosticDpi = dpi;
        show(w);
    }
    static LRESULT CALLBACK procedure(HWND w, UINT message, WPARAM wp, LPARAM lp) noexcept {
        auto* self = reinterpret_cast<SampleWindow*>(GetWindowLongPtrW(w, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<SampleWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(w, message, wp, lp);
        try {
            switch (message) {
                case WM_CREATE:
                    self->create(w);
                    return 0;
                case WM_COMMAND:
                    if (LOWORD(wp) == 10 || LOWORD(wp) == 11) {
                        self->store();
                        if (LOWORD(wp) == 10 && self->current > 0) --self->current;
                        if (LOWORD(wp) == 11 && self->current + 1 < self->report.samples.size())
                            ++self->current;
                        self->show(w);
                    } else if (LOWORD(wp) == 12) {
                        self->store();
                        self->report.userSaved = true;
                        DestroyWindow(w);
                    }
                    return 0;
                case WM_PAINT: {
                    PAINTSTRUCT ps{};
                    HDC dc = BeginPaint(w, &ps);
                    auto& s = self->report.samples.at(self->current);
                    if (s.bitmap) {
                        HDC memory = CreateCompatibleDC(dc);
                        if (memory) {
                            const auto old = SelectObject(memory, s.bitmap.get());
                            BITMAP bits{};
                            if (GetObjectW(s.bitmap.get(), sizeof(bits), &bits) && bits.bmWidth > 0 &&
                                bits.bmHeight > 0) {
                                BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
                                AlphaBlend(dc, 20, 100, 64, 64, memory, 0, 0, bits.bmWidth, bits.bmHeight,
                                           blend);
                            }
                            SelectObject(memory, old);
                            DeleteDC(memory);
                        }
                    }
                    EndPaint(w, &ps);
                    return 0;
                }
                case WM_DISPLAYCHANGE:
                case WM_DPICHANGED:
                    self->report.displayChanged = true;
                    DestroyWindow(w);
                    return 0;
                case WM_CLOSE:
                    DestroyWindow(w);
                    return 0;
                case WM_DESTROY:
                    PostQuitMessage(0);
                    return 0;
            }
        } catch (...) {
            self->failed = true;
            DestroyWindow(w);
            return 0;
        }
        return DefWindowProcW(w, message, wp, lp);
    }
    bool run() {
        WNDCLASSW wc{};
        wc.lpfnWndProc = procedure;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"DPPReadOnlyShellSamples";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        if (!RegisterClassW(&wc)) return false;
        RECT bounds{0, 0, 710, 490};
        constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        AdjustWindowRectEx(&bounds, style, FALSE, 0);
        HWND w = CreateWindowExW(0, wc.lpszClassName, L"DPP 只读 Shell 样本（非桌面覆盖层）", style,
                                 CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left,
                                 bounds.bottom - bounds.top, nullptr, nullptr, wc.hInstance, this);
        if (w) {
            ShowWindow(w, SW_SHOWNORMAL);
            MSG message{};
            BOOL status;
            while ((status = GetMessageW(&message, nullptr, 0, 0)) > 0) {
                if (!IsDialogMessageW(w, &message)) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            }
            if (status < 0) failed = true;
            if (IsWindow(w)) DestroyWindow(w);
        } else
            failed = true;
        if (font) DeleteObject(font);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return !failed;
    }
};
}  // namespace
static int captureItems(Report& report, int sampleCount, DesktopView& desktop, bool full) {
    if ((!full && (sampleCount < 3 || sampleCount > 5)) || !report.samples.empty()) return 2;
    report.fullSnapshot = full;
    const auto& environment = report.environment;
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) return 1;
    wchar_t uuid[40]{};
    StringFromGUID2(guid, uuid, 40);
    for (const wchar_t* p = uuid; *p; ++p) report.snapshot += static_cast<char>(*p);
    SYSTEMTIME captured{};
    GetSystemTime(&captured);
    std::ostringstream timestamp;
    timestamp << std::setfill('0') << std::setw(4) << captured.wYear << '-' << std::setw(2) << captured.wMonth
              << '-' << std::setw(2) << captured.wDay << 'T' << std::setw(2) << captured.wHour << ':'
              << std::setw(2) << captured.wMinute << ':' << std::setw(2) << captured.wSecond << 'Z';
    report.capturedAtUtc = timestamp.str();
    int code = 0;
    if (!interactive(environment)) {
        report.shellStage = "environment_requires_same_interactive_desktop";
        std::cerr << "environment_error=requires_visible_input_desktop_and_same_session_shell; no Shell "
                     "query attempted\n";
        code = 3;
    } else {
        desktop = openDesktopView();
        report.shellHr = desktop.hr;
        report.shellStage = desktop.stage;
        HWND viewWindow = nullptr;
        if (SUCCEEDED(desktop.hr)) {
            report.shellStage = "IShellView.GetWindow";
            report.shellHr = desktop.shell->GetWindow(&viewWindow);
        }
        if (succeeded(report.shellHr) && !viewWindow) report.shellHr = E_UNEXPECTED;
        if (succeeded(report.shellHr)) {
            DWORD pid = 0, session = 0;
            if (!GetWindowThreadProcessId(viewWindow, &pid) || !ProcessIdToSessionId(pid, &session) ||
                session != environment.session) {
                report.shellStage = "Shell_view_session_mismatch_or_unknown";
                report.shellHr = E_ACCESSDENIED;
            }
        }
        collectDisplay(report, viewWindow);
        if (succeeded(report.shellHr)) {
            int count = 0;
            report.shellStage = "IFolderView.ItemCount";
            report.shellHr = desktop.view->ItemCount(SVGIO_ALLVIEW, &count);
            if (succeeded(report.shellHr) && count >= 0) {
                if (full) {
                    const int cappedCount = std::min(count, 512);
                    for (int i = 0; i < cappedCount; ++i) {
                        Sample sample;
                        readSample(desktop, i, sample);
                        if (!succeeded(sample.itemHr) || !sample.child || !succeeded(sample.positionHr) ||
                            !sample.position) {
                            continue;
                        }
                        bool dup = false;
                        for (const auto& previous : report.samples) {
                            if (previous.child && ILIsEqual(previous.child.get(), sample.child.get())) {
                                dup = true;
                                break;
                            }
                        }
                        if (dup) continue;
                        if (!succeeded(sample.nameHr) || sample.localName.empty()) {
                            sample.localName = L"桌面项目";
                            sample.nameHr = S_OK;
                        }
                        if (!succeeded(sample.imageHr) || !sample.bitmap) {
                            sample.bitmap = createFallbackIconDib(desktop.folder.Get(), sample.child.get());
                            if (sample.bitmap) sample.imageHr = S_OK;
                        }
                        if (!sample.bitmap) continue;
                        sample.id = report.snapshot + "/S" + std::to_string(report.samples.size() + 1);
                        report.samples.push_back(std::move(sample));
                    }
                    report.itemCount = static_cast<int>(report.samples.size());
                } else {
                    report.itemCount = count;
                    for (int i = 0; i < std::min(count, sampleCount); ++i) {
                        Sample sample;
                        sample.id = report.snapshot + "/S" + std::to_string(i + 1);
                        readSample(desktop, i, sample);
                        for (const auto& previous : report.samples) {
                            if (previous.child && sample.child &&
                                ILIsEqual(previous.child.get(), sample.child.get())) {
                                sample.itemHr = HRESULT_FROM_WIN32(ERROR_DUP_NAME);
                                sample.localName.clear();
                                sample.bitmap.reset();
                                sample.position.reset();
                                break;
                            }
                        }
                        report.samples.push_back(std::move(sample));
                    }
                }
            }
        }
        if (!succeeded(report.shellHr) || (!full && report.samples.size() < 3u)) code = 1;
        for (const auto& sample : report.samples)
            if (!succeeded(sample.itemHr) || !succeeded(sample.nameHr) || !succeeded(sample.imageHr) ||
                !succeeded(sample.positionHr))
                code = 1;
    }
    return code;
}
int captureSamples(Report& report, int sampleCount, DesktopView& desktop) {
    return captureItems(report, sampleCount, desktop, false);
}
int captureAll(Report& report, DesktopView& desktop) { return captureItems(report, 128, desktop, true); }
int runSamples(const Options& options, const Environment& environment, bool dpiSet, DWORD dpiError) {
    Report report;
    report.environment = environment;
    report.dpiSet = dpiSet;
    report.dpiSetError = dpiError;
    DesktopView desktop;
    int code = captureSamples(report, options.samples, desktop);
    if (!report.samples.empty()) {
        SampleWindow window{report};
        if (!window.run() || report.displayChanged) code = 5;
    }
    if (!report.userSaved || report.displayChanged) {
        report.userSaved = false;
        for (auto& s : report.samples) s.identity = s.image = s.layout = Confirmation::Unchecked;
    }
    const auto error = writeNewReport(options.report, encodeReport(report));
    if (error) {
        std::cerr << "report_write_win32_error=" << error << " (existing files are not replaced)\n";
        return 4;
    }
    std::cout << "snapshot_id=" << report.snapshot << " sample_attempts=" << report.samples.size()
              << " report_saved=true human_saved=" << report.userSaved << " exit_code=" << code << '\n';
    return code;
}
}  // namespace dpp::winprobe
