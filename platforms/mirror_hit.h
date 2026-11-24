#pragma once
#include <cmath>

#include "core/render/draw_list.h"
namespace dpp {
// Render order, current centers and inverse rotation; no desktop coordinates.
inline TextureId mirrorHit(const DrawList& draw, Vec2 point) {
    for (auto i = draw.commands().rbegin(); i != draw.commands().rend(); ++i) {
        const auto* s = std::get_if<SpriteItem>(&*i);
        if (!s || !s->interactive || s->opacity <= 0) continue;
        const auto delta = point - s->center;
        const float c = std::cos(s->rotation), r = std::sin(s->rotation);
        const float x = c * delta.x + r * delta.y, y = -r * delta.x + c * delta.y;
        if (std::abs(x) > s->size.x / 2 || std::abs(y) > s->size.y / 2) continue;
        if (s->clipCircle && x * x + y * y > s->size.x * s->size.x / 4) continue;
        return s->texture;
    }
    return kNoTexture;
}
}  // namespace dpp
