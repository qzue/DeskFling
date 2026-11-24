#pragma once
#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

#include "core/render/draw_list.h"

namespace dpp {
struct FilenamePeer {
    TextureId texture = kNoTexture;
    float centerX = 0.0f;
    float labelY = 0.0f;
};

inline float filenameLabelY(const SpriteItem& sprite, Vec2 workArea, float height) {
    const float extent = (std::abs(std::sin(sprite.rotation)) * sprite.size.x +
                          std::abs(std::cos(sprite.rotation)) * sprite.size.y) /
                         2;
    float y = sprite.center.y + extent + 3;
    if (y + height > workArea.y) y = sprite.center.y - extent - 3 - height;
    return std::clamp(y, 0.f, std::max(0.f, workArea.y - height));
}

inline void buildFilenamePeers(const DrawList& list, Vec2 workArea, std::vector<FilenamePeer>& out) {
    constexpr float height = 32.0f;
    out.clear();
    for (const auto& command : list.commands()) {
        const auto* s = std::get_if<SpriteItem>(&command);
        if (!s || s->opacity <= 0.0f || !s->interactive) continue;
        out.push_back(FilenamePeer{s->texture, s->center.x, filenameLabelY(*s, workArea, height)});
    }
}

inline Rect filenameLabelBoundsFromPeers(const SpriteItem& sprite, float precomputedY, Vec2 workArea,
                                         std::span<const FilenamePeer> peers, float maximumWidth = 120) {
    constexpr float height = 32, gap = 6;
    float width = std::min(maximumWidth,
                           2 * std::max(0.f, std::min(sprite.center.x, workArea.x - sprite.center.x) - 2));
    for (const FilenamePeer& other : peers) {
        if (other.texture == sprite.texture) continue;
        if (other.labelY + height <= precomputedY || precomputedY + height <= other.labelY) continue;
        const float distance = std::abs(other.centerX - sprite.center.x);
        if (distance >= 1) width = std::min(width, std::max(24.f, distance - gap));
    }
    width = std::floor(width / 4) * 4;
    if (width < 24) width = 0;
    return {sprite.center.x - width / 2, precomputedY, width, height};
}

// Rendering geometry only: current sprites, never original desktop coordinates or names.
// Keep the box centred instead of shifting the first column's names into its neighbour.
inline Rect filenameLabelBounds(const SpriteItem& sprite, Vec2 workArea, const DrawList* peers = nullptr,
                                float maximumWidth = 120) {
    constexpr float height = 32, gap = 6;
    const float y = filenameLabelY(sprite, workArea, height);
    float width = std::min(maximumWidth,
                           2 * std::max(0.f, std::min(sprite.center.x, workArea.x - sprite.center.x) - 2));
    if (peers) {
        for (const auto& command : peers->commands()) {
            const auto* other = std::get_if<SpriteItem>(&command);
            if (!other || other->texture == sprite.texture || other->opacity <= 0 || !other->interactive)
                continue;
            const float otherY = filenameLabelY(*other, workArea, height);
            if (otherY + height <= y || y + height <= otherY) continue;
            const float distance = std::abs(other->center.x - sprite.center.x);
            if (distance >= 1) width = std::min(width, std::max(24.f, distance - gap));
        }
    }
    // Whole four-point steps avoid relayout for subpixel motion. Keep a compact
    // ellipsis box in dense piles; do not silently turn filenames off.
    width = std::floor(width / 4) * 4;
    if (width < 24) width = 0;
    return {sprite.center.x - width / 2, y, width, height};
}
}  // namespace dpp
