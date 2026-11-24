#pragma once
// clang-format off
#include <windows.h>
#include <objbase.h>
// clang-format on
#include <shlobj.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace dpp::winprobe {
struct Options {
    bool help = false;
    int samples = 0;
    std::wstring report;
};
Options parseOptions(const std::vector<std::wstring>& args);
const char* helpText();

struct PidlFree {
    using pointer = PITEMID_CHILD;
    void operator()(pointer p) const { CoTaskMemFree(p); }
};
struct BitmapFree {
    void operator()(HBITMAP p) const {
        if (p) DeleteObject(p);
    }
};
using Pidl = std::unique_ptr<ITEMIDLIST, PidlFree>;
using Bitmap = std::unique_ptr<std::remove_pointer_t<HBITMAP>, BitmapFree>;

// Deliberately not POINT/Vec2: a Shell view anchor cannot be passed as a screen
// or physics point. No Shell->client/screen conversion is asserted here.
struct ShellItemPosition {
    LONG x, y;
};
enum class Confirmation { Unchecked, Matches, Differs };
struct Sample {
    std::string id;          // snapshot UUID + local label; NEVER stable across snapshots
    std::wstring localName;  // local UI only; never serialized
    Pidl child;              // same retained child used for name, image and position
    Bitmap bitmap;
    std::optional<HRESULT> itemHr, createHr, nameHr, factoryHr, imageHr, positionHr, attributesHr;
    std::optional<ShellItemPosition> position;
    std::optional<bool> filesystemItem;
    Confirmation identity = Confirmation::Unchecked;
    Confirmation image = Confirmation::Unchecked;
    Confirmation layout = Confirmation::Unchecked;
};
void setPosition(Sample& sample, HRESULT hr, POINT value);

struct Environment {
    std::optional<DWORD> session, shellSession;
    std::optional<bool> stationVisible, inputDesktop;
    std::optional<HWND> shellWindow;
    std::vector<std::pair<std::string, DWORD>> errors;
};
Environment inspectEnvironment();
bool interactive(const Environment& environment);
struct DesktopView {
    Microsoft::WRL::ComPtr<IShellView> shell;
    Microsoft::WRL::ComPtr<IFolderView> view;
    Microsoft::WRL::ComPtr<IShellFolder> folder;
    HRESULT hr = E_PENDING;
    const char* stage = "not_attempted";
};
DesktopView openDesktopView();

struct Monitor {
    std::optional<RECT> bounds, work;
    std::optional<bool> primary;
    std::optional<unsigned> scalePercent;
    std::optional<HRESULT> scaleHr;
    std::optional<DWORD> infoError;
};
struct Report {
    std::string snapshot;
    std::string capturedAtUtc;
    Environment environment;
    std::optional<DWORD> osMajor, osMinor, osBuild, osRevision;
    std::string nativeArchitecture = "unknown", programArchitecture = "unknown";
    std::optional<bool> remoteSession;
    bool dpiSet = false;
    DWORD dpiSetError = 0;
    std::string threadDpi = "unknown", viewDpiContext = "unknown";
    std::optional<unsigned> viewDpi, diagnosticDpi;
    std::optional<RECT> viewClientRect, viewWindowScreenRect;
    std::optional<HWND> viewWindow;
    std::vector<Monitor> monitors;
    std::optional<DWORD> monitorError;
    std::optional<int> itemCount;
    std::optional<HRESULT> shellHr;
    std::string shellStage = "not_attempted";
    std::vector<Sample> samples;
    bool fullSnapshot = false;  // explicit bounded all-item capture for protected sessions
    bool displayChanged = false;
    bool userSaved = false;
};
// One fresh read, no UI or file write. Retains the same Shell folder and child PIDLs.
int captureSamples(Report& report, int sampleCount, DesktopView& desktop);
int captureAll(Report& report, DesktopView& desktop);
std::string encodeReport(const Report& report);
// CREATE_NEW: a prior report can never be replaced, even if its name is reused.
DWORD writeNewReport(const std::wstring& path, const std::string& content);
int runSamples(const Options& options, const Environment& environment, bool dpiSet, DWORD dpiError);
}  // namespace dpp::winprobe
