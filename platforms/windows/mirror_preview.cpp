#include "mirror_preview.h"

#include <commctrl.h>
#include <commoncontrols.h>
#include <shellapi.h>

#include <map>

#include "billiards_audio.h"
#include "billiards_lan.h"
#include "black_hole_d3d11.h"
#include "desktop_layout_writeback.h"
#include "direct_input.h"
#include "filename_labels.h"
#include "floating_hud_bar.h"
#include "mac_settings_card.h"
#include "open_target.h"
#include "platforms/display_migration.h"
#include "platforms/mirror_hit.h"

#ifdef _MSC_VER
#pragma comment(linker,                                                                          \
                "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' "   \
                "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' " \
                "language='*'\"")
#endif
#include <d2d1_1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <wincodec.h>
#include <windowsx.h>

#include <array>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#include "core/scene/coordinates.h"
#include "frame_timer.h"
#include "protected_session.h"

using Microsoft::WRL::ComPtr;
namespace dpp::winmirror {
namespace {
struct Failure : std::runtime_error {
    HRESULT code;
    const char* stage;  // Internal static labels only; never paths or arbitrary exception text.
    Failure(const char* label, HRESULT value) : std::runtime_error(label), code(value), stage(label) {}
};
void check(HRESULT hr, const char* stage) {
    if (FAILED(hr)) throw Failure(stage, hr);
}
void require(bool condition, const char* stage) {
    if (!condition) throw Failure(stage, E_FAIL);
}
void wincheck(BOOL ok, const char* stage) {
    if (!ok) {
        auto error = GetLastError();
        throw Failure(stage, HRESULT_FROM_WIN32(error ? error : ERROR_INVALID_DATA));
    }
}
std::string rectJson(RECT r) {
    return "[" + std::to_string(r.left) + "," + std::to_string(r.top) + "," + std::to_string(r.right) + "," +
           std::to_string(r.bottom) + "]";
}
std::string sizeJson(SIZE value) {
    if (value.cx <= 0 || value.cy <= 0) return "null";
    return "[" + std::to_string(value.cx) + "," + std::to_string(value.cy) + "]";
}
std::string pointerJson(const void* value) {
    std::ostringstream o;
    o << '"' << "0x" << std::hex << reinterpret_cast<std::uintptr_t>(value) << '"';
    return o.str();
}
bool sameRect(RECT a, RECT b) { return EqualRect(&a, &b) != FALSE; }
bool pmv2(HWND w) {
    return AreDpiAwarenessContextsEqual(GetWindowDpiAwarenessContext(w),
                                        DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE;
}
std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}
std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n =
        WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}
struct ComScope {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~ComScope() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};
// No reparenting, Shell messages with pointers, remote memory, hooks or Explorer mutations.
HWND uniqueDirectList(HWND shell) {
    HWND found = nullptr;
    for (HWND w = GetWindow(shell, GW_CHILD); w; w = GetWindow(w, GW_HWNDNEXT)) {
        wchar_t name[64]{};
        if (GetClassNameW(w, name, 64) && std::wstring(name) == L"SysListView32") {
            if (found) return nullptr;
            found = w;
        }
    }
    return found;
}

bool liveDisplayContractMatches(const ViewContract& c, bool ownsVisibility, bool allowMinorWorkJitter) {
    if (contractStillValid(c, ownsVisibility)) return true;
    if (!allowMinorWorkJitter) return false;
    const LONG_PTR ignored = ownsVisibility ? (WS_VISIBLE | WS_HSCROLL | WS_VSCROLL) : 0;
    if (!IsWindow(c.shell) || !IsWindow(c.list) || uniqueDirectList(c.shell) != c.list ||
        (!c.multiMonitorSession &&
         (GetDpiForWindow(c.list) != c.dpi || !pmv2(c.list) || !pmv2(c.shell))) ||
        (GetWindowLongPtrW(c.list, GWL_STYLE) & ~ignored) != (c.listStyle & ~ignored) ||
        GetSystemMetrics(SM_CMONITORS) != c.expectedMonitorCount)
        return false;
    DWORD process = 0;
    GetWindowThreadProcessId(c.list, &process);
    RECT rect{};
    if (process != c.process || !GetClientRect(c.list, &rect)) return false;
    auto withinTaskbarJitter = [](RECT a, RECT b) {
        return std::abs(a.left - b.left) <= 96 && std::abs(a.top - b.top) <= 96 &&
               std::abs(a.right - b.right) <= 96 && std::abs(a.bottom - b.bottom) <= 96;
    };
    if (c.multiMonitorSession) {
        return true;
    }
    MONITORINFO info{sizeof(info), {}, {}, 0};
    if (MonitorFromWindow(c.list, MONITOR_DEFAULTTONULL) != c.monitor || !GetMonitorInfoW(c.monitor, &info) ||
        !sameRect(info.rcMonitor, c.bounds))
        return false;
    return withinTaskbarJitter(info.rcWork, c.work) && withinTaskbarJitter(rect, c.listClient);
}

struct WinMonitorEntry {
    HMONITOR hmon = nullptr;
    RECT bounds{};
    RECT work{};
    UINT dpi = 96u;
    bool primary = false;
};

BOOL CALLBACK collectMonitorsEnumProc(HMONITOR hmon, HDC, LPRECT, LPARAM lparam) noexcept {
    auto* out = reinterpret_cast<std::vector<WinMonitorEntry>*>(lparam);
    if (!out || !hmon) return TRUE;
    MONITORINFO mi{sizeof(mi), {}, {}, 0};
    if (!GetMonitorInfoW(hmon, &mi)) return TRUE;
    WinMonitorEntry e{};
    e.hmon = hmon;
    e.bounds = mi.rcMonitor;
    e.work = mi.rcWork;
    e.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    UINT dpiX = 96u, dpiY = 96u;
    if (HMODULE shcore = GetModuleHandleW(L"shcore.dll")) {
        using GetDpiFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
        if (auto fn = reinterpret_cast<GetDpiFn>(GetProcAddress(shcore, "GetDpiForMonitor"))) {
            if (SUCCEEDED(fn(hmon, 0 /*MDT_EFFECTIVE_DPI*/, &dpiX, &dpiY)) && dpiX > 0) {
                e.dpi = dpiX;
            }
        }
    }
    out->push_back(e);
    return TRUE;
}

std::vector<WinMonitorEntry> enumerateMonitorsSorted() {
    std::vector<WinMonitorEntry> list;
    EnumDisplayMonitors(nullptr, nullptr, collectMonitorsEnumProc, reinterpret_cast<LPARAM>(&list));
    std::stable_sort(list.begin(), list.end(), [](const WinMonitorEntry& a, const WinMonitorEntry& b) {
        if (a.primary != b.primary) return a.primary;
        if (a.bounds.left != b.bounds.left) return a.bounds.left < b.bounds.left;
        return a.bounds.top < b.bounds.top;
    });
    return list;
}

std::size_t owningMonitorIndexForScreenPoint(POINT pt, const std::vector<WinMonitorEntry>& mons) noexcept {
    if (mons.size() <= 1) return 0;
    for (std::size_t i = 0; i < mons.size(); ++i) {
        if (PtInRect(&mons[i].bounds, pt)) return i;
    }
    std::size_t bestIdx = 0;
    double bestDist2 = 1e30;
    for (std::size_t i = 0; i < mons.size(); ++i) {
        const RECT& r = mons[i].bounds;
        const double dx = pt.x < r.left ? static_cast<double>(r.left - pt.x)
                                        : (pt.x > r.right ? static_cast<double>(pt.x - r.right) : 0.0);
        const double dy = pt.y < r.top ? static_cast<double>(r.top - pt.y)
                                       : (pt.y > r.bottom ? static_cast<double>(pt.y - r.bottom) : 0.0);
        const double d2 = dx * dx + dy * dy;
        if (d2 < bestDist2) {
            bestDist2 = d2;
            bestIdx = i;
        }
    }
    return bestIdx;
}

// D3D11 hardware -> DXGI premultiplied flip swapchain -> D2D -> DirectComposition.
// All objects belong to this invocation and all operations run on its UI thread.
struct Graphics {
    ComPtr<ID3D11Device> d3d;
    ComPtr<IDXGISwapChain1> swap;
    ComPtr<ID2D1Factory1> d2dFactory;
    ComPtr<ID2D1DeviceContext> draw;
    ComPtr<ID2D1Bitmap1> targetBitmap;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<ID2D1StrokeStyle> roundStroke, dashedStroke;
    ComPtr<IDWriteFactory> writeFactory;
    ComPtr<IDWriteTextFormat> text, badgeText;
    std::map<int, ComPtr<IDWriteTextFormat>> hudFormatsLeft, hudFormatsCentered;
    ComPtr<ID2D1SolidColorBrush> badgeBackground, filenameBrush;
    FilenameLabels filenames;
    ComPtr<IDCompositionDevice> composition;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> visual;
    std::vector<ComPtr<ID2D1Bitmap1>> images;
    std::vector<ComPtr<ID2D1BitmapBrush1>> imageBrushes;
    std::vector<std::uint8_t> claimedBuf;
    std::vector<FilenamePeer> filenamePeersBuf;
    BlackHoleD3D11 blackHole;
    void initSharedDrawingResources() {
        draw->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        check(draw->CreateSolidColorBrush(D2D1::ColorF(0.05f, 1.0f, 0.65f, 1), &brush), "Brush");
        const auto roundProps = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                            D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
        check(d2dFactory->CreateStrokeStyle(roundProps, nullptr, 0, &roundStroke), "RoundStroke");
        const float dashes[] = {2.8f, 2.4f};
        const auto dashProps =
            D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                        D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_CUSTOM, 0.0f);
        check(d2dFactory->CreateStrokeStyle(dashProps, dashes, 2, &dashedStroke), "DashedStroke");
        check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                  reinterpret_cast<IUnknown**>(writeFactory.GetAddressOf())),
              "DWriteFactory");
        check(writeFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD,
                                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 18,
                                             L"en-us", &text),
              "TextFormat");
        check(writeFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD,
                                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 9,
                                             L"en-us", &badgeText),
              "BadgeTextFormat");
        check(draw->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, .75f), &badgeBackground), "BadgeBrush");
        check(draw->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &filenameBrush), "FilenameBrush");
    }
    IDWriteTextFormat* hudTextFormat(float sizePt, bool centered) {
        if (!writeFactory) return nullptr;
        const int halfPtKey = std::clamp(static_cast<int>(std::lround(sizePt * 2.0f)), 14, 72);
        auto& pool = centered ? hudFormatsCentered : hudFormatsLeft;
        auto it = pool.find(halfPtKey);
        if (it != pool.end()) return it->second.Get();
        ComPtr<IDWriteTextFormat> fmt;
        const float actualPt = static_cast<float>(halfPtKey) * 0.5f;
        if (FAILED(writeFactory->CreateTextFormat(L"Microsoft YaHei UI", nullptr,
                                                  DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                                  DWRITE_FONT_STRETCH_NORMAL, actualPt, L"zh-CN", &fmt))) {
            return nullptr;
        }
        fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        fmt->SetTextAlignment(centered ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_LEADING);
        auto* raw = fmt.Get();
        pool.emplace(halfPtKey, std::move(fmt));
        return raw;
    }
    void initialize(HWND window, UINT width, UINT height) {
        require(width > 0 && height > 0, "invalid_surface_size");
        check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr),
              "D3D11CreateDevice");
        ComPtr<IDXGIDevice> device;
        check(d3d.As(&device), "DXGIDevice");
        ComPtr<IDXGIAdapter> adapter;
        check(device->GetAdapter(&adapter), "GetAdapter");
        ComPtr<IDXGIFactory2> factory;
        check(adapter->GetParent(IID_PPV_ARGS(&factory)), "DXGIFactory2");
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = width;
        desc.Height = height;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        check(factory->CreateSwapChainForComposition(d3d.Get(), &desc, nullptr, &swap),
              "CreateSwapChainForComposition");
        check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr,
                                reinterpret_cast<void**>(d2dFactory.GetAddressOf())),
              "D2D1CreateFactory");
        ComPtr<ID2D1Device> d2dDevice;
        check(d2dFactory->CreateDevice(device.Get(), &d2dDevice), "D2DCreateDevice");
        check(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &draw), "D2DContext");
        ComPtr<IDXGISurface> surface;
        check(swap->GetBuffer(0, IID_PPV_ARGS(&surface)), "GetBuffer");
        const auto properties =
            D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                    D2D1::PixelFormat(desc.Format, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        check(draw->CreateBitmapFromDxgiSurface(surface.Get(), &properties, &targetBitmap), "TargetBitmap");
        draw->SetTarget(targetBitmap.Get());
        // Explicit device pixels: not inferred from the monitor-scale API.
        draw->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
        draw->SetDpi(96, 96);
        initSharedDrawingResources();
        check(DCompositionCreateDevice(device.Get(), IID_PPV_ARGS(&composition)), "DCompositionCreateDevice");
        check(composition->CreateTargetForHwnd(window, TRUE, &target), "CompositionTarget");
        check(composition->CreateVisual(&visual), "CompositionVisual");
        check(visual->SetContent(swap.Get()), "SetContent");
        check(target->SetRoot(visual.Get()), "SetRoot");
    }
    void resize(UINT width, UINT height) {
        draw->SetTarget(nullptr);
        targetBitmap.Reset();
        check(swap->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0), "MigrationResizeBuffers");
        ComPtr<IDXGISurface> surface;
        check(swap->GetBuffer(0, IID_PPV_ARGS(&surface)), "MigrationSurface");
        const auto props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        check(draw->CreateBitmapFromDxgiSurface(surface.Get(), &props, &targetBitmap), "MigrationTarget");
        draw->SetTarget(targetBitmap.Get());
    }
    void render(const std::vector<POINT>& positions) {
        draw->BeginDraw();
        draw->Clear(D2D1::ColorF(0, 0, 0, 0));
        for (std::size_t i = 0; i < positions.size(); ++i) {
            const float x = static_cast<float>(positions[i].x), y = static_cast<float>(positions[i].y);
            // Cross centre is the ITEM ANCHOR, never the icon image centre.
            draw->DrawLine({x - 6, y}, {x + 6, y}, brush.Get(), 2);
            draw->DrawLine({x, y - 6}, {x, y + 6}, brush.Get(), 2);
            const auto label = L"S" + std::to_wstring(i + 1);
            draw->DrawTextW(label.c_str(), static_cast<UINT32>(label.size()), text.Get(),
                            D2D1::RectF(x + 9, y + 3, x + 60, y + 29), brush.Get());
        }
        check(draw->EndDraw(), "EndDraw");
        check(swap->Present(1, 0), "Present");
        check(composition->Commit(), "Commit");
    }
    void cacheImages(const winprobe::Report& report) {
        filenames.cache(report);
        images.clear();
        imageBrushes.clear();
        blackHole.clearIcons();
        ComPtr<IWICImagingFactory> factory;
        check(
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)),
            "WICFactory");
        const auto bp = D2D1::BitmapBrushProperties1(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP,
                                                     D2D1_INTERPOLATION_MODE_LINEAR);
        for (const auto& sample : report.samples) {
            ComPtr<IWICBitmap> bitmap;
            check(factory->CreateBitmapFromHBITMAP(sample.bitmap.get(), nullptr,
                                                   WICBitmapUsePremultipliedAlpha, &bitmap),
                  "WICFromCachedHBITMAP");
            ComPtr<IWICFormatConverter> converter;
            check(factory->CreateFormatConverter(&converter), "WICConverter");
            check(converter->Initialize(bitmap.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                        nullptr, 0, WICBitmapPaletteTypeCustom),
                  "WICPBGRA");
            blackHole.cacheIcon(d3d.Get(), converter.Get());
            ComPtr<ID2D1Bitmap1> image;
            check(draw->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &image), "CachedD2DImage");
            ComPtr<ID2D1BitmapBrush1> imgBrush;
            check(draw->CreateBitmapBrush(image.Get(), bp, &imgBrush), "CachedD2DBitmapBrush");
            images.push_back(std::move(image));
            imageBrushes.push_back(std::move(imgBrush));
        }
    }
    void drawCopiesToTarget(const DrawList& list, UINT dpi, bool originalsOnly, bool showFilenames,
                            Vec2 workArea, bool showDebugBadges = true) {
        const BlackHoleItem* holeBack = nullptr;
        if (!originalsOnly) {
            for (const auto& command : list.commands())
                if (const auto* h = std::get_if<BlackHoleItem>(&command);
                    h && h->layer == BlackHoleItem::Layer::Back)
                    holeBack = h;
        }
        const bool useGpuHole =
            holeBack && blackHole.begin(d3d.Get(), draw.Get(), *holeBack, windowsDpiScale(dpi));
        claimedBuf.assign(list.commands().size(), 0u);
        if (useGpuHole) {
            for (std::size_t ci = 0; ci < list.commands().size(); ++ci) {
                if (const auto* s = std::get_if<SpriteItem>(&list.commands()[ci])) {
                    if (blackHole.claim(*s) != BlackHoleD3D11::Claim::None) claimedBuf[ci] = 1u;
                }
            }
            blackHole.renderToTarget(d3d.Get());
        }
        std::size_t filenamePeerCursor = 0;
        if (showFilenames && !originalsOnly) {
            buildFilenamePeers(list, workArea, filenamePeersBuf);
        } else {
            filenamePeersBuf.clear();
        }

        draw->SetUnitMode(D2D1_UNIT_MODE_DIPS);
        draw->SetDpi(static_cast<float>(dpi), static_cast<float>(dpi));
        draw->BeginDraw();
        draw->Clear(D2D1::ColorF(0, 0, 0, 0));
        bool identityXf = false;
        const auto ensureIdentityXf = [&]() {
            if (!identityXf) {
                draw->SetTransform(D2D1::Matrix3x2F::Identity());
                identityXf = true;
            }
        };
        ensureIdentityXf();
        for (std::size_t ci = 0; ci < list.commands().size(); ++ci) {
            if (originalsOnly) break;
            const auto& command = list.commands()[ci];
            const auto color = [](Color c) {
                return D2D1::ColorF(c.r / 255.f, c.g / 255.f, c.b / 255.f, c.a / 255.f);
            };
            if (const auto* hole = std::get_if<BlackHoleItem>(&command)) {
                ensureIdentityXf();
                if (useGpuHole) {
                    if (hole->layer == BlackHoleItem::Layer::Front)
                        draw->DrawBitmap(blackHole.bitmap(), blackHole.destRect(), 1.0f,
                                         D2D1_INTERPOLATION_MODE_LINEAR);
                } else if (hole->layer == BlackHoleItem::Layer::Back) {
                    const auto shadow =
                        D2D1::Ellipse({hole->center.x, hole->center.y}, hole->radius, hole->radius);
                    brush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.96f));
                    draw->FillEllipse(shadow, brush.Get());
                    brush->SetColor(D2D1::ColorF(0.98f, 0.36f, 0.05f, 0.9f));
                    draw->DrawEllipse(shadow, brush.Get(), 2.0f);
                }
                continue;
            }
            if (const auto* c = std::get_if<CircleItem>(&command)) {
                ensureIdentityXf();
                const auto ellipse = D2D1::Ellipse({c->center.x, c->center.y}, c->radius, c->radius);
                if (c->fill.a > 0) {
                    brush->SetColor(color(c->fill));
                    draw->FillEllipse(ellipse, brush.Get());
                }
                if (c->stroke.a > 0 && c->strokeWidth > 0.0f) {
                    brush->SetColor(color(c->stroke));
                    draw->DrawEllipse(ellipse, brush.Get(), c->strokeWidth);
                }
                continue;
            }
            if (const auto* l = std::get_if<LineItem>(&command)) {
                if (l->color.a > 0 && l->width > 0.0f) {
                    ensureIdentityXf();
                    brush->SetColor(color(l->color));
                    draw->DrawLine({l->a.x, l->a.y}, {l->b.x, l->b.y}, brush.Get(), l->width,
                                   l->dashed ? dashedStroke.Get() : roundStroke.Get());
                }
                continue;
            }
            if (const auto* bx = std::get_if<BoxItem>(&command)) {
                if (bx->rotation != 0.0f) {
                    draw->SetTransform(
                        D2D1::Matrix3x2F::Rotation(bx->rotation * 57.2957795f, {bx->center.x, bx->center.y}));
                    identityXf = false;
                } else {
                    ensureIdentityXf();
                }
                const float hw = bx->size.x * 0.5f;
                const float hh = bx->size.y * 0.5f;
                const float rx = std::min(bx->cornerRadius, std::min(hw, hh));
                const auto rrect = D2D1::RoundedRect(
                    D2D1::RectF(bx->center.x - hw, bx->center.y - hh, bx->center.x + hw, bx->center.y + hh),
                    rx, rx);
                if (bx->fill.a > 0) {
                    brush->SetColor(color(bx->fill));
                    draw->FillRoundedRectangle(rrect, brush.Get());
                }
                if (bx->stroke.a > 0 && bx->strokeWidth > 0.0f) {
                    brush->SetColor(color(bx->stroke));
                    draw->DrawRoundedRectangle(rrect, brush.Get(), bx->strokeWidth);
                }
                continue;
            }
            if (const auto* poly = std::get_if<PolygonItem>(&command)) {
                const std::uint8_t n = std::min<std::uint8_t>(poly->count, 8u);
                if (n >= 3) {
                    ensureIdentityXf();
                    ComPtr<ID2D1PathGeometry> geom;
                    if (SUCCEEDED(d2dFactory->CreatePathGeometry(&geom))) {
                        ComPtr<ID2D1GeometrySink> sink;
                        if (SUCCEEDED(geom->Open(&sink))) {
                            sink->BeginFigure(
                                {poly->points[0].x, poly->points[0].y},
                                poly->fill.a > 0 ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
                            for (std::uint8_t i = 1; i < n; ++i) {
                                sink->AddLine({poly->points[i].x, poly->points[i].y});
                            }
                            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                            if (SUCCEEDED(sink->Close())) {
                                if (poly->fill.a > 0) {
                                    brush->SetColor(color(poly->fill));
                                    draw->FillGeometry(geom.Get(), brush.Get());
                                }
                                if (poly->stroke.a > 0 && poly->strokeWidth > 0.0f) {
                                    brush->SetColor(color(poly->stroke));
                                    draw->DrawGeometry(geom.Get(), brush.Get(), poly->strokeWidth,
                                                       roundStroke.Get());
                                }
                            }
                        }
                    }
                }
                continue;
            }
            if (const auto* t = std::get_if<TextItem>(&command)) {
                if (t->color.a > 0 && t->text[0] != '\0') {
                    ensureIdentityXf();
                    wchar_t wbuf[160]{};
                    const int wn = MultiByteToWideChar(CP_UTF8, 0, t->text.data(), -1, wbuf, 159);
                    if (wn > 1) {
                        const UINT32 wlen = static_cast<UINT32>(wn - 1);
                        if (auto* fmt = hudTextFormat(t->size, t->centered)) {
                            const float h = std::max(28.0f, t->size * 1.75f);
                            const D2D1_RECT_F rc =
                                t->centered ? D2D1::RectF(t->position.x - 380.0f, t->position.y,
                                                          t->position.x + 380.0f, t->position.y + h)
                                            : D2D1::RectF(t->position.x, t->position.y,
                                                          t->position.x + 700.0f, t->position.y + h);
                            const D2D1_RECT_F shadowRc =
                                D2D1::RectF(rc.left + 1.0f, rc.top + 1.0f, rc.right + 1.0f, rc.bottom + 1.0f);
                            brush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, (t->color.a / 255.0f) * 0.82f));
                            draw->DrawTextW(wbuf, wlen, fmt, shadowRc, brush.Get());
                            brush->SetColor(color(t->color));
                            draw->DrawTextW(wbuf, wlen, fmt, rc, brush.Get());
                        }
                    }
                }
                continue;
            }
            const auto* sprite = std::get_if<SpriteItem>(&command);
            if (!sprite || sprite->opacity <= 0) continue;
            const auto index = static_cast<unsigned>(sprite->texture);
            require(index > 0 && index <= images.size(), "invalid_cached_texture_association");
            const auto r = rectFromCenter(sprite->center, sprite->size);
            if (!claimedBuf[ci]) {
                float opacity = sprite->opacity;
                float scale = 1.0f;
                if (!useGpuHole && sprite->consume > 0.0f) {
                    scale = 1.0f - sprite->consume;
                    opacity *= (1.0f - sprite->consume * sprite->consume);
                }
                const auto sr = rectFromCenter(sprite->center, sprite->size * scale);
                if (sprite->rotation != 0.0f) {
                    draw->SetTransform(D2D1::Matrix3x2F::Rotation(sprite->rotation * 57.2957795f,
                                                                  {sprite->center.x, sprite->center.y}));
                    identityXf = false;
                } else {
                    ensureIdentityXf();
                }
                if (sprite->clipCircle && index <= imageBrushes.size()) {
                    auto* ib = imageBrushes[index - 1].Get();
                    const D2D1_SIZE_F bmpSz = images[index - 1]->GetSize();
                    const float bw = std::max(1.0f, bmpSz.width);
                    const float bh = std::max(1.0f, bmpSz.height);
                    const float fit = std::min(sr.width / bw, sr.height / bh);
                    const float ox = sprite->center.x - bw * fit * 0.5f;
                    const float oy = sprite->center.y - bh * fit * 0.5f;
                    ib->SetTransform(D2D1::Matrix3x2F::Scale(fit, fit) *
                                     D2D1::Matrix3x2F::Translation(ox, oy));
                    ib->SetOpacity(opacity);
                    draw->FillEllipse(D2D1::Ellipse({sprite->center.x, sprite->center.y}, sr.width * 0.5f,
                                                    sr.height * 0.5f),
                                      ib);
                } else {
                    const auto bounds = D2D1::RectF(sr.x, sr.y, sr.x + sr.width, sr.y + sr.height);
                    brush->SetColor(D2D1::ColorF(1, 1, 1, 1));
                    draw->DrawBitmap(images[index - 1].Get(), bounds, opacity,
                                     D2D1_INTERPOLATION_MODE_LINEAR);
                    if (showDebugBadges && sprite->interactive && !sprite->clipCircle) {
                        // Small in-image badge: never paint debug text across Explorer file names.
                        const auto badge = D2D1::RectF(r.x, r.y, r.x + 12, r.y + 12);
                        draw->FillRectangle(badge, badgeBackground.Get());
                        const auto label = std::to_wstring(index);
                        draw->DrawTextW(label.c_str(), static_cast<UINT32>(label.size()), badgeText.Get(),
                                        badge, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    }
                }
            }
            if (!sprite->interactive) continue;
            if (showFilenames) {
                const float preLabelY =
                    (filenamePeerCursor < filenamePeersBuf.size())
                        ? filenamePeersBuf[filenamePeerCursor++].labelY
                        : filenameLabelY(*sprite, workArea, filenameHeight);
                const auto name = filenameBoundsFast(*sprite, preLabelY, workArea, filenamePeersBuf);
                if (name.width <= 0) continue;
                auto* layout = filenames.fit(sprite->texture, name.width);
                require(layout != nullptr, "invalid_cached_filename_association");
                ensureIdentityXf();
                draw->DrawTextLayout({name.x + 1, name.y + 1}, layout, badgeBackground.Get(),
                                     D2D1_DRAW_TEXT_OPTIONS_CLIP);
                draw->DrawTextLayout({name.x, name.y}, layout, filenameBrush.Get(),
                                     D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }
        ensureIdentityXf();
        check(draw->EndDraw(), "CopiesEndDraw");
    }
    void renderCopies(const DrawList& list, UINT dpi, bool originalsOnly, bool showFilenames, Vec2 workArea,
                      bool showDebugBadges = true) {
        drawCopiesToTarget(list, dpi, originalsOnly, showFilenames, workArea, showDebugBadges);
        check(swap->Present(1, 0), "CopiesPresent");
        check(composition->Commit(), "CopiesCommit");
    }
};

struct MarkerWindow {
    winprobe::Report& report;
    ViewContract contract;
    DisplayMigration migration;
    ViewContract pendingDisplay;
    DWORD committedInputTime = 0;
    bool nativePreparing = false;
    uint64_t preparedNativeRevision = 0;
    static double migrationTime() { return static_cast<double>(GetTickCount64()) / 1000.0; }
    std::optional<ViewContract> readMigrationDisplay() {
        if (contract.multiMonitorSession) {
            if (protectedSession && protectedSession->isContinuous()) return std::nullopt;
            require(false, "migration_multi_display_changed");
        }
        ViewContract next = contract;
        require(GetSystemMetrics(SM_CMONITORS) == contract.expectedMonitorCount && IsWindow(contract.list) &&
                    IsWindow(contract.shell),
                "migration_single_display");
        require(MonitorFromWindow(contract.list, MONITOR_DEFAULTTONULL) == contract.monitor,
                "migration_display_identity");
        MONITORINFO info{sizeof(info), {}, {}, 0};
        wincheck(GetMonitorInfoW(contract.monitor, &info), "migration_monitor");
        next.work = info.rcWork;
        next.bounds = info.rcMonitor;
        next.dpi = GetDpiForWindow(contract.list);
        wincheck(GetClientRect(contract.list, &next.listClient), "migration_list_client");
        const auto a = mapPoint(contract.list, nullptr, {next.listClient.left, next.listClient.top});
        const auto b = mapPoint(contract.list, nullptr, {next.listClient.right, next.listClient.bottom});
        next.listScreen = {a.x, a.y, b.x, b.y};
        require(next.dpi > 0, "migration_dpi_unknown");
        require(contractStillValid(next, true), "migration_contract_changed");
        if (!winprobe::interactive(winprobe::inspectEnvironment())) {
            if (protectedSession && protectedSession->isContinuous()) return std::nullopt;
            require(false, "migration_input_desktop");
        }
        if (!migrationListBoundsReady(migration, next, migrationTime())) return std::nullopt;
        return next;
    }
    static bool sameDisplay(const ViewContract& a, const ViewContract& b) {
        return a.monitor == b.monitor && a.dpi == b.dpi && sameRect(a.work, b.work) &&
               sameRect(a.bounds, b.bounds) && sameRect(a.listScreen, b.listScreen) &&
               sameRect(a.listClient, b.listClient);
    }
    void beginMigration() {
        if (nativePreparing) return;  // synchronous window callbacks are verified before commit
        require(!stopping && protectedMode && protectedSession && protectedSession->active() && playback &&
                    playback->game(),
                "migration_requires_protected_scene");
        if (!migration.active()) {
            if (menuActive) EndMenu();
            openRequest.cancel();
            leaveInteraction();
            releaseOwnCapture();
            interactionStyle(false);
            desktopReady = false;
            billiardsAudio.stop();
            invalidateCachedOverlayRgn();
            migration.begin(*playback->game(), migrationTime());
            backgroundAfterOpen = true;  // Display settings may own foreground while input remains released.
            ShowWindow(overlay, SW_HIDE);
            trace("display_migration_started");
        }
        const auto sample = readMigrationDisplay();
        if (!sample) return;  // Keep frozen and input-free while Explorer finishes its resize.
        const auto& next = *sample;
        if (!sameDisplay(next, pendingDisplay)) {
            pendingDisplay = next;
            migration.changed(migrationTime());
        }
    }
    void advanceMigration() {
        if (!migration.active()) return;
        require(!stopping && protectedSession && protectedSession->heartbeat() &&
                    !migration.expired(migrationTime()),
                "migration_deadline_or_recovery");
        const auto sample = readMigrationDisplay();
        if (!sample) return;  // Keep frozen and input-free while Explorer finishes its resize.
        const auto& next = *sample;
        if (!sameDisplay(next, pendingDisplay)) {
            pendingDisplay = next;
            migration.changed(migrationTime());
            return;
        }
        if (!migration.settled(migrationTime())) return;
        const auto token = migration.revision();
        const auto w = next.work;
        if (!migration.candidate())
            migration.prepare(
                windowsWorkAreaSceneSize({w.left, w.top, w.right, w.bottom}, windowsDpiScale(next.dpi)));
        nativePreparing = true;
        try {
            wincheck(SetWindowPos(overlay, nullptr, w.left, w.top, w.right - w.left, w.bottom - w.top,
                                  SWP_NOZORDER | SWP_NOACTIVATE),
                     "migration_window");
            if (preparedNativeRevision != token) {
                graphics.resize(static_cast<UINT>(w.right - w.left), static_cast<UINT>(w.bottom - w.top));
                preparedNativeRevision = token;
            }
            nativePreparing = false;
        } catch (...) {
            nativePreparing = false;
            throw;
        }
        if (stopping || !migration.active() || token != migration.revision()) return;
        const auto verified = readMigrationDisplay();
        if (!verified) return;  // Also invalidate a prepared candidate if Shell lags again.
        if (!sameDisplay(next, *verified)) {
            pendingDisplay = *verified;
            migration.changed(migrationTime());
            return;
        }
        RECT frame{}, actualClient{};
        require(GetWindowRect(overlay, &frame) && GetClientRect(overlay, &actualClient),
                "migration_actual_window");
        if (!sameRect(frame, w) || actualClient.right != w.right - w.left ||
            actualClient.bottom != w.bottom - w.top || GetDpiForWindow(overlay) != next.dpi)
            return;
        check(graphics.d3d->GetDeviceRemovedReason(), "migration_device");
        const bool protectionValid = !stopping && protectedSession->heartbeat();
        auto candidate = migration.commit(token, migrationTime(), protectionValid);
        require(candidate != nullptr, "migration_commit_invalidated");
        playback->commitMigration(std::move(candidate));
        syncLanRole();
        if (billiardsLan.state() == BilliardsLan::State::ConnectedHost && playback->game() &&
            (playback->game()->billiardsEnabled() || playback->game()->airHockeyEnabled())) {
            syncLanOutbound({}, true);
        }
        contract = next;
        previewDpi = next.dpi;
        hudWorkRect = next.work;
        if (hudBar.visible()) hudBar.createAndShow(effectiveHudWorkRect(), previewDpi, userPrefs);
        if (settingsCard.visible() && protectedSession && protectedSession->active()) {
            const PlayMode curPm =
                (playback && playback->game()) ? playback->game()->playMode() : PlayMode::Gravity;
            settingsCard.showModeSettings(panel, phaseLabel, effectiveHudWorkRect(), hudBar.pillScreenRect(),
                                          hudBar.visible(), previewDpi, curPm, hasAnyLayoutBackup(), true);
        }
        syncHudBar();
        committedInputTime = GetTickCount();
        previous = SteadyClock::now();
        renderPlayback();
        placeAboveShell(true);
        armDesktopReturn();
        trace("display_migration_committed");
    }

