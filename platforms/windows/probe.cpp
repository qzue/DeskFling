// COM headers require the Windows type and interface declarations first.
// clang-format off
#include <windows.h>
#include <objbase.h>
// clang-format on

#include <exdisp.h>
#include <servprov.h>
#include <shlguid.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <iostream>
#include <memory>

#include "probe_samples.h"
using Microsoft::WRL::ComPtr;
struct ComScope {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~ComScope() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};
using namespace dpp::winprobe;
int wmain(int argc, wchar_t** argv) {
    Options options;
    try {
        options = parseOptions(std::vector<std::wstring>(argv + 1, argv + argc));
    } catch (const std::exception&) {
        std::cerr << helpText();
        return 2;
    }
    if (options.help) {
        std::cout << helpText();
        return 0;
    }
    const BOOL dpi = SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const DWORD dpiError = dpi ? ERROR_SUCCESS : GetLastError();
    const auto environment = inspectEnvironment();
    if (!interactive(environment) && !options.samples) {
        std::cerr << "environment_error=requires_visible_input_desktop_and_same_session_shell; no Shell "
                     "query attempted\n";
        return 3;
    }
    ComScope com;
    if (FAILED(com.hr)) {
        std::cerr << "stage=CoInitializeEx hresult=0x" << std::hex << static_cast<unsigned long>(com.hr)
                  << '\n';
        return 1;
    }
    try {
        if (options.samples) return runSamples(options, environment, dpi != FALSE, dpiError);
    } catch (const std::exception&) {
        std::cerr << "sample_check_failed; no acceptance claimed\n";
        return 1;
    }
    std::cout << "dpi_awareness_set=" << (dpi ? "true" : "false")
              << " screens=" << GetSystemMetrics(SM_CMONITORS) << '\n';
    auto desktopView = openDesktopView();
    HRESULT hr = desktopView.hr;
    auto& view = desktopView.view;
    auto& folder = desktopView.folder;
    int count = 0, positions = 0, images = 0;
    if (SUCCEEDED(hr)) {
        desktopView.stage = "IFolderView.ItemCount";
        hr = view->ItemCount(SVGIO_ALLVIEW, &count);
    }
    for (int i = 0; SUCCEEDED(hr) && i < count; ++i) {
        PITEMID_CHILD raw = nullptr;
        const HRESULT itemHr = view->Item(i, &raw);
        std::unique_ptr<ITEMIDLIST, PidlFree> child(raw);
        if (FAILED(itemHr) || !raw) continue;
        POINT point{};
        if (SUCCEEDED(view->GetItemPosition(raw, &point))) ++positions;
        ComPtr<IShellItemImageFactory> image;
        if (SUCCEEDED(SHCreateItemWithParent(nullptr, folder.Get(), raw, IID_PPV_ARGS(&image)))) {
            HBITMAP bitmap = nullptr;
            if (SUCCEEDED(image->GetImage(SIZE{64, 64}, SIIGBF_ICONONLY, &bitmap))) {
                ++images;
                DeleteObject(bitmap);
            }
        }
    }
    std::cout << "shell_hresult=0x" << std::hex << static_cast<unsigned long>(hr) << std::dec
              << " items=" << count << " view_positions=" << positions << " icon_images=" << images << '\n'
              << "positions_are_view_coordinates=true icon_image_bounds=unverified\n"
              << "original_icon_suppression=unimplemented transparent_composition=unimplemented\n"
              << "real_desktop_mode=DISABLED\n";
    if (FAILED(hr)) std::cerr << "shell_stage=" << desktopView.stage << '\n';
    if (GetSystemMetrics(SM_CMONITORS) != 1) std::cout << "multi_monitor_guard=DISABLED_REAL_DESKTOP\n";
    return FAILED(hr) ? 1 : 0;
}
