#pragma once
// clang-format off
#include <windows.h>
#include <shellapi.h>
// clang-format on

#include <utility>

#include "probe_samples.h"
namespace dpp::winmirror {
// Filesystem targets and simple filesystem shortcuts only. No virtual Shell targets.
// No child PIDL escapes its snapshot or is reinterpreted as an absolute PIDL.
struct OpenTarget {
    std::wstring path;
    BY_HANDLE_FILE_INFORMATION identity{};
    std::wstring linkPath;
    BY_HANDLE_FILE_INFORMATION linkIdentity{};
};
inline bool openIdentity(const std::wstring& path, BY_HANDLE_FILE_INFORMATION& identity) {
    HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const bool ok = GetFileInformationByHandle(file, &identity) &&
                    !(identity.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
    CloseHandle(file);
    return ok;
}
inline bool sameOpenIdentity(const BY_HANDLE_FILE_INFORMATION& a, const BY_HANDLE_FILE_INFORMATION& b) {
    return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber && a.nFileIndexHigh == b.nFileIndexHigh &&
           a.nFileIndexLow == b.nFileIndexLow;
}
inline std::optional<std::wstring> simpleLinkPath(const std::wstring& path) {
    if (path.size() < 4 || _wcsicmp(path.c_str() + path.size() - 4, L".lnk")) return std::nullopt;
    Microsoft::WRL::ComPtr<IShellLinkW> link;
    Microsoft::WRL::ComPtr<IPersistFile> file;
    Microsoft::WRL::ComPtr<IShellLinkDataList> data;
    DWORD flags = 0;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) ||
        FAILED(link.As(&file)) || FAILED(file->Load(path.c_str(), STGM_READ)) || FAILED(link.As(&data)) ||
        FAILED(data->GetFlags(&flags)) ||
        (flags & (SLDF_HAS_ARGS | SLDF_RUNAS_USER | SLDF_HAS_DARWINID | SLDF_RUN_WITH_SHIMLAYER)))
        return std::nullopt;
    wchar_t arguments[2]{};
    wchar_t target[32768]{};
    // No Resolve: never search for a replacement, mount a volume, or show resolution UI.
    if (FAILED(link->GetArguments(arguments, 2)) || arguments[0] ||
        link->GetPath(target, 32768, nullptr, SLGP_RAWPATH) != S_OK)
        return std::nullopt;
    std::wstring result(target);
    // Only unambiguous local absolute paths; network/virtual/URL/expandable links stay disabled.
    if (result.size() < 3 || result[1] != L':' || result[2] != L'\\' ||
        result.find(L'%') != std::wstring::npos ||
        (result.size() >= 4 && !_wcsicmp(result.c_str() + result.size() - 4, L".lnk")))
        return std::nullopt;
    return result;
}
inline std::optional<OpenTarget> bindOpenPath(const std::wstring& path, bool isLink) {
    OpenTarget target;
    target.path = path;
    if (!openIdentity(path, target.identity)) return std::nullopt;
    if (isLink) {
        const auto destination = simpleLinkPath(path);
        if (!destination) return std::nullopt;
        target.linkPath = path;
        target.linkIdentity = target.identity;
        target.path = *destination;
        if (!openIdentity(target.path, target.identity)) return std::nullopt;
    }
    return target;
}
inline std::optional<OpenTarget> bindOpenTarget(IShellFolder* folder, PCUITEMID_CHILD child) {
    Microsoft::WRL::ComPtr<IShellItem> item;
    if (!folder || !child || FAILED(SHCreateItemWithParent(nullptr, folder, child, IID_PPV_ARGS(&item))))
        return std::nullopt;
    SFGAOF attributes = 0;
    if (FAILED(item->GetAttributes(SFGAO_FILESYSTEM | SFGAO_LINK, &attributes)) ||
        !(attributes & SFGAO_FILESYSTEM))
        return std::nullopt;
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return std::nullopt;
    OpenTarget target;
    try {
        target.path = path;
    } catch (...) {
        CoTaskMemFree(path);
        throw;
    }
    CoTaskMemFree(path);
    return bindOpenPath(target.path, (attributes & SFGAO_LINK) != 0);
}
inline bool validOpenTarget(const OpenTarget& target) {
    BY_HANDLE_FILE_INFORMATION current{};
    if (!openIdentity(target.path, current) || !sameOpenIdentity(current, target.identity)) return false;
    if (target.linkPath.empty()) return true;
    if (!openIdentity(target.linkPath, current) || !sameOpenIdentity(current, target.linkIdentity) ||
        CompareFileTime(&current.ftLastWriteTime, &target.linkIdentity.ftLastWriteTime) != 0 ||
        current.nFileSizeHigh != target.linkIdentity.nFileSizeHigh ||
        current.nFileSizeLow != target.linkIdentity.nFileSizeLow)
        return false;
    const auto destination = simpleLinkPath(target.linkPath);
    return destination && *destination == target.path;
}
inline bool submitOpenTarget(const OpenTarget& target, HWND owner) {
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.hwnd = owner;
    info.lpVerb = L"open";
    info.lpFile = target.path.c_str();
    info.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&info) != FALSE;
}

enum class OpenResult { ignored, cancelled, invalid, rejected, accepted };
// Session-local request only; never serialized. Tokens distinguish stale posted messages,
// not desktop item identity. Dispatch owns the target independently of preview resources.
class OpenRequest {
    std::optional<OpenTarget> pending_;
    uintptr_t token_ = 0;
    bool submitting_ = false;

   public:
    bool busy() const { return pending_.has_value() || submitting_; }
    uintptr_t queue(const OpenTarget& target) {
        if (busy()) return 0;
        pending_ = target;
        if (++token_ == 0) ++token_;
        return token_;
    }
    void cancel() { pending_.reset(); }
    template <class Active, class Release, class Valid, class Submit>
    OpenResult dispatch(uintptr_t token, Active active, Release release, Valid valid, Submit submit) {
        if (submitting_ || !pending_ || token != token_) return OpenResult::ignored;
        const auto target = std::exchange(pending_, std::nullopt);
        submitting_ = true;
        struct Guard {
            bool& flag;
            ~Guard() { flag = false; }
        } guard{submitting_};
        if (!active()) return OpenResult::cancelled;
        if (!release() || !active()) return OpenResult::cancelled;
        if (!valid(*target)) return OpenResult::invalid;
        if (!active()) return OpenResult::cancelled;
        return submit(*target) ? OpenResult::accepted : OpenResult::rejected;
    }
};
}  // namespace dpp::winmirror