    winprobe::DesktopView* sourceView = nullptr;
    std::unique_ptr<winprotected::ProtectedSession> protectedSession;
    winprotected::SessionTrace sessionTrace;
    void trace(const char* operation, uintptr_t request = 0, int result = -1) noexcept {
        try {
            sessionTrace.record(report.snapshot, operation, GetTickCount64(),
                                protectedSession ? protectedSession->timing() : std::nullopt, request,
                                result);
        } catch (...) { /* Reporting must never obstruct recovery. */
        }
    }
    bool protectedMode = false, protectedRestoreFailed = false;
    ULONGLONG lastFullCheck = 0;
    bool fullSnapshotStillValid() {
        if (!protectedMode) return true;
        if (!sourceView || !report.itemCount) return false;
        int count = 0;
        if (FAILED(sourceView->view->ItemCount(SVGIO_ALLVIEW, &count)) || count != *report.itemCount ||
            static_cast<size_t>(count) != report.samples.size())
            return false;
        for (int i = 0; i < count; ++i) {
            PITEMID_CHILD raw = nullptr;
            if (FAILED(sourceView->view->Item(i, &raw))) return false;
            winprobe::Pidl item(raw);
            POINT position{};
            const auto& sample = report.samples[static_cast<size_t>(i)];
            if (!raw || !ILIsEqual(raw, sample.child.get()) ||
                FAILED(sourceView->view->GetItemPosition(raw, &position)) || !sample.position ||
                ((!migration.active() && migration.generation() == 0 && !layoutWritebackApplied) &&
                 (position.x != sample.position->x || position.y != sample.position->y)))
                return false;
        }
        return true;
    }
    HWND panel = nullptr, overlay = nullptr;
    std::array<HWND, 5> answers{};
    std::array<int, 5> savedAnswers{};
    std::vector<POINT> screen, client;
    std::vector<Vec2> scene;
    Graphics graphics;
    std::unique_ptr<PreviewPlayback> playback;
    FrameTimer frames;
    SteadyClock::time_point previous = SteadyClock::now();
    bool copiesEligible = false, markerConfirmed = false, copiesWereShown = false, staticConfirmed = false,
         gravityWasStarted = false, automaticStopped = false;
    unsigned pauseCommands = 0, resetCommands = 0;
    bool showFilenames = true;
    unsigned filenameToggles = 0;
    size_t cachedFilenameLayouts = 0;
    unsigned interactionEntries = 0, pointerDowns = 0, pointerMoves = 0, pointerUps = 0, inputCancels = 0;
    unsigned captureRequests = 0;
    bool releasingCapture = false, stopping = false;
    bool menuActive = false, backgroundAfterOpen = false, desktopReady = false;
    OpenRequest openRequest;
    DirectInput directInput;
    float holeScale = 1.0f;
    float holeElevation = BlackHoleState::kDefaultElevation;
    float holeTilt = 0.0f;
    bool holeDevour = false;
    float gravityMps2 = kEarthGravityMps2;
    GravityDirection gravityDir = GravityDirection::Down;
    float gravityRestitution = kDefaultGravityRestitution;
    bool savedGravityFilenames = true;
    bool soundMuted = false;
    unsigned billiardsToggles = 0, billiardsTubeOpens = 0;
    int loggedBilliardsShots = 0, loggedBilliardsPocketed = 0, loggedBilliardsPlacements = 0;
    int bestBilliardsShotsObserved = 0;
    BilliardsConfig pendingBilliardsConfig = BilliardsConfig::desktop();
    SlingshotConfig pendingSlingshotConfig{};
    SlingshotProgress pendingSlingshotProgress{};
    std::vector<SlingshotCustomPiece> pendingSlingshotCustom = defaultSlingshotCustomPieces();
    int pendingSlingshotHighScore = 0;
    AirHockeyConfig pendingAirHockeyConfig{};
    unsigned int p2KeyMask = 0;
    bool systemSuspended = false, sessionLocked = false, wtsNotificationRegistered = false;
    bool escapeHotkeyArmed = false;
    bool layoutWritebackApplied = false;
    std::optional<WinLayoutWritebackPlan> savedWinLayoutPlan;
    std::map<std::wstring, POINT> persistedWinLayoutBackup;
    HRGN cachedOverlayRgn = nullptr;
    HRGN scratchPartRgn = nullptr;
    std::vector<FilenamePeer> regionFilenamePeersBuf;
    ULONGLONG lastHoverRenderTick = 0;
    WinUserPrefs userPrefs{};
    bool suppressPrefSave = false;
    WinFloatingHudBar hudBar;
    WinModeSettingsCard settingsCard;
    WinDarkPopupMenu iconPopupMenu;
    HFONT panelUiFont = nullptr;

    static std::wstring layoutBackupFilePath() {
        const std::wstring dir = WinUserPrefs::prefsDirPath();
        return dir.empty() ? std::wstring{} : (dir + L"\\layout_backup_v1.cfg");
    }
    bool hasAnyLayoutBackup() const noexcept {
        return savedWinLayoutPlan.has_value() || !persistedWinLayoutBackup.empty();
    }
    void invalidateCachedOverlayRgn() noexcept {
        if (cachedOverlayRgn) {
            DeleteObject(cachedOverlayRgn);
            cachedOverlayRgn = nullptr;
        }
        if (scratchPartRgn) {
            DeleteObject(scratchPartRgn);
            scratchPartRgn = nullptr;
        }
    }
    UINT preferredActiveInterval() const noexcept {
        return (playback && playback->game() && playback->game()->airHockeyEnabled())
                   ? FrameTimer::fastArenaInterval
                   : FrameTimer::activeInterval;
    }
    BilliardsAudio billiardsAudio;
    BilliardsLan billiardsLan;
    double lastLanSnapSent = 0.0;
    std::vector<SoundEvent> soundBuf, lanSoundBuf;
    std::vector<std::size_t> customIconIndices;
    std::uint32_t randomSeed = 0x20261122u;
#ifdef DPP_DIRECT_INPUT_TESTS
    bool fixtureInput = false, fixtureProtection = true, fixtureAltDown = false;
    unsigned fixtureMenus = 0;
    TextureId fixtureMenuTarget = kNoTexture;
#endif
    bool altModifier() const {
#ifdef DPP_DIRECT_INPUT_TESTS
        if (fixtureInput) return fixtureAltDown;
#endif
        return GetKeyState(VK_MENU) < 0;
    }
    static std::wstring formatDeg(int deg, bool alwaysSign) {
        if (!alwaysSign && deg == 0) return L"0\u00B0";
        return (deg >= 0 ? L"+" : L"") + std::to_wstring(deg) + L"\u00B0";
    }
    void syncAngleControls() {
        if (!panel) return;
        const int elDeg =
            std::clamp(static_cast<int>(std::lround(holeElevation * 180.0f / 3.14159265f)), -85, 85);
        const int tiltDeg =
            std::clamp(static_cast<int>(std::lround(holeTilt * 180.0f / 3.14159265f)), -180, 180);
        if (HWND elBar = GetDlgItem(panel, 33)) SendMessageW(elBar, TBM_SETPOS, TRUE, elDeg + 85);
        if (HWND tiltBar = GetDlgItem(panel, 34)) SendMessageW(tiltBar, TBM_SETPOS, TRUE, tiltDeg + 180);
        if (HWND elTxt = GetDlgItem(panel, 36)) SetWindowTextW(elTxt, formatDeg(elDeg, true).c_str());
        if (HWND tiltTxt = GetDlgItem(panel, 37)) SetWindowTextW(tiltTxt, formatDeg(tiltDeg, false).c_str());
        if (settingsCard.visible()) settingsCard.refresh(hasAnyLayoutBackup());
    }
    void syncGravityReadoutLabels() {
        if (!panel) return;
        const float g = std::clamp(gravityMps2, kMinGravityMps2, kMaxGravityMps2);
        const float gRatio = g / kEarthGravityMps2;
        if (HWND gTxt = GetDlgItem(panel, 223)) {
            std::wostringstream oss;
            oss << std::fixed << std::setprecision(2) << g << L" m/s\u00B2 (" << gRatio << L"g)";
            SetWindowTextW(gTxt, oss.str().c_str());
        }
        const float e = std::clamp(gravityRestitution, kMinGravityRestitution, kMaxGravityRestitution);
        if (HWND eTxt = GetDlgItem(panel, 225)) {
            std::wostringstream oss;
            oss << L"e = " << std::fixed << std::setprecision(2) << e;
            SetWindowTextW(eTxt, oss.str().c_str());
        }
        if (settingsCard.visible()) settingsCard.refresh(hasAnyLayoutBackup());
    }
    void syncGravityControls() {
        if (!panel) return;
        const int gPos = std::clamp(static_cast<int>(std::lround(gravityMps2 * 100.0f)), 0, 4000);
        const int ePos = std::clamp(static_cast<int>(std::lround(gravityRestitution * 100.0f)), 0, 95);
        if (HWND gBar = GetDlgItem(panel, 75)) SendMessageW(gBar, TBM_SETPOS, TRUE, gPos);
        if (HWND eBar = GetDlgItem(panel, 76)) SendMessageW(eBar, TBM_SETPOS, TRUE, ePos);
        if (HWND dirCombo = GetDlgItem(panel, 74)) {
            SendMessageW(dirCombo, CB_SETCURSEL, std::clamp<int>(static_cast<int>(gravityDir), 0, 3), 0);
        }
        if (HWND presetCombo = GetDlgItem(panel, 73)) {
            const GravityPreset matched = matchGravityPreset(gravityMps2);
            SendMessageW(presetCombo, CB_SETCURSEL, std::clamp<int>(static_cast<int>(matched), 0, 7), 0);
        }
        syncGravityReadoutLabels();
    }
    void applyGravitySettingsToGame() {
        syncGravityReadoutLabels();
        if (playback && playback->game() && !stopping && !migration.active()) {
            playback->game()->setGravitySettings(gravityMps2, gravityDir, gravityRestitution);
            previous = SteadyClock::now();
            renderPlayback();
            frames.set(panel, preferredActiveInterval());
        }
        saveCurrentPrefs();
    }
    bool inputProtectionValid() {
#ifdef DPP_DIRECT_INPUT_TESTS
        if (fixtureInput) return fixtureProtection;
#endif
        return !protectedMode ||
               (protectedSession && protectedSession->active() && protectedSession->heartbeat());
    }
    bool activateInput() {
#ifdef DPP_DIRECT_INPUT_TESTS
        if (fixtureInput) return true;  // Synthetic dispatch cannot prove foreground acquisition.
#endif
        if (GetForegroundWindow() != overlay) {
            (void)SetForegroundWindow(overlay);
        }
        placeAboveShell(false);
        if (protectedSession && protectedSession->active() && protectedSession->isContinuous()) {
            return IsWindow(overlay) != FALSE;
        }
        return GetForegroundWindow() == overlay;
    }
    struct OpenAttempt {
        uintptr_t token;
        bool inputReleased;
        OpenResult result;
        DWORD error;
    };
    std::vector<OpenAttempt> openAttempts;
    std::optional<bool> openRecoveryCompleted, openSystemAccepted;
    std::map<uint32_t, OpenTarget> openTargets;
    std::optional<bool> captureOwnedAfterStop;
    HWND phaseLabel = nullptr;
    UINT previewDpi = 0;
    bool displayed = false, saved = false, failed = false, originalsOnly = false;
    unsigned comparisonToggles = 0, offsetAdjustments = 0;
    bool startupExperiment = false;
    int manualOffsetYPixels = 0;  // User-requested uniform downshift, not automatic Shell alignment.
    std::vector<WinMonitorEntry> monitorsList;
    int selectedDisplayMode = 0;  // 0..15 = single screen index, -1 = all-screens connected span
    RECT hudWorkRect{};
    std::vector<std::size_t> activeOrigIndices;
    std::vector<HWND> passiveOverlayWindows;
    MacGlassContext passiveGlass;

    RECT effectiveHudWorkRect() const noexcept {
        return (hudWorkRect.right > hudWorkRect.left && hudWorkRect.bottom > hudWorkRect.top) ? hudWorkRect
                                                                                              : contract.work;
    }

    int readSelectedDisplayModeFromPanel() const noexcept {
        if (!panel || monitorsList.size() <= 1) return 0;
        HWND cb = GetDlgItem(panel, 78);
        if (!cb) return selectedDisplayMode;
        const int sel = static_cast<int>(SendMessageW(cb, CB_GETCURSEL, 0, 0));
        if (sel == CB_ERR) return selectedDisplayMode;
        if (sel >= static_cast<int>(monitorsList.size())) return -1;
        return std::clamp<int>(sel, 0, static_cast<int>(monitorsList.size()) - 1);
    }

    void syncDisplayComboSelection() noexcept {
        if (!panel || monitorsList.empty()) return;
        HWND cb = GetDlgItem(panel, 78);
        if (!cb) return;
        if (monitorsList.size() <= 1) {
            SendMessageW(cb, CB_SETCURSEL, 0, 0);
            return;
        }
        if (selectedDisplayMode < 0) {
            SendMessageW(cb, CB_SETCURSEL, static_cast<WPARAM>(monitorsList.size()), 0);
        } else {
            const int clamped =
                std::clamp<int>(selectedDisplayMode, 0, static_cast<int>(monitorsList.size()) - 1);
            SendMessageW(cb, CB_SETCURSEL, static_cast<WPARAM>(clamped), 0);
        }
    }

    void destroyPassiveOverlays() noexcept {
        for (HWND hw : passiveOverlayWindows) {
            if (IsWindow(hw)) DestroyWindow(hw);
        }
        passiveOverlayWindows.clear();
    }

