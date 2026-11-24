#pragma once
// Structured, reusable per-frame draw data. Produced by game modes in scene
// points; consumed by native renderers (Direct2D, Core Animation/Core Graphics).
// Renderers own textures; the draw list only references TextureId values.

#include <array>
#include <cstddef>
#include <cstdint>
#include <variant>
#include <vector>

#include "core/scene/types.h"

namespace dpp {

struct SpriteItem {
    TextureId texture = kNoTexture;
    Vec2 center;         // scene points
    Vec2 size;           // scene points
    float rotation = 0;  // radians, clockwise-positive on screen (scene space)
    float opacity = 1.0f;
    bool clipCircle = false;  // billiards: icon drawn inside a ball
    bool interactive = true;
    // Cinematic black hole only (purely visual): approach shedding and
    // swallow dissolve progress in [0, 1]; grabbed copies stay unlensed.
    float tidal = 0, consume = 0;
    bool grabbed = false;
};

struct CircleItem {
    Vec2 center;
    float radius = 0;
    Color fill{255, 255, 255, 255};
    Color stroke{0, 0, 0, 0};
    float strokeWidth = 0;
};

struct LineItem {
    Vec2 a, b;
    Color color{255, 255, 255, 200};
    float width = 1.0f;
    bool dashed = false;
};

struct BoxItem {
    Vec2 center;
    Vec2 size;
    float rotation = 0.0f;  // radians, clockwise-positive on screen (scene space)
    float cornerRadius = 3.0f;
    Color fill{255, 255, 255, 255};
    Color stroke{0, 0, 0, 0};
    float strokeWidth = 0.0f;
};

struct PolygonItem {
    std::array<Vec2, 8> points{};
    std::uint8_t count = 0;  // 3..8 closed vertices in scene points
    Color fill{255, 255, 255, 255};
    Color stroke{0, 0, 0, 0};
    float strokeWidth = 0.0f;
};

struct TextItem {
    Vec2 position;                 // top-left (or top-center when centered == true)
    std::array<char, 128> text{};  // fixed buffer: no per-frame heap allocation
    Color color{255, 255, 255, 255};
    float size = 13.0f;
    bool centered = false;
};

// Cinematic toy black hole (accretion disk + lensed arcs). Purely visual and
// never hit-tested; renderers draw it from cached images. Back is emitted
// before swallowing sprites, Front (near half of the disk) after them.
struct BlackHoleItem {
    enum class Layer : std::uint8_t { Back, Front };
    Vec2 center;
    float radius = 0;     // shadow radius, scene points
    float phase = 0;      // radians in [0, 2*pi); integer rate multiples stay continuous
    float time = 0;       // seconds, wrapped at 1200 (a multiple of renderer flow periods)
    float elevation = 0;  // 3D camera pitch above disk plane, radians in [-85 deg, +85 deg]
    float tilt = 0;       // 2D screen roll angle, radians in [-pi, +pi] (clockwise-positive)
    Layer layer = Layer::Back;
};

using DrawCommand =
    std::variant<SpriteItem, CircleItem, LineItem, BoxItem, PolygonItem, TextItem, BlackHoleItem>;

class DrawList {
   public:
    void clear() { commands_.clear(); }  // keeps capacity across frames
    void reserve(std::size_t n) { commands_.reserve(n); }
    void add(const DrawCommand& c) { commands_.push_back(c); }
    void addText(Vec2 pos, const char* text, Color color = {255, 255, 255, 255}, float size = 13.0f,
                 bool centered = false);
    void translate(Vec2 delta);
    [[nodiscard]] const std::vector<DrawCommand>& commands() const { return commands_; }
    [[nodiscard]] std::size_t size() const { return commands_.size(); }

   private:
    std::vector<DrawCommand> commands_;
};

}  // namespace dpp
