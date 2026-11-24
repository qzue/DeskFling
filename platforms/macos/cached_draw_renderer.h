#pragma once
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#include <array>
#include <cstring>
#include <type_traits>
#include "core/render/draw_list.h"
#include "platforms/macos/black_hole_metal.h"
#include "platforms/macos/black_hole_visual.h"
namespace dpp::mac {
// UI-thread-only shared layer pool. Retains decoded CGImages; never reads files per frame.
class CachedDrawRenderer {
    enum class ShapeKind : std::uint8_t {
        Unset = 0,
        Sprite = 1,
        Circle = 2,
        Line = 3,
        Box = 4,
        Polygon = 5
    };
    struct ShapeCache {
        ShapeKind kind = ShapeKind::Unset;
        std::uint32_t texId = 0;
        float cx = 0.0f, cy = 0.0f;
        float sx = 0.0f, sy = 0.0f;
        float rot = 0.0f, scaleK = 1.0f;
        float opacity = -1.0f;
        float cornerR = -1.0f;
        float lineWidth = -1.0f;
        std::uint32_t fillRgba = 0;
        std::uint32_t strokeRgba = 0;
        CGRect frame = CGRectZero;
        std::array<Vec2, 8> pts{};
        std::uint8_t ptCount = 0;
        bool dashed = false;
        bool fillSet = false;
        bool strokeSet = false;
    };
    struct TextCache {
        std::array<char, 128> text{};
        CGRect frame = CGRectZero;
        float size = 0.0f;
        std::uint32_t rgba = 0;
        CGFloat scale = 0.0;
        bool centered = false;
        bool valid = false;
    };
    NSMutableArray<CAShapeLayer*>* shapes_ = [NSMutableArray array];
    NSMutableArray<CATextLayer*>* texts_ = [NSMutableArray array];
    NSArray* textures_;
    std::vector<ShapeCache> shapeCaches_;
    std::vector<TextCache> textCaches_;
    BlackHoleVisual blackHole_;  // cached-layer fallback (no Metal / compiling)
    BlackHoleMetal metalHole_;
    static std::uint32_t packColor(Color c) {
        return (static_cast<std::uint32_t>(c.r) << 24) | (static_cast<std::uint32_t>(c.g) << 16) |
               (static_cast<std::uint32_t>(c.b) << 8) | static_cast<std::uint32_t>(c.a);
    }
    static CGColorRef createSrgbColor(Color c) {
        static CGColorSpaceRef sSpace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        const CGFloat comps[4] = {c.r / 255.0, c.g / 255.0, c.b / 255.0, c.a / 255.0};
        return CGColorCreate(sSpace, comps);
    }
    static void applyFillColor(CAShapeLayer* l, ShapeCache& sc, Color c) {
        const std::uint32_t rgba = packColor(c);
        if (sc.fillSet && sc.fillRgba == rgba) return;
        if (c.a == 0) {
            l.fillColor = nil;
        } else {
            CGColorRef cg = createSrgbColor(c);
            l.fillColor = cg;
            CGColorRelease(cg);
        }
        sc.fillRgba = rgba;
        sc.fillSet = true;
    }
    static void applyStrokeColor(CAShapeLayer* l, ShapeCache& sc, Color c) {
        const std::uint32_t rgba = packColor(c);
        if (sc.strokeSet && sc.strokeRgba == rgba) return;
        if (c.a == 0) {
            l.strokeColor = nil;
        } else {
            CGColorRef cg = createSrgbColor(c);
            l.strokeColor = cg;
            CGColorRelease(cg);
        }
        sc.strokeRgba = rgba;
        sc.strokeSet = true;
    }
    static NSArray<NSNumber*>* dashedPattern() {
        static NSArray<NSNumber*>* sDash = @[ @6, @5 ];
        return sDash;
    }

