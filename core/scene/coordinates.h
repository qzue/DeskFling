#pragma once
// Coordinate contract. Every conversion between spaces goes through this file.
// Scattering scale constants elsewhere is not allowed (see docs/architecture.md).
//
// Spaces:
//  1. Windows screen pixels   : physical pixels, origin = top-left of the primary
//                               monitor, +Y down. Work area given as a pixel rect.
//                               Per-monitor DPI scale s = dpi / 96.
//  2. macOS global points     : AppKit points, origin = bottom-left of the primary
//                               screen, +Y UP. Work area = NSScreen.visibleFrame.
//     macOS backing pixels    : points * backingScaleFactor (Retina = 2.0).
//  3. Scene (logical) points  : origin = top-left of the work area, +X right,
//                               +Y DOWN. 1 scene point = 1 Windows DIP = 1 AppKit point.
//                               All game modes, input and draw lists use this space.
//  4. Physics (Box2D) metres  : origin = bottom-left of the work area, +X right,
//                               +Y UP, gravity (0, -g). pointsPerMeter (default 100)
//                               keeps a 64pt icon at 0.64 m, a 1080pt-high work area at 10.8 m.
//
// Angles: physics angle is CCW-positive (Y up). Scene rotation is CW-positive
// on screen (Y down), therefore sceneRotation = -physicsAngle.
//
// Icon rectangles: platforms usually report top-left + size; modes use centres.

#include <span>

#include "core/scene/types.h"

namespace dpp {

inline constexpr float kDefaultPointsPerMeter = 100.0f;

class SceneToPhysics {
   public:
    SceneToPhysics(Vec2 workAreaSizePoints, float pointsPerMeter = kDefaultPointsPerMeter);

    [[nodiscard]] Vec2 toPhysics(Vec2 scenePoint) const;
    [[nodiscard]] Vec2 toScene(Vec2 physicsPoint) const;
    [[nodiscard]] float lengthToPhysics(float scenePoints) const { return scenePoints / pointsPerMeter_; }
    [[nodiscard]] float lengthToScene(float meters) const { return meters * pointsPerMeter_; }
    // Velocities: same scale, Y flipped.
    [[nodiscard]] Vec2 velocityToPhysics(Vec2 scenePointsPerSecond) const;
    [[nodiscard]] Vec2 velocityToScene(Vec2 metersPerSecond) const;
    [[nodiscard]] static float angleToScene(float physicsRadians) { return -physicsRadians; }

    [[nodiscard]] Vec2 worldSizeMeters() const { return worldSize_; }
    [[nodiscard]] float pointsPerMeter() const { return pointsPerMeter_; }