    void presentPassiveOverlaysForSelectedMonitor(std::size_t selectedMonIdx) {
        destroyPassiveOverlays();
        if (!protectedMode || monitorsList.size() <= 1 || selectedDisplayMode < 0) return;
        if (!passiveGlass.ensure()) return;

        static constexpr const wchar_t* kPassiveCls = L"DPPPassiveStaticMonitorOverlayV1";
        static bool passiveRegistered = false;
        if (!passiveRegistered) {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = kPassiveCls;
            if (RegisterClassExW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
                passiveRegistered = true;
            }
        }
        if (!passiveRegistered) return;

        ComPtr<IWICImagingFactory> wic;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&wic)))) {
            return;
        }
        auto lblFmt = passiveGlass.makeFormat(11.5f, DWRITE_FONT_WEIGHT_MEDIUM, true, false);
        HWND shellRoot = GetAncestor(contract.shell, GA_ROOT);
        HWND prevWin = (shellRoot && shellRoot != GetDesktopWindow()) ? GetWindow(shellRoot, GW_HWNDPREV)
                                                                      : HWND_BOTTOM;

        for (std::size_t mIdx = 0; mIdx < monitorsList.size(); ++mIdx) {
            if (mIdx == selectedMonIdx) continue;
            const auto& mon = monitorsList[mIdx];
            const RECT mw = mon.work;
            const int pxW = mw.right - mw.left;
            const int pxH = mw.bottom - mw.top;
            if (pxW < 100 || pxH < 100) continue;

            std::vector<std::size_t> monItems;
            for (std::size_t i = 0; i < report.samples.size() && i < screen.size(); ++i) {
                if (owningMonitorIndexForScreenPoint(screen[i], monitorsList) == mIdx) {
                    monItems.push_back(i);
                }
            }
            if (monItems.empty()) continue;

            HWND hw = CreateWindowExW(
                WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, kPassiveCls,
                L"DPP Passive Icons", WS_POPUP, mw.left, mw.top, pxW, pxH, nullptr, nullptr,
                GetModuleHandleW(nullptr), nullptr);
            if (!hw) continue;

            const UINT monDpi = mon.dpi ? mon.dpi : (GetDpiForWindow(hw) ? GetDpiForWindow(hw) : 96u);
            const float dpiScale = windowsDpiScale(monDpi);
            const float dipW = static_cast<float>(pxW) / dpiScale;
            const float dipH = static_cast<float>(pxH) / dpiScale;
            const PixelRect workPx{mw.left, mw.top, mw.right, mw.bottom};

            passiveGlass.commitLayered(hw, mw.left, mw.top, dipW, dipH, monDpi, [&](MacGlassContext& g) {
                for (std::size_t origIdx : monItems) {
                    const auto& sample = report.samples[origIdx];
                    if (!sample.bitmap) continue;
                    BITMAP bmpInfo{};
                    if (GetObjectW(sample.bitmap.get(), sizeof(bmpInfo), &bmpInfo) !=
                        static_cast<int>(sizeof(bmpInfo)))
                        continue;
                    const Vec2 anchorDip =
                        windowsScreenPixelsToScene(screen[origIdx].x, screen[origIdx].y, workPx, dpiScale);
                    const Rect imgRc = windowsIconImageRect(
                        anchorDip, {static_cast<float>(bmpInfo.bmWidth), static_cast<float>(bmpInfo.bmHeight)},
                        contract.systemImagePixels.cx > 0 ? contract.systemImagePixels.cx : 48, dpiScale,
                        manualOffsetYPixels);

                    ComPtr<IWICBitmap> wb;
                    ComPtr<IWICFormatConverter> conv;
                    ComPtr<ID2D1Bitmap> d2dBmp;
                    if (SUCCEEDED(wic->CreateBitmapFromHBITMAP(sample.bitmap.get(), nullptr,
                                                               WICBitmapUsePremultipliedAlpha, &wb)) &&
                        SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
                        SUCCEEDED(conv->Initialize(wb.Get(), GUID_WICPixelFormat32bppPBGRA,
                                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                                   WICBitmapPaletteTypeCustom)) &&
                        SUCCEEDED(g.dcTarget->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &d2dBmp))) {
                        g.dcTarget->DrawBitmap(
                            d2dBmp.Get(),
                            D2D1::RectF(imgRc.x, imgRc.y, imgRc.x + imgRc.width, imgRc.y + imgRc.height), 1.0f,
                            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    }
                    if (showFilenames && lblFmt && !sample.localName.empty()) {
                        const float cx = imgRc.x + imgRc.width * 0.5f;
                        const float lw = 86.0f;
                        const D2D1_RECT_F trc{cx - lw * 0.5f, imgRc.y + imgRc.height + 2.0f, cx + lw * 0.5f,
                                              imgRc.y + imgRc.height + 20.0f};
                        g.brush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.55f));
                        g.dcTarget->FillRoundedRectangle(D2D1::RoundedRect(trc, 4.0f, 4.0f), g.brush.Get());
                        g.brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.96f));
                        g.dcTarget->DrawTextW(sample.localName.c_str(),
                                              static_cast<UINT32>(sample.localName.size()), lblFmt.Get(), trc,
                                              g.brush.Get());
                    }
                }
            });

            SetWindowPos(hw, prevWin ? prevWin : HWND_TOP, mw.left, mw.top, pxW, pxH,
                         SWP_NOACTIVATE | SWP_SHOWWINDOW);
            passiveOverlayWindows.push_back(hw);
        }
    }

    void updateOffsetLabel() {
        const auto label = L"人工统一偏移 Y：" + std::to_wstring(manualOffsetYPixels) + L" 屏幕像素";
        SetWindowTextW(GetDlgItem(panel, 28), label.c_str());
    }
    void prepareStaticCopies() {
        trace("preview_scene_preparing");
        activeOrigIndices.clear();
        if (!protectedMode || monitorsList.empty()) {
            const auto w = contract.work;
            const PixelRect work{w.left, w.top, w.right, w.bottom};
            for (std::size_t i = 0; i < report.samples.size(); ++i) activeOrigIndices.push_back(i);
            playback = std::make_unique<PreviewPlayback>(
                makePlaybackScene(report, scene, windowsWorkAreaSceneSize(work, windowsDpiScale(previewDpi)),
                                  contract.systemImagePixels.cx, previewDpi, manualOffsetYPixels));
        } else {
            selectedDisplayMode = readSelectedDisplayModeFromPanel();
            const bool spanAll = (selectedDisplayMode < 0 && monitorsList.size() > 1);
            const std::size_t selMonIdx =
                spanAll ? 0u
                        : std::min<std::size_t>(static_cast<std::size_t>(std::max(0, selectedDisplayMode)),
                                                monitorsList.size() - 1u);
            RECT targetWork{};
            RECT targetBounds{};
            if (spanAll) {
                targetWork = monitorsList[0].work;
                targetBounds = monitorsList[0].bounds;
                for (std::size_t mi = 1; mi < monitorsList.size(); ++mi) {
                    UnionRect(&targetWork, &targetWork, &monitorsList[mi].work);
                    UnionRect(&targetBounds, &targetBounds, &monitorsList[mi].bounds);
                }
                hudWorkRect = monitorsList[0].work;
            } else {
                targetWork = monitorsList[selMonIdx].work;
                targetBounds = monitorsList[selMonIdx].bounds;
                hudWorkRect = targetWork;
            }
            contract.work = targetWork;
            contract.bounds = targetBounds;
            contract.monitor = monitorsList[selMonIdx].hmon;
            contract.multiMonitorSession = (monitorsList.size() > 1);
            contract.expectedMonitorCount = static_cast<int>(monitorsList.size());

            const UINT surfW = static_cast<UINT>(std::max<LONG>(100, targetWork.right - targetWork.left));
            const UINT surfH = static_cast<UINT>(std::max<LONG>(100, targetWork.bottom - targetWork.top));
            wincheck(SetWindowPos(overlay, nullptr, targetWork.left, targetWork.top, static_cast<int>(surfW),
                                  static_cast<int>(surfH), SWP_NOZORDER | SWP_NOACTIVATE),
                     "ConfigureOverlayForDisplay");
            previewDpi = spanAll ? (monitorsList[0].dpi ? monitorsList[0].dpi : GetDpiForWindow(overlay))
                                 : GetDpiForWindow(overlay);
            require(previewDpi > 0, "preview_dpi_unknown");
            invalidateCachedOverlayRgn();
            graphics.resize(surfW, surfH);

            if (sourceView && sourceView->view && IsWindow(contract.list)) {
                for (std::size_t i = 0; i < report.samples.size() && i < screen.size(); ++i) {
                    POINT liveRaw{};
                    if (report.samples[i].child &&
                        SUCCEEDED(sourceView->view->GetItemPosition(report.samples[i].child.get(), &liveRaw))) {
                        report.samples[i].position = winprobe::ShellItemPosition{liveRaw.x, liveRaw.y};
                        try {
                            screen[i] = mapPoint(contract.list, nullptr, liveRaw);
                        } catch (...) {
                        }
                    }
                }
            }
            const float dpiScale = windowsDpiScale(previewDpi);
            const PixelRect workPx{targetWork.left, targetWork.top, targetWork.right, targetWork.bottom};
            SceneSetup setup;
            setup.workAreaSize = windowsWorkAreaSceneSize(workPx, dpiScale);
            if (spanAll) {
                auto rectPxToScene = [&](RECT r) -> Rect {
                    const Vec2 tl = windowsScreenPixelsToScene(r.left, r.top, workPx, dpiScale);
                    const Vec2 br = windowsScreenPixelsToScene(r.right, r.bottom, workPx, dpiScale);
                    return {tl.x, tl.y, br.x - tl.x, br.y - tl.y};
                };
                for (const auto& m : monitorsList) {
                    setup.screenWorkAreas.push_back(rectPxToScene(m.work));
                }
                setup.primaryArenaRect =
                    computeMultiScreenSpanArenaRect(setup.workAreaSize, setup.screenWorkAreas);
            }

            const int slotPx = contract.systemImagePixels.cx > 0 ? contract.systemImagePixels.cx : 48;
            for (std::size_t i = 0; i < report.samples.size() && i < screen.size(); ++i) {
                const std::size_t ownerIdx = owningMonitorIndexForScreenPoint(screen[i], monitorsList);
                if (!spanAll && monitorsList.size() > 1 && ownerIdx != selMonIdx) continue;

                BITMAP bitmap{};
                require(GetObjectW(report.samples[i].bitmap.get(), sizeof(bitmap), &bitmap) ==
                            static_cast<int>(sizeof(bitmap)),
                        "cached_bitmap_dimensions_unknown");
                const Vec2 anchor = windowsScreenPixelsToScene(screen[i].x, screen[i].y, workPx, dpiScale);
                const Rect custom = windowsIconImageRect(
                    anchor, {static_cast<float>(bitmap.bmWidth), static_cast<float>(bitmap.bmHeight)}, slotPx,
                    dpiScale, manualOffsetYPixels);
                const Vec2 sz{custom.width, custom.height};
                const Vec2 half = sz * 0.5f;
                Rect clampRc{0.0f, 0.0f, setup.workAreaSize.x, setup.workAreaSize.y};
                if (spanAll && ownerIdx < setup.screenWorkAreas.size()) {
                    const Rect& sw = setup.screenWorkAreas[ownerIdx];
                    if (sw.width >= sz.x && sw.height >= sz.y) clampRc = sw;
                }
                const Vec2 rawCenter = rectCenter(custom);
                const Vec2 clampedCenter{
                    std::clamp(rawCenter.x, clampRc.x + half.x,
                               std::max(clampRc.x + half.x, clampRc.x + clampRc.width - half.x)),
                    std::clamp(rawCenter.y, clampRc.y + half.y,
                               std::max(clampRc.y + half.y, clampRc.y + clampRc.height - half.y))};
                const auto oneBased = static_cast<std::uint32_t>(i + 1);
                setup.icons.push_back({IconId{oneBased}, TextureId{oneBased}, clampedCenter, sz});
                activeOrigIndices.push_back(i);
            }

            // Refresh custom icon picker ListBox (ID 56) to match the active screen's icons
            if (HWND lb = panel ? GetDlgItem(panel, 56) : nullptr) {
                SendMessageW(lb, LB_RESETCONTENT, 0, 0);
                customIconIndices.clear();
                for (std::size_t k = 0; k < activeOrigIndices.size(); ++k) {
                    const std::size_t origIdx = activeOrigIndices[k];
                    const auto itemLabel =
                        std::to_wstring(k + 1) + L". " + report.samples[origIdx].localName;
                    SendMessageW(lb, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(itemLabel.c_str()));
                    if (k < 15u) {
                        SendMessageW(lb, LB_SETSEL, TRUE, static_cast<LPARAM>(k));
                        customIconIndices.push_back(k);
                    }
                }
                pendingBilliardsConfig.customIconIndices = customIconIndices;
                pendingSlingshotConfig.customIconIndices = customIconIndices;
            }
            playback = std::make_unique<PreviewPlayback>(std::move(setup));
        }
        if (playback && playback->game()) {
            playback->game()->setMuted(soundMuted);
            playback->game()->setGravitySettings(gravityMps2, gravityDir, gravityRestitution);
            playback->game()->setBilliardsConfig(pendingBilliardsConfig);
            playback->game()->setSlingshotConfig(pendingSlingshotConfig);
            playback->game()->setSlingshotProgress(pendingSlingshotProgress);
            playback->game()->setSlingshotHighScore(pendingSlingshotHighScore);
            playback->game()->setAirHockeyConfig(pendingAirHockeyConfig);
        }
    }

    void applyLiveDisplayScopeChange() {
        if (!protectedMode || stopping || migration.active() || !protectedSession ||
            !protectedSession->active() || !playback || !playback->game() || !overlay) {
            return;
        }
        const PlayMode curPm = playback->game()->playMode();
        leaveInteraction();
        releaseOwnCapture();
        destroyPassiveOverlays();
        monitorsList = enumerateMonitorsSorted();
        prepareStaticCopies();
        playback->startGravity(true);
        if (playback->game()) {
            playback->game()->setMuted(soundMuted);
            playback->game()->setGravitySettings(gravityMps2, gravityDir, gravityRestitution);
            playback->game()->setBilliardsConfig(pendingBilliardsConfig);
            playback->game()->setSlingshotConfig(pendingSlingshotConfig);
            playback->game()->setSlingshotProgress(pendingSlingshotProgress);
            playback->game()->setSlingshotHighScore(pendingSlingshotHighScore);
            playback->game()->setAirHockeyConfig(pendingAirHockeyConfig);
        }
        if (selectedDisplayMode >= 0 && monitorsList.size() > 1) {
            const std::size_t selIdx = std::min<std::size_t>(
                static_cast<std::size_t>(selectedDisplayMode), monitorsList.size() - 1u);
            presentPassiveOverlaysForSelectedMonitor(selIdx);
        }
        committedInputTime = GetTickCount();
        previous = SteadyClock::now();
        if (curPm != PlayMode::Gravity) {
            setPlayModeSelection(curPm);
        } else {
            armDesktopReturn();
            renderPlayback();
            frames.set(panel, preferredActiveInterval());
        }
        placeAboveShell(true);
        if (hudBar.visible()) {
            hudBar.createAndShow(effectiveHudWorkRect(), previewDpi, userPrefs);
        }
        if (settingsCard.visible()) {
            const PlayMode shownPm =
                (playback && playback->game()) ? playback->game()->playMode() : PlayMode::Gravity;
            settingsCard.showModeSettings(panel, phaseLabel, effectiveHudWorkRect(), hudBar.pillScreenRect(),
                                          hudBar.visible(), previewDpi, shownPm, hasAnyLayoutBackup(), true);
        }
        syncHudBar();
        trace("display_scope_switched",
              selectedDisplayMode < 0 ? 999u : static_cast<uintptr_t>(selectedDisplayMode));
    }
    void registerSessionPowerNotifications() noexcept {
        if (!panel || wtsNotificationRegistered) return;
        if (HMODULE wts = LoadLibraryW(L"wtsapi32.dll")) {
            using RegisterFn = BOOL(WINAPI*)(HWND, DWORD);
            if (auto fn =
                    reinterpret_cast<RegisterFn>(GetProcAddress(wts, "WTSRegisterSessionNotification"))) {
                wtsNotificationRegistered = (fn(panel, 0 /*NOTIFY_FOR_THIS_SESSION*/) != FALSE);
            }
        }
    }
    void unregisterSessionPowerNotifications() noexcept {
        if (!panel || !wtsNotificationRegistered) return;
        if (HMODULE wts = GetModuleHandleW(L"wtsapi32.dll")) {
            using UnregisterFn = BOOL(WINAPI*)(HWND);
            if (auto fn =
                    reinterpret_cast<UnregisterFn>(GetProcAddress(wts, "WTSUnRegisterSessionNotification"))) {
                (void)fn(panel);
            }
        }
        wtsNotificationRegistered = false;
    }
    bool rebuildGraphicsIfDeviceLost(bool forceRecreate = false) noexcept {
#ifdef DPP_DIRECT_INPUT_TESTS
        if (fixtureInput) return false;
#endif
        if (!overlay || !graphics.d3d) return false;
        if (!forceRecreate && SUCCEEDED(graphics.d3d->GetDeviceRemovedReason())) return true;
        if (!protectedSession || !protectedSession->isContinuous()) return false;
        try {
            const RECT w = contract.work;
            invalidateCachedOverlayRgn();
            graphics = {};
            graphics.initialize(overlay, static_cast<UINT>(w.right - w.left),
                                static_cast<UINT>(w.bottom - w.top));
            if (playback) {
                graphics.cacheImages(report);
                graphics.renderCopies(
                    playback->drawList(), previewDpi, originalsOnly, showFilenames,
                    windowsWorkAreaSceneSize({w.left, w.top, w.right, w.bottom}, windowsDpiScale(previewDpi)),
                    !protectedMode);
            } else {
                graphics.render(client);
            }
            trace("graphics_device_recreated");
            return true;
        } catch (...) {
            return false;
        }
    }
    void saveCurrentLayoutToExplorer() {
        if (!protectedMode || !sourceView || !sourceView->view || !playback || !playback->game()) return;
        const auto centers = playback->game()->currentIconSceneCenters();
        if (centers.empty()) return;
        const PixelRect work{contract.work.left, contract.work.top, contract.work.right,
                             contract.work.bottom};
        WinLayoutWritebackPlan plan{};
        const bool built =
            (centers.size() == report.samples.size())
                ? buildWinLayoutWritebackPlan(report, centers, work, contract.systemImagePixels.cx,
                                              previewDpi, manualOffsetYPixels, plan)
                : buildWinLayoutWritebackPlanSubset(report, activeOrigIndices, centers, work,
                                                    contract.systemImagePixels.cx, previewDpi,
                                                    manualOffsetYPixels, plan);
        if (!built || plan.targetViewPoints.empty()) {
            return;
        }

        const std::wstring msg =
            L"即将把当前游乐场中 " + std::to_wstring(centers.size()) +
            L" 个图标的位置写入真实 Windows 桌面。\n\n"
            L"• 默认退出（Esc / 还原）从不改动真实桌面图标坐标；\n"
            L"• 仅当您确认此处操作时才会写回，且写回后可随时点击“撤销写回”恢复原布局。\n\n"
            L"确认写回当前布局吗？";
        modalDialogActive = true;
        const int confirmRes =
            MessageBoxW(IsWindow(panel) ? panel : nullptr, msg.c_str(), L"保存当前图标位置到桌面",
                        MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
        modalDialogActive = false;
        if (confirmRes != IDYES) {
            return;
        }
        std::wstring errMsg;
        if (applyWinLayoutToLiveExplorer(report, plan.targetViewPoints, &errMsg)) {
            if (!savedWinLayoutPlan) {
                savedWinLayoutPlan = plan;
                if (persistedWinLayoutBackup.empty()) {
                    (void)saveWinLayoutBackupFile(layoutBackupFilePath(), report, plan.originalViewPoints);
                    (void)loadWinLayoutBackupFile(layoutBackupFilePath(), persistedWinLayoutBackup);
                }
            } else {
                savedWinLayoutPlan->targetViewPoints = plan.targetViewPoints;
            }
            layoutWritebackApplied = true;
            hudBar.setHasLayoutBackup(true);
            if (HWND bUndo = panel ? GetDlgItem(panel, 72) : nullptr) EnableWindow(bUndo, TRUE);
            if (phaseLabel)
                SetWindowTextW(phaseLabel,
                               L"✓ 已将当前图标位置写回 Windows 桌面（需要时可点击“撤销写回”还原）。");
            if (settingsCard.visible()) settingsCard.refresh(hasAnyLayoutBackup());
            trace("desktop_layout_writeback_applied", plan.targetViewPoints.size());
        } else {
            modalDialogActive = true;
            MessageBoxW(
                IsWindow(panel) ? panel : nullptr,
                errMsg.empty() ? L"Windows 桌面视图拒绝更新图标坐标，原桌面未做保留更改。" : errMsg.c_str(),
                L"桌面位置写回失败", MB_OK | MB_ICONWARNING);
            modalDialogActive = false;
        }
    }
    void undoSavedLayoutInExplorer() {
        if (!hasAnyLayoutBackup()) return;
        std::wstring errMsg;
        const bool ok =
            savedWinLayoutPlan
                ? applyWinLayoutToLiveExplorer(report, savedWinLayoutPlan->originalViewPoints, &errMsg)
                : applyWinNamedLayoutToLiveExplorer(report, persistedWinLayoutBackup, &errMsg);
        if (ok) {
            savedWinLayoutPlan.reset();
            persistedWinLayoutBackup.clear();
            clearWinLayoutBackupFile(layoutBackupFilePath());
            layoutWritebackApplied = true;
            hudBar.setHasLayoutBackup(false);
            if (HWND bUndo = panel ? GetDlgItem(panel, 72) : nullptr) EnableWindow(bUndo, FALSE);
            if (protectedMode && panel && !(protectedSession && protectedSession->active()) &&
                (GetWindowLongPtrW(panel, GWL_STYLE) & WS_VISIBLE) != 0) {
                layoutProtectedPanel(false);
            }
            if (phaseLabel) SetWindowTextW(phaseLabel, L"✓ 已撤销写回并恢复进入游乐场前的原桌面图标坐标。");
            if (settingsCard.visible()) settingsCard.refresh(hasAnyLayoutBackup());
            trace("desktop_layout_writeback_undone");
        }
    }
    bool sessionControlsAllowed() const {
#ifdef DPP_DIRECT_INPUT_TESTS
        if (fixtureInput) return playback != nullptr;
#endif
        return playback && (!protectedMode || (protectedSession && protectedSession->active()));
    }
    void syncLanRole() {
        if (!playback || !playback->game()) return;
        const int role = billiardsLan.isHost() ? 0 : (billiardsLan.isGuest() ? 1 : -1);
        if (auto* bm = playback->game()->mutableBilliardsMode()) {
            bm->setLanLocalPlayer(role);
        }
        if (auto* ah = playback->game()->mutableAirHockeyMode()) {
            ah->setLanLocalPlayer(role);
        }
    }
    void syncLanOutbound(std::span<const SoundEvent> sounds, bool force) {
        if (!playback || !playback->game()) return;
        if (auto* bm = playback->game()->mutableBilliardsMode()) {
            if (billiardsLan.state() == BilliardsLan::State::ConnectedGuest) {
                if (auto act = bm->takeLocalLanAction()) {
                    billiardsLan.sendAction(*act);
                }
                return;
            }
            if (billiardsLan.state() == BilliardsLan::State::ConnectedHost) {
                const double now = migrationTime();
                const bool activeMotion = !playback->idle() || bm->state() == BilliardsState::Charging;
                if (!force && sounds.empty() && !activeMotion && (now - lastLanSnapSent) < 0.065) return;
                lastLanSnapSent = now;
                billiardsLan.sendSnapshot(bm->exportLanSnapshot(), sounds);
            }
            return;
        }
        if (auto* ah = playback->game()->mutableAirHockeyMode()) {
            if (billiardsLan.state() == BilliardsLan::State::ConnectedGuest) {
                AirHockeyRemoteInput in{};
                if (ah->consumePendingRemoteInput(in)) {
                    billiardsLan.sendHockeyInput(in);
                }
                return;
            }
            if (billiardsLan.state() == BilliardsLan::State::ConnectedHost) {
                const double now = migrationTime();
                if (!force && sounds.empty() && (now - lastLanSnapSent) < 0.022) return;
                lastLanSnapSent = now;
                billiardsLan.sendHockeyState(ah->exportLanState(), sounds);
            }
        }
    }
    void flushSounds() {
        if (!playback || !playback->game()) return;
        playback->game()->drainSounds(soundBuf);
        lanSoundBuf = soundBuf;
        if (!soundMuted && !stopping && !migration.active() && !soundBuf.empty()) {
            billiardsAudio.play(soundBuf, false);
        }
        soundBuf.clear();
        syncLanOutbound(lanSoundBuf, false);
        lanSoundBuf.clear();
    }
    void recordBilliardsProgress() {
        if (!playback || !playback->game()) return;
        const auto bs = playback->game()->billiardsSessionStats();
        if (bs.bestShots > 0 &&
            (bestBilliardsShotsObserved == 0 || bs.bestShots < bestBilliardsShotsObserved)) {
            bestBilliardsShotsObserved = bs.bestShots;
        }
        if (bs.totalShots > loggedBilliardsShots) {
            loggedBilliardsShots = bs.totalShots;
            trace("billiards_shot", static_cast<uintptr_t>(bs.totalShots));
        }
        if (bs.totalPocketed > loggedBilliardsPocketed) {
            loggedBilliardsPocketed = bs.totalPocketed;
            trace("billiards_pocketed", static_cast<uintptr_t>(bs.totalPocketed));
        }
        if (bs.totalWhitePlacements > loggedBilliardsPlacements) {
            loggedBilliardsPlacements = bs.totalWhitePlacements;
            trace("billiards_white_placed", static_cast<uintptr_t>(bs.totalWhitePlacements));
        }
    }
    BilliardsConfig readBilliardsConfigFromPanel() {
        BilliardsConfig cfg = pendingBilliardsConfig;
        cfg.style = BilliardsStyle::Desktop;
        if (HWND c41 = panel ? GetDlgItem(panel, 41) : nullptr) {
            const int rIdx = static_cast<int>(SendMessageW(c41, CB_GETCURSEL, 0, 0));
            cfg.rule = rIdx == 1 ? BilliardsRule::EightBall
                                 : (rIdx == 2 ? BilliardsRule::Snooker : BilliardsRule::Free);
        }
        if (HWND c42 = panel ? GetDlgItem(panel, 42) : nullptr) {
            const int mIdx = static_cast<int>(SendMessageW(c42, CB_GETCURSEL, 0, 0));
            cfg.matchMode =
                mIdx == 1 ? BilliardsMatchMode::VsAi
                          : (mIdx == 2 ? BilliardsMatchMode::Hotseat
                                       : (mIdx == 3 ? BilliardsMatchMode::Lan : BilliardsMatchMode::Solo));
        }
        if (HWND c60 = panel ? GetDlgItem(panel, 60) : nullptr) {
            const int dIdx = static_cast<int>(SendMessageW(c60, CB_GETCURSEL, 0, 0));
            cfg.aiDifficulty =
                dIdx == 0
                    ? BilliardsAiDifficulty::Easy
                    : (dIdx == 2 ? BilliardsAiDifficulty::Master
                                 : (dIdx == 3 ? BilliardsAiDifficulty::Goat : BilliardsAiDifficulty::Medium));
        }
        if (HWND c43 = panel ? GetDlgItem(panel, 43) : nullptr)
            cfg.showBallBorders = SendMessageW(c43, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (HWND c44 = panel ? GetDlgItem(panel, 44) : nullptr)
            cfg.showBallNumbers = SendMessageW(c44, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (HWND c45 = panel ? GetDlgItem(panel, 45) : nullptr) {
            const int tIdx = static_cast<int>(SendMessageW(c45, CB_GETCURSEL, 0, 0));
            cfg.tableTheme = tIdx == 1
                                 ? BilliardsTableTheme::Minimal
                                 : (tIdx == 2 ? BilliardsTableTheme::Borderless : BilliardsTableTheme::Full);
            cfg.showTableChrome = (cfg.tableTheme == BilliardsTableTheme::Full);
        }
        if (HWND c46 = panel ? GetDlgItem(panel, 46) : nullptr)
            cfg.showAimGuide = SendMessageW(c46, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (HWND c68 = panel ? GetDlgItem(panel, 68) : nullptr)
            cfg.showAiIntent = SendMessageW(c68, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (HWND c47 = panel ? GetDlgItem(panel, 47) : nullptr) {
            const int iIdx = static_cast<int>(SendMessageW(c47, CB_GETCURSEL, 0, 0));
            cfg.iconSelection = iIdx == 1
                                    ? IconSelectionPolicy::Random
                                    : (iIdx == 2 ? IconSelectionPolicy::Custom : IconSelectionPolicy::FirstN);
        }
        if (HWND c49 = panel ? GetDlgItem(panel, 49) : nullptr) {
            cfg.extraIcons = SendMessageW(c49, CB_GETCURSEL, 0, 0) == 1 ? ExtraIconsPolicy::BottomGravity
                                                                        : ExtraIconsPolicy::Hide;
        }
        if (HWND lb = panel ? GetDlgItem(panel, 56) : nullptr) {
            const int total = static_cast<int>(SendMessageW(lb, LB_GETCOUNT, 0, 0));
            if (total > 0) {
                customIconIndices.clear();
                for (int i = 0; i < total; ++i) {
                    if (SendMessageW(lb, LB_GETSEL, static_cast<WPARAM>(i), 0) > 0) {
                        customIconIndices.push_back(static_cast<std::size_t>(i));
                    }
                }
            }
        }
        cfg.customIconIndices = customIconIndices;
        cfg.randomSeed = randomSeed;
        cfg.freeModeMaxRackBalls = (cfg.rule == BilliardsRule::Free) ? 21u : 0u;
        return cfg;
    }
    void applyBilliardsConfig(const BilliardsConfig& cfg) {
        pendingBilliardsConfig = cfg;
        pendingBilliardsConfig.style = BilliardsStyle::Desktop;
        billiardsLan.setAdvertisedGame(lan_codec::LanGameKind::Billiards, static_cast<int>(cfg.rule));
        const bool hockeyLanActive =
            playback && playback->game() && playback->game()->airHockeyEnabled() &&
            pendingAirHockeyConfig.matchMode == AirHockeyMatchMode::Lan;
        if (cfg.matchMode == BilliardsMatchMode::Lan) {
            if (billiardsLan.state() == BilliardsLan::State::Disconnected) {
                billiardsLan.startDiscovery();
                billiardsLan.sendDiscoveryProbe(lan_codec::LanGameKind::Billiards);
            }
        } else if (!hockeyLanActive &&
                   (billiardsLan.state() != BilliardsLan::State::Disconnected ||
                    billiardsLan.discovering())) {
            billiardsLan.stop();
        }
        if (stopping || migration.active() || !playback || !playback->game()) return;
        playback->game()->setBilliardsConfig(pendingBilliardsConfig);
        syncLanRole();
        if (playback->game()->billiardsEnabled() &&
            billiardsLan.state() == BilliardsLan::State::ConnectedHost) {
            billiardsLan.sendRuleSync(pendingBilliardsConfig.rule);
            syncLanOutbound({}, true);
        }
        previous = SteadyClock::now();
        renderPlayback();
        frames.set(panel, FrameTimer::activeInterval);
    }
    SlingshotConfig readSlingshotConfigFromPanel() {
        SlingshotConfig cfg = pendingSlingshotConfig;
        if (HWND c61 = panel ? GetDlgItem(panel, 61) : nullptr) {
            const int pIdx = std::clamp(static_cast<int>(SendMessageW(c61, CB_GETCURSEL, 0, 0)), 0, 5);
            cfg.preset = static_cast<SlingshotPreset>(pIdx);
        }
        if (HWND c62 = panel ? GetDlgItem(panel, 62) : nullptr) {
            const int aIdx = std::clamp(static_cast<int>(SendMessageW(c62, CB_GETCURSEL, 0, 0)), 0, 3);
            cfg.ammoCount = 3 + aIdx;
        }
        if (HWND c63 = panel ? GetDlgItem(panel, 63) : nullptr) {
            cfg.showTrajectory = SendMessageW(c63, BM_GETCHECK, 0, 0) == BST_CHECKED;
        }
        if (HWND c47 = panel ? GetDlgItem(panel, 47) : nullptr) {
            const int iIdx = static_cast<int>(SendMessageW(c47, CB_GETCURSEL, 0, 0));
            cfg.iconSelection = iIdx == 1
                                    ? IconSelectionPolicy::Random
                                    : (iIdx == 2 ? IconSelectionPolicy::Custom : IconSelectionPolicy::FirstN);
        }
        cfg.customIconIndices = customIconIndices;
        cfg.randomSeed = randomSeed;
        if (playback && playback->game() && !playback->game()->slingshotConfig().customPieces.empty()) {
            pendingSlingshotCustom = playback->game()->slingshotConfig().customPieces;
        }
        cfg.customPieces = pendingSlingshotCustom;
        return cfg;
    }
    void applySlingshotConfig(const SlingshotConfig& cfg) {
        pendingSlingshotConfig = cfg;
        if (stopping || migration.active() || !playback || !playback->game()) return;
        playback->game()->setSlingshotConfig(pendingSlingshotConfig);
        previous = SteadyClock::now();
        renderPlayback();
        frames.set(panel, FrameTimer::activeInterval);
    }
    AirHockeyConfig readAirHockeyConfigFromPanel() {
        AirHockeyConfig cfg = pendingAirHockeyConfig;
        if (HWND c64 = panel ? GetDlgItem(panel, 64) : nullptr) {
            const int mIdx = static_cast<int>(SendMessageW(c64, CB_GETCURSEL, 0, 0));
            cfg.matchMode = mIdx == 1 ? AirHockeyMatchMode::Hotseat2P
                                      : (mIdx == 2 ? AirHockeyMatchMode::Lan : AirHockeyMatchMode::SoloAI);
        }
        if (HWND c65 = panel ? GetDlgItem(panel, 65) : nullptr) {
            const int dIdx = static_cast<int>(SendMessageW(c65, CB_GETCURSEL, 0, 0));
            cfg.difficulty = dIdx == 0
                                 ? AirHockeyDifficulty::Casual
                                 : (dIdx == 2 ? AirHockeyDifficulty::Master : AirHockeyDifficulty::Standard);
        }
        if (HWND c66 = panel ? GetDlgItem(panel, 66) : nullptr) {
            const int gIdx = static_cast<int>(SendMessageW(c66, CB_GETCURSEL, 0, 0));
            cfg.targetGoals = gIdx == 0 ? 5 : (gIdx == 2 ? 11 : 7);
        }
        if (HWND c67 = panel ? GetDlgItem(panel, 67) : nullptr) {
            cfg.showRinkChrome = SendMessageW(c67, BM_GETCHECK, 0, 0) == BST_CHECKED;
        }
        if (HWND c49 = panel ? GetDlgItem(panel, 49) : nullptr) {
            cfg.showSpectators = SendMessageW(c49, CB_GETCURSEL, 0, 0) == 1;
        }
        return cfg;
    }
    void applyAirHockeyConfig(const AirHockeyConfig& cfg) {
        pendingAirHockeyConfig = cfg;
        billiardsLan.setAdvertisedGame(lan_codec::LanGameKind::AirHockey, cfg.targetGoals);
        if (cfg.matchMode == AirHockeyMatchMode::Lan) {
            if (billiardsLan.state() == BilliardsLan::State::Disconnected) {
                billiardsLan.startDiscovery();
                billiardsLan.sendDiscoveryProbe(lan_codec::LanGameKind::AirHockey);
            }
        } else if ((!playback || !playback->game() || !playback->game()->billiardsEnabled()) &&
                   (billiardsLan.state() != BilliardsLan::State::Disconnected ||
                    billiardsLan.discovering())) {
            billiardsLan.stop();
        }
        if (stopping || migration.active() || !playback || !playback->game()) return;
        playback->game()->setAirHockeyConfig(pendingAirHockeyConfig);
        syncLanRole();
        previous = SteadyClock::now();
        renderPlayback();
        frames.set(panel, FrameTimer::activeInterval);
    }
    void applyLoadedPrefsToControls() {
        if (!panel) return;
        suppressPrefSave = true;
        const UserPreferences& p = userPrefs.appPrefs;
        selectedDisplayMode = p.displayMode;
        syncDisplayComboSelection();
        soundMuted = p.soundMuted;
        showFilenames = p.showFilenames;
        savedGravityFilenames = p.showFilenames;
        holeScale = p.blackHoleScale;
        holeElevation = p.blackHoleElevation;
        holeTilt = p.blackHoleTilt;
        gravityMps2 = p.gravityMagnitude;
        gravityDir = p.gravityDirection;
        gravityRestitution = p.gravityRestitution;
        pendingSlingshotProgress = p.slingshotProgress;
        pendingSlingshotHighScore = std::max(p.slingshotHighScore, p.slingshotProgress.highScore);
        pendingSlingshotCustom =
            p.slingshotCustomPieces.empty() ? defaultSlingshotCustomPieces() : p.slingshotCustomPieces;

        if (HWND c29 = GetDlgItem(panel, 29))
            SendMessageW(c29, BM_SETCHECK, showFilenames ? BST_CHECKED : BST_UNCHECKED, 0);
        if (HWND c50 = GetDlgItem(panel, 50))
            SendMessageW(c50, BM_SETCHECK, soundMuted ? BST_CHECKED : BST_UNCHECKED, 0);
        if (HWND c32 = GetDlgItem(panel, 32))
            SendMessageW(c32, TBM_SETPOS, TRUE,
                         std::clamp<int>(static_cast<int>(std::lround(holeScale * 100.0f)), 50, 200));
        syncAngleControls();
        syncGravityControls();

        if (HWND c40 = GetDlgItem(panel, 40))
            SendMessageW(c40, CB_SETCURSEL, std::clamp<int>(static_cast<int>(p.lastPlayMode), 0, 4), 0);
        if (HWND c41 = GetDlgItem(panel, 41))
            SendMessageW(c41, CB_SETCURSEL, std::clamp<int>(static_cast<int>(p.billiardsRule), 0, 2), 0);
        const int mIdx = p.billiardsMatchMode == BilliardsMatchMode::VsAi
                             ? 1
                             : (p.billiardsMatchMode == BilliardsMatchMode::Hotseat
                                    ? 2
                                    : (p.billiardsMatchMode == BilliardsMatchMode::Lan ? 3 : 0));
        if (HWND c42 = GetDlgItem(panel, 42)) SendMessageW(c42, CB_SETCURSEL, mIdx, 0);
        if (HWND c60 = GetDlgItem(panel, 60))
            SendMessageW(c60, CB_SETCURSEL, std::clamp<int>(static_cast<int>(p.billiardsAiDifficulty), 0, 3),
                         0);
        if (HWND c43 = GetDlgItem(panel, 43))
            SendMessageW(c43, BM_SETCHECK, p.showBallBorders ? BST_CHECKED : BST_UNCHECKED, 0);
        if (HWND c44 = GetDlgItem(panel, 44))
            SendMessageW(c44, BM_SETCHECK, p.showBallNumbers ? BST_CHECKED : BST_UNCHECKED, 0);
        if (HWND c45 = GetDlgItem(panel, 45))
            SendMessageW(c45, CB_SETCURSEL, std::clamp<int>(static_cast<int>(p.tableTheme), 0, 2), 0);
        if (HWND c46 = GetDlgItem(panel, 46))
            SendMessageW(c46, BM_SETCHECK, p.showAimGuide ? BST_CHECKED : BST_UNCHECKED, 0);
        if (HWND c68 = GetDlgItem(panel, 68))
            SendMessageW(c68, BM_SETCHECK, p.showAiIntent ? BST_CHECKED : BST_UNCHECKED, 0);
        if (HWND c47 = GetDlgItem(panel, 47))
            SendMessageW(c47, CB_SETCURSEL, std::clamp<int>(static_cast<int>(p.iconSelection), 0, 2), 0);
        if (HWND c49 = GetDlgItem(panel, 49))
            SendMessageW(c49, CB_SETCURSEL, std::clamp<int>(static_cast<int>(p.extraIcons), 0, 1), 0);

        if (HWND c61 = GetDlgItem(panel, 61))
            SendMessageW(c61, CB_SETCURSEL, std::clamp<int>(static_cast<int>(p.slingshotPreset), 0, 5), 0);
        if (HWND c62 = GetDlgItem(panel, 62))
            SendMessageW(c62, CB_SETCURSEL, std::clamp<int>(p.slingshotAmmoCount - 3, 0, 3), 0);
        if (HWND c63 = GetDlgItem(panel, 63))
            SendMessageW(c63, BM_SETCHECK, p.slingshotShowTrajectory ? BST_CHECKED : BST_UNCHECKED, 0);

        if (HWND c64 = GetDlgItem(panel, 64))
            SendMessageW(c64, CB_SETCURSEL, std::clamp<int>(static_cast<int>(p.hockeyMatchMode), 0, 2), 0);
        if (HWND c65 = GetDlgItem(panel, 65))
            SendMessageW(c65, CB_SETCURSEL, std::clamp<int>(static_cast<int>(p.hockeyDifficulty), 0, 2), 0);
        if (HWND c66 = GetDlgItem(panel, 66))
            SendMessageW(c66, CB_SETCURSEL,
                         p.hockeyTargetGoals == 5 ? 0 : (p.hockeyTargetGoals == 11 ? 2 : 1), 0);
        if (HWND c67 = GetDlgItem(panel, 67))
            SendMessageW(c67, BM_SETCHECK, p.hockeyShowRinkChrome ? BST_CHECKED : BST_UNCHECKED, 0);

        if (!p.lastLanEndpoint.empty() && p.lastLanEndpoint != "127.0.0.1:39457") {
            if (HWND ed = GetDlgItem(panel, 51)) {
                SetWindowTextW(ed, utf8ToWide(p.lastLanEndpoint).c_str());
            }
        }

        pendingBilliardsConfig = readBilliardsConfigFromPanel();
        pendingSlingshotConfig = readSlingshotConfigFromPanel();
        pendingAirHockeyConfig = readAirHockeyConfigFromPanel();
        suppressPrefSave = false;
    }
    void saveCurrentPrefs() {
#ifdef DPP_DIRECT_INPUT_TESTS
        if (fixtureInput) return;
#endif
        if (suppressPrefSave || !panel) return;
        const bool inArena = playback && playback->game() &&
                             (playback->game()->billiardsEnabled() || playback->game()->slingshotEnabled() ||
                              playback->game()->airHockeyEnabled());
        userPrefs.soundMuted = soundMuted;
        userPrefs.showFilenames = inArena ? savedGravityFilenames : showFilenames;
        if (playback && playback->game() && sessionControlsAllowed()) {
            userPrefs.lastMode = static_cast<int>(playback->game()->playMode());
            pendingSlingshotProgress = playback->game()->slingshotProgress();
            pendingSlingshotHighScore =
                std::max(pendingSlingshotHighScore, playback->game()->slingshotHighScore());
            pendingSlingshotProgress.highScore =
                std::max(pendingSlingshotProgress.highScore, pendingSlingshotHighScore);
            if (!playback->game()->slingshotConfig().customPieces.empty()) {
                pendingSlingshotCustom = playback->game()->slingshotConfig().customPieces;
            }
        } else if (HWND c40 = GetDlgItem(panel, 40)) {
            userPrefs.lastMode =
                std::clamp<int>(static_cast<int>(SendMessageW(c40, CB_GETCURSEL, 0, 0)), 0, 4);
        }
        UserPreferences& p = userPrefs.appPrefs;
        p.lastPlayMode = static_cast<PlayMode>(std::clamp<int>(userPrefs.lastMode, 0, 4));
        p.displayMode = selectedDisplayMode;
        p.soundMuted = userPrefs.soundMuted;
        p.showFilenames = userPrefs.showFilenames;
        p.gravityMagnitude = gravityMps2;
        p.gravityDirection = gravityDir;
        p.gravityRestitution = gravityRestitution;
        p.blackHoleScale = holeScale;
        p.blackHoleElevation = holeElevation;
        p.blackHoleTilt = holeTilt;
        p.fromBilliardsConfig(readBilliardsConfigFromPanel());
        p.fromSlingshotConfig(readSlingshotConfigFromPanel());
        p.slingshotProgress = pendingSlingshotProgress;
        p.slingshotHighScore = pendingSlingshotHighScore;
        p.slingshotCustomPieces = pendingSlingshotCustom;
        p.fromAirHockeyConfig(readAirHockeyConfigFromPanel());
        wchar_t addrBuf[256]{};
        if (HWND ed = GetDlgItem(panel, 51)) {
            GetWindowTextW(ed, addrBuf, 255);
            const std::string ep = wideToUtf8(addrBuf);
            if (!ep.empty()) p.lastLanEndpoint = ep;
        }
        userPrefs.save();
    }
    void syncHudBar() {
        const bool active = sessionControlsAllowed();
        const PlayMode pm =
            (active && playback && playback->game()) ? playback->game()->playMode() : PlayMode::Gravity;
        const bool paused = active && playback && playback->paused();
        const bool canBil = active && playback && playback->game() &&
                            (playback->game()->canEnableBilliardsWith(pendingBilliardsConfig) ||
                             playback->game()->canEnableAnyBilliards());
        const bool settingsVis = settingsCard.visible() ||
                                 (IsWindow(panel) && (GetWindowLongPtrW(panel, GWL_STYLE) & WS_VISIBLE) != 0);
        hudBar.syncState(active, pm, paused, soundMuted, canBil, settingsVis);
    }
    void syncPlayModeControls(PlayMode mode) {
        if (!panel) return;
        if (HWND c40 = GetDlgItem(panel, 40)) {
            SendMessageW(c40, CB_SETCURSEL, static_cast<WPARAM>(mode), 0);
        }
        if (HWND c30 = GetDlgItem(panel, 30)) {
            SendMessageW(c30, BM_SETCHECK, mode == PlayMode::BlackHole ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        if (protectedMode && settingsCard.visible() && protectedSession && protectedSession->active()) {
            settingsCard.showModeSettings(panel, phaseLabel, effectiveHudWorkRect(), hudBar.pillScreenRect(),
                                          hudBar.visible(), previewDpi, mode, hasAnyLayoutBackup(), true);
        } else if (protectedMode && (GetWindowLongPtrW(panel, GWL_STYLE) & WS_VISIBLE) != 0) {
            layoutProtectedPanel(true);
        }
        syncHudBar();
    }

    void repositionPanelUnderHud(int totalW, int totalH) {
        if (!IsWindow(panel)) return;
        const RECT hw = effectiveHudWorkRect();
        int nx = hw.left + ((hw.right - hw.left) - totalW) / 2;
        int ny = hw.top + 64;
        if (hudBar.visible()) {
            RECT hr{};
            GetWindowRect(hudBar.window(), &hr);
            nx = std::clamp<int>(static_cast<int>((hr.left + hr.right - totalW) / 2),
                                 static_cast<int>(hw.left + 6),
                                 std::max<int>(static_cast<int>(hw.left + 6),
                                               static_cast<int>(hw.right - totalW - 6)));
            ny = static_cast<int>(hr.bottom + 8);
            if (ny + totalH > hw.bottom - 6) {
                ny = std::max<int>(static_cast<int>(hw.top + 6),
                                   static_cast<int>(hr.top - totalH - 8));
            }
        }
        SetWindowPos(panel, HWND_TOP, nx, ny, totalW, totalH, SWP_NOACTIVATE);
    }

    void layoutProtectedPanel(bool repositionToHud = false) {
        if (!protectedMode || !IsWindow(panel)) return;
#ifdef DPP_DIRECT_INPUT_TESTS
        if (fixtureInput) return;
#endif
        const bool sessionLive = (protectedSession && protectedSession->active());

        // Hide all child controls first; selectively reveal only what belongs to the current view.
        for (int i = 0; i < 5; ++i) {
            if (HWND s = GetDlgItem(panel, 100 + i)) ShowWindow(s, SW_HIDE);
            if (answers[static_cast<std::size_t>(i)])
                ShowWindow(answers[static_cast<std::size_t>(i)], SW_HIDE);
        }
        for (int id : {10,  11,  20,  21,  22,  23,  24,  25,  26,  27,  28,  29,  30,  31,  32,  33,  34,
                       35,  36,  37,  40,  41,  42,  43,  44,  45,  46,  47,  49,  50,  51,  52,  53,  54,
                       55,  56,  57,  58,  59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,
                       72,  73,  74,  75,  76,  77,  200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210,
                       211, 212, 213, 214, 215, 216, 217, 218, 219, 220, 221, 222, 223, 224, 225, 226}) {
            if (HWND h = GetDlgItem(panel, id)) ShowWindow(h, SW_HIDE);
        }

        auto showAt = [&](HWND h, int x, int y, int w, int ht) {
            if (!h) return;
            SetWindowPos(h, nullptr, x, y, w, ht, SWP_NOZORDER | SWP_NOACTIVATE);
            ShowWindow(h, SW_SHOWNA);
        };
        auto showId = [&](int id, int x, int y, int w, int ht) {
            showAt(GetDlgItem(panel, id), x, y, w, ht);
        };

        if (!sessionLive) {
            SetWindowTextW(panel, L"桌面游乐场 · DeskFling");
            const int clientW = 520;
            const int clientH = 196;
            showId(200, 16, 12, 488, 64);
            showId(20, 16, 84, 488, 44);
            if (hasAnyLayoutBackup()) {
                showAt(phaseLabel, 16, 138, 266, 44);
                showId(72, 290, 138, 94, 34);
                if (HWND b72 = GetDlgItem(panel, 72)) EnableWindow(b72, TRUE);
            } else {
                showAt(phaseLabel, 16, 138, 366, 44);
            }
            showId(11, 392, 138, 112, 34);
            RECT rc{0, 0, clientW, clientH};
            AdjustWindowRectEx(&rc, static_cast<DWORD>(GetWindowLongPtrW(panel, GWL_STYLE)), FALSE,
                               static_cast<DWORD>(GetWindowLongPtrW(panel, GWL_EXSTYLE)));
            const int winW = rc.right - rc.left;
            const int winH = rc.bottom - rc.top;
            const int wx = contract.work.left + ((contract.work.right - contract.work.left) - winW) / 2;
            const int wy = contract.work.top + ((contract.work.bottom - contract.work.top) - winH) / 2;
            SetWindowPos(panel, HWND_TOP, wx, wy, winW, winH, SWP_NOACTIVATE);
            InvalidateRect(panel, nullptr, TRUE);
            return;
        }

        const PlayMode pm = (playback && playback->game()) ? playback->game()->playMode() : PlayMode::Gravity;
        const int clientW = 420;
        int y = 14;

        if (pm == PlayMode::Gravity) {
            SetWindowTextW(panel, L"普通重力 · 科学物理设置");
            showId(226, 16, y + 4, 66, 22);
            showId(73, 86, y, 222, 180);
            showId(74, 314, y, 90, 140);
            y += 34;
            showId(222, 16, y + 3, 66, 22);
            showId(75, 86, y, 196, 26);
            showId(223, 286, y + 3, 120, 22);
            y += 32;
            showId(224, 16, y + 3, 66, 22);
            showId(76, 86, y, 236, 26);
            showId(225, 328, y + 3, 76, 22);
            y += 32;
            showId(29, 16, y, 184, 26);
            showId(77, clientW - 122, y - 1, 106, 28);
            y += 34;
            showId(71, 16, y, 232, 28);
            showId(72, 256, y, clientW - 272, 28);
            if (HWND b72 = GetDlgItem(panel, 72)) EnableWindow(b72, savedWinLayoutPlan.has_value());
            y += 36;
            if (phaseLabel) {
                SetWindowTextW(
                    phaseLabel,
                    L"加速度遵循真实 SI 物理单位 (s = ½gt²)；默认退出绝不改动原桌面，仅点击保存按钮时写回。");
                showAt(phaseLabel, 16, y, clientW - 32, 38);
                y += 42;
            }
        } else if (pm == PlayMode::BlackHole) {
            SetWindowTextW(panel, L"3D 拟真黑洞 · 专属设置");
            showId(31, 16, y, 210, 26);
            showId(35, clientW - 112, y - 1, 96, 28);
            y += 36;
            showId(206, 16, y + 3, 66, 22);
            showId(32, 86, y, clientW - 102, 26);
            y += 34;
            showId(207, 16, y + 3, 66, 22);
            showId(33, 86, y, clientW - 154, 26);
            showId(36, clientW - 62, y + 3, 46, 22);
            y += 34;
            showId(208, 16, y + 3, 66, 22);
            showId(34, 86, y, clientW - 154, 26);
            showId(37, clientW - 62, y + 3, 46, 22);
            y += 34;
            showId(209, 16, y, clientW - 32, 24);
            y += 30;
        } else if (pm == PlayMode::Billiards) {
            SetWindowTextW(panel, L"桌面台球 · 专属设置");
            showId(212, 16, y + 4, 68, 22);
            showId(41, 88, y, clientW - 104, 140);
            y += 34;
            const int mIdx = static_cast<int>(SendMessageW(GetDlgItem(panel, 42), CB_GETCURSEL, 0, 0));
            showId(213, 16, y + 4, 68, 22);
            if (mIdx == 1) {
                showId(42, 88, y, 186, 140);
                showId(60, 282, y, clientW - 298, 140);
            } else {
                showId(42, 88, y, clientW - 104, 140);
            }
            y += 34;
            showId(221, 16, y + 4, 68, 22);
            showId(45, 88, y, clientW - 104, 140);
            y += 34;
            showId(43, 16, y, 184, 24);
            showId(44, 206, y, 198, 24);
            y += 28;
            showId(46, 16, y, 184, 24);
            if (mIdx == 1) {
                showId(68, 206, y, 202, 24);
            }
            y += 32;
            const int iIdx = static_cast<int>(SendMessageW(GetDlgItem(panel, 47), CB_GETCURSEL, 0, 0));
            showId(217, 16, y + 4, 68, 22);
            showId(47, 88, y, clientW - 104, 140);
            y += 34;
            showId(218, 16, y + 4, 68, 22);
            showId(49, 88, y, clientW - 104, 140);
            y += 34;
            if (iIdx == 2) {
                showId(219, 16, y, clientW - 32, 20);
                y += 22;
                showId(56, 16, y, clientW - 32, 118);
                y += 124;
                showId(57, 16, y, 96, 28);
                showId(58, 120, y, 96, 28);
                showId(59, 224, y, clientW - 240, 28);
                y += 36;
            }
            if (mIdx == 3) {
                showId(220, 16, y, clientW - 32, 20);
                y += 22;
                showId(51, 16, y, clientW - 32, 26);
                y += 32;
                showId(52, 16, y, 92, 28);
                showId(70, 114, y, 96, 28);
                showId(53, 216, y, 92, 28);
                showId(54, 314, y, 90, 28);
                y += 34;
                showId(55, 16, y, clientW - 32, 42);
                y += 46;
            }
        } else if (pm == PlayMode::Slingshot) {
            SetWindowTextW(panel, L"弹弓攻城 · 专属设置");
            showId(214, 16, y + 4, 68, 22);
            showId(61, 88, y, 216, 160);
            showId(69, 312, y - 1, 92, 26);
            y += 34;
            showId(62, 88, y, 132, 120);
            showId(63, 232, y + 2, 172, 24);
            y += 34;
            const int iIdx = static_cast<int>(SendMessageW(GetDlgItem(panel, 47), CB_GETCURSEL, 0, 0));
            showId(217, 16, y + 4, 68, 22);
            showId(47, 88, y, clientW - 104, 140);
            y += 34;
            if (iIdx == 2) {
                showId(219, 16, y, clientW - 32, 20);
                y += 22;
                showId(56, 16, y, clientW - 32, 118);
                y += 124;
                showId(57, 16, y, 96, 28);
                showId(58, 120, y, 96, 28);
                showId(59, 224, y, clientW - 240, 28);
                y += 36;
            }
        } else {
            SetWindowTextW(panel, L"空气曲棍球 · 专属设置");
            const int hmIdx = static_cast<int>(SendMessageW(GetDlgItem(panel, 64), CB_GETCURSEL, 0, 0));
            showId(215, 16, y + 4, 68, 22);
            if (hmIdx == 0) {
                showId(64, 88, y, 186, 120);
                showId(65, 282, y, clientW - 298, 120);
            } else {
                showId(64, 88, y, clientW - 104, 120);
            }
            y += 34;
            showId(216, 16, y + 4, 68, 22);
            showId(66, 88, y, 136, 120);
            showId(67, 236, y + 2, 168, 24);
            y += 34;
            showId(218, 16, y + 4, 68, 22);
            showId(49, 88, y, clientW - 104, 140);
            y += 34;
            if (hmIdx == 2) {
                showId(220, 16, y, clientW - 32, 20);
                y += 22;
                showId(51, 16, y, clientW - 32, 26);
                y += 32;
                showId(52, 16, y, 92, 28);
                showId(70, 114, y, 96, 28);
                showId(53, 216, y, 92, 28);
                showId(54, 314, y, 90, 28);
                y += 34;
                showId(55, 16, y, clientW - 32, 42);
                y += 46;
            }
        }

        const int clientH = y + 8;
        RECT rc{0, 0, clientW, clientH};
        AdjustWindowRectEx(&rc, static_cast<DWORD>(GetWindowLongPtrW(panel, GWL_STYLE)), FALSE,
                           static_cast<DWORD>(GetWindowLongPtrW(panel, GWL_EXSTYLE)));
        const int winW = rc.right - rc.left;
        const int winH = rc.bottom - rc.top;
        if (repositionToHud) {
            repositionPanelUnderHud(winW, winH);
        } else {
            SetWindowPos(panel, nullptr, 0, 0, winW, winH, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        InvalidateRect(panel, nullptr, TRUE);
    }
    void setPlayModeSelection(PlayMode mode) {
        if (stopping || migration.active()) return;
        if (!sessionControlsAllowed()) {
            if (protectedMode) {
                userPrefs.lastMode = std::clamp<int>(static_cast<int>(mode), 0, 4);
                syncPlayModeControls(mode);
                saveCurrentPrefs();
                if (settingsCard.visible()) settingsCard.refresh(hasAnyLayoutBackup());
            } else {
                syncPlayModeControls(PlayMode::Gravity);
                if (phaseLabel) SetWindowTextW(phaseLabel, L"请先点击“一键进入桌面游乐场”后再切换玩法模式。");
            }
            return;
        }
        if (!playback->gravityStarted()) {
            playback->startGravity(true);
            gravityWasStarted = true;
            if (HWND b20 = GetDlgItem(panel, 20)) EnableWindow(b20, FALSE);
            for (int id : {21, 22, 23})
                if (HWND b = GetDlgItem(panel, id)) EnableWindow(b, TRUE);
        }
        if (!playback->game()) return;
        if (playback->game()->playMode() == mode) {
            syncPlayModeControls(mode);
            syncHudBar();
            armDesktopReturn();
            return;
        }
        playback->game()->setMuted(soundMuted);
        p2KeyMask = 0;
        const bool wasBilliards = playback->game()->billiardsEnabled();
        const bool wasArena =
            wasBilliards || playback->game()->slingshotEnabled() || playback->game()->airHockeyEnabled();

        if (mode == PlayMode::Billiards) {
            pendingBilliardsConfig = readBilliardsConfigFromPanel();
            if (!playback->game()->canEnableBilliardsWith(pendingBilliardsConfig)) {
                if (!playback->game()->canEnableAnyBilliards()) {
                    syncPlayModeControls(playback->game()->playMode());
                    if (phaseLabel) SetWindowTextW(phaseLabel, L"当前工作区尺寸过小，无法容纳台球桌。");
                    return;
                }
            }
            leaveInteraction();
            if (!wasArena) {
                savedGravityFilenames = showFilenames;
                showFilenames = false;
                if (HWND c29 = GetDlgItem(panel, 29)) SendMessageW(c29, BM_SETCHECK, BST_UNCHECKED, 0);
            }
            if (playback->paused()) {
                playback->togglePause();
                if (HWND b21 = GetDlgItem(panel, 21)) SetWindowTextW(b21, L"暂停");
            }
            playback->game()->setBilliardsConfig(pendingBilliardsConfig);
            playback->game()->enableBilliardsAutoFit();
            if (!playback->game()->billiardsEnabled()) {
                syncPlayModeControls(playback->game()->playMode());
                return;
            }
            pendingBilliardsConfig = playback->game()->billiardsConfig();
            if (HWND c41 = GetDlgItem(panel, 41)) {
                SendMessageW(c41, CB_SETCURSEL, static_cast<WPARAM>(pendingBilliardsConfig.rule), 0);
            }
            billiardsLan.setAdvertisedGame(lan_codec::LanGameKind::Billiards,
                                           static_cast<int>(pendingBilliardsConfig.rule));
            if (pendingBilliardsConfig.matchMode == BilliardsMatchMode::Lan &&
                billiardsLan.state() == BilliardsLan::State::Disconnected) {
                billiardsLan.startDiscovery();
                billiardsLan.sendDiscoveryProbe(lan_codec::LanGameKind::Billiards);
            }
            syncLanRole();
            if (billiardsLan.state() == BilliardsLan::State::ConnectedHost) {
                billiardsLan.sendRuleSync(pendingBilliardsConfig.rule);
                syncLanOutbound({}, true);
            }
            if (!wasBilliards) {
                ++billiardsToggles;
                trace("billiards_mode_enabled", billiardsToggles);
            }
            syncPlayModeControls(PlayMode::Billiards);
            saveCurrentPrefs();
            armDesktopReturn();
            previous = SteadyClock::now();
            renderPlayback();
            frames.set(panel, FrameTimer::activeInterval);
            if (phaseLabel)
                SetWindowTextW(phaseLabel,
                               L"台球模式：移动鼠标瞄准，按住左键往后拉控力出杆；右键图标球可打开原项目。");
            return;
        }

        if (mode == PlayMode::Slingshot) {
            pendingSlingshotConfig = readSlingshotConfigFromPanel();
            leaveInteraction();
            billiardsLan.stop();
            if (!wasArena) {
                savedGravityFilenames = showFilenames;
                showFilenames = false;
                if (HWND c29 = GetDlgItem(panel, 29)) SendMessageW(c29, BM_SETCHECK, BST_UNCHECKED, 0);
            }
            if (playback->paused()) {
                playback->togglePause();
                if (HWND b21 = GetDlgItem(panel, 21)) SetWindowTextW(b21, L"暂停");
            }
            playback->game()->setSlingshotProgress(pendingSlingshotProgress);
            playback->game()->setSlingshotConfig(pendingSlingshotConfig);
            playback->game()->setPlayMode(PlayMode::Slingshot);
            syncPlayModeControls(PlayMode::Slingshot);
            saveCurrentPrefs();
            armDesktopReturn();
            previous = SteadyClock::now();
            renderPlayback();
            frames.set(panel, FrameTimer::activeInterval);
            if (phaseLabel)
                SetWindowTextW(phaseLabel,
                               L"弹弓攻城：按住左侧弹兜后拉发射，空中再次点击触发二段冲刺，轰塌右侧城堡！");
            return;
        }

        if (mode == PlayMode::AirHockey) {
            pendingAirHockeyConfig = readAirHockeyConfigFromPanel();
            leaveInteraction();
            if (pendingAirHockeyConfig.matchMode != AirHockeyMatchMode::Lan) {
                billiardsLan.stop();
            }
            if (!wasArena) {
                savedGravityFilenames = showFilenames;
                showFilenames = false;
                if (HWND c29 = GetDlgItem(panel, 29)) SendMessageW(c29, BM_SETCHECK, BST_UNCHECKED, 0);
            }
            if (playback->paused()) {
                playback->togglePause();
                if (HWND b21 = GetDlgItem(panel, 21)) SetWindowTextW(b21, L"暂停");
            }
            playback->game()->setAirHockeyConfig(pendingAirHockeyConfig);
            playback->game()->setPlayMode(PlayMode::AirHockey);
            syncLanRole();
            syncPlayModeControls(PlayMode::AirHockey);
            saveCurrentPrefs();
            armDesktopReturn();
            previous = SteadyClock::now();
            renderPlayback();
            frames.set(panel, FrameTimer::fastArenaInterval);
            if (phaseLabel)
                SetWindowTextW(
                    phaseLabel,
                    L"空气曲棍球：移动鼠标直接握持左半场球槌抽射；双人同屏右方用 WASD/方向键控制。");
            return;
        }

        if (wasArena) {
            leaveInteraction();
            billiardsAudio.stop();
            billiardsLan.stop();
            playback->game()->setPlayMode(PlayMode::Gravity);
            showFilenames = savedGravityFilenames;
            if (HWND c29 = GetDlgItem(panel, 29))
                SendMessageW(c29, BM_SETCHECK, showFilenames ? BST_CHECKED : BST_UNCHECKED, 0);
            if (wasBilliards) {
                ++billiardsToggles;
                trace("billiards_mode_disabled", billiardsToggles);
            }
        }

        if (mode == PlayMode::BlackHole) {
            playback->game()->setBlackHoleStyle(BlackHoleStyle::Cinematic);
            playback->game()->setBlackHoleScale(holeScale);
            playback->game()->setBlackHoleAngles(holeElevation, holeTilt);
            playback->game()->setBlackHoleDevour(holeDevour);
            playback->game()->setBlackHole(true);
            syncPlayModeControls(PlayMode::BlackHole);
            flushSounds();
        } else {
            playback->game()->setBlackHole(false);
            syncPlayModeControls(PlayMode::Gravity);
        }
        saveCurrentPrefs();
        armDesktopReturn();
        previous = SteadyClock::now();
        renderPlayback();
        frames.set(panel, FrameTimer::activeInterval);
    }
    HRESULT failureCode = S_OK;
    const char* failureStage = nullptr;
    const char* stopReason = "not_started";
    bool modalDialogActive = false;
    ULONGLONG started = 0;
    static constexpr const wchar_t* panelClass = L"DPPMirrorMarkerPanel";
    static constexpr const wchar_t* overlayClass = L"DPPMirrorMarkers";
    explicit MarkerWindow(winprobe::Report& r, ViewContract c, winprobe::DesktopView* source = nullptr,
                          bool protect = false, bool experiment = false)
        : report(r), contract(c), sourceView(source), protectedMode(protect), startupExperiment(experiment) {
        billiardsLan.onStatus = [this](BilliardsLan::State st, const std::string& statusUtf8) {
            try {
                if (stopping) return;
                syncLanRole();
                if (st == BilliardsLan::State::ConnectedHost && playback && playback->game() &&
                    playback->game()->billiardsEnabled()) {
                    billiardsLan.sendRuleSync(playback->game()->billiardsConfig().rule);
                    syncLanOutbound({}, true);
                }
                if (playback && playback->game() && playback->game()->billiardsEnabled()) {
                    playback->game()->forceRedraw();
                    renderPlayback();
                    frames.set(panel, FrameTimer::activeInterval);
                }
                if (panel) {
                    if (HWND lbl = GetDlgItem(panel, 55)) {
                        const auto ws = utf8ToWide(statusUtf8);
                        SetWindowTextW(lbl, ws.empty() ? L"局域网未连接" : ws.c_str());
                    }
                    if (settingsCard.visible()) settingsCard.refresh(hasAnyLayoutBackup());
                }
            } catch (...) {
            }
        };
        billiardsLan.onRuleSync = [this](BilliardsRule rule) {
            try {
                if (stopping || migration.active()) return;
                if (playback && playback->game() && playback->game()->billiardsEnabled()) {
                    pendingBilliardsConfig = playback->game()->billiardsConfig();
                }
                pendingBilliardsConfig.rule = rule;
                pendingBilliardsConfig.matchMode = BilliardsMatchMode::Lan;
                if (rule != BilliardsRule::Free) pendingBilliardsConfig.freeModeMaxRackBalls = 0u;
                if (panel) {
                    if (HWND c41 = GetDlgItem(panel, 41))
                        SendMessageW(c41, CB_SETCURSEL, static_cast<WPARAM>(rule), 0);
                    if (HWND c42 = GetDlgItem(panel, 42)) SendMessageW(c42, CB_SETCURSEL, 3, 0);
                    if (settingsCard.visible()) settingsCard.refresh(hasAnyLayoutBackup());
                }
                if (playback && playback->game()) {
                    playback->game()->setBilliardsConfig(pendingBilliardsConfig);
                    syncLanRole();
                    renderPlayback();
                }
            } catch (...) {
            }
        };
        billiardsLan.onAction = [this](const BilliardsRemoteAction& act) {
            try {
                if (stopping || migration.active() || !playback || !playback->game() ||
                    !playback->game()->billiardsEnabled())
                    return;
                if (auto* bm = playback->game()->mutableBilliardsMode()) {
                    bm->applyRemoteLanAction(act);
                    playback->game()->forceRedraw();
                    renderPlayback();
                    flushSounds();
                    recordBilliardsProgress();
                    previous = SteadyClock::now();
                    frames.set(panel, FrameTimer::activeInterval);
                }
            } catch (...) {
            }
        };
        billiardsLan.onSnapshot = [this](const BilliardsLanSnapshot& snap,
                                         const std::vector<SoundEvent>& sounds) {
            try {
                if (stopping || migration.active() || !playback || !playback->game() ||
                    !playback->game()->billiardsEnabled())
                    return;
                if (auto* bm = playback->game()->mutableBilliardsMode()) {
                    bm->applyLanSnapshot(snap);
                    playback->game()->forceRedraw();
                    renderPlayback();
                    if (!sounds.empty() && !soundMuted) {
                        billiardsAudio.play(sounds, false);
                    }
                    recordBilliardsProgress();
                }
            } catch (...) {
            }
        };
        billiardsLan.onHockeyInput = [this](const AirHockeyRemoteInput& in) {
            try {
                if (stopping || migration.active() || !playback || !playback->game() ||
                    !playback->game()->airHockeyEnabled())
                    return;
                if (auto* ah = playback->game()->mutableAirHockeyMode()) {
                    ah->applyRemoteInput(in);
                    previous = SteadyClock::now();
                    frames.set(panel, FrameTimer::fastArenaInterval);
                }
            } catch (...) {
            }
        };
        billiardsLan.onHockeyState = [this](const AirHockeyLanState& st,
                                            const std::vector<SoundEvent>& sounds) {
            try {
                if (stopping || migration.active() || !playback || !playback->game() ||
                    !playback->game()->airHockeyEnabled())
                    return;
                if (auto* ah = playback->game()->mutableAirHockeyMode()) {
                    ah->applyLanState(st);
                    playback->game()->forceRedraw();
                    renderPlayback();
                    if (!sounds.empty() && !soundMuted) {
                        billiardsAudio.play(sounds, false);
                    }
                }
            } catch (...) {
            }
        };

        billiardsLan.onRoomsUpdated = [this](const std::vector<lan_codec::DiscoveredLanRoom>& rooms) {
            try {
                if (stopping || !panel) return;
                if (rooms.empty()) return;
                if (billiardsLan.state() == BilliardsLan::State::Disconnected &&
                    !billiardsLan.reconnecting()) {
                    if (HWND ed = GetDlgItem(panel, 51)) {
                        SetWindowTextW(ed, utf8ToWide(rooms.front().endpoint()).c_str());
                    }
                    if (HWND lbl = GetDlgItem(panel, 55)) {
                        const std::wstring tip =
                            L"发现房间：" + utf8ToWide(rooms.front().displayLabel()) +
                            (rooms.size() > 1 ? (L" 等 " + std::to_wstring(rooms.size()) + L" 个") : L"");
                        SetWindowTextW(lbl, tip.c_str());
                    }
                    if (settingsCard.visible()) settingsCard.refresh(hasAnyLayoutBackup());
                }
            } catch (...) {
            }
        };

        hudBar.onSaveDesktopLayout = [this]() {
            try {
                saveCurrentLayoutToExplorer();
            } catch (...) {
            }
        };
        hudBar.onUndoDesktopLayout = [this]() {
            try {
                undoSavedLayoutInExplorer();
            } catch (...) {
            }
        };

        hudBar.onSelectMode = [this](PlayMode pm) {
            try {
                setPlayModeSelection(pm);
            } catch (...) {
            }
        };
        hudBar.onQuickStart = [this]() {
            if (IsWindow(panel)) PostMessageW(panel, WM_COMMAND, 20, 0);
        };
        hudBar.onTogglePause = [this]() {
            if (IsWindow(panel)) PostMessageW(panel, WM_COMMAND, 21, 0);
        };
        hudBar.onReset = [this]() {
            if (IsWindow(panel)) PostMessageW(panel, WM_COMMAND, 22, 0);
        };
        hudBar.onToggleMute = [this]() {
            if (!IsWindow(panel)) return;
            if (HWND c50 = GetDlgItem(panel, 50)) {
                const bool cur = SendMessageW(c50, BM_GETCHECK, 0, 0) == BST_CHECKED;
                SendMessageW(c50, BM_SETCHECK, cur ? BST_UNCHECKED : BST_CHECKED, 0);
                PostMessageW(panel, WM_COMMAND, MAKEWPARAM(50, BN_CLICKED), reinterpret_cast<LPARAM>(c50));
            }
        };
        settingsCard.queryModalDialogActive = [this]() { return modalDialogActive; };
        settingsCard.onCloseCard = [this]() {
            if (!stopping && IsWindow(overlay) && protectedSession && protectedSession->active()) {
                armDesktopReturn();
                (void)SetForegroundWindow(overlay);
                placeAboveShell(false);
            }
            syncHudBar();
        };
        settingsCard.onDidChangeVisibility = [this]() { syncHudBar(); };

        hudBar.onToggleSettings = [this]() {
            if (!IsWindow(panel)) return;
            if (protectedMode) {
                const bool live = (protectedSession && protectedSession->active());
                if (settingsCard.visible()) {
                    settingsCard.hide();
                    if (live && IsWindow(overlay)) {
                        armDesktopReturn();
                        (void)SetForegroundWindow(overlay);
                        placeAboveShell(false);
                    }
                } else if (live) {
                    const PlayMode curPm =
                        (playback && playback->game()) ? playback->game()->playMode() : PlayMode::Gravity;
                    settingsCard.showModeSettings(panel, phaseLabel, effectiveHudWorkRect(),
                                                  hudBar.pillScreenRect(), hudBar.visible(), previewDpi,
                                                  curPm, hasAnyLayoutBackup(), true);
                } else {
                    settingsCard.showLauncher(panel, phaseLabel, effectiveHudWorkRect(), previewDpi,
                                              hasAnyLayoutBackup());
                }
                syncHudBar();
                return;
            }
            if ((GetWindowLongPtrW(panel, GWL_STYLE) & WS_VISIBLE) != 0) {
                ShowWindow(panel, SW_HIDE);
                if (IsWindow(overlay)) {
                    (void)SetForegroundWindow(overlay);
                    placeAboveShell(false);
                }
            } else {
                layoutProtectedPanel(true);
                if (IsIconic(panel)) ShowWindow(panel, SW_RESTORE);
                ShowWindow(panel, SW_SHOWNORMAL);
                SetForegroundWindow(panel);
            }
            syncHudBar();
        };
        hudBar.onHideCapsule = [this]() {
            if (protectedMode && protectedSession && protectedSession->active()) {
                settingsCard.hide();
                if (IsWindow(panel) && (GetWindowLongPtrW(panel, GWL_STYLE) & WS_VISIBLE) != 0) {
                    ShowWindow(panel, SW_HIDE);
                }
            }
            syncHudBar();
        };
        hudBar.onRestorePlaygroundFocus = [this]() {
            if (stopping || !IsWindow(overlay)) return;
            if (settingsCard.visible() ||
                (IsWindow(panel) && (GetWindowLongPtrW(panel, GWL_STYLE) & WS_VISIBLE) != 0))
                return;
            armDesktopReturn();
            (void)SetForegroundWindow(overlay);
            placeAboveShell(false);
        };
        hudBar.onDidMove = [this]() {
            if (protectedSession && protectedSession->active()) {
                if (settingsCard.visible()) {
                    settingsCard.followHud(effectiveHudWorkRect(), hudBar.pillScreenRect(), hudBar.visible());
                } else if (IsWindow(panel) && (GetWindowLongPtrW(panel, GWL_STYLE) & WS_VISIBLE) != 0) {
                    RECT pr{};
                    GetWindowRect(panel, &pr);
                    repositionPanelUnderHud(pr.right - pr.left, pr.bottom - pr.top);
                }
            }
        };
        hudBar.onStopSession = [this]() {
            if (IsWindow(panel)) PostMessageW(panel, WM_COMMAND, 11, 0);
        };
    }
    ~MarkerWindow() {
        stop();
        if (IsWindow(panel)) DestroyWindow(panel);
        if (panelUiFont) {
            DeleteObject(panelUiFont);
            panelUiFont = nullptr;
        }
        UnregisterClassW(panelClass, GetModuleHandleW(nullptr));
        UnregisterClassW(overlayClass, GetModuleHandleW(nullptr));
    }
    void stop() noexcept {
        const bool firstStop = !stopping;
        if (firstStop) {
            saveCurrentPrefs();
            trace("exit_requested");
            trace(stopReason);
        }
        stopping = true;
        settingsCard.destroy();
        iconPopupMenu.destroy();
        hudBar.destroy();
        migration.invalidate();
        billiardsLan.setNotifyWindow(0);
        billiardsLan.stop();
        billiardsAudio.stop();
        if (menuActive) EndMenu();
        openRequest.cancel();
        desktopReady = false;
        openTargets.clear();
        if (protectedMode && panel && escapeHotkeyArmed) {
            UnregisterHotKey(panel, 1);
            escapeHotkeyArmed = false;
        }
        unregisterSessionPowerNotifications();
        if (panel) {
            for (int id : {29, 30, 31, 32, 33, 34, 35, 40, 41, 42, 43, 44, 45, 46, 47, 49, 50, 51, 52,
                           53, 54, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72})
                if (HWND ctrl = GetDlgItem(panel, id)) EnableWindow(ctrl, FALSE);
            KillTimer(panel, 2);
            frames.stop(panel);
        }
        if (playback) playback->stop();
        destroyPassiveOverlays();
        releaseOwnCapture();
        invalidateCachedOverlayRgn();
        graphics = {};
        if (IsWindow(overlay)) DestroyWindow(overlay);
        if (overlay) captureOwnedAfterStop = GetCapture() == overlay;
        overlay = nullptr;
        if (protectedSession) {
            protectedRestoreFailed = !protectedSession->stop();
            if (protectedRestoreFailed) {
                failed = true;
                stopReason = "protected_restore_failed";
            }
        }
        if (firstStop) trace(protectedRestoreFailed ? "recovery_failed" : "exit_completed");
    }
    void releaseOwnCapture() noexcept {
        if (overlay && GetCapture() == overlay) {
            releasingCapture = true;
            const BOOL released = ReleaseCapture();
            const DWORD error = released ? ERROR_SUCCESS : GetLastError();
            if (GetCapture() == overlay) {
                failed = true;
                failureCode = HRESULT_FROM_WIN32(error ? error : ERROR_INVALID_DATA);
                // A failed release must not leave a passive window capturing input.
                DestroyWindow(overlay);
                if (!stopping && panel) PostMessageW(panel, WM_APP + 1, 0, 0);
            }
            releasingCapture = false;
        }
    }
    void interactionStyle(bool enabled) {
        require(overlay != nullptr, "interaction_window_missing");
        auto style = GetWindowLongPtrW(overlay, GWL_EXSTYLE);
        style = enabled ? style & ~(WS_EX_TRANSPARENT | WS_EX_NOACTIVATE)
                        : style | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
        SetLastError(ERROR_SUCCESS);
        const auto old = SetWindowLongPtrW(overlay, GWL_EXSTYLE, style);
        if (!old && GetLastError()) wincheck(FALSE, "SetInteractionStyle");
        wincheck(SetWindowPos(overlay, nullptr, 0, 0, 0, 0,
                              SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED),
                 "ApplyInteractionStyle");
    }
    void placeAboveShell(bool show) {
#ifdef DPP_DIRECT_INPUT_TESTS
        if (fixtureInput) return;
#endif
        HWND shellRoot = GetAncestor(contract.shell, GA_ROOT);  // Z order only, never a coordinate source
        if (!shellRoot || shellRoot == GetDesktopWindow()) {
            if (show) ShowWindow(overlay, SW_SHOWNOACTIVATE);
            return;
        }
        // Walk upward from shellRoot skipping topmost/foreign-band windows that reject SetWindowPos on Win11
        HWND candidateAfter = nullptr;
        HWND cur = shellRoot;
        for (int step = 0; step < 32 && cur; ++step) {
            HWND prev = GetWindow(cur, GW_HWNDPREV);
            if (!prev) break;
            if (prev == overlay) {
                if (show) ShowWindow(overlay, SW_SHOWNOACTIVATE);
                if ((GetWindowLongPtrW(overlay, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) {
                    SetWindowPos(overlay, HWND_NOTOPMOST, 0, 0, 0, 0,
                                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                }
                return;
            }
            if ((GetWindowLongPtrW(prev, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0) {
                candidateAfter = prev;
                break;
            }
            cur = prev;
        }
        BOOL placed = FALSE;
        if (candidateAfter) {
            placed = SetWindowPos(overlay, candidateAfter, 0, 0, 0, 0,
                                  SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | (show ? SWP_SHOWWINDOW : 0));
        }
        if (!placed) {
            // Fallback on Win11 when the window above WorkerW/Progman belongs to an immersive Z-band:
            placed = SetWindowPos(overlay, HWND_NOTOPMOST, 0, 0, 0, 0,
                                  SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | (show ? SWP_SHOWWINDOW : 0));
            if (!placed && show) ShowWindow(overlay, SW_SHOWNOACTIVATE);
        }
        if ((GetWindowLongPtrW(overlay, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) {
            SetWindowPos(overlay, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }
    void armDesktopReturn() {
        if (migration.active() || !backgroundAfterOpen || stopping || !overlay || !playback ||
            !directInput.enabled())
            return;
        desktopReady = true;
        trace("desktop_return_armed");
        interactionStyle(true);
        placeAboveShell(false);
    }
    bool ownForeground() const {
        const auto foreground = GetForegroundWindow();
        return foreground == panel || foreground == overlay ||
               (hudBar.window() != nullptr && foreground == hudBar.window()) ||
               (hudBar.popupWindow() != nullptr && foreground == hudBar.popupWindow()) ||
               (settingsCard.window() != nullptr && foreground == settingsCard.window()) ||
               (settingsCard.popupWindow() != nullptr && foreground == settingsCard.popupWindow()) ||
               (iconPopupMenu.window() != nullptr && foreground == iconPopupMenu.window());
    }
    bool ownOrDesktopForeground() const {
        if (ownForeground()) return true;
        const HWND fg = GetForegroundWindow();
        if (!fg || fg == contract.shell || fg == contract.list) return true;
        DWORD fgProc = 0;
        GetWindowThreadProcessId(fg, &fgProc);
        if (fgProc != 0 && fgProc == contract.process) {
            wchar_t cls[64]{};
            if (GetClassNameW(fg, cls, 64)) {
                if (std::wcscmp(cls, L"Progman") == 0 || std::wcscmp(cls, L"WorkerW") == 0 ||
                    std::wcscmp(cls, L"Shell_TrayWnd") == 0 ||
                    std::wcscmp(cls, L"SecondaryTrayWnd") == 0) {
                    return true;
                }
            }
        }
        return false;
    }
    void syncEscapeHotkey() noexcept {
        if (!protectedMode || !IsWindow(panel) || !protectedSession || !protectedSession->active() ||
            !protectedSession->isContinuous()) {
            return;
        }
        const bool shouldArm = !systemSuspended && !sessionLocked && ownOrDesktopForeground();
        if (shouldArm && !escapeHotkeyArmed) {
            escapeHotkeyArmed = (RegisterHotKey(panel, 1, MOD_NOREPEAT, VK_ESCAPE) != FALSE);
        } else if (!shouldArm && escapeHotkeyArmed) {
            UnregisterHotKey(panel, 1);
            escapeHotkeyArmed = false;
        }
    }
    void leaveInteraction() {
        p2KeyMask = 0;
        if (playback && playback->game()) {
            if (auto* ah = playback->game()->mutableAirHockeyMode()) {
                ah->setPlayer2KeyDirection({});
            }
        }
        if (!playback || !playback->interacting()) return;
        directInput.cancel();
        playback->leaveInteraction();
        ++inputCancels;
        releaseOwnCapture();
        if (overlay && !stopping) {
            interactionStyle(backgroundAfterOpen && directInput.enabled());
            // Activation can raise our interactive window. Put ONLY our window
            // back below ordinary apps when returning to passive playback.
            placeAboveShell(false);
            desktopReady = backgroundAfterOpen && directInput.enabled();
            renderPlayback();
            SetWindowTextW(GetDlgItem(panel, 23),
                           protectedMode ? L"开启交互（手动回退）" : L"进入交互（先确认播放正常）");
            SetWindowTextW(phaseLabel, protectedMode
                                           ? (directInput.enabled() ? L"输入已释放；返回桌面可直接点击镜像。"
                                                                    : L"交互已关闭；用开启交互按钮恢复。")
                                           : L"交互已释放，恢复不接管鼠标的播放。需要时可重新点击进入交互。");
        }
    }
    void pointerEvent(InputType type, LPARAM lp, TextureId target = kNoTexture, bool alt = false) {
        if (migration.active() ||
            (committedInputTime &&
             static_cast<LONG>(static_cast<DWORD>(GetMessageTime()) - committedInputTime) < 0) ||
            !playback || !playback->interacting())
            return;
        const auto p = mapPoint(overlay, nullptr, {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        const auto w = contract.work;
        const PixelRect work{w.left, w.top, w.right, w.bottom};
        const auto position = windowsScreenPixelsToScene(p.x, p.y, work, windowsDpiScale(previewDpi));
        playback->pointer({type, position,
                           std::chrono::duration<double>(SteadyClock::now().time_since_epoch()).count(),
                           target, alt});
        if (playback->game()) {
            if (playback->game()->billiardsEnabled()) {
                const bool guideNow = playback->game()->billiardsConfig().showAimGuide;
                if (guideNow != pendingBilliardsConfig.showAimGuide) {
                    pendingBilliardsConfig.showAimGuide = guideNow;
                    if (panel) {
                        if (HWND c46 = GetDlgItem(panel, 46)) {
                            SendMessageW(c46, BM_SETCHECK, guideNow ? BST_CHECKED : BST_UNCHECKED, 0);
                        }
                    }
                    saveCurrentPrefs();
                    if (settingsCard.visible()) settingsCard.refresh(hasAnyLayoutBackup());
                }
                const bool intentNow = playback->game()->billiardsConfig().showAiIntent;
                if (intentNow != pendingBilliardsConfig.showAiIntent) {
                    pendingBilliardsConfig.showAiIntent = intentNow;
                    if (panel) {
                        if (HWND c68 = GetDlgItem(panel, 68)) {
                            SendMessageW(c68, BM_SETCHECK, intentNow ? BST_CHECKED : BST_UNCHECKED, 0);
                        }
                    }
                    saveCurrentPrefs();
                    if (settingsCard.visible()) settingsCard.refresh(hasAnyLayoutBackup());
                }
                if (playback->game()->tick(0)) renderPlayback();
                flushSounds();
                recordBilliardsProgress();
            } else if (playback->game()->slingshotEnabled()) {
                const bool trajNow = playback->game()->slingshotConfig().showTrajectory;
                bool slingChanged = false;
                if (trajNow != pendingSlingshotConfig.showTrajectory) {
                    pendingSlingshotConfig.showTrajectory = trajNow;
                    slingChanged = true;
                    if (panel) {
                        if (HWND c63 = GetDlgItem(panel, 63)) {
                            SendMessageW(c63, BM_SETCHECK, trajNow ? BST_CHECKED : BST_UNCHECKED, 0);
                        }
                    }
                }
                const auto presetNow = playback->game()->slingshotConfig().preset;
                if (presetNow != pendingSlingshotConfig.preset) {
                    pendingSlingshotConfig.preset = presetNow;
                    slingChanged = true;
                    if (panel) {
                        if (HWND c61 = GetDlgItem(panel, 61)) {
                            SendMessageW(c61, CB_SETCURSEL, static_cast<WPARAM>(presetNow), 0);
                        }
                    }
                }
                if (const auto* sm = playback->game()->slingshotMode()) {
                    if (HWND b69 = panel ? GetDlgItem(panel, 69) : nullptr) {
                        SetWindowTextW(b69, sm->editing() ? L"完成开战" : L"关卡工坊");
                    }
                }
                if (playback->game()->takeSlingshotPersistenceDirty() || slingChanged) {
                    pendingSlingshotProgress = playback->game()->slingshotProgress();
                    pendingSlingshotHighScore =
                        std::max(pendingSlingshotHighScore, playback->game()->slingshotHighScore());
                    if (!playback->game()->slingshotConfig().customPieces.empty()) {
                        pendingSlingshotCustom = playback->game()->slingshotConfig().customPieces;
                    }
                    saveCurrentPrefs();
                    if (settingsCard.visible()) settingsCard.refresh(hasAnyLayoutBackup());
                } else {
                    pendingSlingshotHighScore =
                        std::max(pendingSlingshotHighScore, playback->game()->slingshotHighScore());
                }
                if (playback->game()->tick(0)) renderPlayback();
                flushSounds();
            } else if (playback->game()->airHockeyEnabled()) {
                const auto& hc = playback->game()->airHockeyConfig();
                bool hcChanged = false;
                if (hc.showRinkChrome != pendingAirHockeyConfig.showRinkChrome) {
                    pendingAirHockeyConfig.showRinkChrome = hc.showRinkChrome;
                    hcChanged = true;
                    if (panel) {
                        if (HWND c67 = GetDlgItem(panel, 67)) {
                            SendMessageW(c67, BM_SETCHECK, hc.showRinkChrome ? BST_CHECKED : BST_UNCHECKED,
                                         0);
                        }
                    }
                }
                if (hc.difficulty != pendingAirHockeyConfig.difficulty) {
                    pendingAirHockeyConfig.difficulty = hc.difficulty;
                    hcChanged = true;
                    if (panel) {
                        if (HWND c65 = GetDlgItem(panel, 65)) {
                            SendMessageW(c65, CB_SETCURSEL, static_cast<WPARAM>(hc.difficulty), 0);
                        }
                    }
                }
                if (hcChanged) {
                    saveCurrentPrefs();
                    if (settingsCard.visible()) settingsCard.refresh(hasAnyLayoutBackup());
                }
                if (playback->game()->tick(0)) renderPlayback();
                flushSounds();
            } else if (const auto* g = dynamic_cast<const GravityMode*>(&playback->game()->mode())) {
                const auto bh = g->blackHole();
                if (bh.elevation != holeElevation || bh.tilt != holeTilt) {
                    holeElevation = bh.elevation;
                    holeTilt = bh.tilt;
                    syncAngleControls();
                    saveCurrentPrefs();
                    if (playback->game()->tick(0)) renderPlayback();
                }
                flushSounds();
            }
        }
        frames.set(panel, preferredActiveInterval());
    }
    void renderPlayback() {
#ifdef DPP_DIRECT_INPUT_TESTS
        if (fixtureInput) return;
#endif
        require(playback != nullptr && overlay != nullptr, "playback_not_initialized");
        // Keep the transparent window clipped to drawn/interactive elements. No opaque full-screen fallback.
        HRGN region = CreateRectRgn(0, 0, 0, 0);
        require(region != nullptr, "CreateCopiesRegion");
        if (!scratchPartRgn) {
            scratchPartRgn = CreateRectRgn(0, 0, 0, 0);
            require(scratchPartRgn != nullptr, "CreateScratchRegion");
        }
        try {
            const PixelRect local{0, 0, contract.work.right - contract.work.left,
                                  contract.work.bottom - contract.work.top};
            const float dpiScale = windowsDpiScale(previewDpi);
            const auto ws = windowsWorkAreaSceneSize(
                {contract.work.left, contract.work.top, contract.work.right, contract.work.bottom}, dpiScale);
            auto unionSceneRect = [&](Rect bounds, bool quantizeGrid = false) {
                if (!(bounds.width > 0.0f && bounds.height > 0.0f)) return;
                const auto a = windowsSceneToScreenPixels({bounds.x, bounds.y}, local, dpiScale);
                const auto b = windowsSceneToScreenPixels({bounds.x + bounds.width, bounds.y + bounds.height},
                                                          local, dpiScale);
                int x0 = static_cast<int>(std::floor(a.x));
                int y0 = static_cast<int>(std::floor(a.y));
                int x1 = static_cast<int>(std::ceil(b.x));
                int y1 = static_cast<int>(std::ceil(b.y));
                if (quantizeGrid) {
                    x0 = std::max(0, x0 & ~7);
                    y0 = std::max(0, y0 & ~7);
                    x1 = std::min(static_cast<int>(local.right), (x1 + 7) & ~7);
                    y1 = std::min(static_cast<int>(local.bottom), (y1 + 7) & ~7);
                }
                if (x1 <= x0 || y1 <= y0) return;
                wincheck(SetRectRgn(scratchPartRgn, x0, y0, x1, y1), "SetCopyRegion");
                const int result = CombineRgn(region, region, scratchPartRgn, RGN_OR);
                require(result != ERROR, "CombineCopyRegion");
            };

            bool billiardsSurplusOnly = false;
            bool fullArenaMask = false;
            if (playback->game() && playback->game()->billiardsEnabled()) {
                if (const auto* bm = playback->game()->billiardsMode()) {
                    const Vec2 tl = bm->tableMinScene();
                    const Vec2 sz = bm->tableSizeScene();
                    const float cuePad = 232.0f;
                    const float envX = std::max(0.0f, tl.x - cuePad);
                    const float envY = std::max(0.0f, tl.y - cuePad);
                    const float envR = std::min(ws.x, tl.x + sz.x + cuePad);
                    const float envB = std::min(ws.y, tl.y + sz.y + cuePad);
                    unionSceneRect({envX, envY, std::max(10.0f, envR - envX), std::max(10.0f, envB - envY)});
                    const Vec2 sw = bm->spinWidgetCenterScene();
                    const float sr = bm->spinWidgetRadiusScene() + 12.0f;
                    unionSceneRect({0.0f, 0.0f, ws.x, std::max(68.0f, sw.y + sr)});
                    if (bm->config().effectiveTableTheme() == BilliardsTableTheme::Full) {
                        const float sidePad = bm->ballRadiusScene() * 4.5f;
                        unionSceneRect({std::max(0.0f, tl.x - sidePad * 3.0f),
                                        std::max(0.0f, tl.y - 32.0f),
                                        std::min(ws.x, sz.x + sidePad * 6.0f),
                                        std::min(ws.y, sz.y + 64.0f)});
                    }
                    billiardsSurplusOnly = true;
                }
            } else if (playback->game() && playback->game()->slingshotEnabled()) {
                if (const auto* sm = playback->game()->slingshotMode()) {
                    const Vec2 pr = sm->pouchRestScene();
                    const float pullR = sm->maxPullScene() + 44.0f;
                    unionSceneRect({pr.x - pullR, pr.y - pullR, pullR * 2.0f, pullR * 2.0f});
                    unionSceneRect(sm->trajectoryToggleRect());
                    unionSceneRect(sm->nextStageButtonRect());
                    unionSceneRect(sm->editorToggleRect());
                    if (!sm->isIdle() || sm->editing()) {
                        unionSceneRect({0.0f, 0.0f, ws.x, ws.y});
                        fullArenaMask = true;
                    }
                }
            } else if (playback->game() && playback->game()->airHockeyEnabled()) {
                if (const auto* ah = playback->game()->airHockeyMode()) {
                    const Vec2 rMin = ah->rinkMinScene();
                    const Vec2 rSz = ah->rinkSizeScene();
                    const float envLeft = std::max(0.0f, std::min(rMin.x - 56.0f, ws.x * 0.5f - 396.0f));
                    const float envRight =
                        std::min(ws.x, std::max(rMin.x + rSz.x + 56.0f, ws.x * 0.5f + 396.0f));
                    unionSceneRect({envLeft, 0.0f, std::max(10.0f, envRight - envLeft), ws.y});
                    unionSceneRect({16.0f, 12.0f, 236.0f, 42.0f});
                    fullArenaMask = true;
                }
            }

            const bool skipPerCommandRegion =
                fullArenaMask ||
                (billiardsSurplusOnly && playback->game()->billiardsConfig().extraIcons == ExtraIconsPolicy::Hide);
            std::size_t peerCursor = 0;
            if (!skipPerCommandRegion && !billiardsSurplusOnly && showFilenames) {
                buildFilenamePeers(playback->drawList(), ws, regionFilenamePeersBuf);
            }
            bool hasSurplusBounds = false;
            float surpMinX = 1e9f, surpMinY = 1e9f, surpMaxX = -1e9f, surpMaxY = -1e9f;
            for (const auto& command : playback->drawList().commands()) {
                if (skipPerCommandRegion) break;
                const auto* sprite = std::get_if<SpriteItem>(&command);
                if (billiardsSurplusOnly) {
                    if (!sprite || sprite->opacity <= 0.0f || sprite->clipCircle) continue;
                    const Rect sb = rectFromCenter(sprite->center, sprite->size * 1.5f);
                    surpMinX = std::min(surpMinX, sb.x);
                    surpMinY = std::min(surpMinY, sb.y);
                    surpMaxX = std::max(surpMaxX, sb.x + sb.width);
                    surpMaxY = std::max(surpMaxY, sb.y + sb.height);
                    hasSurplusBounds = true;
                    continue;
                }
                const auto* bx = std::get_if<BoxItem>(&command);
                const auto* poly = std::get_if<PolygonItem>(&command);
                const auto* circle = std::get_if<CircleItem>(&command);
                const auto* line = std::get_if<LineItem>(&command);
                const auto* txt = std::get_if<TextItem>(&command);
                const auto* hole = std::get_if<BlackHoleItem>(&command);
                if (!sprite && !bx && !poly && !circle && !line && !txt &&
                    !(hole && hole->layer == BlackHoleItem::Layer::Back))
                    continue;
                if (sprite && sprite->opacity <= 0) continue;
                float preLabelY = 0.0f;
                if (sprite && sprite->interactive && showFilenames &&
                    peerCursor < regionFilenamePeersBuf.size()) {
                    preLabelY = regionFilenamePeersBuf[peerCursor++].labelY;
                }
                Rect bounds{};
                if (hole) {
                    bounds = rectFromCenter(
                        hole->center,
                        Vec2{hole->radius * bhd3d::kRegionRadii, hole->radius * bhd3d::kRegionRadii} * 2.0f);
                } else if (bx) {
                    const Vec2 he = rotatedIconHalfExtent(bx->size, bx->rotation) +
                                    Vec2{bx->strokeWidth + 6.0f, bx->strokeWidth + 6.0f};
                    bounds = rectFromCenter(bx->center, he * 2.0f);
                } else if (poly) {
                    const std::uint8_t n = std::min<std::uint8_t>(poly->count, 8u);
                    if (n < 3) continue;
                    float minX = poly->points[0].x, maxX = poly->points[0].x;
                    float minY = poly->points[0].y, maxY = poly->points[0].y;
                    for (std::uint8_t i = 1; i < n; ++i) {
                        minX = std::min(minX, poly->points[i].x);
                        maxX = std::max(maxX, poly->points[i].x);
                        minY = std::min(minY, poly->points[i].y);
                        maxY = std::max(maxY, poly->points[i].y);
                    }
                    const float pad = poly->strokeWidth + 5.0f;
                    bounds = {minX - pad, minY - pad, (maxX - minX) + pad * 2.0f, (maxY - minY) + pad * 2.0f};
                } else if (circle) {
                    const float rad = circle->radius + circle->strokeWidth * 0.5f + 10.0f;
                    bounds = rectFromCenter(circle->center, Vec2{rad * 2.0f, rad * 2.0f});
                } else if (line) {
                    const float pad = line->width * 0.6f + 6.0f;
                    const float minX = std::min(line->a.x, line->b.x) - pad;
                    const float maxX = std::max(line->a.x, line->b.x) + pad;
                    const float minY = std::min(line->a.y, line->b.y) - pad;
                    const float maxY = std::max(line->a.y, line->b.y) + pad;
                    bounds = {minX, minY, maxX - minX, maxY - minY};
                } else if (txt) {
                    const float h = std::max(28.0f, txt->size * 1.85f);
                    bounds = txt->centered
                                 ? Rect{txt->position.x - 390.0f, txt->position.y - 4.0f, 780.0f, h + 8.0f}
                                 : Rect{txt->position.x - 4.0f, txt->position.y - 4.0f, 710.0f, h + 8.0f};
                } else {
                    // Conservative rotated sprite bounds; badges stay inside. Not Explorer image bounds.
                    bounds = rectFromCenter(sprite->center, sprite->size * 1.5f);
                }
                if (sprite && sprite->interactive && showFilenames) {
                    const auto name = filenameBoundsFast(*sprite, preLabelY, ws, regionFilenamePeersBuf);
                    const auto right = std::max(bounds.x + bounds.width, name.x + name.width + 1);
                    const auto bottom = std::max(bounds.y + bounds.height, name.y + name.height + 1);
                    bounds.x = std::min(bounds.x, name.x);
                    bounds.y = std::min(bounds.y, name.y);
                    bounds.width = right - bounds.x;
                    bounds.height = bottom - bounds.y;
                }
                unionSceneRect(bounds, sprite != nullptr || hole != nullptr);
            }
            if (hasSurplusBounds && surpMaxX > surpMinX && surpMaxY > surpMinY) {
                unionSceneRect({surpMinX, surpMinY, surpMaxX - surpMinX, surpMaxY - surpMinY}, true);
            }
            if (cachedOverlayRgn && EqualRgn(region, cachedOverlayRgn)) {
                DeleteObject(region);
                region = nullptr;
            } else {
                if (!cachedOverlayRgn) cachedOverlayRgn = CreateRectRgn(0, 0, 0, 0);
                if (cachedOverlayRgn) (void)CombineRgn(cachedOverlayRgn, region, nullptr, RGN_COPY);
                wincheck(SetWindowRgn(overlay, region, FALSE), "SetOwnCopiesRegion");
                region = nullptr;
            }
        } catch (...) {
            if (region) DeleteObject(region);
            throw;
        }
        const auto w = contract.work;
        try {
            graphics.renderCopies(
                playback->drawList(), previewDpi, originalsOnly, showFilenames,
                windowsWorkAreaSceneSize({w.left, w.top, w.right, w.bottom}, windowsDpiScale(previewDpi)),
                !protectedMode);
        } catch (const Failure& renderErr) {
            const bool deviceLost =
                (renderErr.code == D2DERR_RECREATE_TARGET || renderErr.code == DXGI_ERROR_DEVICE_REMOVED ||
                 renderErr.code == DXGI_ERROR_DEVICE_RESET);
            if (!(deviceLost && rebuildGraphicsIfDeviceLost(/*forceRecreate=*/true))) {
                throw;
            }
        }
    }
    void finish(const char* why) noexcept {
        stopReason = why;
        stop();
        if (protectedRestoreFailed) {
            SetWindowTextW(phaseLabel, L"恢复失败，记录已保留。请使用独立恢复入口；再次停止可重试恢复。");
            return;
        }
        if (IsWindow(panel)) DestroyWindow(panel);
    }
    void openMenu(LPARAM lp, TextureId capturedTarget = kNoTexture) {
#ifdef DPP_DIRECT_INPUT_TESTS
        if (fixtureInput) {
            ++fixtureMenus;
            fixtureMenuTarget = capturedTarget;
            return;
        }
#endif
        if (committedInputTime &&
            static_cast<LONG>(static_cast<DWORD>(GetMessageTime()) - committedInputTime) < 0)
            return;
        if (migration.active() || !protectedMode || !protectedSession || !protectedSession->active() ||
            !protectedSession->remainingSeconds() || menuActive || openRequest.busy() || !playback ||
            !directInput.enabled())
            return;
        POINT p = mapPoint(overlay, nullptr, {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        const auto w = contract.work;
        const auto point = windowsScreenPixelsToScene(p.x, p.y, {w.left, w.top, w.right, w.bottom},
                                                      windowsDpiScale(previewDpi));
        const auto id = static_cast<uint32_t>(
            capturedTarget != kNoTexture ? capturedTarget : mirrorHit(playback->drawList(), point));
        if (!id) return;
        const auto found = openTargets.find(id);
        const std::optional<OpenTarget> target =
            found == openTargets.end() ? std::nullopt : std::optional<OpenTarget>(found->second);
        playback->pointer({InputType::PointerCancel, {}, 0});
        releaseOwnCapture();
        if (failed || !IsWindow(overlay)) return;
        std::vector<DarkMenuEntry> openItems;
        openItems.push_back({1, target ? L"打开（保留桌面会话）" : L"此项目暂不支持安全打开", false,
                             target.has_value(), false, false});
        const auto revision = migration.revision();
        menuActive = true;
        const int selected = iconPopupMenu.showModal(overlay, p.x, p.y, previewDpi, openItems, 188.0f);
        menuActive = false;
        previous = SteadyClock::now();
        if (selected == 1 && target && revision == migration.revision() && !migration.active() && !stopping &&
            protectedSession->remainingSeconds()) {
            const auto token = openRequest.queue(*target);
            if (token) trace("open_queued", token);
            if (token && !PostMessageW(panel, WM_APP + 2, token, 0)) openRequest.cancel();
        }
    }
    void openKeepingSession(uintptr_t token) {
        if (migration.active()) return;
        trace("open_dispatch", token);
        bool released = false;
        DWORD error = ERROR_SUCCESS;
        const auto result = openRequest.dispatch(
            token,
            [&] {
                return !stopping && protectedSession && protectedSession->active() &&
                       protectedSession->remainingSeconds() && !failed;
            },
            [&] {
                // Release all input before allowing Shell activation. No session reset or restart.
                // Style/Z-order changes can synchronously deliver deactivation messages too.
                backgroundAfterOpen = true;
                leaveInteraction();
                releaseOwnCapture();
                released = !failed && IsWindow(overlay) && GetCapture() != overlay && playback &&
                           !playback->grabbing();
                if (released) {
                    armDesktopReturn();
                }
                return released;
            },
            validOpenTarget,
            [&](const OpenTarget& target) {
                const bool accepted = submitOpenTarget(target, panel);
                error = accepted ? ERROR_SUCCESS : GetLastError();
                return accepted;
            });
        if (result == OpenResult::ignored) return;
        openAttempts.push_back({token, released, result, error});
        trace("open_result", token, static_cast<int>(result));
        if (result == OpenResult::accepted || result == OpenResult::rejected)
            openSystemAccepted = result == OpenResult::accepted;
        // Shell may pump messages and permit timeout/close/recovery during submission.
        if (stopping) return;
        if (result == OpenResult::invalid || result == OpenResult::rejected) {
            stopReason = result == OpenResult::invalid ? "open_target_invalid" : "open_system_rejected";
            stop();
            for (int id : {20, 21, 22, 23, 24, 25, 26, 27}) EnableWindow(GetDlgItem(panel, id), FALSE);
            SetWindowTextW(phaseLabel, protectedRestoreFailed
                                           ? L"打开失败；恢复失败，记录保留，请使用独立恢复入口。"
                                           : L"打开失败；桌面已恢复，会话已停止。");
            MessageBoxW(
                IsWindow(panel) ? panel : nullptr,
                protectedRestoreFailed
                    ? L"打开失败且桌面恢复失败。请使用独立恢复入口，恢复记录已保留。"
                    : (result == OpenResult::invalid ? L"原项目已失效，未打开。桌面已恢复。"
                                                     : L"系统未接受打开请求。桌面已恢复，不会重新接管。"),
                L"DPP 打开失败", MB_OK | MB_ICONERROR);
            return;
        }
        armDesktopReturn();
        previous = SteadyClock::now();
        if (result == OpenResult::accepted)
            SetWindowTextW(phaseLabel, L"系统已接受打开请求；会话保留。回到桌面可继续右键镜像，Esc 恢复。");
    }
    static LRESULT CALLBACK overlayProcedure(HWND w, UINT m, WPARAM wp, LPARAM lp) noexcept {
        auto* self = reinterpret_cast<MarkerWindow*>(GetWindowLongPtrW(w, GWLP_USERDATA));
        if (m == WM_NCCREATE) {
            self = static_cast<MarkerWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        bool interactive = self && self->playback && self->playback->interacting();
        const bool desktopReady = self && self->desktopReady && !self->stopping;
        try {
            if (m == WM_NCHITTEST) {
                if (!self || self->migration.active() || !(interactive || desktopReady) || !self->playback)
                    return HTTRANSPARENT;
                const auto area = self->contract.work;
                const auto point = windowsScreenPixelsToScene(GET_X_LPARAM(lp), GET_Y_LPARAM(lp),
                                                              {area.left, area.top, area.right, area.bottom},
                                                              windowsDpiScale(self->previewDpi));
                if (self->playback->game() && (self->playback->game()->billiardsEnabled() ||
                                               self->playback->game()->slingshotEnabled() ||
                                               self->playback->game()->airHockeyEnabled())) {
                    return (self->playback->game()->interactiveArenaContains(point) ||
                            mirrorHit(self->playback->drawList(), point) != kNoTexture)
                               ? HTCLIENT
                               : HTTRANSPARENT;
                }
                // Labels add drawing area only; clicking their text must not capture mouse input.
                const bool alt = self->altModifier();
                return (mirrorHit(self->playback->drawList(), point) != kNoTexture ||
                        (self->playback->game() && self->playback->game()->blackHoleEdge(point, alt)))
                           ? HTCLIENT
                           : HTTRANSPARENT;
            }
            // Preserve the original button message. Only the validated down below may activate us.
            if (m == WM_MOUSEACTIVATE)
                return self && self->protectedMode
                           ? MA_NOACTIVATE
                           : ((interactive || desktopReady) ? MA_ACTIVATE : MA_NOACTIVATE);
            if (self && !self->stopping) {
                if (self->menuActive &&
                    (m == WM_KILLFOCUS || m == WM_CANCELMODE || m == WM_ACTIVATE || m == WM_CAPTURECHANGED ||
                     m == WM_LBUTTONDOWN || m == WM_MOUSEMOVE || m == WM_LBUTTONUP))
                    return 0;
                if ((m == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE) || (m == WM_SHOWWINDOW && !wp) ||
                    m == WM_KILLFOCUS || m == WM_CANCELMODE ||
                    (m == WM_CAPTURECHANGED && !self->releasingCapture && reinterpret_cast<HWND>(lp) != w)) {
                    self->leaveInteraction();
                    return 0;
                }
                if (m == WM_KEYDOWN && wp == VK_ESCAPE && (interactive || desktopReady)) {
                    self->finish("interactive_escape");
                    return 0;
                }
                if (m == WM_KEYDOWN && (lp & (1LL << 30)) == 0 && self->sessionControlsAllowed()) {
                    const bool hasCmdMod = (GetKeyState(VK_CONTROL) & 0x8000) != 0 ||
                                           (GetKeyState(VK_MENU) & 0x8000) != 0 ||
                                           (GetKeyState(VK_LWIN) & 0x8000) != 0 ||
                                           (GetKeyState(VK_RWIN) & 0x8000) != 0;
                    if (!hasCmdMod && wp == VK_SPACE && IsWindow(self->panel)) {
                        PostMessageW(self->panel, WM_COMMAND, 21, 0);
                        return 0;
                    }
                    if (!hasCmdMod && wp == 'R' && IsWindow(self->panel) &&
                        !(self->playback && self->playback->game() &&
                          self->playback->game()->airHockeyEnabled() && self->p2KeyMask != 0)) {
                        PostMessageW(self->panel, WM_COMMAND, 22, 0);
                        return 0;
                    }
                    if (!hasCmdMod && wp == VK_TAB && self->protectedMode && self->protectedSession &&
                        self->protectedSession->active()) {
                        if (self->hudBar.visible()) {
                            self->hudBar.hide();
                            self->settingsCard.hide();
                            if (IsWindow(self->panel) &&
                                (GetWindowLongPtrW(self->panel, GWL_STYLE) & WS_VISIBLE) != 0) {
                                ShowWindow(self->panel, SW_HIDE);
                            }
                        } else {
                            self->hudBar.createAndShow(self->effectiveHudWorkRect(), self->previewDpi,
                                                       self->userPrefs);
                        }
                        self->syncHudBar();
                        return 0;
                    }
                }
                if ((m == WM_KEYDOWN || m == WM_KEYUP) && self->playback && self->playback->game() &&
                    self->playback->game()->airHockeyEnabled()) {
                    unsigned int bit = 0;
                    if (wp == 'W' || wp == VK_UP)
                        bit = 1u;
                    else if (wp == 'S' || wp == VK_DOWN)
                        bit = 2u;
                    else if (wp == 'A' || wp == VK_LEFT)
                        bit = 4u;
                    else if (wp == 'D' || wp == VK_RIGHT)
                        bit = 8u;
                    if (bit != 0u) {
                        if (m == WM_KEYDOWN)
                            self->p2KeyMask |= bit;
                        else
                            self->p2KeyMask &= ~bit;
                        Vec2 dir{};
                        if (self->p2KeyMask & 4u) dir.x -= 1.0f;
                        if (self->p2KeyMask & 8u) dir.x += 1.0f;
                        if (self->p2KeyMask & 1u) dir.y -= 1.0f;
                        if (self->p2KeyMask & 2u) dir.y += 1.0f;
                        if (auto* ah = self->playback->game()->mutableAirHockeyMode()) {
                            ah->setPlayer2KeyDirection(dir);
                        }
                        self->frames.set(self->panel, FrameTimer::fastArenaInterval);
                        return 0;
                    }
                }
                if ((self->migration.active() ||
                     (self->committedInputTime && static_cast<LONG>(static_cast<DWORD>(GetMessageTime()) -
                                                                    self->committedInputTime) < 0)) &&
                    (m == WM_LBUTTONDOWN || m == WM_LBUTTONUP || m == WM_MOUSEMOVE || m == WM_RBUTTONDOWN))
                    return 0;
                if (m == WM_RBUTTONUP) {
                    self->directInput.up(true);
                    return 0;
                }
                if ((m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN || m == WM_MBUTTONDOWN) &&
                    self->protectedMode && self->protectedSession && self->protectedSession->active() &&
                    !self->modalDialogActive) {
                    bool dismissed = false;
                    if (self->settingsCard.visible()) {
                        self->settingsCard.hide();
                        dismissed = true;
                    }
                    if (IsWindow(self->panel) &&
                        (GetWindowLongPtrW(self->panel, GWL_STYLE) & WS_VISIBLE) != 0) {
                        ShowWindow(self->panel, SW_HIDE);
                        dismissed = true;
                    }
                    if (dismissed) self->syncHudBar();
                }
                if ((m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN) && (interactive || desktopReady)) {
                    const bool right = m == WM_RBUTTONDOWN;
                    if (self->migration.active() || self->menuActive || self->openRequest.busy() ||
                        (self->committedInputTime && static_cast<LONG>(static_cast<DWORD>(GetMessageTime()) -
                                                                       self->committedInputTime) < 0))
                        return 0;
#ifdef DPP_DIRECT_INPUT_TESTS
                    if (!self->fixtureInput)
#endif
                    {
                        const bool cont = self->protectedSession && self->protectedSession->active() &&
                                          self->protectedSession->isContinuous();
                        if (self->protectedMode && (!liveDisplayContractMatches(self->contract, true, cont) ||
                                                    GetDpiForWindow(w) != self->previewDpi)) {
                            self->beginMigration();
                            return 0;
                        }
                    }
                    const auto p = mapPoint(w, nullptr, {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
                    const auto area = self->contract.work;
                    const auto point =
                        windowsScreenPixelsToScene(p.x, p.y, {area.left, area.top, area.right, area.bottom},
                                                   windowsDpiScale(self->previewDpi));
                    const bool alt = !right && self->altModifier();
                    const bool inBilliards =
                        self->playback->game() && self->playback->game()->billiardsEnabled();
                    const bool inArenaMode = inBilliards || (self->playback->game() &&
                                                             (self->playback->game()->slingshotEnabled() ||
                                                              self->playback->game()->airHockeyEnabled()));
                    if (inArenaMode && right) {
                        self->playback->game()->input({InputType::PointerCancel, {}, 0});
                        self->releaseOwnCapture();
                        if (self->playback->game()->tick(0)) self->renderPlayback();
                    }
                    const auto iconHit = mirrorHit(self->playback->drawList(), point);
                    const bool arenaLeftHit =
                        inArenaMode && !right && self->playback->game()->interactiveArenaContains(point);
                    const auto* gMode =
                        (!inArenaMode && self->playback->game())
                            ? dynamic_cast<const GravityMode*>(&self->playback->game()->mode())
                            : nullptr;
                    const bool classicHole = gMode && gMode->blackHole().style == BlackHoleStyle::Classic;
                    const bool holeEdge = !inArenaMode && !right && self->playback->game() &&
                                          self->playback->game()->blackHoleEdge(point, alt) &&
                                          (classicHole || alt || iconHit == kNoTexture);
                    const auto target = arenaLeftHit ? TextureId{0xfffffffeu}
                                        : holeEdge   ? TextureId{0xffffffffu}
                                                     : iconHit;
                    const auto pointerTarget = (arenaLeftHit || holeEdge) ? kNoTexture : iconHit;
                    const bool valid = self->inputProtectionValid();
                    if (!self->directInput.claim(target, right, valid && (right || !self->playback->paused()),
                                                 static_cast<DWORD>(GetMessageTime())))
                        return 0;
                    const auto revision = self->migration.revision();
                    if (!self->activateInput()) {
                        self->directInput.cancel();
                        return 0;
                    }
                    if (self->stopping || self->migration.active() ||
                        revision != self->migration.revision() || !self->directInput.enabled() ||
                        !self->inputProtectionValid())
                        return 0;
                    if (right) {
                        if (inBilliards && self->playback->game()->billiardsTubeHit(point)) {
                            ++self->billiardsTubeOpens;
                        }
                        self->openMenu(lp, target);
                        self->directInput.up(true);  // TrackPopupMenu consumes the corresponding release.
                        return 0;
                    }
                    if (!self->playback->gravityStarted()) {
                        self->playback->startGravity(true);
                        self->gravityWasStarted = true;
                        EnableWindow(GetDlgItem(self->panel, 20), FALSE);
                        for (int id : {21, 22, 23}) EnableWindow(GetDlgItem(self->panel, id), TRUE);
                    }
                    if (!self->playback->interacting()) {
                        self->playback->enterInteraction(true);
                        self->desktopReady = false;
                        ++self->interactionEntries;
                        self->trace("direct_interaction_entered", static_cast<uintptr_t>(target));
                    }
                    ++self->pointerDowns;
                    self->pointerEvent(InputType::PointerDown, lp, pointerTarget, alt);
                    if (self->playback->grabbing()) {
                        ++self->captureRequests;
                        SetCapture(w);
                        require(GetCapture() == w, "input_capture_failed");
                    }
                    return 0;
                }
                if (m == WM_MOUSEMOVE && interactive && GetCapture() == w) {
                    if (!(wp & MK_LBUTTON)) {
                        self->leaveInteraction();
                        return 0;
                    }
                    ++self->pointerMoves;
                    self->pointerEvent(InputType::PointerMove, lp, kNoTexture, self->altModifier());
                    return 0;
                }
                if (m == WM_MOUSEMOVE && (interactive || desktopReady) && GetCapture() != w &&
                    self->playback->game() &&
                    ((!(wp & MK_LBUTTON) && self->playback->game()->billiardsEnabled()) ||
                     self->playback->game()->airHockeyEnabled()) &&
                    !self->playback->paused() && self->directInput.enabled() && !self->menuActive &&
                    !self->openRequest.busy() && self->inputProtectionValid()) {
                    const auto p = mapPoint(w, nullptr, {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
                    const auto area = self->contract.work;
                    const auto point =
                        windowsScreenPixelsToScene(p.x, p.y, {area.left, area.top, area.right, area.bottom},
                                                   windowsDpiScale(self->previewDpi));
                    self->playback->game()->input(
                        {InputType::PointerMove, point,
                         std::chrono::duration<double>(SteadyClock::now().time_since_epoch()).count()});
                    self->syncLanOutbound({}, false);
                    if (self->playback->game()->airHockeyEnabled()) {
                        self->frames.set(self->panel, FrameTimer::fastArenaInterval);
                    } else {
                        const ULONGLONG nowTick = GetTickCount64();
                        if (nowTick - self->lastHoverRenderTick >= 14u) {
                            if (self->playback->game()->tick(0)) {
                                self->lastHoverRenderTick = nowTick;
                                self->renderPlayback();
                            }
                        }
                    }
                    return 0;
                }
                if (m == WM_DISPLAYCHANGE || m == WM_DPICHANGED || m == WM_SETTINGCHANGE) {
                    const bool activeProt = self->protectedSession && self->protectedSession->active();
                    const bool cont = activeProt && self->protectedSession->isContinuous();
                    if (!liveDisplayContractMatches(self->contract, activeProt, cont) ||
                        GetDpiForWindow(w) != self->previewDpi) {
                        self->beginMigration();
                        return 0;
                    }
                }
                if (m == WM_LBUTTONUP) {
                    self->directInput.up(false);
                    if (!interactive) return 0;
                    ++self->pointerUps;
                    self->pointerEvent(InputType::PointerUp, lp, kNoTexture, self->altModifier());
                    self->releaseOwnCapture();
                    return 0;
                }
            }
        } catch (const Failure& e) {
            if (self) {
                self->failed = true;
                self->failureCode = e.code;
                self->failureStage = e.stage;
                self->finish("overlay_api_failure");
            }
            return 0;
        } catch (...) {
            if (self) {
                self->failed = true;
                self->failureCode = E_UNEXPECTED;
                self->finish("overlay_exception");
            }
            return 0;
        }
        if (m == WM_ERASEBKGND) return 1;
        if (m == WM_PAINT) {
            PAINTSTRUCT ps{};
            BeginPaint(w, &ps);
            EndPaint(w, &ps);
            return 0;
        }
        return DefWindowProcW(w, m, wp, lp);
    }
    static LRESULT CALLBACK panelProcedure(HWND w, UINT m, WPARAM wp, LPARAM lp) noexcept {
        auto* self = reinterpret_cast<MarkerWindow*>(GetWindowLongPtrW(w, GWLP_USERDATA));
        if (m == WM_NCCREATE) {
            self = static_cast<MarkerWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(w, m, wp, lp);
        try {
            if (m == BilliardsLan::kSocketMessage) {
                if (!self->stopping) self->billiardsLan.poll();
                return 0;
            }
            if (m == WinFloatingHudBar::kTrayCallbackMsg) {
                self->hudBar.handleTrayMessage(w, lp);
                return 0;
            }
            if (m == WM_APP + 2) {
                self->openKeepingSession(static_cast<uintptr_t>(wp));
                return 0;
            }
            if (m == WM_HOTKEY && self->protectedMode && self->menuActive) {
                EndMenu();
                return 0;
            }
            if (m == WM_HOTKEY && self->protectedMode && self->settingsCard.popupActive()) {
                self->settingsCard.cancelPopup();
                return 0;
            }
            if (m == WM_HOTKEY && self->protectedMode && self->protectedSession &&
                self->protectedSession->active() && self->settingsCard.visible()) {
                self->settingsCard.hide();
                if (self->settingsCard.onCloseCard) self->settingsCard.onCloseCard();
                return 0;
            }
            if (m == WM_HOTKEY && self->protectedMode) {
                if (self->protectedSession && self->protectedSession->active() &&
                    self->protectedSession->isContinuous() && !self->ownOrDesktopForeground()) {
                    return 0;
                }
                self->finish("protected_emergency_escape");
                return 0;
            }
            if (m == WM_APP + 1) {
                self->finish("capture_release_failed");
                return 0;
            }
            if (m == WM_CLOSE) {
                if (self->protectedMode && self->protectedSession && self->protectedSession->active()) {
                    ShowWindow(w, SW_HIDE);
                    self->syncHudBar();
                    return 0;
                }
                self->finish("closed_without_confirmation");
                return 0;
            }
            if (m == WM_DESTROY) {
                PostQuitMessage(0);
                return 0;
            }
            if (m == WM_KEYDOWN && wp == VK_ESCAPE) {
                self->finish("panel_escape");
                return 0;
            }
            if (m == WM_POWERBROADCAST) {
                if (wp == PBT_APMSUSPEND) {
                    self->systemSuspended = true;
                    self->leaveInteraction();
                    self->billiardsAudio.stop();
                    if (self->protectedSession) self->protectedSession->notifyPowerSuspend(true);
                    self->trace("power_suspend");
                    return TRUE;
                }
                if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) {
                    self->systemSuspended = false;
                    if (self->protectedSession) {
                        self->protectedSession->notifyPowerSuspend(false);
                        (void)self->protectedSession->heartbeat();
                    }
                    self->previous = SteadyClock::now();
                    self->lastFullCheck = GetTickCount64();
                    (void)self->rebuildGraphicsIfDeviceLost();
                    if (self->playback && self->overlay && !self->sessionLocked) self->renderPlayback();
                    self->trace("power_resume");
                    return TRUE;
                }
            }
            if (m == 0x02B1 /*WM_WTSSESSION_CHANGE*/) {
                if (wp == 0x7 /*WTS_SESSION_LOCK*/) {
                    self->sessionLocked = true;
                    self->leaveInteraction();
                    self->billiardsAudio.stop();
                    self->trace("session_locked");
                    return 0;
                }
                if (wp == 0x8 /*WTS_SESSION_UNLOCK*/) {
                    self->sessionLocked = false;
                    self->previous = SteadyClock::now();
                    self->lastFullCheck = GetTickCount64();
                    if (self->protectedSession) (void)self->protectedSession->heartbeat();
                    (void)self->rebuildGraphicsIfDeviceLost();
                    if (self->playback && self->overlay && !self->systemSuspended) self->renderPlayback();
                    self->trace("session_unlocked");
                    return 0;
                }
            }
            if (m == WM_DISPLAYCHANGE || m == WM_DPICHANGED || m == WM_SETTINGCHANGE) {
                const bool activeProt = self->protectedSession && self->protectedSession->active();
                const bool cont = activeProt && self->protectedSession->isContinuous();
                if (cont && (self->systemSuspended || self->sessionLocked)) {
                    return 0;
                }
                if (!liveDisplayContractMatches(self->contract, activeProt, cont) ||
                    (self->overlay && GetDpiForWindow(self->overlay) != self->previewDpi)) {
                    self->beginMigration();
                    return 0;
                }
            }
            if (m == WM_COMMAND && LOWORD(wp) == 10) {
                self->saved = true;
                for (std::size_t i = 0; i < self->answers.size(); ++i)
                    self->savedAnswers[i] =
                        static_cast<int>(SendMessageW(self->answers[i], CB_GETCURSEL, 0, 0));
                self->finish("user_saved_and_stopped");
                return 0;
            }
            if (m == WM_COMMAND && LOWORD(wp) == 11) {
                self->finish("stop_button");
                return 0;
            }
            // Explicit input release outranks migration/stale-command rejection.
            if (m == WM_COMMAND && LOWORD(wp) == 24) {
                self->directInput.disable();
                self->leaveInteraction();
                self->desktopReady = false;
                if (self->overlay && !self->stopping) self->interactionStyle(false);
                return 0;
            }
            if (m == WM_COMMAND &&
                (self->migration.active() ||
                 (self->committedInputTime &&
                  static_cast<LONG>(static_cast<DWORD>(GetMessageTime()) - self->committedInputTime) < 0)))
                return 0;
            if (m == WM_HSCROLL && lp && !self->stopping && !self->migration.active()) {
                const HWND ctrl = reinterpret_cast<HWND>(lp);
                const int id = GetDlgCtrlID(ctrl);
                if (id == 32) {
                    const int raw =
                        std::clamp(static_cast<int>(SendMessageW(ctrl, TBM_GETPOS, 0, 0)), 50, 200);
                    self->holeScale = static_cast<float>(raw) / 100.0f;
                    if (self->playback && self->playback->game()) {
                        self->playback->game()->setBlackHoleScale(self->holeScale);
                        self->renderPlayback();
                    }
                    self->saveCurrentPrefs();
                    return 0;
                }
                if (id == 33 || id == 34) {
                    const int elDeg = std::clamp(
                        static_cast<int>(SendMessageW(GetDlgItem(w, 33), TBM_GETPOS, 0, 0)) - 85, -85, 85);
                    const int tiltDeg = std::clamp(
                        static_cast<int>(SendMessageW(GetDlgItem(w, 34), TBM_GETPOS, 0, 0)) - 180, -180, 180);
                    self->holeElevation = static_cast<float>(elDeg) * 3.14159265f / 180.0f;
                    self->holeTilt = static_cast<float>(tiltDeg) * 3.14159265f / 180.0f;
                    if (HWND elTxt = GetDlgItem(w, 36)) SetWindowTextW(elTxt, formatDeg(elDeg, true).c_str());
                    if (HWND tiltTxt = GetDlgItem(w, 37))
                        SetWindowTextW(tiltTxt, formatDeg(tiltDeg, false).c_str());
                    if (self->playback && self->playback->game()) {
                        self->playback->game()->setBlackHoleAngles(self->holeElevation, self->holeTilt);
                        self->renderPlayback();
                    }
                    self->saveCurrentPrefs();
                    return 0;
                }
                if (id == 75 || id == 76) {
                    if (HWND gBar = GetDlgItem(w, 75)) {
                        const int rawG =
                            std::clamp(static_cast<int>(SendMessageW(gBar, TBM_GETPOS, 0, 0)), 0, 4000);
                        self->gravityMps2 = static_cast<float>(rawG) / 100.0f;
                        if (HWND presetCombo = GetDlgItem(w, 73)) {
                            const GravityPreset matched = matchGravityPreset(self->gravityMps2);
                            SendMessageW(presetCombo, CB_SETCURSEL,
                                         std::clamp<int>(static_cast<int>(matched), 0, 7), 0);
                        }
                    }
                    if (HWND eBar = GetDlgItem(w, 76)) {
                        const int rawE =
                            std::clamp(static_cast<int>(SendMessageW(eBar, TBM_GETPOS, 0, 0)), 0, 95);
                        self->gravityRestitution = static_cast<float>(rawE) / 100.0f;
                    }
                    self->applyGravitySettingsToGame();
                    return 0;
                }
            }
            if (m == WM_COMMAND && LOWORD(wp) == 73 && HIWORD(wp) == CBN_SELCHANGE && !self->stopping) {
                if (HWND presetCombo = GetDlgItem(w, 73)) {
                    const int idx =
                        std::clamp(static_cast<int>(SendMessageW(presetCombo, CB_GETCURSEL, 0, 0)), 0, 7);
                    const auto preset = static_cast<GravityPreset>(idx);
                    if (preset != GravityPreset::Custom) {
                        self->gravityMps2 = gravityPresetMps2(preset);
                        if (HWND gBar = GetDlgItem(w, 75)) {
                            SendMessageW(
                                gBar, TBM_SETPOS, TRUE,
                                std::clamp<int>(static_cast<int>(std::lround(self->gravityMps2 * 100.0f)), 0,
                                                4000));
                        }
                    }
                }
                self->applyGravitySettingsToGame();
                return 0;
            }
            if (m == WM_COMMAND && LOWORD(wp) == 74 && HIWORD(wp) == CBN_SELCHANGE && !self->stopping) {
                if (HWND dirCombo = GetDlgItem(w, 74)) {
                    const int idx =
                        std::clamp(static_cast<int>(SendMessageW(dirCombo, CB_GETCURSEL, 0, 0)), 0, 3);
                    self->gravityDir = static_cast<GravityDirection>(idx);
                }
                self->applyGravitySettingsToGame();
                return 0;
            }
            if (m == WM_COMMAND && LOWORD(wp) == 77 && HIWORD(wp) == BN_CLICKED && !self->stopping) {
                self->gravityMps2 = kEarthGravityMps2;
                self->gravityDir = GravityDirection::Down;
                self->gravityRestitution = kDefaultGravityRestitution;
                self->syncGravityControls();
                self->applyGravitySettingsToGame();
                return 0;
            }
            if (m == WM_COMMAND && LOWORD(wp) == 30 && !self->stopping) {
                const bool checked = SendMessageW(GetDlgItem(w, 30), BM_GETCHECK, 0, 0) == BST_CHECKED;
                self->setPlayModeSelection(checked ? PlayMode::BlackHole : PlayMode::Gravity);
                return 0;
            }
            if (m == WM_COMMAND && LOWORD(wp) == 40 && HIWORD(wp) == CBN_SELCHANGE && !self->stopping) {
                const int sel =
                    std::clamp(static_cast<int>(SendMessageW(GetDlgItem(w, 40), CB_GETCURSEL, 0, 0)), 0, 4);
                const PlayMode pm = static_cast<PlayMode>(sel);
                self->userPrefs.lastMode = sel;
                self->saveCurrentPrefs();
                self->setPlayModeSelection(pm);
                return 0;
            }
            if (m == WM_COMMAND &&
                (LOWORD(wp) == 41 || LOWORD(wp) == 42 || LOWORD(wp) == 45 || LOWORD(wp) == 47 ||
                 LOWORD(wp) == 49 || LOWORD(wp) == 60) &&
                HIWORD(wp) == CBN_SELCHANGE && !self->stopping) {
                if (LOWORD(wp) == 47 && self->playback && self->playback->game() &&
                    self->playback->game()->slingshotEnabled()) {
                    self->applySlingshotConfig(self->readSlingshotConfigFromPanel());
                } else if (LOWORD(wp) == 49 && self->playback && self->playback->game() &&
                           self->playback->game()->airHockeyEnabled()) {
                    self->applyAirHockeyConfig(self->readAirHockeyConfigFromPanel());
                } else {
                    self->applyBilliardsConfig(self->readBilliardsConfigFromPanel());
                }
                self->saveCurrentPrefs();
                if (self->protectedMode && (LOWORD(wp) == 42 || LOWORD(wp) == 47) &&
                    (GetWindowLongPtrW(w, GWL_STYLE) & WS_VISIBLE) != 0) {
                    self->layoutProtectedPanel(false);
                }
                return 0;
            }
            if (m == WM_COMMAND && (LOWORD(wp) == 61 || LOWORD(wp) == 62) && HIWORD(wp) == CBN_SELCHANGE &&
                !self->stopping) {
                self->applySlingshotConfig(self->readSlingshotConfigFromPanel());
                self->saveCurrentPrefs();
                return 0;
            }
            if (m == WM_COMMAND && LOWORD(wp) == 63 && HIWORD(wp) == BN_CLICKED && !self->stopping) {
                self->applySlingshotConfig(self->readSlingshotConfigFromPanel());
                self->saveCurrentPrefs();
                return 0;
            }
            if (m == WM_COMMAND && (LOWORD(wp) == 64 || LOWORD(wp) == 65 || LOWORD(wp) == 66) &&
                HIWORD(wp) == CBN_SELCHANGE && !self->stopping) {
                self->applyAirHockeyConfig(self->readAirHockeyConfigFromPanel());
                self->saveCurrentPrefs();
                if (self->protectedMode && LOWORD(wp) == 64 &&
                    (GetWindowLongPtrW(w, GWL_STYLE) & WS_VISIBLE) != 0) {
                    self->layoutProtectedPanel(false);
                }
                return 0;
            }
            if (m == WM_COMMAND && LOWORD(wp) == 67 && HIWORD(wp) == BN_CLICKED && !self->stopping) {
                self->applyAirHockeyConfig(self->readAirHockeyConfigFromPanel());
                self->saveCurrentPrefs();
                return 0;
            }
            if (m == WM_COMMAND &&
                (LOWORD(wp) == 43 || LOWORD(wp) == 44 || LOWORD(wp) == 46 || LOWORD(wp) == 68) &&
                HIWORD(wp) == BN_CLICKED && !self->stopping) {
                self->applyBilliardsConfig(self->readBilliardsConfigFromPanel());
                self->saveCurrentPrefs();
                return 0;
            }
            if (m == WM_COMMAND && LOWORD(wp) == 50 && HIWORD(wp) == BN_CLICKED && !self->stopping) {
                self->soundMuted = SendMessageW(GetDlgItem(w, 50), BM_GETCHECK, 0, 0) == BST_CHECKED;
                self->userPrefs.soundMuted = self->soundMuted;
                self->saveCurrentPrefs();
                if (self->soundMuted) self->billiardsAudio.stop();
                if (self->playback && self->playback->game())
                    self->playback->game()->setMuted(self->soundMuted);
                self->syncHudBar();
                self->trace("sound_muted", self->soundMuted ? 1u : 0u);
                return 0;
            }
            if (m == WM_COMMAND && (LOWORD(wp) == 57 || LOWORD(wp) == 58) && HIWORD(wp) == BN_CLICKED &&
                !self->stopping) {
                if (HWND lb = GetDlgItem(w, 56)) {
                    SendMessageW(lb, LB_SETSEL, LOWORD(wp) == 57 ? TRUE : FALSE, -1);
                }
                return 0;
            }
            if (m == WM_COMMAND && LOWORD(wp) == 59 && HIWORD(wp) == BN_CLICKED && !self->stopping) {
                if (HWND c47 = GetDlgItem(w, 47)) {
                    const int cur = static_cast<int>(SendMessageW(c47, CB_GETCURSEL, 0, 0));
                    if (cur == 1) {
                        self->randomSeed = self->randomSeed * 1664525u + 1013904223u +
                                           static_cast<std::uint32_t>(GetTickCount());
                    } else if (cur == 0) {
                        SendMessageW(c47, CB_SETCURSEL, 2, 0);
                    }
                }
                if (self->playback && self->playback->game() && self->playback->game()->slingshotEnabled()) {
                    self->applySlingshotConfig(self->readSlingshotConfigFromPanel());
                } else {
                    self->applyBilliardsConfig(self->readBilliardsConfigFromPanel());
                }
                return 0;
            }
            if (m == WM_COMMAND && LOWORD(wp) == 69 && HIWORD(wp) == BN_CLICKED && !self->stopping) {
                if (!self->playback || !self->playback->game()) return 0;
                if (!self->playback->game()->slingshotEnabled()) {
                    if (HWND c61 = GetDlgItem(w, 61)) SendMessageW(c61, CB_SETCURSEL, 5, 0);
                    self->setPlayModeSelection(PlayMode::Slingshot);
                }
                if (auto* sm = self->playback->game()->mutableSlingshotMode()) {
                    const bool nextEdit = !sm->editing();
                    sm->setEditing(nextEdit);
                    if (HWND c61 = GetDlgItem(w, 61)) {
                        SendMessageW(c61, CB_SETCURSEL,
                                     static_cast<WPARAM>(self->playback->game()->slingshotConfig().preset),
                                     0);
                    }
                    if (HWND b69 = GetDlgItem(w, 69)) {
                        SetWindowTextW(b69, nextEdit ? L"完成开战" : L"关卡工坊");
                    }
                    self->playback->game()->forceRedraw();
                    self->renderPlayback();
                    self->saveCurrentPrefs();
                }
                return 0;
            }
            if (m == WM_COMMAND && LOWORD(wp) == 71 && HIWORD(wp) == BN_CLICKED && !self->stopping) {
                self->saveCurrentLayoutToExplorer();
                return 0;
            }
            if (m == WM_COMMAND && LOWORD(wp) == 72 && HIWORD(wp) == BN_CLICKED && !self->stopping) {
                self->undoSavedLayoutInExplorer();
                return 0;
            }
            if (m == WM_COMMAND && (LOWORD(wp) == 52 || LOWORD(wp) == 53 || LOWORD(wp) == 70) &&
                HIWORD(wp) == BN_CLICKED && !self->stopping) {
                wchar_t addrBuf[256]{};
                if (HWND ed = GetDlgItem(w, 51)) GetWindowTextW(ed, addrBuf, 255);
                const std::string ep = wideToUtf8(addrBuf);
                const bool inHockey =
                    self->playback && self->playback->game() && self->playback->game()->airHockeyEnabled();
                if (inHockey) {
                    if (HWND c64 = GetDlgItem(w, 64)) SendMessageW(c64, CB_SETCURSEL, 2, 0);
                    self->pendingAirHockeyConfig = self->readAirHockeyConfigFromPanel();
                    self->pendingAirHockeyConfig.matchMode = AirHockeyMatchMode::Lan;
                    if (!self->migration.active()) {
                        self->playback->game()->setAirHockeyConfig(self->pendingAirHockeyConfig);
                    }
                    self->billiardsLan.setAdvertisedGame(lan_codec::LanGameKind::AirHockey,
                                                         self->pendingAirHockeyConfig.targetGoals);
                } else {
                    if (HWND c42 = GetDlgItem(w, 42)) SendMessageW(c42, CB_SETCURSEL, 3, 0);
                    self->pendingBilliardsConfig = self->readBilliardsConfigFromPanel();
                    self->pendingBilliardsConfig.matchMode = BilliardsMatchMode::Lan;
                    if (self->playback && self->playback->game() && !self->migration.active()) {
                        self->playback->game()->setBilliardsConfig(self->pendingBilliardsConfig);
                    }
                    self->billiardsLan.setAdvertisedGame(lan_codec::LanGameKind::Billiards,
                                                         static_cast<int>(self->pendingBilliardsConfig.rule));
                }
                if (LOWORD(wp) == 52) {
                    std::string parsedIp;
                    std::uint16_t port = BilliardsLan::kDefaultPort;
                    if (!BilliardsLan::parseEndpoint(ep, parsedIp, port, BilliardsLan::kDefaultPort)) {
                        port = BilliardsLan::kDefaultPort;
                    }
                    self->billiardsLan.host(port, false);
                } else if (LOWORD(wp) == 70) {
                    self->billiardsLan.quickScanAndJoin(inHockey ? lan_codec::LanGameKind::AirHockey
                                                                 : lan_codec::LanGameKind::Billiards);
                } else {
                    self->billiardsLan.join(ep.empty() ? std::string("127.0.0.1") : ep,
                                            BilliardsLan::kDefaultPort);
                    self->saveCurrentPrefs();
                }
                self->syncLanRole();
                if (self->playback && self->playback->game()) {
                    self->playback->game()->forceRedraw();
                    self->renderPlayback();
                    self->frames.set(self->panel, FrameTimer::activeInterval);
                }
                return 0;
            }
            if (m == WM_COMMAND && LOWORD(wp) == 54 && HIWORD(wp) == BN_CLICKED && !self->stopping) {
                self->billiardsLan.stop();
                self->syncLanRole();
                if (self->playback && self->playback->game()) {
                    self->playback->game()->forceRedraw();
                    self->renderPlayback();
                }
                return 0;
            }
            if (m == WM_COMMAND && LOWORD(wp) == 31 && !self->stopping) {
                self->holeDevour = SendMessageW(GetDlgItem(w, 31), BM_GETCHECK, 0, 0) == BST_CHECKED;
                if (self->playback && self->playback->game()) {
                    self->playback->game()->setBlackHoleDevour(self->holeDevour);
                    self->trace("black_hole_devour", self->holeDevour ? 1u : 0u);
                    self->frames.set(self->panel, FrameTimer::activeInterval);
                }
                return 0;
            }
            if (m == WM_COMMAND && LOWORD(wp) == 35 && !self->stopping) {
                self->holeElevation = BlackHoleState::kDefaultElevation;
                self->holeTilt = 0.0f;
                self->syncAngleControls();
                self->saveCurrentPrefs();
                if (self->playback && self->playback->game()) {
                    self->playback->game()->setBlackHoleAngles(self->holeElevation, self->holeTilt);
                    self->renderPlayback();
                }
                return 0;
            }
            if (m == WM_COMMAND && LOWORD(wp) == 78 && HIWORD(wp) == CBN_SELCHANGE && !self->stopping) {
                const int nextDispMode = self->readSelectedDisplayModeFromPanel();
                const bool changed = (nextDispMode != self->selectedDisplayMode);
                self->selectedDisplayMode = nextDispMode;
                self->saveCurrentPrefs();
                if (changed && self->protectedMode && self->protectedSession &&
                    self->protectedSession->active() && self->playback && self->playback->game()) {
                    self->applyLiveDisplayScopeChange();
                } else if (self->settingsCard.visible()) {
                    self->settingsCard.refresh(self->hasAnyLayoutBackup());
                }
                return 0;
            }
            if (m == WM_COMMAND && LOWORD(wp) == 29 && HIWORD(wp) == BN_CLICKED && !self->stopping) {
                self->showFilenames = SendMessageW(GetDlgItem(w, 29), BM_GETCHECK, 0, 0) == BST_CHECKED;
                const bool inArena = self->playback && self->playback->game() &&
                                     (self->playback->game()->billiardsEnabled() ||
                                      self->playback->game()->slingshotEnabled() ||
                                      self->playback->game()->airHockeyEnabled());
                if (!inArena) self->savedGravityFilenames = self->showFilenames;
                self->userPrefs.showFilenames = self->savedGravityFilenames;
                self->saveCurrentPrefs();
                ++self->filenameToggles;
                if (!self->passiveOverlayWindows.empty() && self->selectedDisplayMode >= 0 &&
                    self->monitorsList.size() > 1) {
                    const std::size_t selIdx = std::min<std::size_t>(
                        static_cast<std::size_t>(self->selectedDisplayMode), self->monitorsList.size() - 1u);
                    self->presentPassiveOverlaysForSelectedMonitor(selIdx);
                }
                if (self->playback && self->overlay) self->renderPlayback();
                return 0;
            }
            if (m == WM_COMMAND && LOWORD(wp) == 20 && self->copiesEligible && self->overlay) {
                if (!self->playback) {
                    self->markerConfirmed = true;
                    self->graphics.cacheImages(self->report);
                    self->cachedFilenameLayouts = self->graphics.filenames.size();
                    self->prepareStaticCopies();
                    if (self->protectedMode) {
                        require(self->sourceView != nullptr && self->sourceView->folder != nullptr,
                                "full_snapshot_changed_before_hide");
                        self->openTargets.clear();
                        for (size_t i = 0; i < self->report.samples.size(); ++i) {
                            auto target = bindOpenTarget(self->sourceView->folder.Get(),
                                                         self->report.samples[i].child.get());
                            if (target) self->openTargets.emplace(static_cast<uint32_t>(i + 1), *target);
                        }
                        self->escapeHotkeyArmed =
                            (RegisterHotKey(self->panel, 1, MOD_NOREPEAT, VK_ESCAPE) != FALSE);
                        self->protectedSession = std::make_unique<winprotected::ProtectedSession>();
                        require(self->protectedSession->begin(
                                    reinterpret_cast<std::uintptr_t>(self->contract.list),
                                    self->startupExperiment, /*continuousSession=*/true),
                                "protected_session_start_failed");
                        self->registerSessionPowerNotifications();
                        if (self->selectedDisplayMode >= 0 && self->monitorsList.size() > 1) {
                            const std::size_t selIdx = std::min<std::size_t>(
                                static_cast<std::size_t>(self->selectedDisplayMode),
                                self->monitorsList.size() - 1u);
                            self->presentPassiveOverlaysForSelectedMonitor(selIdx);
                        }
                        self->placeAboveShell(true);
                        self->trace("protected_started");
                        self->originalsOnly = false;
                        self->playback->startGravity(true);
                        if (self->playback->game()) self->playback->game()->setMuted(self->soundMuted);
                        self->staticConfirmed = self->gravityWasStarted = true;
                        self->copiesWereShown = true;
                        self->backgroundAfterOpen = true;
                        self->directInput.enable();
                        self->committedInputTime = GetTickCount();
                        self->armDesktopReturn();
                        SetWindowTextW(GetDlgItem(self->panel, 23), L"开启交互（手动回退）");
                        SetWindowTextW(GetDlgItem(self->panel, 24), L"关闭交互");
                        EnableWindow(GetDlgItem(w, 20), FALSE);
                        for (int id : {21, 22, 23}) EnableWindow(GetDlgItem(w, id), TRUE);
                        self->lastFullCheck = GetTickCount64();
                        self->started = GetTickCount64();
                        self->previous = SteadyClock::now();
                        const int initSel = std::clamp(
                            static_cast<int>(SendMessageW(GetDlgItem(w, 40), CB_GETCURSEL, 0, 0)), 0, 4);
                        if (initSel >= 1 && initSel <= 4) {
                            self->setPlayModeSelection(static_cast<PlayMode>(initSel));
                        } else {
                            self->renderPlayback();
                            self->frames.set(w, self->preferredActiveInterval());
                        }
                        SetWindowTextW(self->phaseLabel,
                                       L"桌面游乐场运行中：已收起为系统托盘与顶部悬浮条；右键图标打开原项目，"
                                       L"Esc 随时还原。");
                        self->settingsCard.hide();
                        self->hudBar.createAndShow(self->effectiveHudWorkRect(), self->previewDpi,
                                                   self->userPrefs);
                        ShowWindow(self->panel, SW_HIDE);
                        if (IsWindow(self->overlay)) {
                            (void)SetForegroundWindow(self->overlay);
                            self->placeAboveShell(false);
                        }
                        self->syncHudBar();
                        return 0;
                    }
                    self->renderPlayback();
                    self->copiesWereShown = true;
                    for (int id : {25, 26, 27}) EnableWindow(GetDlgItem(w, id), TRUE);
                    const wchar_t* labels[] = {L"静态副本尺寸、边缘和位置可接受", L"重力下落、碰撞正常",
                                               L"暂停/继续和重置正常", L"透明、普通应用和任务栏操作正常",
                                               L"拖拽/抛掷与切换应用后释放正常"};
                    for (int i = 0; i < 5; ++i) {
                        SetWindowTextW(GetDlgItem(self->panel, 100 + i), labels[i]);
                        SendMessageW(self->answers[static_cast<std::size_t>(i)], CB_SETCURSEL, 0, 0);
                    }
                    SetWindowTextW(GetDlgItem(self->panel, 20), L"静态副本正常，播放重力");
                    SetWindowTextW(
                        self->phaseLabel,
                        L"先用“对照”按钮切换原图标/副本，比较大小、重影和清晰度。位置不符请选有问题并停止。");
                    self->started = GetTickCount64();
                    return 0;
                }
                const bool confirmed = SendMessageW(self->answers[0], CB_GETCURSEL, 0, 0) == 1;
                if (!confirmed) {
                    SetWindowTextW(self->phaseLabel, L"请先核对静态副本，并将第一项选为符合；有问题请停止。");
                    return 0;
                }
                self->originalsOnly = false;
                for (int id : {25, 26, 27}) EnableWindow(GetDlgItem(w, id), FALSE);
                self->playback->startGravity(true);
                if (self->protectedMode)
                    self->backgroundAfterOpen = true;  // permit manual Display Settings handoff
                self->staticConfirmed = self->gravityWasStarted = true;
                self->started = GetTickCount64();
                self->previous = SteadyClock::now();
                self->renderPlayback();
                self->frames.set(w, FrameTimer::activeInterval);
                EnableWindow(GetDlgItem(w, 20), FALSE);
                EnableWindow(GetDlgItem(w, 21), TRUE);
                EnableWindow(GetDlgItem(w, 22), TRUE);
                EnableWindow(GetDlgItem(w, 23), TRUE);
                SetWindowTextW(
                    self->phaseLabel,
                    L"重力播放中，默认不接管鼠标。确认播放正常后可明确进入交互；也可暂停、重置或停止。");
                return 0;
            }
            if (m == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE &&
                reinterpret_cast<HWND>(lp) != self->overlay &&
                reinterpret_cast<HWND>(lp) != self->hudBar.window() && !self->menuActive &&
                !self->modalDialogActive) {
                self->leaveInteraction();
                if (self->protectedMode && self->protectedSession && self->protectedSession->active()) {
                    if (!self->backgroundAfterOpen) {
                        self->finish("protected_app_switch");
                    } else if ((GetWindowLongPtrW(w, GWL_STYLE) & WS_VISIBLE) != 0) {
                        ShowWindow(w, SW_HIDE);
                        self->syncHudBar();
                    }
                }
                return 0;
            }
            if (m == WM_COMMAND && (LOWORD(wp) == 26 || LOWORD(wp) == 27) && self->overlay &&
                self->playback && !self->playback->gravityStarted()) {
                if (!matchesValidatedMarkerConfiguration(self->report, self->contract, self->previewDpi))
                    return 0;
                const int next = self->manualOffsetYPixels + (LOWORD(wp) == 26 ? -1 : 1);
                if (next < -16 || next > 16) return 0;
                self->manualOffsetYPixels = next;
                ++self->offsetAdjustments;
                self->originalsOnly = false;
                SetWindowTextW(GetDlgItem(w, 25), L"对照：仅看原图标");
                self->prepareStaticCopies();
                self->renderPlayback();
                self->updateOffsetLabel();
                SendMessageW(self->answers[0], CB_SETCURSEL, 0, 0);
                return 0;
            }
            if (m == WM_COMMAND && LOWORD(wp) == 25 && self->overlay && self->playback &&
                !self->playback->gravityStarted()) {
                self->originalsOnly = !self->originalsOnly;
                ++self->comparisonToggles;
                self->renderPlayback();
                SetWindowTextW(GetDlgItem(w, 25),
                               self->originalsOnly ? L"对照：显示副本" : L"对照：仅看原图标");
                return 0;
            }
            if (m == WM_COMMAND && LOWORD(wp) == 23 && self->playback && self->overlay) {
                if (self->playback->interacting()) {
                    self->directInput.disable();
                    self->leaveInteraction();
                    return 0;
                }
                self->directInput.enable();
                if (self->protectedMode) {
                    if (!self->inputProtectionValid()) return 0;
                    self->armDesktopReturn();
                    if (self->playback->paused()) {
                        SetWindowTextW(self->phaseLabel, L"交互已开启；当前保持暂停，请先明确继续。");
                        return 0;
                    }
                    // Explicit button remains a real focus/interaction fallback, not just a re-arm.
                    if (!self->activateInput()) return 0;
                    if (!self->playback->gravityStarted()) {
                        self->playback->startGravity(true);
                        self->gravityWasStarted = true;
                        EnableWindow(GetDlgItem(w, 20), FALSE);
                        for (int id : {21, 22, 23}) EnableWindow(GetDlgItem(w, id), TRUE);
                    }
                    self->playback->enterInteraction(true);
                    self->desktopReady = false;
                    ++self->interactionEntries;
                    self->trace("manual_interaction_entered");
                    self->renderPlayback();
                    self->frames.set(w, FrameTimer::activeInterval);
                    SetWindowTextW(self->phaseLabel, L"已手动进入交互；可拖动镜像或右键打开。");
                    return 0;
                }
                const bool confirmed = SendMessageW(self->answers[1], CB_GETCURSEL, 0, 0) == 1;
                if (!confirmed || self->playback->paused() || !self->playback->gravityStarted()) {
                    SetWindowTextW(self->phaseLabel,
                                   L"请先确认重力播放正常（第二项符合），并保持未暂停，再进入交互。");
                    return 0;
                }
                self->playback->enterInteraction(true);
                self->interactionStyle(true);
                self->renderPlayback();
                ++self->interactionEntries;
                self->trace("interaction_entered");
                self->started = GetTickCount64();
                SetWindowTextW(self->phaseLabel,
                               L"交互中：只拖动带 DPP 标识的副本。切换应用或操作面板会释放交互；Esc "
                               L"在副本窗口收到键盘时停止。");
                return 0;
            }
            if (m == WM_COMMAND && self->playback && self->overlay && self->playback->gravityStarted()) {
                if (LOWORD(wp) == 21) {
                    self->leaveInteraction();
                    self->playback->togglePause();
                    if (self->playback->paused()) self->billiardsAudio.stop();
                    ++self->pauseCommands;
                    self->trace(self->playback->paused() ? "pause" : "resume");
                    SetWindowTextW(GetDlgItem(w, 21), self->playback->paused() ? L"继续" : L"暂停");
                    self->syncHudBar();
                    self->armDesktopReturn();
                } else if (LOWORD(wp) == 22) {
                    self->leaveInteraction();
                    self->playback->reset();
                    self->syncLanRole();
                    if (self->billiardsLan.state() == BilliardsLan::State::ConnectedHost &&
                        self->playback->game() &&
                        (self->playback->game()->billiardsEnabled() ||
                         self->playback->game()->airHockeyEnabled())) {
                        self->syncLanOutbound({}, true);
                    }
                    ++self->resetCommands;
                    self->trace("reset");
                    self->syncHudBar();
                    self->armDesktopReturn();
                } else
                    return DefWindowProcW(w, m, wp, lp);
                self->previous = SteadyClock::now();
                self->renderPlayback();
                self->frames.set(w, (self->playback->idle() && !self->billiardsLan.connected())
                                        ? FrameTimer::idleInterval
                                        : self->preferredActiveInterval());
                return 0;
            }
            if (m == WM_TIMER && self->frames.accepts(wp) && self->playback && self->overlay) {
                if (!self->stopping && self->billiardsLan.needsFastPoll()) {
                    self->billiardsLan.poll();
                }
                if (self->migration.active()) {
                    self->advanceMigration();
                    return 0;
                }
                const auto now = SteadyClock::now();
                const double dt = std::chrono::duration<double>(now - self->previous).count();
                self->previous = now;
                const bool continuousActive = self->protectedSession && self->protectedSession->active() &&
                                              self->protectedSession->isContinuous();
                const bool lanBusy =
                    self->billiardsLan.connected() || self->billiardsLan.needsFastPoll();
                const bool suspended = self->systemSuspended || self->sessionLocked ||
                                       self->migration.active() || self->menuActive ||
                                       self->openRequest.busy() ||
                                       (!continuousActive && self->backgroundAfterOpen &&
                                        !self->ownForeground() && !lanBusy);
                if (!suspended && self->playback->tick(dt)) self->renderPlayback();
                if (!suspended) {
                    self->flushSounds();
                    self->recordBilliardsProgress();
                    if (self->playback->game() && self->playback->game()->takeSlingshotPersistenceDirty()) {
                        self->pendingSlingshotProgress = self->playback->game()->slingshotProgress();
                        self->pendingSlingshotHighScore = std::max(
                            self->pendingSlingshotHighScore, self->playback->game()->slingshotHighScore());
                        if (!self->playback->game()->slingshotConfig().customPieces.empty()) {
                            self->pendingSlingshotCustom =
                                self->playback->game()->slingshotConfig().customPieces;
                        }
                        self->saveCurrentPrefs();
                    }
                }
                self->frames.set(w, (suspended || (self->playback->idle() && !lanBusy))
                                        ? FrameTimer::idleInterval
                                        : self->preferredActiveInterval());
                return 0;
            }
            if (m == WM_TIMER && wp == 2 && self->overlay) {
                if (!self->stopping &&
                    (self->billiardsLan.state() != BilliardsLan::State::Disconnected ||
                     self->billiardsLan.reconnecting() || self->billiardsLan.discovering())) {
                    self->billiardsLan.poll();
                }
                const bool continuousGuard = self->protectedSession && self->protectedSession->active() &&
                                             self->protectedSession->isContinuous();
                self->syncEscapeHotkey();
                const bool interactiveDesktop = winprobe::interactive(winprobe::inspectEnvironment());
                const bool sleepingOrLocked =
                    continuousGuard && (self->systemSuspended || self->sessionLocked || !interactiveDesktop);
                if (!sleepingOrLocked && self->migration.active()) {
                    self->advanceMigration();
                    if (!self->overlay) return 0;
                }
                if (self->protectedSession && self->protectedSession->active()) {
                    if (!self->protectedSession->heartbeat()) {
                        self->stopReason = "protected_deadline_or_helper_stop";
                        self->automaticStopped = true;
                        self->stop();
                        for (int id : {20, 21, 22, 23, 24, 25, 26, 27})
                            EnableWindow(GetDlgItem(w, id), FALSE);
                        if (!IsWindowVisible(w)) ShowWindow(w, SW_SHOWNORMAL);
                        SetWindowTextW(self->phaseLabel, self->protectedRestoreFailed
                                                             ? L"恢复失败，记录保留，请使用独立恢复入口。"
                                                             : L"守护时限已到：原图标已自动恢复。");
                        SetWindowTextW(w, L"桌面游乐场 · DeskFling——会话已停止，原图标已恢复");
                        return 0;
                    }
                    if (!self->protectedSession->isContinuous()) {
                        const auto title = L"实验性真实桌面会话——剩余 " +
                                           std::to_wstring(self->protectedSession->remainingSeconds()) +
                                           L" 秒（Esc 恢复）";
                        SetWindowTextW(w, title.c_str());
                    }
                    if (sleepingOrLocked) {
                        self->lastFullCheck = GetTickCount64();
                        self->previous = SteadyClock::now();
                        return 0;
                    }
                    if (!self->menuActive && !self->backgroundAfterOpen && !self->ownForeground()) {
                        self->finish("protected_external_focus");
                        return 0;
                    }
                    if (!continuousGuard && GetTickCount64() - self->lastFullCheck >= 2000) {
                        self->lastFullCheck = GetTickCount64();
                        if (!self->fullSnapshotStillValid()) {
                            self->finish("protected_items_changed");
                            return 0;
                        }
                    }
                }
                if (!self->protectedMode && GetTickCount64() - self->started >= 90000) {
                    self->automaticStopped = true;
                    self->stop();
                    for (int id : {20, 21, 22, 23, 24, 25, 26, 27}) EnableWindow(GetDlgItem(w, id), FALSE);
                    SetWindowTextW(self->phaseLabel,
                                   L"90 秒已自动停止桌面绘制。请补选结果，再点保存结果并停止。");
                } else if ((!self->protectedMode ||
                            (self->protectedSession && self->protectedSession->active())) &&
                           (!liveDisplayContractMatches(
                                self->contract, self->protectedSession && self->protectedSession->active(),
                                continuousGuard) ||
                            !interactiveDesktop))
                    self->beginMigration();
                else if (!self->rebuildGraphicsIfDeviceLost())
                    check(self->graphics.d3d->GetDeviceRemovedReason(), "DeviceRemoved");
                return 0;
            }
        } catch (const Failure& e) {
            self->failed = true;
            self->failureCode = e.code;
            self->failureStage = e.stage;
            self->finish("callback_api_failure");
            return 0;
        } catch (...) {
            self->failed = true;
            self->failureCode = E_UNEXPECTED;
            self->finish("callback_exception");
            return 0;
        }
        return DefWindowProcW(w, m, wp, lp);
    }
    void create() {
        INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES};
        InitCommonControlsEx(&icc);
#ifdef DPP_DIRECT_INPUT_TESTS
        if (!fixtureInput)
#endif
        {
            userPrefs.load();
            soundMuted = userPrefs.soundMuted;
            showFilenames = userPrefs.showFilenames;
        }
        WNDCLASSW wc{};
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = panelProcedure;
        wc.lpszClassName = panelClass;
        wc.hIcon = static_cast<HICON>(
            LoadImageW(wc.hInstance, MAKEINTRESOURCEW(101), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED));
        if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wincheck(RegisterClassW(&wc), "RegisterPanel");
        wc.hIcon = nullptr;
        wc.lpfnWndProc = overlayProcedure;
        wc.lpszClassName = overlayClass;
        wc.hbrBackground = nullptr;
        wincheck(RegisterClassW(&wc), "RegisterOverlay");
        require(contract.work.right - contract.work.left >= (protectedMode ? 320 : 760) &&
                    contract.work.bottom - contract.work.top >= (protectedMode ? 240 : 750),
                "work_area_too_small_for_panel");
        const int workW = contract.work.right - contract.work.left;
        const int panelWidth = workW >= 1116 ? 1116 : std::max<int>(protectedMode ? 320 : 760, workW);
        const int panelHeight =
            std::min<int>(772, std::max<int>(240, contract.work.bottom - contract.work.top));
        panel = CreateWindowExW(
            0, panelClass,
            protectedMode ? L"桌面游乐场 · DeskFling" : L"桌面镜像开发预览——保留原图标",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, contract.work.left, contract.work.top,
            panelWidth, panelHeight, nullptr, nullptr, wc.hInstance, this);
        require(panel != nullptr, "CreatePanel");
        billiardsLan.setNotifyWindow(reinterpret_cast<std::uintptr_t>(panel));
        if (!panelUiFont) {
            const UINT initDpi = contract.dpi ? contract.dpi : 96u;
            const int fontPx = -static_cast<int>(std::lround(13.0f * (static_cast<float>(initDpi) / 96.0f)));
            panelUiFont = CreateFontW(fontPx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                      DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");
        }
        auto control = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int width,
                           int height, int id = 0) {
            HWND result =
                CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, width, height, panel,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), wc.hInstance, nullptr);
            require(result != nullptr, "CreateControl");
            if (panelUiFont) {
                SendMessageW(result, WM_SETFONT, reinterpret_cast<WPARAM>(panelUiFont), FALSE);
            }
            return result;
        };
        control(
            L"STATIC",
            protectedMode
                ? L"点击下方按钮进入桌面游乐场，通过顶部胶囊悬浮条切换【重力 / 黑洞 / 台球 / 弹弓 / "
                  L"曲棍球】，"
                  L"点击“设置”仅展开当前玩法专属选项。\r\n"
                  L"独立守护进程实时监控心跳（卡死/崩溃自动恢复）；按 Esc 或点【还原】立即退出。"
                : L"开发预览，原图标保留，可能出现重复显示。\r\n绿色十字 = Shell "
                  L"项目左上锚点，不是图像中心。\r\n副本采用系统图像列表实际像素，等比例绘制；"
                  L"精确图像边界尚未验证。\r\n90 秒自动停止绘制。默认不接管鼠标。进入交互后只拖动带编号的 "
                  L"副本，不动真实图标。",
            0, 16, 12, 710, 92, 200);
        for (std::size_t i = 0; i < std::min(report.samples.size(), size_t{5}); ++i) {
            const auto label = L"S" + std::to_wstring(i + 1) + L": " + report.samples[i].localName;
            control(L"STATIC", label.c_str(), SS_ENDELLIPSIS, 16 + static_cast<int>(i % 2) * 350,
                    112 + static_cast<int>(i / 2) * 25, 340, 24, 201 + static_cast<int>(i));
        }
        const wchar_t* labels[] = {L"编号与项目位置大致对应", L"背景透明（无黑色/实色块）",
                                   L"桌面与任务栏点击不受阻", L"切换普通应用后，标记不盖住它",
                                   L"交互：须先通过副本和播放"};
        for (std::size_t i = 0; i < answers.size(); ++i) {
            const int y = 196 + static_cast<int>(i) * 37;
            control(L"STATIC", labels[i], 0, 16, y + 4, 410, 26, 100 + static_cast<int>(i));
            answers[i] = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 440, y, 245, 120);
            for (auto text : {L"未核对", L"符合 / 正常", L"不符合 / 有问题"})
                SendMessageW(answers[i], CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
            SendMessageW(answers[i], CB_SETCURSEL, 0, 0);
        }
        control(L"BUTTON", L"当前标记正常，显示副本", WS_TABSTOP, 16, protectedMode ? 198 : 395,
                protectedMode ? 684 : 290, protectedMode ? 44 : 36, 20);
        control(L"BUTTON", L"暂停", WS_TABSTOP, protectedMode ? 16 : 325, protectedMode ? 254 : 395,
                protectedMode ? 190 : 150, 36, 21);
        control(L"BUTTON", L"重置", WS_TABSTOP, protectedMode ? 220 : 495, protectedMode ? 254 : 395,
                protectedMode ? 190 : 150, 36, 22);
        control(L"BUTTON", L"进入交互（先确认播放正常）", WS_TABSTOP, 16, protectedMode ? 302 : 440,
                protectedMode ? 430 : 400, 36, 23);
        control(L"BUTTON", L"退出交互", WS_TABSTOP, protectedMode ? 460 : 440, protectedMode ? 302 : 440,
                protectedMode ? 240 : 205, 36, 24);
        control(L"BUTTON", L"对照：仅看原图标", WS_TABSTOP, 460, 485, 230, 40, 25);
        control(L"BUTTON", L"上移 1 像素", WS_TABSTOP, 16, 600, 150, 32, 26);
        control(L"BUTTON", L"下移 1 像素", WS_TABSTOP, 180, 600, 150, 32, 27);
        control(L"STATIC", L"", 0, 345, 605, 350, 26, 28);
        control(L"BUTTON", L"显示文件名", BS_AUTOCHECKBOX | WS_TABSTOP, 16, 636, 118, 26, 29);
        SendMessageW(GetDlgItem(panel, 29), BM_SETCHECK, showFilenames ? BST_CHECKED : BST_UNCHECKED, 0);
        control(L"BUTTON", L"玩具黑洞", BS_AUTOCHECKBOX | WS_TABSTOP, 140, 636, 102, 26, 30);
        control(L"BUTTON", L"吞噬全部图标副本", BS_AUTOCHECKBOX | WS_TABSTOP, 248, 636, 170, 26, 31);
        control(L"STATIC", L"黑洞大小", 0, 426, 640, 64, 22, 206);
        HWND sizeBar =
            control(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 490, 636, 142, 26, 32);
        SendMessageW(sizeBar, TBM_SETRANGE, TRUE, MAKELPARAM(50, 200));
        SendMessageW(sizeBar, TBM_SETPOS, TRUE, 100);
        control(L"BUTTON", L"默认角度", WS_TABSTOP, 638, 635, 92, 28, 35);

        control(L"STATIC", L"俯仰角", 0, 16, 668, 50, 22, 207);
        HWND elBar = control(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 66, 664, 152, 26, 33);
        SendMessageW(elBar, TBM_SETRANGE, TRUE, MAKELPARAM(0, 170));
        control(L"STATIC", L"+3\u00B0", 0, 220, 668, 44, 22, 36);

        control(L"STATIC", L"倾斜角", 0, 270, 668, 50, 22, 208);
        HWND tiltBar =
            control(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 320, 664, 152, 26, 34);
        SendMessageW(tiltBar, TBM_SETRANGE, TRUE, MAKELPARAM(0, 360));
        control(L"STATIC", L"0\u00B0", 0, 474, 668, 46, 22, 37);
        syncAngleControls();

        control(L"STATIC", L"拖黑洞中心移动，Alt+拖动自由调 3D 视角。", 0, 16, 694, 714, 24, 209);

        // Right-hand column: unified 5-mode PlayMode selector + Billiards / Slingshot / AirHockey / LAN
        // settings.
        control(L"BUTTON", L"玩法模式 · 详细参数 · 局域网联机", BS_GROUPBOX, 746, 8, 352, 720, 210);
        control(L"STATIC", L"玩法选项", 0, 758, 32, 68, 20, 211);
        HWND modeCombo = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 828, 28, 166, 160, 40);
        for (auto txt : {L"普通重力", L"玩具黑洞", L"台球模式", L"弹弓攻城", L"空气曲棍球"})
            SendMessageW(modeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(txt));
        SendMessageW(modeCombo, CB_SETCURSEL, 0, 0);
        HWND cbMute = control(L"BUTTON", L"静音音效", BS_AUTOCHECKBOX | WS_TABSTOP, 1002, 30, 88, 22, 50);
        SendMessageW(cbMute, BM_SETCHECK, soundMuted ? BST_CHECKED : BST_UNCHECKED, 0);

        control(L"STATIC", L"台球规则", 0, 758, 60, 68, 20, 212);
        HWND ruleCombo = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 828, 56, 256, 140, 41);
        for (auto txt : {L"自由清台", L"国际黑八(15球)", L"国际斯诺克(21球)"})
            SendMessageW(ruleCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(txt));
        SendMessageW(ruleCombo, CB_SETCURSEL, 0, 0);

        control(L"STATIC", L"台球对局", 0, 758, 88, 68, 20, 213);
        HWND matchCombo = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 828, 84, 150, 140, 42);
        for (auto txt : {L"单人挑战", L"人机对战", L"双人同屏轮流", L"局域网对战"})
            SendMessageW(matchCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(txt));
        SendMessageW(matchCombo, CB_SETCURSEL, 0, 0);
        HWND aiDiffCombo = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 982, 84, 102, 120, 60);
        for (auto txt : {L"简单 AI", L"标准 AI", L"大师 AI", L"球王 AI"})
            SendMessageW(aiDiffCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(txt));
        SendMessageW(aiDiffCombo, CB_SETCURSEL, 1, 0);

        HWND cbBorders =
            control(L"BUTTON", L"显示球边框/彩环", BS_AUTOCHECKBOX | WS_TABSTOP, 758, 114, 158, 22, 43);
        SendMessageW(cbBorders, BM_SETCHECK, BST_CHECKED, 0);
        HWND cbNumbers =
            control(L"BUTTON", L"球心显示球号", BS_AUTOCHECKBOX | WS_TABSTOP, 924, 114, 158, 22, 44);
        SendMessageW(cbNumbers, BM_SETCHECK, BST_UNCHECKED, 0);
        control(L"STATIC", L"球桌外观", 0, 758, 140, 68, 20, 221);
        HWND tableThemeCombo =
            control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 828, 136, 256, 140, 45);
        for (auto txt : {L"拟真球桌与球管", L"极简线条球桌", L"完全无边框"})
            SendMessageW(tableThemeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(txt));
        SendMessageW(tableThemeCombo, CB_SETCURSEL, 0, 0);
        HWND cbAim =
            control(L"BUTTON", L"瞄准折射辅助线", BS_AUTOCHECKBOX | WS_TABSTOP, 758, 162, 148, 22, 46);
        SendMessageW(cbAim, BM_SETCHECK, BST_CHECKED, 0);
        HWND cbAiIntent =
            control(L"BUTTON", L"显示人机战术意图提示", BS_AUTOCHECKBOX | WS_TABSTOP, 912, 162, 176, 22, 68);
        SendMessageW(cbAiIntent, BM_SETCHECK, BST_CHECKED, 0);

        control(L"STATIC", L"弹弓堡垒", 0, 758, 168, 68, 20, 214);
        HWND slingPresetCombo =
            control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 828, 164, 150, 160, 61);
        for (auto txt :
             {L"经典双塔", L"金字塔要塞", L"冰晶悬廊", L"巨石碉堡", L"随机无尽攻城", L"自建关卡工坊"})
            SendMessageW(slingPresetCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(txt));
        SendMessageW(slingPresetCombo, CB_SETCURSEL, 0, 0);
        HWND slingAmmoCombo =
            control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 982, 164, 102, 120, 62);
        for (auto txt : {L"3 发/关", L"4 发/关", L"5 发/关", L"6 发/关"})
            SendMessageW(slingAmmoCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(txt));
        SendMessageW(slingAmmoCombo, CB_SETCURSEL, 1, 0);
        HWND cbTraj =
            control(L"BUTTON", L"弹弓抛物线预测", BS_AUTOCHECKBOX | WS_TABSTOP, 758, 192, 164, 22, 63);
        SendMessageW(cbTraj, BM_SETCHECK, BST_CHECKED, 0);
        control(L"BUTTON", L"关卡工坊", WS_TABSTOP, 938, 190, 146, 24, 69);

        control(L"STATIC", L"曲棍球局", 0, 758, 220, 68, 20, 215);
        HWND hockeyMatchCombo =
            control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 828, 216, 150, 120, 64);
        for (auto txt : {L"单挑 AI", L"双人同屏", L"局域网对战"})
            SendMessageW(hockeyMatchCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(txt));
        SendMessageW(hockeyMatchCombo, CB_SETCURSEL, 0, 0);
        HWND hockeyDiffCombo =
            control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 982, 216, 102, 120, 65);
        for (auto txt : {L"休闲 AI", L"标准 AI", L"大师 AI"})
            SendMessageW(hockeyDiffCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(txt));
        SendMessageW(hockeyDiffCombo, CB_SETCURSEL, 1, 0);

        control(L"STATIC", L"获胜球数", 0, 758, 248, 68, 20, 216);
        HWND hockeyGoalsCombo =
            control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 828, 244, 102, 120, 66);
        for (auto txt : {L"5 球胜", L"7 球胜", L"11 球胜"})
            SendMessageW(hockeyGoalsCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(txt));
        SendMessageW(hockeyGoalsCombo, CB_SETCURSEL, 1, 0);
        HWND cbRink =
            control(L"BUTTON", L"霓虹球台外框", BS_AUTOCHECKBOX | WS_TABSTOP, 938, 246, 146, 22, 67);
        SendMessageW(cbRink, BM_SETCHECK, BST_CHECKED, 0);

        control(L"STATIC", L"出场图标", 0, 758, 278, 68, 20, 217);
        HWND iconSelCombo = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 828, 274, 256, 140, 47);
        for (auto txt : {L"取前 N 个", L"随机抽取", L"自选图标…"})
            SendMessageW(iconSelCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(txt));
        SendMessageW(iconSelCombo, CB_SETCURSEL, 0, 0);

        control(L"STATIC", L"其余图标", 0, 758, 306, 68, 20, 218);
        HWND extraCombo = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 828, 302, 256, 140, 49);
        for (auto txt : {L"玩法期间隐藏", L"底部重力堆放 / 场边观众"})
            SendMessageW(extraCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(txt));
        SendMessageW(extraCombo, CB_SETCURSEL, 0, 0);

        control(L"STATIC", L"自选图标（多选后点应用）：", 0, 758, 332, 320, 18, 219);
        HWND iconList = control(L"LISTBOX", L"",
                                LBS_MULTIPLESEL | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_BORDER | WS_TABSTOP,
                                758, 352, 326, 154, 56);
        for (std::size_t i = 0; i < report.samples.size(); ++i) {
            const auto itemLabel = std::to_wstring(i + 1) + L". " + report.samples[i].localName;
            SendMessageW(iconList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(itemLabel.c_str()));
            if (i < 15u) {
                SendMessageW(iconList, LB_SETSEL, TRUE, static_cast<LPARAM>(i));
                customIconIndices.push_back(i);
            }
        }
        control(L"BUTTON", L"全选", WS_TABSTOP, 758, 512, 88, 26, 57);
        control(L"BUTTON", L"清空", WS_TABSTOP, 854, 512, 88, 26, 58);
        control(L"BUTTON", L"应用自选/换阵", WS_TABSTOP, 950, 512, 134, 26, 59);

        control(L"STATIC", L"局域网联机（默认端口 39457 / 自动搜房）：", 0, 758, 546, 326, 18, 220);
        const std::wstring defaultLanAddr =
            utf8ToWide(BilliardsLan::primaryLanIPv4()) + L":" + std::to_wstring(BilliardsLan::kDefaultPort);
        control(L"EDIT", defaultLanAddr.c_str(), WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 758, 568, 326, 26,
                51);
        control(L"BUTTON", L"建房(P1)", WS_TABSTOP, 758, 600, 76, 28, 52);
        control(L"BUTTON", L"搜房加入", WS_TABSTOP, 840, 600, 84, 28, 70);
        control(L"BUTTON", L"加入(P2)", WS_TABSTOP, 930, 600, 76, 28, 53);
        control(L"BUTTON", L"断开", WS_TABSTOP, 1012, 600, 72, 28, 54);
        control(L"STATIC", L"局域网未开启", 0, 758, 634, 326, 86, 55);
        control(L"BUTTON", L"保存当前位置到桌面…", WS_TABSTOP, 16, 528, 220, 30, 71);
        HWND bUndoLayout = control(L"BUTTON", L"撤销写回", WS_TABSTOP, 246, 528, 120, 30, 72);
        EnableWindow(bUndoLayout, FALSE);

        control(L"STATIC", L"天体环境", 0, -600, 0, 66, 22, 226);
        HWND gravPresetCombo =
            control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, -600, 0, 222, 180, 73);
        for (auto txt :
             {L"太空失重 (0.00 m/s\u00B2)", L"冥王星 (0.62 m/s\u00B2 \u00B7 0.06g)",
              L"月球表面 (1.62 m/s\u00B2 \u00B7 0.17g)", L"火星表面 (3.72 m/s\u00B2 \u00B7 0.38g)",
              L"地球标准 (9.80 m/s\u00B2 \u00B7 1.00g)", L"木星强重 (24.79 m/s\u00B2 \u00B7 2.53g)",
              L"超重离心 (39.20 m/s\u00B2 \u00B7 4.00g)", L"自定义加速度"})
            SendMessageW(gravPresetCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(txt));
        SendMessageW(gravPresetCombo, CB_SETCURSEL, 4, 0);

        HWND gravDirCombo = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, -600, 0, 90, 140, 74);
        for (auto txt : {L"向下 \u2193", L"向上 \u2191", L"向左 \u2190", L"向右 \u2192"})
            SendMessageW(gravDirCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(txt));
        SendMessageW(gravDirCombo, CB_SETCURSEL, 0, 0);

        control(L"STATIC", L"重力 g", 0, -600, 0, 66, 22, 222);
        HWND gravBar =
            control(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, -600, 0, 196, 26, 75);
        SendMessageW(gravBar, TBM_SETRANGE, TRUE, MAKELPARAM(0, 4000));
        SendMessageW(gravBar, TBM_SETPOS, TRUE, 980);
        control(L"STATIC", L"9.80 m/s\u00B2 (1.00g)", 0, -600, 0, 120, 22, 223);

        control(L"STATIC", L"碰撞弹性", 0, -600, 0, 66, 22, 224);
        HWND restBar =
            control(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, -600, 0, 236, 26, 76);
        SendMessageW(restBar, TBM_SETRANGE, TRUE, MAKELPARAM(0, 95));
        SendMessageW(restBar, TBM_SETPOS, TRUE, 15);
        control(L"STATIC", L"e = 0.15", 0, -600, 0, 76, 22, 225);
        control(L"BUTTON", L"标准地球重力", WS_TABSTOP, -600, 0, 106, 28, 77);
        monitorsList = enumerateMonitorsSorted();
        hudWorkRect = !monitorsList.empty() ? monitorsList[0].work : contract.work;
        HWND dispScopeCombo =
            control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, -600, 0, 320, 180, 78);
        ShowWindow(dispScopeCombo, SW_HIDE);
        if (monitorsList.size() <= 1) {
            const int mw = contract.bounds.right - contract.bounds.left;
            const int mh = contract.bounds.bottom - contract.bounds.top;
            const std::wstring singleTitle =
                L"\xD83D\xDDA5  主显示器（" + std::to_wstring(mw) + L" \u00D7 " + std::to_wstring(mh) + L"）";
            SendMessageW(dispScopeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(singleTitle.c_str()));
            SendMessageW(dispScopeCombo, CB_SETCURSEL, 0, 0);
        } else {
            for (std::size_t mi = 0; mi < monitorsList.size(); ++mi) {
                const auto& m = monitorsList[mi];
                const int mw = m.bounds.right - m.bounds.left;
                const int mh = m.bounds.bottom - m.bounds.top;
                const std::wstring itemTitle =
                    L"\xD83D\xDDA5  仅屏幕 " + std::to_wstring(mi + 1) +
                    (m.primary ? L"（主屏 \u00B7 " : L"（副屏 \u00B7 ") + std::to_wstring(mw) + L" \u00D7 " +
                    std::to_wstring(mh) + L"）";
                SendMessageW(dispScopeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(itemTitle.c_str()));
            }
            const std::wstring spanTitle = L"\xD83C\xDF10  多屏跨屏显示（共 " +
                                           std::to_wstring(monitorsList.size()) + L" 块屏幕铺满）";
            SendMessageW(dispScopeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(spanTitle.c_str()));
            SendMessageW(dispScopeCombo, CB_SETCURSEL, 0, 0);
        }
        if (!protectedMode) {
            for (int hid : {71, 72, 73, 74, 75, 76, 77, 78, 222, 223, 224, 225, 226}) {
                if (HWND h = GetDlgItem(panel, hid)) ShowWindow(h, SW_HIDE);
            }
        }

        updateOffsetLabel();
        for (int id : {20, 21, 22, 23, 25, 26, 27}) EnableWindow(GetDlgItem(panel, id), FALSE);
        control(L"BUTTON", L"保存结果并停止", WS_TABSTOP, 16, 485, 220, 40, 10);
        control(L"BUTTON", protectedMode ? L"立即还原桌面并退出" : L"立即停止", WS_TABSTOP,
                protectedMode ? 424 : 260, protectedMode ? 254 : 485, protectedMode ? 276 : 180,
                protectedMode ? 36 : 40, 11);
        phaseLabel = control(L"STATIC", L"检查当前配置与已验收标记配置是否相符…", 0, 16,
                             protectedMode ? 354 : 540, 710, 64);
#ifdef DPP_DIRECT_INPUT_TESTS
        if (!fixtureInput)
#endif
        {
            applyLoadedPrefsToControls();
            if (loadWinLayoutBackupFile(layoutBackupFilePath(), persistedWinLayoutBackup)) {
                hudBar.setHasLayoutBackup(true);
                if (bUndoLayout) EnableWindow(bUndoLayout, TRUE);
            }
        }
        if (protectedMode) {
            layoutProtectedPanel(false);
            hudBar.installTray(panel);
            syncHudBar();
        }
        const RECT w = contract.work;
        overlay = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED | WS_EX_TRANSPARENT |
                                      WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                  overlayClass, L"DPP item anchors", WS_POPUP, w.left, w.top,
                                  w.right - w.left, w.bottom - w.top, nullptr, nullptr, wc.hInstance, this);
        require(overlay != nullptr, "CreateOverlay");
        wincheck(SetLayeredWindowAttributes(overlay, 0, 255, LWA_ALPHA), "LayeredAttributes");
        previewDpi = GetDpiForWindow(overlay);
        require(previewDpi > 0 && pmv2(overlay), "preview_dpi_unknown");
        const PixelRect work{w.left, w.top, w.right, w.bottom};
        HRGN region = CreateRectRgn(0, 0, 0, 0);
        require(region != nullptr, "CreateRegion");
        try {
            for (const auto& sample : report.samples) {
                auto raw = *sample.position;
                POINT mapped = mapPoint(contract.list, nullptr, {raw.x, raw.y});
                POINT local = mapPoint(nullptr, overlay, mapped);
                screen.push_back(mapped);
                client.push_back(local);
                scene.push_back(
                    windowsScreenPixelsToScene(mapped.x, mapped.y, work, windowsDpiScale(previewDpi)));
                if (!protectedMode) {
                    HRGN part = CreateRectRgn(local.x - 8, local.y - 8, local.x + 65, local.y + 34);
                    require(part != nullptr, "CreateMarkerRegion");
                    const int combined = CombineRgn(region, region, part, RGN_OR);
                    DeleteObject(part);
                    require(combined != ERROR, "CombineRegion");
                }
            }
            // Even if a driver paints an opaque backdrop, it cannot cover the full screen.
            wincheck(SetWindowRgn(overlay, region, FALSE), "SetOwnWindowRegion");
            region = nullptr;  // window owns it on success
        } catch (...) {
            if (region) DeleteObject(region);
            throw;
        }
        graphics.initialize(overlay, static_cast<UINT>(w.right - w.left),
                            static_cast<UINT>(w.bottom - w.top));
        const bool baseline = matchesValidatedMarkerConfiguration(report, contract, previewDpi);
        manualOffsetYPixels = startupCalibrationPixels(report, contract, previewDpi);
        updateOffsetLabel();
        const bool archSupported =
            (report.nativeArchitecture == "x64" || report.nativeArchitecture == "arm64") &&
            (report.programArchitecture == "x64" || report.programArchitecture == "arm64");
        const bool candidate = startupExperiment && report.osMajor && *report.osMajor >= 10UL &&
                               report.osBuild && *report.osBuild >= 19041UL && archSupported &&
                               !report.monitors.empty() && previewDpi > 0 &&
                               (protectedMode || (report.threadDpi == "per_monitor_v2" &&
                                                  report.viewDpiContext == "per_monitor_v2" &&
                                                  contractStillValid(contract)));
        copiesEligible = contract.sizedImagesReady && (baseline || candidate);
        if (!protectedMode) {
            graphics.render(client);
        }
        if (protectedMode)
            require(copiesEligible && report.fullSnapshot && report.itemCount &&
                        static_cast<size_t>(*report.itemCount) == report.samples.size(),
                    "full_mirror_resources_unavailable");
        if (copiesEligible) {
            EnableWindow(GetDlgItem(panel, 20), TRUE);
            SetWindowTextW(phaseLabel,
                           L"先看当前标记是否正常；点击显示副本即确认本次标记条件。尚未启动副本或重力。");
        } else {
            SetWindowTextW(phaseLabel,
                           L"配置、Shell 图标尺寸或图像读取未满足条件：仅标记，副本/重力未开放。");
        }
        if (protectedMode) {
            SetWindowTextW(phaseLabel, L"");
            SetWindowTextW(GetDlgItem(panel, 20), L"▶ 开始游玩");
        }
        if (!protectedMode) placeAboveShell(true);
        displayed = true;
        started = GetTickCount64();
        require(SetTimer(panel, 2, 500, nullptr) != 0, "StartSafetyTimer");
        if (protectedMode) {
            ShowWindow(panel, SW_HIDE);
            settingsCard.showLauncher(panel, phaseLabel, effectiveHudWorkRect(), previewDpi,
                                      hasAnyLayoutBackup());
            syncHudBar();
        } else {
            ShowWindow(panel, SW_SHOWNORMAL);
        }
        stopReason = "running";
    }
    void run(const std::wstring& checkpointPath) {
        create();
        std::ostringstream checkpoint;
        checkpoint << "{\"snapshot_id\":\"" << report.snapshot << "\",\"preview_dpi\":" << previewDpi
                   << ",\"source_dpi\":" << contract.dpi
                   << ",\"monitor_bounds_pixels\":" << rectJson(contract.bounds)
                   << ",\"work_area_pixels\":" << rectJson(contract.work)
                   << ",\"monitor_identity\":" << pointerJson(contract.monitor)
                   << ",\"startup_display_experiment\":" << (startupExperiment ? "true" : "false")
                   << ",\"scene_units\":\"DIP_top_left_y_down\""
                   << ",\"view_icon_setting\":"
                   << (contract.iconPixels ? std::to_string(contract.iconPixels) : "null")
                   << ",\"view_mode\":" << static_cast<int>(contract.viewMode)
                   << ",\"system_image_list_pixels\":" << sizeJson(contract.systemImagePixels)
                   << ",\"system_image_hresult\":" << static_cast<long>(contract.systemImageHr)
                   << ",\"image_stage\":\"" << contract.sizedImageStage
                   << "\",\"image_hresult\":" << static_cast<long>(contract.sizedImagesHr)
                   << ",\"images_ready\":" << (contract.sizedImagesReady ? "true" : "false")
                   << ",\"manual_uniform_offset_y_pixels\":" << manualOffsetYPixels
                   << ",\"offset_source\":\"baseline_only_other_profiles_zero\""
                   << ",\"shortcut_overlay_policy\":\"omit_for_SFGAO_LINK_copies_only\""
                   << ",\"shortcut_overlays_omitted\":" << contract.shortcutOverlaysOmitted
                   << ",\"human_confirmed\":false,\"screen_alignment_verified\":false}\n";
        require(winprobe::writeNewReport(checkpointPath, checkpoint.str()) == 0,
                "write_initialization_checkpoint");
        MSG message{};
        BOOL result;
        while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
            if (!IsDialogMessageW(panel, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        wincheck(result >= 0, "GetMessage");
    }
};
}  // namespace

bool migrationListBoundsReady(DisplayMigration& migration, const ViewContract& sample, double now) {
    if (sameRect(sample.listScreen, sample.bounds)) return true;
    // Monitor and Explorer can publish different-sized rectangles during a display change.
    // Nothing is committed or shown until they agree. changed() invalidates any candidate
    // and restarts only the 150ms settle interval, never the original three-second limit.
    migration.changed(now);
    return false;
}

bool matchesValidatedMarkerConfiguration(const winprobe::Report& r, const ViewContract& c, UINT dpi) {
    // Narrow evidence scope: 2026-11-12 marker run + explicit human follow-up.
    // This matches a display/OS profile, not a device identity. Even on a match,
    // the current run starts with markers and requires explicit marker confirmation
    // before showing copies; no new device inherits a human result automatically.
    return r.osMajor == 10UL && r.osMinor == 0UL && r.osBuild == 19045UL && r.osRevision == 6466UL &&
           r.nativeArchitecture == "x64" && r.programArchitecture == "x64" && r.monitors.size() == 1 &&
           r.monitors[0].bounds && sameRect(*r.monitors[0].bounds, {0, 0, 1920, 1080}) &&
           sameRect(c.work, {0, 0, 1920, 1030}) && sameRect(c.listScreen, {0, 0, 1920, 1080}) &&
           c.dpi == 120 && dpi == 120 && r.threadDpi == "per_monitor_v2" &&
           r.viewDpiContext == "per_monitor_v2";
}
int startupCalibrationPixels(const winprobe::Report& r, const ViewContract& c, UINT dpi) {
    return matchesValidatedMarkerConfiguration(r, c, dpi) ? 5 : 0;
}
SceneSetup makePlaybackScene(const winprobe::Report& r, const std::vector<Vec2>& anchors, Vec2 workArea,
                             int iconPixels, unsigned dpi, int manualOffsetYPixels) {
    require(dpi > 0, "unknown_preview_dpi");
    require(snapshotUsable(r) && anchors.size() == r.samples.size(), "invalid_snapshot_scene_association");
    require(workArea.isFinite() && workArea.x >= 100 && workArea.y >= 100, "invalid_scene_size");
    SceneSetup setup;
    setup.workAreaSize = workArea;
    for (std::size_t i = 0; i < anchors.size(); ++i) {
        require(anchors[i].isFinite(), "nonfinite_scene_anchor");
        BITMAP bitmap{};
        require(GetObjectW(r.samples[i].bitmap.get(), sizeof(bitmap), &bitmap) ==
                    static_cast<int>(sizeof(bitmap)),
                "cached_bitmap_dimensions_unknown");
        const Rect custom = windowsIconImageRect(
            anchors[i], {static_cast<float>(bitmap.bmWidth), static_cast<float>(bitmap.bmHeight)}, iconPixels,
            windowsDpiScale(dpi), manualOffsetYPixels);
        if (r.fullSnapshot)
            require(custom.x >= 0 && custom.y >= 0 && custom.x + custom.width <= workArea.x &&
                        custom.y + custom.height <= workArea.y,
                    "full_mirror_item_outside_replacement_area");
        const auto index = static_cast<std::uint32_t>(i + 1);
        setup.icons.push_back(
            {IconId{index}, TextureId{index}, rectCenter(custom), {custom.width, custom.height}});
    }
    return setup;
}
PreviewPlayback::PreviewPlayback(SceneSetup setup) : setup_(std::move(setup)) {
    static_.reserve(setup_.icons.size());
    for (const auto& icon : setup_.icons)
        static_.add(SpriteItem{icon.texture, icon.center, icon.iconSize, 0, 1, false});
}
void PreviewPlayback::startGravity(bool confirmed) {
    if (!confirmed || game_) throw std::logic_error("static_confirmation_required_or_already_started");
    auto game = std::make_unique<Playground>(setup_);
    game->tick(0);
    game_ = std::move(game);
}
bool PreviewPlayback::tick(double seconds) { return game_ && game_->tick(seconds); }
void PreviewPlayback::enterInteraction(bool confirmed) {
    if (!confirmed || !game_ || game_->paused()) throw std::logic_error("playback_confirmation_required");
    interacting_ = true;
}
void PreviewPlayback::leaveInteraction() noexcept {
    interacting_ = false;
    try {
        if (game_) game_->input({InputType::PointerCancel, {}, 0});
    } catch (...) {
        game_.reset();
    }
}
void PreviewPlayback::pointer(const InputEvent& event) {
    if (interacting_ && game_) game_->input(event);
}
void PreviewPlayback::togglePause() {
    leaveInteraction();
    if (!game_) throw std::logic_error("gravity_not_started");
    game_->input({InputType::TogglePause, {}, 0});
    game_->tick(0);
}
void PreviewPlayback::reset() {
    leaveInteraction();
    if (!game_) throw std::logic_error("gravity_not_started");
    game_->input({InputType::PointerCancel, {}, 0});
    game_->reset();
    game_->tick(0);
}
void PreviewPlayback::stop() noexcept {
    leaveInteraction();
    game_.reset();
}
const DrawList& PreviewPlayback::drawList() const { return game_ ? game_->drawList() : static_; }

winprobe::Bitmap bitmapFromIcon(HICON icon) {
    require(icon != nullptr, "null_system_icon");
    ComPtr<IWICImagingFactory> factory;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)),
          "WICSystemIconFactory");
    ComPtr<IWICBitmap> source;
    check(factory->CreateBitmapFromHICON(icon, &source), "WICFromHICON");
    UINT width = 0, height = 0;
    check(source->GetSize(&width, &height), "WICIconSize");
    require(width > 0 && width <= 1024 && height > 0 && height <= 1024, "invalid_system_icon_size");
    ComPtr<IWICFormatConverter> converter;
    check(factory->CreateFormatConverter(&converter), "WICIconConverter");
    check(converter->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr,
                                0, WICBitmapPaletteTypeCustom),
          "WICIconPBGRA");
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(width);
    info.bmiHeader.biHeight = -static_cast<LONG>(height);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    winprobe::Bitmap bitmap(CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0));
    require(bitmap && pixels, "CreateOwnedIconDIB");
    check(converter->CopyPixels(nullptr, width * 4, width * height * 4, static_cast<BYTE*>(pixels)),
          "WICIconCopyPixels");
    return bitmap;
}

bool snapshotUsable(const winprobe::Report& r) {
    if (r.snapshot.empty() || r.displayChanged || !winprobe::interactive(r.environment) ||
        (r.fullSnapshot ? (r.samples.size() > 256 || !r.itemCount ||
                           static_cast<size_t>(*r.itemCount) != r.samples.size())
                        : (r.samples.size() < 3 || r.samples.size() > 5)))
        return false;
    for (std::size_t i = 0; i < r.samples.size(); ++i) {
        const auto& s = r.samples[i];
        if (s.id != r.snapshot + "/S" + std::to_string(i + 1) || !s.child || !s.bitmap || !s.position ||
            !s.itemHr || FAILED(*s.itemHr) || !s.imageHr || FAILED(*s.imageHr) || !s.positionHr ||
            FAILED(*s.positionHr))
            return false;
        for (std::size_t j = 0; j < i; ++j)
            if (ILIsEqual(s.child.get(), r.samples[j].child.get())) return false;
    }
    return true;
}
POINT mapPoint(HWND from, HWND to, POINT point) {
    if ((from && !IsWindow(from)) || (to && !IsWindow(to)))
        throw Failure("MapWindowPoints.invalid_window", E_INVALIDARG);
    SetLastError(ERROR_SUCCESS);
    const int value = MapWindowPoints(from, to, &point, 1);
    const DWORD error = GetLastError();
    if (!value && error) throw Failure("MapWindowPoints", HRESULT_FROM_WIN32(error));
    return point;
}
ViewContract inspectContract(winprobe::DesktopView& desktop, const winprobe::Report& report) {
    require(snapshotUsable(report), "invalid_or_changed_snapshot");
    const int sysMonCount = GetSystemMetrics(SM_CMONITORS);
    if (report.fullSnapshot) {
        require(!report.monitors.empty() && sysMonCount >= 1, "no_monitors_detected");
    } else {
        require(report.monitors.size() == 1 && sysMonCount == 1, "prototype_requires_one_monitor");
    }
    ViewContract c;
    c.expectedMonitorCount = std::max(1, sysMonCount);
    c.multiMonitorSession = (c.expectedMonitorCount > 1);
    check(desktop.shell->GetWindow(&c.shell), "IShellView.GetWindow");
    require(c.shell == report.viewWindow, "view_changed_since_snapshot");
    c.list = uniqueDirectList(c.shell);
    require(c.list != nullptr, "no_unique_direct_SysListView32_child");
    ComPtr<IFolderView2> view;
    check(desktop.view.As(&view), "IFolderView2");
    check(view->GetCurrentFolderFlags(&c.flags), "GetCurrentFolderFlags");
    c.iconSizeHr = view->GetViewModeAndIconSize(&c.viewMode, &c.iconPixels);
    if (FAILED(c.iconSizeHr) || c.iconPixels <= 0 || c.iconPixels > 1024) c.iconPixels = 0;
    require((report.fullSnapshot || (c.flags & FWF_DESKTOP) != 0) && (c.flags & FWF_NOICONS) == 0,
            "not_visible_desktop_folder");
    c.listStyle = GetWindowLongPtrW(c.list, GWL_STYLE);
    if (!report.fullSnapshot) {
        require((c.listStyle & LVS_NOSCROLL) != 0 && (c.listStyle & (WS_HSCROLL | WS_VSCROLL)) == 0,
                "listview_scroll_origin_not_proved_zero");
    }
    require((GetWindowLongPtrW(c.list, GWL_EXSTYLE) & WS_EX_LAYOUTRTL) == 0, "rtl_layout_not_supported");
    DWORD shellProcess = 0;
    GetWindowThreadProcessId(c.shell, &shellProcess);
    GetWindowThreadProcessId(c.list, &c.process);
    require(c.process && c.process == shellProcess, "listview_process_mismatch");
    c.dpi = GetDpiForWindow(c.list);
    if (report.fullSnapshot) {
        if (c.dpi == 0) c.dpi = GetDpiForWindow(c.shell);
        if (c.dpi == 0) c.dpi = 96u;
    } else {
        require(c.dpi > 0 && c.dpi == GetDpiForWindow(c.shell) && pmv2(c.shell) && pmv2(c.list),
                "source_dpi_context_not_supported");
    }
    wincheck(GetClientRect(c.list, &c.listClient), "GetListClientRect");
    POINT a = mapPoint(c.list, nullptr, {c.listClient.left, c.listClient.top});
    POINT b = mapPoint(c.list, nullptr, {c.listClient.right, c.listClient.bottom});
    c.listScreen = {a.x, a.y, b.x, b.y};
    c.monitor = c.multiMonitorSession ? MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY)
                                      : MonitorFromWindow(c.list, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi), {}, {}, 0};
    wincheck(GetMonitorInfoW(c.monitor, &mi), "GetMonitorInfo");
    c.bounds = mi.rcMonitor;
    c.work = mi.rcWork;
    if (report.fullSnapshot) {
        c.work.left = std::max(c.work.left, c.bounds.left);
        c.work.top = std::max(c.work.top, c.bounds.top);
        c.work.right = std::min(c.work.right, c.bounds.right);
        c.work.bottom = std::min(c.work.bottom, c.bounds.bottom);
        if (c.work.right - c.work.left < 200 || c.work.bottom - c.work.top < 200) {
            c.work = c.bounds;
        }
    }
    (void)windowsWorkAreaSceneSize({c.work.left, c.work.top, c.work.right, c.work.bottom},
                                   windowsDpiScale(c.dpi));
    require(c.work.left >= c.bounds.left && c.work.top >= c.bounds.top && c.work.right <= c.bounds.right &&
                c.work.bottom <= c.bounds.bottom,
            "invalid_work_area");
    require(report.fullSnapshot || (c.listClient.right > 0 && c.listClient.bottom > 0), "empty_listview");
    return c;
}
// Read the standard desktop image-list cell (or SHIL_JUMBO for custom desktop icon sizes).
// All PIDLs stay in this fresh Shell snapshot; nothing is keyed by display name.
void loadViewSizedImages(winprobe::DesktopView& desktop, winprobe::Report& report, ViewContract& c) {
    c.sizedImageStage = "standard_icon_view_required";
    if (!report.fullSnapshot && (c.iconPixels != 48 || c.viewMode != FVM_ICON)) {
        c.sizedImagesHr = E_NOTIMPL;
        return;
    }
    if (report.fullSnapshot) {
        if (c.iconPixels <= 0) c.iconPixels = 48;
        c.iconPixels = std::clamp(c.iconPixels, 16, 512);
        if (report.samples.empty()) {
            c.systemImagePixels = {48, 48};
            c.sizedImagesReady = true;
            c.sizedImagesHr = S_OK;
            c.sizedImageStage = "ready_empty_desktop";
            return;
        }
    }
    const bool wantJumbo = report.fullSnapshot && (c.iconPixels > 64);
    ComPtr<IImageList> images;
    c.sizedImageStage = wantJumbo ? "SHGetImageList.SHIL_JUMBO" : "SHGetImageList.SHIL_EXTRALARGE";
    c.systemImageHr = SHGetImageList(wantJumbo ? SHIL_JUMBO : SHIL_EXTRALARGE, IID_PPV_ARGS(&images));
    if (FAILED(c.systemImageHr) && wantJumbo) {
        c.sizedImageStage = "SHGetImageList.SHIL_EXTRALARGE";
        c.systemImageHr = SHGetImageList(SHIL_EXTRALARGE, IID_PPV_ARGS(&images));
    }
    if (FAILED(c.systemImageHr)) {
        if (report.fullSnapshot) {
            c.systemImagePixels = {48, 48};
            for (const auto& sample : report.samples) {
                BITMAP dm{};
                GetObjectW(sample.bitmap.get(), sizeof(dm), &dm);
                c.bitmapPixels.push_back({dm.bmWidth > 0 ? dm.bmWidth : 64, dm.bmHeight > 0 ? dm.bmHeight : 64});
                c.shortcutItems.push_back(false);
            }
            c.sizedImagesReady = true;
            c.sizedImagesHr = S_OK;
            c.sizedImageStage = "ready_fallback_sample_bitmaps";
        }
        return;
    }
    int width = 0, height = 0;
    c.sizedImageStage = "IImageList.GetIconSize";
    c.systemImageHr = images->GetIconSize(&width, &height);
    if (FAILED(c.systemImageHr) || width <= 0 || width > 1024 || width != height) {
        if (!FAILED(c.systemImageHr)) c.systemImageHr = E_UNEXPECTED;
        return;
    }
    if (!report.fullSnapshot && c.dpi == 120 && width == 48) {
        c.sizedImageStage = "system_list_does_not_resolve_observed_48px_regression";
        c.sizedImagesHr = E_NOTIMPL;
        return;
    }
    int slotPx = width;
    if (report.fullSnapshot && c.iconPixels != 48) {
        slotPx = std::clamp(
            static_cast<int>(std::lround(static_cast<float>(c.iconPixels) * windowsDpiScale(c.dpi))), 20,
            256);
    } else if (report.fullSnapshot && c.dpi == 120 && width == 48) {
        slotPx = 60;
    }
    c.systemImagePixels = {slotPx, slotPx};
    ComPtr<IPersistFolder2> persist;
    c.sizedImageStage = "IPersistFolder2";
    c.sizedImagesHr = desktop.folder.As(&persist);
    if (FAILED(c.sizedImagesHr)) return;
    PIDLIST_ABSOLUTE parent = nullptr;
    c.sizedImageStage = "IPersistFolder2.GetCurFolder";
    c.sizedImagesHr = persist->GetCurFolder(&parent);
    struct AbsolutePidlFree {
        using pointer = PIDLIST_ABSOLUTE;
        void operator()(pointer value) const { CoTaskMemFree(value); }
    };
    std::unique_ptr<ITEMIDLIST_ABSOLUTE, AbsolutePidlFree> ownedParent(parent);
    if (FAILED(c.sizedImagesHr)) return;
    if (!parent) {
        c.sizedImagesHr = E_UNEXPECTED;
        return;
    }
    struct IconFree {
        void operator()(HICON value) const {
            if (value) DestroyIcon(value);
        }
    };
    std::vector<winprobe::Bitmap> bitmaps;
    for (std::size_t si = 0; si < report.samples.size(); ++si) {
        auto& sample = report.samples[si];
        ++c.sizedImageAttempt;
        auto pushExistingSampleFallback = [&]() -> bool {
            if (!report.fullSnapshot || !sample.bitmap) return false;
            BITMAP dm{};
            if (GetObjectW(sample.bitmap.get(), sizeof(dm), &dm) != static_cast<int>(sizeof(dm)) ||
                dm.bmWidth <= 0 || dm.bmHeight <= 0) {
                return false;
            }
            c.shortcutItems.push_back(false);
            c.bitmapPixels.push_back({dm.bmWidth, dm.bmHeight});
            bitmaps.push_back(std::move(sample.bitmap));
            return true;
        };
        c.sizedImageStage = "ILCombine.snapshot_parent_and_child";
        std::unique_ptr<ITEMIDLIST_ABSOLUTE, AbsolutePidlFree> absolute(
            ILCombine(parent, sample.child.get()));
        if (!absolute) {
            if (pushExistingSampleFallback()) continue;
            c.sizedImagesHr = E_OUTOFMEMORY;
            return;
        }
        SHFILEINFOW info{};
        c.sizedImageStage = "SHGetFileInfo.PIDL.system_icon_index";
        const auto result = SHGetFileInfoW(
            reinterpret_cast<LPCWSTR>(absolute.get()), 0, &info, sizeof(info),
            SHGFI_PIDL | SHGFI_SYSICONINDEX | SHGFI_ICON | SHGFI_OVERLAYINDEX | SHGFI_ADDOVERLAYS);
        std::unique_ptr<std::remove_pointer_t<HICON>, IconFree> unusedIcon(info.hIcon);
        if (!result) {
            if (pushExistingSampleFallback()) continue;
            c.sizedImagesHr = E_FAIL;
            return;
        }
        const auto packed = static_cast<unsigned>(info.iIcon);
        const int index = static_cast<int>(packed & 0x00ffffffu);
        const UINT overlay = (packed >> 24) & 0xffu;
        if (overlay > 15) {
            if (pushExistingSampleFallback()) continue;
            c.sizedImagesHr = E_UNEXPECTED;
            return;
        }
        SFGAOF attributes = SFGAO_LINK;
        PCUITEMID_CHILD children[] = {sample.child.get()};
        c.sizedImageStage = "IShellFolder.GetAttributesOf.SFGAO_LINK";
        const HRESULT attrHr = desktop.folder->GetAttributesOf(1, children, &attributes);
        if (FAILED(attrHr)) {
            if (pushExistingSampleFallback()) continue;
            c.sizedImagesHr = attrHr;
            return;
        }
        const bool shortcut = (attributes & SFGAO_LINK) != 0;
        const UINT drawnOverlay = shortcut ? 0 : overlay;
        HICON icon = nullptr;
        c.sizedImageStage = "IImageList.GetIcon";
        const HRESULT iconHr =
            images->GetIcon(index, ILD_TRANSPARENT | INDEXTOOVERLAYMASK(drawnOverlay), &icon);
        std::unique_ptr<std::remove_pointer_t<HICON>, IconFree> ownedIcon(icon);
        if (FAILED(iconHr) || !icon) {
            if (unusedIcon) {
                try {
                    auto bmpFromSfi = bitmapFromIcon(unusedIcon.get());
                    BITMAP dm{};
                    if (GetObjectW(bmpFromSfi.get(), sizeof(dm), &dm) == static_cast<int>(sizeof(dm)) &&
                        dm.bmWidth > 0 && dm.bmHeight > 0) {
                        c.shortcutItems.push_back(shortcut);
                        c.bitmapPixels.push_back({dm.bmWidth, dm.bmHeight});
                        bitmaps.push_back(std::move(bmpFromSfi));
                        continue;
                    }
                } catch (...) {
                }
            }
            if (pushExistingSampleFallback()) continue;
            c.sizedImagesHr = FAILED(iconHr) ? iconHr : E_FAIL;
            return;
        }
        try {
            c.sizedImageStage = "WIC.copy_owned_system_icon";
            auto bitmap = bitmapFromIcon(icon);
            BITMAP dimensions{};
            if (GetObjectW(bitmap.get(), sizeof(dimensions), &dimensions) !=
                    static_cast<int>(sizeof(dimensions)) ||
                dimensions.bmWidth <= 0 || dimensions.bmHeight <= 0) {
                if (pushExistingSampleFallback()) continue;
                c.sizedImagesHr = E_UNEXPECTED;
                return;
            }
            c.shortcutItems.push_back(shortcut);
            c.bitmapPixels.push_back({dimensions.bmWidth, dimensions.bmHeight});
            bitmaps.push_back(std::move(bitmap));
            if (shortcut && overlay) ++c.shortcutOverlaysOmitted;
        } catch (const Failure& e) {
            if (pushExistingSampleFallback()) continue;
            c.sizedImageStage = "WIC.system_icon_conversion";
            c.sizedImagesHr = e.code;
            return;
        }
    }
    for (std::size_t i = 0; i < bitmaps.size(); ++i) report.samples[i].bitmap = std::move(bitmaps[i]);
    c.sizedImagesReady = true;
    c.sizedImagesHr = S_OK;
    c.sizedImageStage = "ready_system_image_list";
}

bool contractStillValid(const ViewContract& c, bool ownsVisibility) {
    const LONG_PTR ignored = ownsVisibility ? (WS_VISIBLE | WS_HSCROLL | WS_VSCROLL) : 0;
    if (!IsWindow(c.shell) || !IsWindow(c.list) || uniqueDirectList(c.shell) != c.list ||
        (!c.multiMonitorSession &&
         (GetDpiForWindow(c.list) != c.dpi || !pmv2(c.list) || !pmv2(c.shell))) ||
        (!c.multiMonitorSession &&
         (GetWindowLongPtrW(c.list, GWL_STYLE) & ~ignored) != (c.listStyle & ~ignored)) ||
        GetSystemMetrics(SM_CMONITORS) != c.expectedMonitorCount)
        return false;
    DWORD process = 0;
    GetWindowThreadProcessId(c.list, &process);
    if (process != c.process) return false;
    if (c.multiMonitorSession) return true;
    RECT rect{};
    if (!GetClientRect(c.list, &rect) || !sameRect(rect, c.listClient)) return false;
    try {
        const auto a = mapPoint(c.list, nullptr, {rect.left, rect.top});
        const auto b = mapPoint(c.list, nullptr, {rect.right, rect.bottom});
        if (!sameRect({a.x, a.y, b.x, b.y}, c.listScreen)) return false;
    } catch (...) {
        return false;
    }
    MONITORINFO info{sizeof(info), {}, {}, 0};
    return MonitorFromWindow(c.list, MONITOR_DEFAULTTONULL) == c.monitor &&
           GetMonitorInfoW(c.monitor, &info) && sameRect(info.rcWork, c.work) &&
           sameRect(info.rcMonitor, c.bounds);
}

int runMarkers(const std::wstring& path, bool protectedMode, bool startupExperiment, std::string* outStage,
               HRESULT* outHr) {
    // Refuse before creating any preview resource. Final write remains CREATE_NEW.
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (outStage) *outStage = "report_file_already_exists";
        if (outHr) *outHr = HRESULT_FROM_WIN32(ERROR_FILE_EXISTS);
        return 4;
    }
    winprobe::Report report;
    const BOOL dpiCallOk = SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    report.dpiSet = (dpiCallOk != FALSE) ||
                    (AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(),
                                                  DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE);
    report.dpiSetError = report.dpiSet ? 0 : GetLastError();
    ComScope com;
    check(com.hr, "CoInitializeEx");
    report.environment = winprobe::inspectEnvironment();
    winprobe::DesktopView desktop;
    std::unique_ptr<MarkerWindow> window;
    std::string stage = "not_started";
    HRESULT hr = S_OK;
    int code = 0;
    try {
        require(report.dpiSet, "PMv2_not_established");
        require(winprobe::interactive(report.environment), "requires_visible_same_session_input_desktop");
        const auto snapshotStarted = GetTickCount64();
        const int capRc =
            protectedMode ? winprobe::captureAll(report, desktop) : winprobe::captureSamples(report, 5, desktop);
        if (capRc != 0) {
            const std::string detailStage =
                !report.shellStage.empty() ? ("snapshot." + report.shellStage) : "fresh_snapshot_read_failed";
            if (outStage) *outStage = detailStage;
            if (outHr) *outHr = report.shellHr ? *report.shellHr : E_FAIL;
            require(false, "fresh_snapshot_read_failed");
        }
        require(GetTickCount64() - snapshotStarted < 15000, "snapshot_acquisition_too_old");
        auto contract = inspectContract(desktop, report);
        loadViewSizedImages(desktop, report, contract);
        require(GetTickCount64() - snapshotStarted < 15000, "sized_snapshot_acquisition_too_old");
        window = std::make_unique<MarkerWindow>(report, std::move(contract), &desktop, protectedMode,
                                                startupExperiment);
        window->run(path + L".initialized.json");
        stage = window->stopReason;
        hr = window->failureCode;
        code = window->failed ? 1 : 0;
        if (code != 0 && window->failureStage && *window->failureStage) {
            stage = std::string(stage) + ":" + window->failureStage;
        }
    } catch (const Failure& e) {
        if (!outStage || outStage->empty()) stage = e.stage;
        else stage = *outStage;
        hr = (outHr && *outHr != S_OK && e.code == E_FAIL) ? *outHr : e.code;
        code = 1;
    } catch (...) {
        stage = "unexpected_initialization_exception";
        hr = E_UNEXPECTED;
        code = 1;
    }
    if (outStage) *outStage = stage;
    if (outHr) *outHr = hr;
    if (window) window->stop();
    std::ostringstream out;
    out << "{\"schema\":1,\"mode\":\""
        << (protectedMode ? "experimental_protected_full_mirror" : "non_hiding_staged_development_preview")
        << "\",\"real_desktop_mode\":\"" << (protectedMode ? "EXPERIMENTAL_LIMITED" : "DISABLED")
        << "\",\"mirrored_item_count\":" << report.samples.size()
        << ",\"filenames_visible\":" << (window && window->showFilenames ? "true" : "false")
        << ",\"filename_toggle_count\":" << (window ? window->filenameToggles : 0)
        << ",\"filename_cached_layout_count\":" << (window ? window->cachedFilenameLayouts : 0)
        << ",\"protected_maximum_seconds\":"
        << (protectedMode
                ? (window && window->protectedSession && window->protectedSession->isContinuous() ? 43200
                                                                                                  : 180)
                : 0)
        << ",\"full_snapshot\":" << (report.fullSnapshot ? "true" : "false")
        << ",\"protected_restore_failed\":" << (window && window->protectedRestoreFailed ? "true" : "false")
        << ",\"open_recovery_completed\":"
        << (window && window->openRecoveryCompleted ? (*window->openRecoveryCompleted ? "true" : "false")
                                                    : "null")
        << ",\"open_system_accepted\":"
        << (window && window->openSystemAccepted ? (*window->openSystemAccepted ? "true" : "false") : "null")
        << ",\"open_user_confirmed\":null,\"open_mode\":\"keep_session\",\"open_attempts\":[";
    if (window) {
        bool first = true;
        for (const auto& attempt : window->openAttempts) {
            if (!first) out << ',';
            first = false;
            out << "{\"request\":" << attempt.token
                << ",\"input_released\":" << (attempt.inputReleased ? "true" : "false")
                << ",\"recovery_before_open\":null,\"result\":" << static_cast<int>(attempt.result)
                << ",\"system_accepted\":"
                << (attempt.result == OpenResult::accepted   ? "true"
                    : attempt.result == OpenResult::rejected ? "false"
                                                             : "null")
                << ",\"error\":" << attempt.error << ",\"user_confirmed\":null}";
        }
    }
    out << "]"
        << ",\"session_trace\":" << (window ? window->sessionTrace.json() : "null") << ","
        << "\"stage\":\"" << stage << "\",\"hresult\":\"0x" << std::hex << static_cast<unsigned long>(hr)
        << std::dec << "\",\"exit_code\":" << code << ",\"native_failure_stage\":"
        << (window && window->failureStage ? "\"" + std::string(window->failureStage) + "\"" : "null")
        << ",\"show_and_present_returned_success\":" << (window && window->displayed ? "true" : "false")
        << ",\"measured_fps\":null,\"resource_leak_count\":null,\"input_capture_ever_requested\":"
        << (window && window->captureRequests > 0 ? "true" : "false") << ",\"capture_owned_after_stop\":"
        << (window && window->captureOwnedAfterStop ? (*window->captureOwnedAfterStop ? "true" : "false")
                                                    : "null")
        << ",\"interaction_entries\":" << (window ? window->interactionEntries : 0)
        << ",\"pointer_down_events\":" << (window ? window->pointerDowns : 0)
        << ",\"captured_move_events\":" << (window ? window->pointerMoves : 0)
        << ",\"pointer_up_events\":" << (window ? window->pointerUps : 0)
        << ",\"input_cancel_transitions\":" << (window ? window->inputCancels : 0)
        << ",\"static_copies_available\":" << (window && window->copiesEligible ? "true" : "false")
        << ",\"gravity_available_after_static_confirmation\":"
        << (window && window->copiesEligible ? "true" : "false")
        << ",\"interaction_requires_explicit_playback_confirmation\":true,\"marker_confirmed_this_run\":"
        << (window && window->markerConfirmed ? "true" : "false")
        << ",\"static_copies_shown\":" << (window && window->copiesWereShown ? "true" : "false")
        << ",\"static_confirmed_before_gravity\":" << (window && window->staticConfirmed ? "true" : "false")
        << ",\"gravity_started\":" << (window && window->gravityWasStarted ? "true" : "false")
        << ",\"automatic_overlay_stop\":" << (window && window->automaticStopped ? "true" : "false")
        << ",\"pause_toggle_commands\":" << (window ? window->pauseCommands : 0)
        << ",\"reset_commands\":" << (window ? window->resetCommands : 0)
        << ",\"billiards_toggles\":" << (window ? window->billiardsToggles : 0)
        << ",\"billiards_shots\":" << (window ? window->loggedBilliardsShots : 0)
        << ",\"billiards_pocketed\":" << (window ? window->loggedBilliardsPocketed : 0)
        << ",\"billiards_white_placements\":" << (window ? window->loggedBilliardsPlacements : 0)
        << ",\"billiards_tube_opens\":" << (window ? window->billiardsTubeOpens : 0)
        << ",\"billiards_best_shots\":" << (window ? window->bestBilliardsShotsObserved : 0)
        << ",\"sound_muted\":" << (window && window->soundMuted ? "true" : "false")
        << ",\"manual_uniform_offset_y_pixels\":"
        << (window ? std::to_string(window->manualOffsetYPixels) : "null")
        << ",\"shortcut_overlay_policy\":\"omit_for_SFGAO_LINK_copies_only\""
        << ",\"shortcut_overlays_omitted\":" << (window ? window->contract.shortcutOverlaysOmitted : 0)
        << ",\"offset_adjustments\":" << (window ? window->offsetAdjustments : 0)
        << ",\"offset_source\":\"baseline_only_other_profiles_zero\""
        << ",\"comparison_toggle_commands\":" << (window ? window->comparisonToggles : 0)
        << ",\"custom_visual_size_DIP\":null,\"custom_collision_scale\":0.9,"
           "\"layering\":\"unowned_non_topmost_above_shell_root_restored_after_interaction_no_host_"
           "attachment\","
           "\"screen_alignment_machine_verified\":false,\"human_saved\":"
        << (window && window->saved ? "true" : "false") << ",\"human\":{";
    const char* markerNames[] = {"anchor_correspondence", "transparent_background", "mouse_passthrough",
                                 "ordinary_app_layering", "interaction_not_started"};
    const char* copyNames[] = {"static_copies", "gravity_motion_collision", "pause_resume_reset",
                               "transparency_and_app_behaviour", "drag_throw_and_release"};
    const auto* names = window && window->copiesWereShown ? copyNames : markerNames;
    for (std::size_t i = 0; i < 5; ++i) {
        if (i) out << ',';
        const int answer = window && window->saved && (i != 4 || window->interactionEntries > 0)
                               ? window->savedAnswers[i]
                               : 0;
        out << '"' << names[i] << "\":\""
            << (answer == 1   ? "user_reports_match"
                : answer == 2 ? "user_reports_problem"
                              : "not_checked")
            << '"';
    }
    out << "},\"conversion\":";
    if (!window)
        out << "null";
    else {
        const auto& c = window->contract;
        out << "{\"adapter\":\"guarded_direct_SysListView32_LVS_NOSCROLL_convention\",\"source_shell_hwnd\":"
            << pointerJson(c.shell) << ",\"source_list_hwnd\":" << pointerJson(c.list)
            << ",\"startup_display_experiment\":" << (startupExperiment ? "true" : "false")
            << ",\"monitor_bounds_pixels\":" << rectJson(c.bounds)
            << ",\"source_hmonitor\":" << pointerJson(c.monitor) << ",\"source_dpi\":" << c.dpi
            << ",\"icon_size_hresult\":" << static_cast<long>(c.iconSizeHr)
            << ",\"shell_icon_size_pixels\":" << (c.iconPixels ? std::to_string(c.iconPixels) : "null")
            << ",\"system_image_list_pixels\":" << sizeJson(c.systemImagePixels)
            << ",\"system_image_hresult\":" << static_cast<long>(c.systemImageHr)
            << ",\"view_mode\":" << static_cast<int>(c.viewMode)
            << ",\"image_source\":\"SHGetFileInfo_absolute_PIDL_then_SHIL_EXTRALARGE_GetIcon\""
            << ",\"sized_image_stage\":\"" << c.sizedImageStage
            << "\",\"sized_image_attempt\":" << c.sizedImageAttempt
            << ",\"sized_images_hresult\":" << static_cast<long>(c.sizedImagesHr)
            << ",\"sized_images_ready\":" << (c.sizedImagesReady ? "true" : "false")
            << ",\"preview_dpi\":" << window->previewDpi << ",\"folder_flags\":" << c.flags
            << ",\"list_style\":" << c.listStyle << ",\"work_screen_pixels\":" << rectJson(c.work)
            << ",\"list_client_rect\":" << rectJson(c.listClient)
            << ",\"list_client_screen_rect\":" << rectJson(c.listScreen)
            << ",\"rules\":[\"LVS_NOSCROLL: view anchor equals list client anchor (restricted Shell "
               "adapter)\","
               "\"MapWindowPoints(list,NULL,one_point): device screen pixels\","
               "\"MapWindowPoints(NULL,preview,one_point): preview client pixels\","
               "\"D2D unit mode PIXELS, DPI 96: draw client pixels without scale multiplication\","
               "\"core windowsScreenPixelsToScene: subtract work origin then divide by preview DPI/96\","
               "\"copies: system image list GetIconSize actual pixels divided by preview DPI/96; fit bitmap "
               "without upscaling, centered in DPP slot at item anchor plus explicit uniform manual Y pixel "
               "offset; Explorer image bounds unknown\"],"
               "\"historical_120dpi_100percent_discrepancy\":\"unexplained_not_used_as_scale\",\"items\":[";
        for (std::size_t i = 0; i < window->screen.size(); ++i) {
            if (i) out << ',';
            const auto p = window->screen[i], q = window->client[i];
            const auto s = window->scene[i];
            out << "{\"sample_id\":\"" << report.samples[i].id << "\",\"screen_pixels\":[" << p.x << ','
                << p.y << "],\"preview_client_pixels\":[" << q.x << ',' << q.y << "],\"scene_DIP\":[" << s.x
                << ',' << s.y << "]";
            if (i < c.shortcutItems.size())
                out << ",\"shell_shortcut_attribute\":" << (c.shortcutItems[i] ? "true" : "false");
            if (i < c.bitmapPixels.size())
                out << ",\"sized_bitmap_pixels\":[" << c.bitmapPixels[i].cx << ',' << c.bitmapPixels[i].cy
                    << ']';
            if (c.sizedImagesReady) {
                const auto imageRect = windowsIconImageRect(
                    s, {static_cast<float>(c.bitmapPixels[i].cx), static_cast<float>(c.bitmapPixels[i].cy)},
                    c.systemImagePixels.cx, windowsDpiScale(window->previewDpi), window->manualOffsetYPixels);
                out << ",\"dpp_image_rect_DIP\":[" << imageRect.x << ',' << imageRect.y << ','
                    << imageRect.width << ',' << imageRect.height << ']';
            }
            out << ",\"explorer_image_bounds\":null}";
        }
        out << "]}";
    }
    out << ",\"snapshot\":" << winprobe::encodeReport(report) << "}\n";
    if (winprobe::writeNewReport(path, out.str())) return 4;
    return code;
}
}  // namespace dpp::winmirror