   public:
    explicit CachedDrawRenderer(NSArray* textures) : textures_([textures copy]) {}
    NSUInteger layerCount() const { return shapes_.count + texts_.count; }
    const BlackHoleVisual& blackHoleVisual() const { return blackHole_; }
    NSString* blackHoleRenderer() const { return metalHole_.status(); }
    void render(const DrawList& draw, CALayer* root, CGRect bounds, CGFloat scale, bool outlined = false,
                bool manageTransaction = true) {
        (void)bounds;
        if (manageTransaction) {
            [CATransaction begin];
            [CATransaction setDisableActions:YES];
        }
        NSUInteger si = 0, ti = 0;
        blackHole_.beginFrame();
        const BlackHoleItem* back = nullptr;
        for (const auto& command : draw.commands())
            if (const auto* hole = std::get_if<BlackHoleItem>(&command);
                hole && hole->layer == BlackHoleItem::Layer::Back)
                back = hole;
        const bool metal = back && metalHole_.begin(*back, root, scale, outlined);
        for (const auto& command : draw.commands()) {
            if (const auto* hole = std::get_if<BlackHoleItem>(&command)) {
                // Between the previous and the next pooled shape in draw order.
                if (!metal)
                    blackHole_.update(*hole, root, scale, static_cast<CGFloat>(si) + .5);
                else if (hole->layer == BlackHoleItem::Layer::Front)
                    metalHole_.setZ(static_cast<CGFloat>(si) + .5);
                continue;
            }
            if (const auto* text = std::get_if<TextItem>(&command)) {
                if (ti == texts_.count) {
                    CATextLayer* l = [CATextLayer layer];
                    l.shadowColor = NSColor.blackColor.CGColor;
                    l.shadowOpacity = .85f;
                    l.shadowRadius = 2.0;
                    l.shadowOffset = CGSizeZero;
                    l.zPosition = 10000;
                    [texts_ addObject:l];
                    textCaches_.push_back({});
                    [root addSublayer:l];
                }
                const NSUInteger tIdx = ti++;
                CATextLayer* l = texts_[tIdx];
                TextCache& tc = textCaches_[tIdx];
                if (l.hidden) l.hidden = NO;
                if (!tc.valid || tc.scale != scale) {
                    l.contentsScale = scale;
                    tc.scale = scale;
                }
                const CGFloat h = std::max<CGFloat>(26.0, text->size * 1.65);
                const CGRect targetFrame =
                    text->centered ? CGRectMake(text->position.x - 380, text->position.y, 760, h)
                                   : CGRectMake(text->position.x, text->position.y, 700, h);
                if (!tc.valid || tc.centered != text->centered) {
                    l.alignmentMode = text->centered ? kCAAlignmentCenter : kCAAlignmentLeft;
                    tc.centered = text->centered;
                }
                if (!tc.valid || !CGRectEqualToRect(tc.frame, targetFrame)) {
                    l.frame = targetFrame;
                    tc.frame = targetFrame;
                }
                if (!tc.valid || tc.size != text->size) {
                    l.fontSize = text->size;
                    tc.size = text->size;
                }
                const std::uint32_t rgba = packColor(text->color);
                if (!tc.valid || tc.rgba != rgba) {
                    CGColorRef cg = createSrgbColor(text->color);
                    l.foregroundColor = cg;
                    CGColorRelease(cg);
                    tc.rgba = rgba;
                }
                if (!tc.valid || std::strcmp(tc.text.data(), text->text.data()) != 0) {
                    l.string = [NSString stringWithUTF8String:text->text.data()];
                    tc.text = text->text;
                }
                tc.valid = true;
                continue;
            }
            if (si == shapes_.count) {
                CAShapeLayer* l = [CAShapeLayer layer];
                [shapes_ addObject:l];
                shapeCaches_.push_back({});
                [root addSublayer:l];
            }
            const NSUInteger sIdx = si++;
            CAShapeLayer* l = shapes_[sIdx];
            ShapeCache& sc = shapeCaches_[sIdx];
            if (l.contentsScale != scale) l.contentsScale = scale;
            const CGFloat targetZ = static_cast<CGFloat>(si);
            if (l.zPosition != targetZ) l.zPosition = targetZ;

            std::visit(
                [&](const auto& item) {
                    using T = std::decay_t<decltype(item)>;
                    if constexpr (std::is_same_v<T, SpriteItem>) {
                        const auto index = static_cast<std::uint32_t>(item.texture);
                        if (metal && index) {
                            id image = textures_[(index - 1) % textures_.count];
                            CGImageRef cg = CFGetTypeID((__bridge CFTypeRef)image) == CGImageGetTypeID()
                                                ? (__bridge CGImageRef)image
                                                : nullptr;
                            if (metalHole_.claim(item, index, cg) != BlackHoleMetal::Claim::None) {
                                if (!l.hidden) l.hidden = YES;
                                return;
                            }
                        }
                        if (l.hidden) l.hidden = NO;
                        if (sc.kind != ShapeKind::Sprite) {
                            l.path = nil;
                            l.backgroundColor = nil;
                            l.fillColor = nil;
                            l.strokeColor = nil;
                            l.lineWidth = 0;
                            l.lineDashPattern = nil;
                            l.contentsGravity = kCAGravityResizeAspect;
                            l.masksToBounds = YES;
                            l.borderColor = NSColor.systemOrangeColor.CGColor;
                            sc = {};
                            sc.kind = ShapeKind::Sprite;
                        }
                        if (sc.sx != item.size.x || sc.sy != item.size.y) {
                            l.bounds = CGRectMake(0, 0, item.size.x, item.size.y);
                            sc.sx = item.size.x;
                            sc.sy = item.size.y;
                        }
                        if (sc.cx != item.center.x || sc.cy != item.center.y) {
                            l.position = CGPointMake(item.center.x, item.center.y);
                            sc.cx = item.center.x;
                            sc.cy = item.center.y;
                        }
                        const float effScale = (!metal && item.consume > 0.0f) ? (1.0f - item.consume) : 1.0f;
                        const float effOpacity =
                            (!metal && item.consume > 0.0f)
                                ? (item.opacity * (1.0f - item.consume * item.consume))
                                : item.opacity;
                        if (sc.rot != item.rotation || sc.scaleK != effScale) {
                            CATransform3D xf = CATransform3DMakeRotation(item.rotation, 0, 0, 1);
                            if (effScale != 1.0f) xf = CATransform3DScale(xf, effScale, effScale, 1);
                            l.transform = xf;
                            sc.rot = item.rotation;
                            sc.scaleK = effScale;
                        }
                        if (sc.opacity != effOpacity) {
                            l.opacity = effOpacity;
                            sc.opacity = effOpacity;
                        }
                        if (sc.texId != index) {
                            l.contents = index ? textures_[(index - 1) % textures_.count] : nil;
                            sc.texId = index;
                        }
                        const float cr = item.clipCircle ? item.size.x * 0.5f : 0.0f;
                        if (sc.cornerR != cr) {
                            l.cornerRadius = cr;
                            sc.cornerR = cr;
                        }
                        const CGFloat bw = (outlined && !item.clipCircle) ? 1.5 : 0.0;
                        if (l.borderWidth != bw) l.borderWidth = bw;
                    } else {
                        // Reset sprite/incompatible vector state only when transitioning between shape kinds.
                        auto resetVectorLayer = [&](ShapeKind nextKind) {
                            if (sc.kind == ShapeKind::Sprite || sc.texId != 0) {
                                l.contents = nil;
                                l.cornerRadius = 0;
                                l.masksToBounds = NO;
                                l.borderWidth = 0;
                            }
                            l.backgroundColor = nil;
                            l.transform = CATransform3DIdentity;
                            l.opacity = 1.0f;
                            l.lineDashPattern = nil;
                            l.fillColor = nil;
                            l.strokeColor = nil;
                            l.lineWidth = 0.0;
                            sc = {};
                            sc.kind = nextKind;
                        };

                        if constexpr (std::is_same_v<T, CircleItem>) {
                            if (l.hidden) l.hidden = NO;
                            const bool fresh = (sc.kind != ShapeKind::Circle);
                            if (fresh) resetVectorLayer(ShapeKind::Circle);
                            if (fresh || sc.sx != item.radius) {
                                const CGFloat d = item.radius * 2.0;
                                l.bounds = CGRectMake(0, 0, d, d);
                                CGPathRef path = CGPathCreateWithEllipseInRect(l.bounds, nullptr);
                                l.path = path;
                                CGPathRelease(path);
                                sc.sx = item.radius;
                            }
                            if (fresh || sc.cx != item.center.x || sc.cy != item.center.y) {
                                l.position = CGPointMake(item.center.x, item.center.y);
                                sc.cx = item.center.x;
                                sc.cy = item.center.y;
                            }
                            applyFillColor(l, sc, item.fill);
                            applyStrokeColor(l, sc, item.stroke);
                            if (sc.lineWidth != item.strokeWidth) {
                                l.lineWidth = item.strokeWidth;
                                sc.lineWidth = item.strokeWidth;
                            }
                        } else if constexpr (std::is_same_v<T, LineItem>) {
                            if (l.hidden) l.hidden = NO;
                            const bool fresh = (sc.kind != ShapeKind::Line);
                            if (fresh) {
                                resetVectorLayer(ShapeKind::Line);
                                l.lineCap = kCALineCapRound;
                            }
                            const CGFloat pad = std::max<CGFloat>(4.0, item.width * 0.65 + 2.0);
                            const CGFloat ox = std::floor(std::min<CGFloat>(item.a.x, item.b.x) - pad);
                            const CGFloat oy = std::floor(std::min<CGFloat>(item.a.y, item.b.y) - pad);
                            const CGFloat fw =
                                std::max<CGFloat>(4.0, std::ceil(std::max<CGFloat>(item.a.x, item.b.x) + pad) - ox);
                            const CGFloat fh =
                                std::max<CGFloat>(4.0, std::ceil(std::max<CGFloat>(item.a.y, item.b.y) + pad) - oy);
                            const CGRect tightFrame = CGRectMake(ox, oy, fw, fh);
                            const bool frameChanged = (fresh || !CGRectEqualToRect(sc.frame, tightFrame));
                            if (frameChanged) {
                                l.frame = tightFrame;
                                sc.frame = tightFrame;
                            }
                            if (frameChanged || sc.cx != item.a.x || sc.cy != item.a.y || sc.sx != item.b.x ||
                                sc.sy != item.b.y) {
                                CGMutablePathRef path = CGPathCreateMutable();
                                CGPathMoveToPoint(path, nullptr, item.a.x - ox, item.a.y - oy);
                                CGPathAddLineToPoint(path, nullptr, item.b.x - ox, item.b.y - oy);
                                l.path = path;
                                CGPathRelease(path);
                                sc.cx = item.a.x;
                                sc.cy = item.a.y;
                                sc.sx = item.b.x;
                                sc.sy = item.b.y;
                            }
                            applyStrokeColor(l, sc, item.color);
                            if (sc.lineWidth != item.width) {
                                l.lineWidth = item.width;
                                sc.lineWidth = item.width;
                            }
                            if (fresh || sc.dashed != item.dashed) {
                                l.lineDashPattern = item.dashed ? dashedPattern() : nil;
                                sc.dashed = item.dashed;
                            }
                        } else if constexpr (std::is_same_v<T, BoxItem>) {
                            if (l.hidden) l.hidden = NO;
                            const bool fresh = (sc.kind != ShapeKind::Box);
                            if (fresh) resetVectorLayer(ShapeKind::Box);
                            const float rx = std::min<float>(
                                item.cornerRadius, std::min(item.size.x, item.size.y) * 0.5f);
                            if (fresh || sc.sx != item.size.x || sc.sy != item.size.y || sc.cornerR != rx) {
                                l.bounds = CGRectMake(0, 0, item.size.x, item.size.y);
                                CGPathRef path = CGPathCreateWithRoundedRect(l.bounds, rx, rx, nullptr);
                                l.path = path;
                                CGPathRelease(path);
                                sc.sx = item.size.x;
                                sc.sy = item.size.y;
                                sc.cornerR = rx;
                            }
                            if (fresh || sc.cx != item.center.x || sc.cy != item.center.y) {
                                l.position = CGPointMake(item.center.x, item.center.y);
                                sc.cx = item.center.x;
                                sc.cy = item.center.y;
                            }
                            if (fresh || sc.rot != item.rotation) {
                                l.transform = CATransform3DMakeRotation(item.rotation, 0, 0, 1);
                                sc.rot = item.rotation;
                            }
                            applyFillColor(l, sc, item.fill);
                            applyStrokeColor(l, sc, item.stroke);
                            if (sc.lineWidth != item.strokeWidth) {
                                l.lineWidth = item.strokeWidth;
                                sc.lineWidth = item.strokeWidth;
                            }
                        } else if constexpr (std::is_same_v<T, PolygonItem>) {
                            const std::uint8_t n = std::min<std::uint8_t>(item.count, 8u);
                            if (n >= 3) {
                                if (l.hidden) l.hidden = NO;
                                const bool fresh = (sc.kind != ShapeKind::Polygon);
                                if (fresh) {
                                    resetVectorLayer(ShapeKind::Polygon);
                                    l.lineJoin = kCALineJoinRound;
                                }
                                CGFloat minPX = item.points[0].x, maxPX = item.points[0].x;
                                CGFloat minPY = item.points[0].y, maxPY = item.points[0].y;
                                for (std::uint8_t i = 1; i < n; ++i) {
                                    minPX = std::min<CGFloat>(minPX, item.points[i].x);
                                    maxPX = std::max<CGFloat>(maxPX, item.points[i].x);
                                    minPY = std::min<CGFloat>(minPY, item.points[i].y);
                                    maxPY = std::max<CGFloat>(maxPY, item.points[i].y);
                                }
                                const CGFloat pad = std::max<CGFloat>(4.0, item.strokeWidth * 0.75 + 2.0);
                                const CGFloat ox = std::floor(minPX - pad);
                                const CGFloat oy = std::floor(minPY - pad);
                                const CGRect tightFrame = CGRectMake(
                                    ox, oy, std::max<CGFloat>(4.0, std::ceil(maxPX + pad) - ox),
                                    std::max<CGFloat>(4.0, std::ceil(maxPY + pad) - oy));
                                const bool frameChanged = (fresh || !CGRectEqualToRect(sc.frame, tightFrame));
                                if (frameChanged) {
                                    l.frame = tightFrame;
                                    sc.frame = tightFrame;
                                }
                                bool samePts = (!frameChanged && sc.ptCount == n);
                                if (samePts) {
                                    for (std::uint8_t i = 0; i < n; ++i) {
                                        if (sc.pts[i].x != item.points[i].x ||
                                            sc.pts[i].y != item.points[i].y) {
                                            samePts = false;
                                            break;
                                        }
                                    }
                                }
                                if (!samePts) {
                                    CGMutablePathRef path = CGPathCreateMutable();
                                    CGPathMoveToPoint(path, nullptr, item.points[0].x - ox,
                                                      item.points[0].y - oy);
                                    for (std::uint8_t i = 1; i < n; ++i) {
                                        CGPathAddLineToPoint(path, nullptr, item.points[i].x - ox,
                                                             item.points[i].y - oy);
                                    }
                                    CGPathCloseSubpath(path);
                                    l.path = path;
                                    CGPathRelease(path);
                                    sc.ptCount = n;
                                    for (std::uint8_t i = 0; i < n; ++i) sc.pts[i] = item.points[i];
                                }
                                applyFillColor(l, sc, item.fill);
                                applyStrokeColor(l, sc, item.stroke);
                                if (sc.lineWidth != item.strokeWidth) {
                                    l.lineWidth = item.strokeWidth;
                                    sc.lineWidth = item.strokeWidth;
                                }
                            } else {
                                if (!l.hidden) l.hidden = YES;
                            }
                        }
                    }
                },
                command);
        }
        for (NSUInteger i = si; i < shapes_.count; ++i)
            if (!shapes_[i].hidden) shapes_[i].hidden = YES;
        for (NSUInteger i = ti; i < texts_.count; ++i)
            if (!texts_[i].hidden) texts_[i].hidden = YES;
        blackHole_.endFrame();
        if (metal)
            metalHole_.finish();
        else
            metalHole_.hide();
        if (manageTransaction) {
            [CATransaction commit];
        }
    }
};
}  // namespace dpp::mac