   private:
    Vec2 sceneSize_;
    Vec2 worldSize_;
    float pointsPerMeter_;
};

// ---- Windows (pure math; no Win32 types) ---------------------------------
struct PixelRect {
    int left = 0, top = 0, right = 0, bottom = 0;
    [[nodiscard]] int width() const { return right - left; }
    [[nodiscard]] int height() const { return bottom - top; }
};

[[nodiscard]] float windowsDpiScale(unsigned dpi);  // 96 -> 1.0, 144 -> 1.5
[[nodiscard]] Vec2 windowsScreenPixelsToScene(int px, int py, const PixelRect& workAreaPx, float dpiScale);
[[nodiscard]] Vec2 windowsSceneToScreenPixels(Vec2 scene, const PixelRect& workAreaPx, float dpiScale);
[[nodiscard]] Vec2 windowsWorkAreaSceneSize(const PixelRect& workAreaPx, float dpiScale);

// Fit a cached bitmap into the Shell-reported pixel-sized slot at an item anchor.
// Preserve aspect ratio and never enlarge a low-resolution source. This defines
// DPP placement, not Explorer's image bounds (which remain unmeasured).
[[nodiscard]] Rect windowsIconImageRect(Vec2 anchorScene, Vec2 bitmapPixels, int viewIconPixels,
                                        float dpiScale, int manualOffsetYPixels = 0);
// Exact inverse of rectCenter(windowsIconImageRect(...)): maps an icon image center in scene DIPs
// back to its Shell item anchor in scene DIPs (for opt-in desktop layout write-back).
[[nodiscard]] Vec2 windowsSceneCenterToIconAnchor(Vec2 sceneCenter, int viewIconPixels, float dpiScale,
                                                  int manualOffsetYPixels = 0);

// ---- macOS (pure math; no AppKit types) -----------------------------------
// visibleFrame is in AppKit global points (origin bottom-left, Y up).
[[nodiscard]] Vec2 macGlobalPointsToScene(Vec2 global, const Rect& visibleFrame);
[[nodiscard]] Vec2 macSceneToGlobalPoints(Vec2 scene, const Rect& visibleFrame);
// Local view coordinates of a flipped NSView (isFlipped == YES) that exactly
// covers the work area are identical to scene points.
[[nodiscard]] Vec2 macSceneToBackingPixels(Vec2 scene, float backingScaleFactor);

// Candidate Finder contract, usable ONLY after independent pointer calibration.
// Single primary display, logical points from its top-left, Y down; never Retina-scaled.
[[nodiscard]] Vec2 macTopLeftPointsToGlobal(Vec2 point, const Rect& primaryFrame);
// Exact inverse of macTopLeftPointsToGlobal -> macGlobalPointsToScene: maps a scene point
// (relative to visibleFrame top-left) back to Finder primary-screen top-left logical points.
[[nodiscard]] Vec2 macSceneToTopLeftPoints(Vec2 scene, const Rect& visibleFrame, const Rect& primaryFrame);
[[nodiscard]] bool macPointerMatchesTopLeft(Vec2 raw, Vec2 pointerGlobal, const Rect& primaryFrame,
                                            float tolerancePoints = 12.0f);

[[nodiscard]] bool macTopLeftSampleSetUsable(std::span<const Vec2> rawPoints, const Rect& primaryFrame);
[[nodiscard]] bool macMultiDisplayPointValid(Vec2 globalPt, std::span<const Rect> screenFrames,
                                             float tolerancePoints = 4.0f);

// Exact 2D axis-aligned complement: returns non-overlapping dead-zone rectangles inside [0..unionSize]
// that are NOT covered by any monitor rectangle in `screenRects` (used as static Box2D walls in multi-screen span).
[[nodiscard]] std::vector<Rect> computeMultiScreenVoidRects(Vec2 unionSize, std::span<const Rect> screenRects);

// Computes the largest connected multi-screen inscribed band across `screenRects` so panoramic arena modes
// (Slingshot, AirHockey, Billiards) span all monitors seamlessly without clipping into mixed-height dead zones.
[[nodiscard]] Rect computeMultiScreenSpanArenaRect(Vec2 unionSize, std::span<const Rect> screenRects);

// Migration geometry uses full rotated visual bounds in the existing scene space.
struct MigrationIcon {
    Vec2 center, size;
    float rotation = 0;
};
[[nodiscard]] Vec2 rotatedIconHalfExtent(Vec2 size, float rotation);
// All positions are generated first. Conservative AABB overlap > 1 logical point
// on both axes uses one stable placement pass, at most 2*i+3 horizontal slices
// and i blocked intervals for icon i (up to 128 icons); no iterative relaxation.
// Returns no partial layout on failure. Equal sizes preserve positions exactly.
[[nodiscard]] std::vector<Vec2> migrateIconCenters(std::span<const MigrationIcon> icons, Vec2 oldSize,
                                                   Vec2 newSize);

// ---- Icon rectangle helpers ----------------------------------------------
[[nodiscard]] Vec2 rectCenter(const Rect& topLeftRect);
[[nodiscard]] Rect rectFromCenter(Vec2 center, Vec2 size);

}  // namespace dpp
