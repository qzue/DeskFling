#pragma once
// Plain value types shared by every core module.
// No platform headers, no file paths, no OS handles may appear here.

#include <cmath>
#include <cstdint>
#include <vector>

namespace dpp {

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;

    constexpr Vec2() = default;
    constexpr Vec2(float x_, float y_) : x(x_), y(y_) {}

    constexpr Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
    constexpr Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
    constexpr Vec2 operator*(float s) const { return {x * s, y * s}; }
    constexpr Vec2 operator/(float s) const { return {x / s, y / s}; }
    constexpr Vec2& operator+=(Vec2 o) {
        x += o.x;
        y += o.y;
        return *this;
    }
    constexpr bool operator==(const Vec2&) const = default;

    [[nodiscard]] float length() const { return std::sqrt(x * x + y * y); }
    [[nodiscard]] constexpr float lengthSquared() const { return x * x + y * y; }
    [[nodiscard]] bool isFinite() const { return std::isfinite(x) && std::isfinite(y); }
};

struct Rect {
    float x = 0.0f;  // left
    float y = 0.0f;  // top in scene space (Y down); see coordinates.h for other spaces
    float width = 0.0f;
    float height = 0.0f;
    constexpr bool operator==(const Rect&) const = default;
};

struct Color {
    std::uint8_t r = 255, g = 255, b = 255, a = 255;
};

// Opaque internal identifiers. They are never derived from display names and
// carry no file-system meaning. Platform adapters keep any mapping to real
// desktop items on their side of the boundary.
enum class IconId : std::uint32_t {};
enum class TextureId : std::uint32_t {};
inline constexpr TextureId kNoTexture{0};

// One desktop icon as seen by a game mode: visual + initial layout only.
// The collision shape is derived by the mode from iconSize (labels never collide).
struct IconSeed {
    IconId id{};
    TextureId texture = kNoTexture;
    Vec2 center;    // scene points, icon image centre (not the label)
    Vec2 iconSize;  // scene points, drawing size; adapter records measured vs DPP-defined provenance
};

// Everything a mode needs to build its scene.
struct SceneSetup {
    Vec2 workAreaSize;  // scene points
    std::vector<IconSeed> icons;
    std::uint32_t randomSeed = 1u;  // fixed seeds keep tests reproducible
    // Multi-screen span support (empty for single-screen):
    std::vector<Rect> screenWorkAreas{};
    Rect primaryArenaRect{};  // width > 0 anchors arena modes (Billiards/Slingshot/AirHockey) to primary screen
};

}  // namespace dpp
