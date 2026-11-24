#include "core/render/draw_list.h"

#include <algorithm>
#include <cstring>

namespace dpp {

void DrawList::addText(Vec2 pos, const char* text, Color color, float size, bool centered) {
    TextItem t;
    t.position = pos;
    t.color = color;
    t.size = size;
    t.centered = centered;
    if (text) {
        const std::size_t n = std::min(std::strlen(text), t.text.size() - 1);
        std::memcpy(t.text.data(), text, n);
        t.text[n] = '\0';
    }
    commands_.emplace_back(t);
}

void DrawList::translate(Vec2 delta) {
    if (delta.x == 0.0f && delta.y == 0.0f) return;
    for (auto& cmd : commands_) {
        if (auto* s = std::get_if<SpriteItem>(&cmd)) {
            s->center += delta;
        } else if (auto* c = std::get_if<CircleItem>(&cmd)) {
            c->center += delta;
        } else if (auto* l = std::get_if<LineItem>(&cmd)) {
            l->a += delta;
            l->b += delta;
        } else if (auto* b = std::get_if<BoxItem>(&cmd)) {
            b->center += delta;
        } else if (auto* p = std::get_if<PolygonItem>(&cmd)) {
            const std::uint8_t n = std::min<std::uint8_t>(p->count, 8u);
            for (std::uint8_t i = 0; i < n; ++i) p->points[i] += delta;
        } else if (auto* t = std::get_if<TextItem>(&cmd)) {
            t->position += delta;
        } else if (auto* h = std::get_if<BlackHoleItem>(&cmd)) {
            h->center += delta;
        }
    }
}

}  // namespace dpp
