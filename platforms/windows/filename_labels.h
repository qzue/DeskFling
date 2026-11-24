#pragma once
// clang-format off
#include <windows.h>
#include <dwrite.h>
// clang-format on
#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "platforms/filename_geometry.h"
#include "probe_samples.h"

namespace dpp::winmirror {
inline constexpr float filenameWidth = 120, filenameHeight = 32;
inline Rect filenameBounds(const SpriteItem& sprite, Vec2 workArea, const DrawList* peers = nullptr) {
    return filenameLabelBounds(sprite, workArea, peers, filenameWidth);
}
inline Rect filenameBoundsFast(const SpriteItem& sprite, float precomputedY, Vec2 workArea,
                               std::span<const FilenamePeer> peers) {
    return filenameLabelBoundsFromPeers(sprite, precomputedY, workArea, peers, filenameWidth);
}
// Native UTF-16 layouts are built once per snapshot, never on a physics tick or checkbox click.
// Names stay in the renderer and are not identities or persisted in the diagnostic report.
class FilenameLabels {
    std::vector<Microsoft::WRL::ComPtr<IDWriteTextLayout>> layouts_;
    static void checked(HRESULT result) {
        if (FAILED(result)) throw std::runtime_error("filename_layout_failed");
    }

   public:
    void cache(const winprobe::Report& report) {
        Microsoft::WRL::ComPtr<IDWriteFactory> factory;
        checked(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                    reinterpret_cast<IUnknown**>(factory.GetAddressOf())));
        Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
        checked(factory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                          DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12, L"",
                                          &format));
        checked(format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER));
        checked(format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP));
        checked(format->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 16, 12));
        Microsoft::WRL::ComPtr<IDWriteInlineObject> ellipsis;
        checked(factory->CreateEllipsisTrimmingSign(format.Get(), &ellipsis));
        const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        checked(format->SetTrimming(&trimming, ellipsis.Get()));
        std::vector<Microsoft::WRL::ComPtr<IDWriteTextLayout>> next;
        next.reserve(report.samples.size());
        for (const auto& sample : report.samples) {
            Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
            checked(factory->CreateTextLayout(sample.localName.c_str(),
                                              static_cast<UINT32>(sample.localName.size()), format.Get(),
                                              filenameWidth, filenameHeight, &layout));
            next.push_back(std::move(layout));
        }
        layouts_ = std::move(next);
    }
    IDWriteTextLayout* at(TextureId texture) const {
        const auto index = static_cast<size_t>(texture);
        return index && index <= layouts_.size() ? layouts_[index - 1].Get() : nullptr;
    }
    IDWriteTextLayout* fit(TextureId texture, float width) {
        auto* layout = at(texture);
        if (layout && width > 0 && layout->GetMaxWidth() != width) checked(layout->SetMaxWidth(width));
        return layout;
    }
    size_t size() const { return layouts_.size(); }
};
}  // namespace dpp::winmirror
