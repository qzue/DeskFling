#include "core/scene/coordinates.h"

#include <algorithm>
#include <stdexcept>

namespace dpp {

SceneToPhysics::SceneToPhysics(Vec2 workAreaSizePoints, float pointsPerMeter)
    : sceneSize_(workAreaSizePoints),
      pointsPerMeter_(pointsPerMeter > 0.0f ? pointsPerMeter : kDefaultPointsPerMeter) {
    if (!sceneSize_.isFinite() || sceneSize_.x <= 0 || sceneSize_.y <= 0 || !std::isfinite(pointsPerMeter_) ||
        pointsPerMeter_ < 1)
        throw std::invalid_argument("invalid scene coordinate dimensions or scale");
    worldSize_ = {sceneSize_.x / pointsPerMeter_, sceneSize_.y / pointsPerMeter_};
}

Vec2 SceneToPhysics::toPhysics(Vec2 p) const {
    return {p.x / pointsPerMeter_, (sceneSize_.y - p.y) / pointsPerMeter_};
}

Vec2 SceneToPhysics::toScene(Vec2 m) const {
    return {m.x * pointsPerMeter_, sceneSize_.y - m.y * pointsPerMeter_};
}

Vec2 SceneToPhysics::velocityToPhysics(Vec2 v) const {
    return {v.x / pointsPerMeter_, -v.y / pointsPerMeter_};
}

Vec2 SceneToPhysics::velocityToScene(Vec2 v) const { return {v.x * pointsPerMeter_, -v.y * pointsPerMeter_}; }

float windowsDpiScale(unsigned dpi) {
    if (!dpi) throw std::invalid_argument("unknown window DPI");
    return static_cast<float>(dpi) / 96.0f;
}
namespace {
void validateWindowsGeometry(const PixelRect& wa, float scale) {
    if (!std::isfinite(scale) || scale <= 0 || wa.right <= wa.left || wa.bottom <= wa.top)
        throw std::invalid_argument("invalid display geometry");
}
}  // namespace

Vec2 windowsScreenPixelsToScene(int px, int py, const PixelRect& wa, float s) {
    validateWindowsGeometry(wa, s);
    const float scale = s;
    return {static_cast<float>(px - wa.left) / scale, static_cast<float>(py - wa.top) / scale};
}

Vec2 windowsSceneToScreenPixels(Vec2 scene, const PixelRect& wa, float s) {
    validateWindowsGeometry(wa, s);
    const float scale = s;
    return {static_cast<float>(wa.left) + scene.x * scale, static_cast<float>(wa.top) + scene.y * scale};
}

Vec2 windowsWorkAreaSceneSize(const PixelRect& wa, float s) {
    validateWindowsGeometry(wa, s);
    const float scale = s;
    return {static_cast<float>(wa.width()) / scale, static_cast<float>(wa.height()) / scale};
}

Rect windowsIconImageRect(Vec2 anchor, Vec2 pixels, int iconPixels, float scale, int manualOffsetYPixels) {
    if (!anchor.isFinite() || !pixels.isFinite() || pixels.x <= 0 || pixels.y <= 0 || iconPixels <= 0 ||
        iconPixels > 1024 || !std::isfinite(scale) || scale <= 0 || manualOffsetYPixels < -16 ||
        manualOffsetYPixels > 16)
        throw std::invalid_argument("invalid icon pixel geometry");
    const float slot = static_cast<float>(iconPixels);
    const float fit = std::min({1.0f, slot / pixels.x, slot / pixels.y});
    const Vec2 size = pixels * fit;
    return {anchor.x + (slot - size.x) * 0.5f / scale,
            anchor.y + ((slot - size.y) * 0.5f + static_cast<float>(manualOffsetYPixels)) / scale,
            size.x / scale, size.y / scale};
}

Vec2 windowsSceneCenterToIconAnchor(Vec2 sceneCenter, int iconPixels, float scale, int manualOffsetYPixels) {
    if (!sceneCenter.isFinite() || iconPixels <= 0 || iconPixels > 1024 || !std::isfinite(scale) ||
        scale <= 0 || manualOffsetYPixels < -16 || manualOffsetYPixels > 16)
        throw std::invalid_argument("invalid icon pixel geometry");
    const float halfSlot = static_cast<float>(iconPixels) * 0.5f;
    return {sceneCenter.x - halfSlot / scale,
            sceneCenter.y - (halfSlot + static_cast<float>(manualOffsetYPixels)) / scale};
}

Vec2 macGlobalPointsToScene(Vec2 g, const Rect& vf) { return {g.x - vf.x, (vf.y + vf.height) - g.y}; }

Vec2 macSceneToGlobalPoints(Vec2 scene, const Rect& vf) {
    return {vf.x + scene.x, (vf.y + vf.height) - scene.y};
}

Vec2 macSceneToBackingPixels(Vec2 scene, float backingScaleFactor) {
    if (!std::isfinite(backingScaleFactor) || backingScaleFactor <= 0)
        throw std::invalid_argument("unknown backing scale");
    const float s = backingScaleFactor;
    return scene * s;
}

Vec2 macTopLeftPointsToGlobal(Vec2 point, const Rect& primaryFrame) {
    return macSceneToGlobalPoints(point, primaryFrame);
}

Vec2 macSceneToTopLeftPoints(Vec2 scene, const Rect& visibleFrame, const Rect& primaryFrame) {
    if (!scene.isFinite() || !std::isfinite(visibleFrame.x) || !std::isfinite(visibleFrame.y) ||
        !std::isfinite(visibleFrame.width) || !std::isfinite(visibleFrame.height) || visibleFrame.width <= 0 ||
        visibleFrame.height <= 0 || !std::isfinite(primaryFrame.x) || !std::isfinite(primaryFrame.y) ||
        !std::isfinite(primaryFrame.width) || !std::isfinite(primaryFrame.height) || primaryFrame.width <= 0 ||
        primaryFrame.height <= 0)
        throw std::invalid_argument("invalid macOS desktop frame");
    return macGlobalPointsToScene(macSceneToGlobalPoints(scene, visibleFrame), primaryFrame);
}
bool macPointerMatchesTopLeft(Vec2 raw, Vec2 pointerGlobal, const Rect& frame, float tolerance) {
    if (!raw.isFinite() || !pointerGlobal.isFinite() || !std::isfinite(tolerance) || tolerance < 0 ||
        !std::isfinite(frame.x) || !std::isfinite(frame.y) || !std::isfinite(frame.width) ||
        !std::isfinite(frame.height) || frame.width <= 0 || frame.height <= 0 || raw.x < 0 || raw.y < 0 ||
        raw.x > frame.width || raw.y > frame.height)
        return false;
    const auto expected = macTopLeftPointsToGlobal(raw, frame);
    return std::hypot(expected.x - pointerGlobal.x, expected.y - pointerGlobal.y) <= tolerance;
}

bool macTopLeftSampleSetUsable(std::span<const Vec2> rawPoints, const Rect& frame) {
    if (rawPoints.size() < 2 || rawPoints.size() > 3) return false;
    bool separated = false;
    for (auto a : rawPoints) {
        if (!macPointerMatchesTopLeft(a, macTopLeftPointsToGlobal(a, frame), frame, 0)) return false;
        for (auto b : rawPoints)
            if (std::hypot(a.x - b.x, a.y - b.y) >= 80) separated = true;
    }
    return separated;
}

bool macMultiDisplayPointValid(Vec2 globalPt, std::span<const Rect> screenFrames, float tolerancePoints) {
    if (!globalPt.isFinite() || !std::isfinite(tolerancePoints) || tolerancePoints < 0.0f ||
        screenFrames.empty())
        return false;
    for (const Rect& f : screenFrames) {
        if (!std::isfinite(f.x) || !std::isfinite(f.y) || !std::isfinite(f.width) ||
            !std::isfinite(f.height) || f.width <= 0.0f || f.height <= 0.0f)
            continue;
        if (globalPt.x >= f.x - tolerancePoints && globalPt.x <= f.x + f.width + tolerancePoints &&
            globalPt.y >= f.y - tolerancePoints && globalPt.y <= f.y + f.height + tolerancePoints) {
            return true;
        }
    }
    return false;
}

std::vector<Rect> computeMultiScreenVoidRects(Vec2 unionSize, std::span<const Rect> screenRects) {
    std::vector<Rect> out;
    if (!unionSize.isFinite() || unionSize.x <= 1.0f || unionSize.y <= 1.0f || screenRects.empty()) {
        return out;
    }
    std::vector<float> xs = {0.0f, unionSize.x};
    std::vector<float> ys = {0.0f, unionSize.y};
    xs.reserve(2u + screenRects.size() * 2u);
    ys.reserve(2u + screenRects.size() * 2u);
    for (const Rect& r : screenRects) {
        if (!std::isfinite(r.x) || !std::isfinite(r.y) || !std::isfinite(r.width) ||
            !std::isfinite(r.height) || r.width <= 0.0f || r.height <= 0.0f)
            continue;
        xs.push_back(std::clamp(r.x, 0.0f, unionSize.x));
        xs.push_back(std::clamp(r.x + r.width, 0.0f, unionSize.x));
        ys.push_back(std::clamp(r.y, 0.0f, unionSize.y));
        ys.push_back(std::clamp(r.y + r.height, 0.0f, unionSize.y));
    }
    auto dedup = [](std::vector<float>& v) {
        std::sort(v.begin(), v.end());
        auto outIt = v.begin();
        for (float x : v) {
            if (outIt == v.begin() || x - *(outIt - 1) > 0.25f) *outIt++ = x;
        }
        v.erase(outIt, v.end());
    };
    dedup(xs);
    dedup(ys);
    if (xs.size() < 2u || ys.size() < 2u) return out;

    for (std::size_t yi = 0; yi + 1 < ys.size(); ++yi) {
        const float y0 = ys[yi], y1 = ys[yi + 1];
        if (y1 - y0 < 1.0f) continue;
        const float my = 0.5f * (y0 + y1);
        bool runOpen = false;
        float runX0 = 0.0f;
        for (std::size_t xi = 0; xi + 1 < xs.size(); ++xi) {
            const float x0 = xs[xi], x1 = xs[xi + 1];
            if (x1 - x0 < 1.0f) continue;
            const float mx = 0.5f * (x0 + x1);
            bool covered = false;
            for (const Rect& r : screenRects) {
                if (mx >= r.x && mx <= r.x + r.width && my >= r.y && my <= r.y + r.height) {
                    covered = true;
                    break;
                }
            }
            if (!covered) {
                if (!runOpen) {
                    runOpen = true;
                    runX0 = x0;
                }
            } else if (runOpen) {
                out.push_back({runX0, y0, x0 - runX0, y1 - y0});
                runOpen = false;
            }
        }
        if (runOpen) {
            out.push_back({runX0, y0, xs.back() - runX0, y1 - y0});
        }
    }
    return out;
}

Rect computeMultiScreenSpanArenaRect(Vec2 unionSize, std::span<const Rect> screenRects) {
    const Rect fallback{0.0f, 0.0f, std::max(100.0f, unionSize.x), std::max(100.0f, unionSize.y)};
    if (screenRects.empty() || !unionSize.isFinite() || unionSize.x < 100.0f || unionSize.y < 100.0f) {
        return fallback;
    }
    bool any = false;
    float minX = 0.0f, maxX = 0.0f, minY = 0.0f, maxY = 0.0f;
    float sharedLeft = 0.0f, sharedRight = 0.0f, sharedTop = 0.0f, sharedBottom = 0.0f;
    for (const Rect& raw : screenRects) {
        if (!std::isfinite(raw.x) || !std::isfinite(raw.y) || !std::isfinite(raw.width) ||
            !std::isfinite(raw.height) || raw.width < 100.0f || raw.height < 100.0f) {
            continue;
        }
        const float x0 = std::clamp(raw.x, 0.0f, unionSize.x);
        const float x1 = std::clamp(raw.x + raw.width, 0.0f, unionSize.x);
        const float y0 = std::clamp(raw.y, 0.0f, unionSize.y);
        const float y1 = std::clamp(raw.y + raw.height, 0.0f, unionSize.y);
        if (x1 - x0 < 100.0f || y1 - y0 < 100.0f) continue;
        if (!any) {
            minX = sharedLeft = x0;
            maxX = sharedRight = x1;
            minY = sharedTop = y0;
            maxY = sharedBottom = y1;
            any = true;
        } else {
            minX = std::min(minX, x0);
            maxX = std::max(maxX, x1);
            minY = std::min(minY, y0);
            maxY = std::max(maxY, y1);
            sharedLeft = std::max(sharedLeft, x0);
            sharedRight = std::min(sharedRight, x1);
            sharedTop = std::max(sharedTop, y0);
            sharedBottom = std::min(sharedBottom, y1);
        }
    }
    if (!any) return fallback;
    const float spanW = maxX - minX;
    const float spanH = maxY - minY;
    if (spanW >= spanH) {
        if (sharedBottom - sharedTop >= 260.0f && spanW >= 200.0f) {
            return {minX, sharedTop, spanW, sharedBottom - sharedTop};
        }
    } else {
        if (sharedRight - sharedLeft >= 360.0f && spanH >= 200.0f) {
            return {sharedLeft, minY, sharedRight - sharedLeft, spanH};
        }
    }
    return {minX, minY, std::max(100.0f, spanW), std::max(100.0f, spanH)};
}

Vec2 rectCenter(const Rect& r) { return {r.x + r.width * 0.5f, r.y + r.height * 0.5f}; }

Rect rectFromCenter(Vec2 c, Vec2 size) { return {c.x - size.x * 0.5f, c.y - size.y * 0.5f, size.x, size.y}; }

Vec2 rotatedIconHalfExtent(Vec2 size, float angle) {
    if (!size.isFinite() || size.x <= 0 || size.y <= 0 || !std::isfinite(angle))
        throw std::invalid_argument("invalid migration icon");
    const float c = std::abs(std::cos(angle)), s = std::abs(std::sin(angle));
    return {(c * size.x + s * size.y) * .5f, (s * size.x + c * size.y) * .5f};
}
std::vector<Vec2> migrateIconCenters(std::span<const MigrationIcon> icons, Vec2 oldSize, Vec2 newSize) {
    if (!oldSize.isFinite() || !newSize.isFinite() || oldSize.x < 100 || oldSize.y < 100 || newSize.x < 100 ||
        newSize.y < 100 || icons.size() > 128)
        throw std::invalid_argument("unsupported migration geometry");
    std::vector<Vec2> positions, half;
    for (const auto& icon : icons) {
        if (!icon.center.isFinite()) throw std::invalid_argument("invalid frozen position");
        const auto h = rotatedIconHalfExtent(icon.size, icon.rotation);
        if (2 * h.x > newSize.x || 2 * h.y > newSize.y || 2 * h.x > oldSize.x || 2 * h.y > oldSize.y)
            throw std::invalid_argument("rotated icon cannot fit");
        const auto map = [](float p, float halfExt, float before, float after) {
            const float range = before - 2 * halfExt;
            const float t = range > 0 ? std::clamp((p - halfExt) / range, 0.0f, 1.0f) : .5f;
            // Roundoff in halfExt + (after - 2*halfExt) can exceed after-halfExt even for t=1.
            // Project onto the SAME legal interval; do not relax the boundary test.
            return std::clamp(halfExt + t * (after - 2 * halfExt), halfExt, after - halfExt);
        };
        positions.push_back(oldSize == newSize ? icon.center
                                               : Vec2{map(icon.center.x, h.x, oldSize.x, newSize.x),
                                                      map(icon.center.y, h.y, oldSize.y, newSize.y)});
        half.push_back(h);
    }
    const auto fits = [&](size_t i, Vec2 p) {
        const auto h = half[i];
        return p.isFinite() && p.x >= h.x && p.y >= h.y && p.x <= newSize.x - h.x && p.y <= newSize.y - h.y;
    };
    const auto overlaps = [&](size_t i, Vec2 p, size_t j) {
        const auto d = positions[j] - p;
        return half[i].x + half[j].x - std::abs(d.x) > 1 && half[i].y + half[j].y - std::abs(d.y) > 1;
    };
    // Validate the complete mapped layout before correcting any position.
    bool needsPlacement = false;
    for (size_t i = 0; i < positions.size(); ++i) {
        if (!fits(i, positions[i])) throw std::invalid_argument("full icon outside migration boundary");
        for (size_t j = 0; j < i; ++j) needsPlacement = needsPlacement || overlaps(i, positions[i], j);
    }
    if (!needsPlacement) return positions;
    // No position correction for an unchanged logical area (including backing-only).
    if (oldSize == newSize) throw std::invalid_argument("migration overlap cannot be repaired");

    // One stable input-order placement pass. Placed icons never move again, so
    // fixing one overlap cannot reintroduce an earlier one. Each icon starts at
    // its original normalized position, never at a partially corrected location.
    // At most 2*i+3 horizontal slices for icon i; each has at most i blocked
    // intervals. Their complement gives the nearest legal x on that slice.
    // No retry loop, physics settling, or search proportional to screen pixels.
    std::vector<float> rows;
    std::vector<std::pair<float, float>> blocked;
    rows.reserve(2 * positions.size() + 3);
    blocked.reserve(positions.size());
    const std::vector<Vec2> mapped = positions;
    bool primarySucceeded = true;
    for (size_t i = 0; i < positions.size(); ++i) {
        const Vec2 desired = positions[i];
        bool conflict = false;
        for (size_t j = 0; j < i; ++j) conflict = conflict || overlaps(i, desired, j);
        if (!conflict) continue;
        const Vec2 h = half[i];
        rows = {desired.y, h.y, newSize.y - h.y};
        for (size_t j = 0; j < i; ++j) {
            // Keep the existing 1-point overlap criterion; .01 is inward
            // clearance from that criterion, not extra permitted penetration.
            const float separation = h.y + half[j].y - 1 + .01f;
            rows.push_back(positions[j].y - separation);
            rows.push_back(positions[j].y + separation);
        }
        Vec2 best{};
        double bestDistance = 0;
        bool found = false;
        for (const float y : rows) {
            if (y < h.y || y > newSize.y - h.y) continue;
            blocked.clear();
            for (size_t j = 0; j < i; ++j) {
                if (h.y + half[j].y - std::abs(y - positions[j].y) <= 1 || h.x + half[j].x <= 1) continue;
                const float separation = h.x + half[j].x - 1 + .01f;
                blocked.emplace_back(positions[j].x - separation, positions[j].x + separation);
            }
            std::sort(blocked.begin(), blocked.end());
            const auto consider = [&](float left, float right) {
                if (left > right) return;
                const float x = std::clamp(desired.x, left, right);
                const double dx = static_cast<double>(x) - desired.x;
                const double dy = static_cast<double>(y) - desired.y;
                const double distance = dx * dx + dy * dy;
                // Equal distances keep the earlier slice/gap, for stable ties.
                if (!found || distance < bestDistance) {
                    found = true;
                    bestDistance = distance;
                    best = {x, y};
                }
            };
            float left = h.x;
            const float right = newSize.x - h.x;
            for (const auto& interval : blocked) {
                consider(left, std::min(right, interval.first));
                left = std::max(left, interval.second);
                if (left > right) break;
            }
            consider(left, right);
        }
        if (!found) {
            primarySucceeded = false;
            break;
        }
        positions[i] = best;
    }

    // Crowded-shrink deterministic shelf fallback: only runs when greedy slice placement
    // fragmented space on a changed work area. Preserves full rotated bounds and the exact
    // 1-point serious-overlap criterion; physically impossible packings still refuse cleanly.
    if (!primarySucceeded) {
        std::vector<size_t> order(mapped.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            if (std::abs(half[a].y - half[b].y) > 0.25f) return half[a].y > half[b].y;
            if (std::abs(mapped[a].y - mapped[b].y) > 0.25f) return mapped[a].y < mapped[b].y;
            if (std::abs(mapped[a].x - mapped[b].x) > 0.25f) return mapped[a].x < mapped[b].x;
            return a < b;
        });

        constexpr float kClear = 1.0f - 0.02f;  // Strict inward margin (< 1.0 overlap threshold)
        std::vector<Vec2> packed(mapped.size());
        float shelfMinBottom = 0.0f;
        size_t cursor = 0;
        while (cursor < order.size()) {
            const float shelfHalfH = half[order[cursor]].y;
            const float cy = (cursor == 0) ? shelfHalfH : (shelfMinBottom + shelfHalfH - kClear);
            if (cy + shelfHalfH > newSize.y) {
                throw std::invalid_argument("migration overlap cannot be repaired");
            }
            size_t end = cursor;
            float spanX = 0.0f;
            while (end < order.size()) {
                const Vec2 h = half[order[end]];
                const float nextSpan =
                    (end == cursor) ? (2.0f * h.x) : (spanX + 2.0f * h.x - kClear);
                if (nextSpan > newSize.x) break;
                spanX = nextSpan;
                ++end;
            }
            if (end == cursor) {
                throw std::invalid_argument("migration overlap cannot be repaired");
            }
            // Sort items within this shelf by desired X so horizontal desktop order stays natural.
            std::sort(order.begin() + static_cast<std::ptrdiff_t>(cursor),
                      order.begin() + static_cast<std::ptrdiff_t>(end), [&](size_t a, size_t b) {
                          if (std::abs(mapped[a].x - mapped[b].x) > 0.01f) return mapped[a].x < mapped[b].x;
                          return a < b;
                      });
            // Recompute exact span after X sort and shift toward desired mean X within legal margins.
            float exactSpan = 2.0f * half[order[cursor]].x;
            double desiredSumX = mapped[order[cursor]].x;
            for (size_t k = cursor + 1; k < end; ++k) {
                exactSpan += 2.0f * half[order[k]].x - kClear;
                desiredSumX += mapped[order[k]].x;
            }
            const float slack = std::max(0.0f, newSize.x - exactSpan);
            const float meanX = static_cast<float>(desiredSumX / static_cast<double>(end - cursor));
            const float startLeft = std::clamp(meanX - exactSpan * 0.5f, 0.0f, slack);
            float cx = startLeft + half[order[cursor]].x;
            packed[order[cursor]] = {std::clamp(cx, half[order[cursor]].x, newSize.x - half[order[cursor]].x),
                                     std::clamp(cy, half[order[cursor]].y, newSize.y - half[order[cursor]].y)};
            for (size_t k = cursor + 1; k < end; ++k) {
                cx += half[order[k - 1]].x + half[order[k]].x - kClear;
                const Vec2 hk = half[order[k]];
                packed[order[k]] = {std::clamp(cx, hk.x, newSize.x - hk.x),
                                    std::clamp(cy, hk.y, newSize.y - hk.y)};
            }
            shelfMinBottom = cy + shelfHalfH;
            cursor = end;
        }
        positions = std::move(packed);
    }
    // Publish only a fully verified layout; numerical or greedy-placement
    // failure remains a safe refusal, not permission to weaken the bounds.
    for (size_t i = 0; i < positions.size(); ++i) {
        if (!fits(i, positions[i])) throw std::invalid_argument("full icon outside migration boundary");
        for (size_t j = 0; j < i; ++j)
            if (overlaps(i, positions[i], j))
                throw std::invalid_argument("migration overlap cannot be repaired");
    }
    return positions;
}

}  // namespace dpp
