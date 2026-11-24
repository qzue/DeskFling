#pragma once
// Cinematic toy black hole for the Core Animation renderers (UI thread only).
//
// All imagery (accretion-disk rings, lensed arcs, shadow/photon ring, glow) is
// rasterised once per backing scale at a fixed reference radius and cached as
// CGImages. Each frame only updates layer transforms/opacity: the disk rings
// rotate (inner ring twice as fast: integer multiples keep the wrapped phase
// continuous) and are squashed into an almost edge-on ellipse.
//
// Layering in y-down scene space (root layers are geometry-flipped):
//   Back : glow, lensed upper arch, lensed lower arc, far half of the disk,
//          black shadow + photon ring
//   (swallowing icon copies)
//   Front: near half of the disk, which crosses in front of the shadow.
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "core/render/draw_list.h"

namespace dpp::mac {

class BlackHoleVisual {
   public:
    // Geometry in units of the shadow radius R.
    static constexpr CGFloat kRef = 64;  // reference radius (points) for cached images
    static constexpr CGFloat kShadow = 1.04, kPhoton = .86;
    static constexpr CGFloat kArchIn = 1.04, kArchOut = 2.05, kArchSX = 1.12;
    static constexpr CGFloat kLowIn = 1.0, kLowOut = 1.42, kLowSX = 1.08, kLowSY = .98;
    static constexpr CGFloat kDiskInA = 1.45, kDiskOutA = 2.6, kDiskInB = 2.5, kDiskOutB = 4.4;
    static constexpr CGFloat kTilt = .14;  // vertical squash of the disk plane
    static constexpr CGFloat kGlow = 5.0, kGlowSY = .42;

    BlackHoleVisual() = default;
    BlackHoleVisual(const BlackHoleVisual&) = delete;
    BlackHoleVisual& operator=(const BlackHoleVisual&) = delete;
    ~BlackHoleVisual() { releaseImages(); }

    void beginFrame() { seenBack_ = seenFront_ = false; }
    void endFrame() {
        if (back_) back_.hidden = !seenBack_;
        if (front_) front_.hidden = !seenFront_;
    }
    void update(const BlackHoleItem& item, CALayer* root, CGFloat scale, CGFloat z) {
        if (!std::isfinite(item.radius) || item.radius <= 0 || !item.center.isFinite()) return;
        ensure(root, std::max<CGFloat>(1, scale));
        const bool isBack = item.layer == BlackHoleItem::Layer::Back;
        CALayer* layer = isBack ? back_ : front_;
        (isBack ? seenBack_ : seenFront_) = true;
        layer.hidden = NO;
        layer.zPosition = z;
        layer.position = CGPointMake(item.center.x, item.center.y);
        const CGFloat s = item.radius / kRef;
        const CGFloat tilt = std::isfinite(item.tilt) ? item.tilt : 0;
        const CGFloat el = std::isfinite(item.elevation) ? item.elevation : .056;
        const CGFloat diskSY = std::max<CGFloat>(kTilt, std::abs(std::sin(el)));
        layer.transform = CATransform3DRotate(CATransform3DMakeScale(s, el < 0 ? -s : s, 1), tilt, 0, 0, 1);
        const CGFloat phase = std::isfinite(item.phase) ? item.phase : 0;
        if (isBack) {
            glow_.opacity = static_cast<float>(.82 + .18 * std::sin(phase));
            spin(archRing_, phase, kArchSX, 1);
            spin(lowRing_, -2 * phase, kLowSX, kLowSY);
            spin(farInner_, 2 * phase, 1, diskSY);
            spin(farOuter_, phase, 1, diskSY);
        } else {
            spin(nearInner_, 2 * phase, 1, diskSY);
            spin(nearOuter_, phase, 1, diskSY);
        }
    }
    CALayer* backLayer() const { return back_; }  // tests/diagnostics only

   private:
    CALayer *back_ = nil, *front_ = nil, *glow_ = nil, *shadow_ = nil;
    CALayer *archRing_ = nil, *lowRing_ = nil;
    CALayer *farInner_ = nil, *farOuter_ = nil, *nearInner_ = nil, *nearOuter_ = nil;
    CGImageRef glowImage_ = nullptr, shadowImage_ = nullptr, archImage_ = nullptr, lowImage_ = nullptr;
    CGImageRef diskAImage_ = nullptr, diskBImage_ = nullptr;
    CGFloat builtScale_ = 0;
    bool seenBack_ = false, seenFront_ = false;

    static void spin(CALayer* l, CGFloat angle, CGFloat sx, CGFloat sy) {
        l.transform =
            CATransform3DConcat(CATransform3DMakeRotation(angle, 0, 0, 1), CATransform3DMakeScale(sx, sy, 1));
    }
    void releaseImages() {
        for (CGImageRef* image :
             {&glowImage_, &shadowImage_, &archImage_, &lowImage_, &diskAImage_, &diskBImage_}) {
            if (*image) CGImageRelease(*image);
            *image = nullptr;
        }
    }

    // ---- procedural imagery (deterministic; only on scale change) ----
    struct Rng {
        std::uint32_t state;
        CGFloat next() {
            state = state * 1664525u + 1013904223u;
            return static_cast<CGFloat>(state >> 8) / static_cast<CGFloat>(1u << 24);
        }
    };
    struct Rgb {
        CGFloat r, g, b;
    };
    // Inner (t=0) white-hot to outer (t=1) deep red, like the reference render.
    static Rgb palette(CGFloat t) {
        static constexpr CGFloat stops[][4] = {{0, 1.00, .88, .66},
                                               {.12, 1.00, .66, .30},
                                               {.35, 1.00, .44, .10},
                                               {.70, .80, .19, .03},
                                               {1, .40, .06, .02}};
        t = std::clamp<CGFloat>(t, 0, 1);
        for (int i = 1; i < 5; ++i)
            if (t <= stops[i][0]) {
                const CGFloat u = (t - stops[i - 1][0]) / (stops[i][0] - stops[i - 1][0]);
                return {stops[i - 1][1] + (stops[i][1] - stops[i - 1][1]) * u,
                        stops[i - 1][2] + (stops[i][2] - stops[i - 1][2]) * u,
                        stops[i - 1][3] + (stops[i][3] - stops[i - 1][3]) * u};
            }
        return {stops[4][1], stops[4][2], stops[4][3]};
    }
    static CGContextRef bitmap(CGFloat radiusPoints, CGFloat px, size_t* side) {
        *side = static_cast<size_t>(std::ceil(radiusPoints * 2 * px)) + 4;
        CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef ctx = CGBitmapContextCreate(nullptr, *side, *side, 8, 0, space,
                                                 static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast));
        CGColorSpaceRelease(space);
        if (!ctx) return nullptr;
        CGContextTranslateCTM(ctx, *side / 2.0, *side / 2.0);
        CGContextScaleCTM(ctx, px, px);
        CGContextSetLineCap(ctx, kCGLineCapRound);
        return ctx;
    }
    static CGImageRef finish(CGContextRef ctx) {
        if (!ctx) return nullptr;
        CGImageRef image = CGBitmapContextCreateImage(ctx);
        CGContextRelease(ctx);
        return image;
    }
    static void radial(CGContextRef ctx, CGFloat r0, CGFloat r1, const CGFloat* locations,
                       const CGFloat* rgba, size_t count) {
        CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGGradientRef g = CGGradientCreateWithColorComponents(space, rgba, locations, count);
        CGContextDrawRadialGradient(ctx, g, CGPointZero, r0, CGPointZero, r1, 0);
        CGGradientRelease(g);
        CGColorSpaceRelease(space);
    }
    // Filamentary annulus: a hot base gradient, many additive arc filaments and
    // a few subtractive gaps; a bright hot inner edge.
    static CGImageRef ring(CGFloat r0, CGFloat r1, CGFloat px, std::uint32_t seed, int filaments,
                           CGFloat heat, CGFloat gain) {
        size_t side = 0;
        CGContextRef ctx = bitmap(r1, px, &side);
        if (!ctx) return nullptr;
        const auto at = [&](CGFloat t) { return palette(heat + (1 - heat) * t); };
        CGFloat locations[5] = {0, .05, .3, .7, 1};
        CGFloat rgba[20];
        const CGFloat alpha[5] = {0, .92, .72, .34, 0};
        for (int i = 0; i < 5; ++i) {
            const Rgb c = at(locations[i]);
            const CGFloat a = alpha[i] * gain;
            rgba[i * 4] = c.r, rgba[i * 4 + 1] = c.g, rgba[i * 4 + 2] = c.b, rgba[i * 4 + 3] = a;
        }
        radial(ctx, r0, r1, locations, rgba, 5);
        Rng rng{seed};
        // Soft, wide streaks first (glow), then fine filaments on top.
        for (int i = 0; i < filaments / 3; ++i) {
            const CGFloat t = std::pow(rng.next(), 1.1), r = r0 + (r1 - r0) * t;
            const Rgb c = at(t);
            const CGFloat start = rng.next() * 2 * M_PI, sweep = .8 + rng.next() * 4.5;
            CGContextSetRGBStrokeColor(ctx, c.r, c.g, c.b, (.10 + .18 * rng.next()) * gain);
            CGContextSetLineWidth(ctx, 2.5 + rng.next() * 6);
            CGContextAddArc(ctx, 0, 0, r, start, start + sweep, 0);
            CGContextStrokePath(ctx);
        }
        CGContextSetBlendMode(ctx, kCGBlendModeDestinationOut);
        for (int i = 0; i < filaments / 5; ++i) {
            const CGFloat r = r0 + (r1 - r0) * (.08 + .92 * rng.next());
            const CGFloat start = rng.next() * 2 * M_PI, sweep = .5 + rng.next() * 3.5;
            CGContextSetRGBStrokeColor(ctx, 0, 0, 0, .15 + .3 * rng.next());
            CGContextSetLineWidth(ctx, .5 + rng.next() * 1.8);
            CGContextAddArc(ctx, 0, 0, r, start, start + sweep, 0);
            CGContextStrokePath(ctx);
        }
        CGContextSetBlendMode(ctx, kCGBlendModeNormal);
        for (int i = 0; i < filaments; ++i) {
            const CGFloat t = std::pow(rng.next(), 1.15), r = r0 + (r1 - r0) * t;
            const Rgb c = at(std::max<CGFloat>(0, t - .1 * rng.next()));
            const CGFloat a = (.18 + .5 * rng.next()) * std::pow(1 - t, .5) * gain;
            const CGFloat start = rng.next() * 2 * M_PI, sweep = .35 + rng.next() * 4.6;
            CGContextSetRGBStrokeColor(ctx, c.r, c.g, c.b, std::min<CGFloat>(1, a));
            CGContextSetLineWidth(ctx, .35 + rng.next() * 1.3);
            CGContextAddArc(ctx, 0, 0, r, start, start + sweep, 0);
            CGContextStrokePath(ctx);
        }
        CGContextSetBlendMode(ctx, kCGBlendModePlusLighter);
        const Rgb hot = at(0);
        CGContextSetRGBStrokeColor(ctx, hot.r, hot.g, hot.b, .35 * gain);
        CGContextSetLineWidth(ctx, 2.2);
        CGContextAddArc(ctx, 0, 0, r0 + 2, 0, 2 * M_PI, 0);
        CGContextStrokePath(ctx);
        return finish(ctx);
    }
    static CGImageRef glow(CGFloat r, CGFloat px) {
        size_t side = 0;
        CGContextRef ctx = bitmap(r, px, &side);
        if (!ctx) return nullptr;
        const CGFloat locations[4] = {0, .25, .55, 1};
        const CGFloat rgba[16] = {1, .55, .2, .34, 1, .42, .1, .2, .7, .18, .04, .08, .3, .05, .01, 0};
        radial(ctx, 0, r, locations, rgba, 4);
        return finish(ctx);
    }
    static CGImageRef shadow(CGFloat R, CGFloat px) {
        const CGFloat outer = R * kShadow;
        size_t side = 0;
        CGContextRef ctx = bitmap(outer + 6, px, &side);
        if (!ctx) return nullptr;
        const CGFloat locations[3] = {0, .93, 1};
        const CGFloat rgba[12] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0};
        radial(ctx, 0, outer + 3, locations, rgba, 3);
        // Photon ring: a thin bright line with a faint halo.
        CGContextSetBlendMode(ctx, kCGBlendModePlusLighter);
        CGContextSetRGBStrokeColor(ctx, 1, .45, .12, .22);
        CGContextSetLineWidth(ctx, 4);
        CGContextAddArc(ctx, 0, 0, R * kPhoton, 0, 2 * M_PI, 0);
        CGContextStrokePath(ctx);
        CGContextSetRGBStrokeColor(ctx, 1, .78, .45, .95);
        CGContextSetLineWidth(ctx, 1.1);
        CGContextAddArc(ctx, 0, 0, R * kPhoton, 0, 2 * M_PI, 0);
        CGContextStrokePath(ctx);
        return finish(ctx);
    }

    // ---- layer tree ----
    static CALayer* group() {
        CALayer* l = [CALayer layer];
        l.bounds = CGRectZero;
        l.anchorPoint = CGPointMake(.5, .5);
        return l;
    }
    static CALayer* imageLayer(CGImageRef image, CGFloat px, CALayer* parent, CGPoint position) {
        CALayer* l = [CALayer layer];
        l.contents = (__bridge id)image;
        l.contentsScale = px;
        l.bounds =
            image ? CGRectMake(0, 0, CGImageGetWidth(image) / px, CGImageGetHeight(image) / px) : CGRectZero;
        l.position = position;
        l.minificationFilter = kCAFilterTrilinear;
        [parent addSublayer:l];
        return l;
    }
    // Rectangular clip in hole-centred coordinates; ring layers sit at its local centre.
    static CALayer* clip(CALayer* parent, CGRect rect, bool doppler) {
        CALayer* l = [CALayer layer];
        l.anchorPoint = CGPointZero;
        l.bounds = CGRectMake(0, 0, rect.size.width, rect.size.height);
        l.position = rect.origin;
        l.masksToBounds = YES;
        if (doppler) {
            // Approaching (left) side of the disk is brighter.
            CAGradientLayer* mask = [CAGradientLayer layer];
            mask.frame = l.bounds;
            mask.startPoint = CGPointMake(0, .5);
            mask.endPoint = CGPointMake(1, .5);
            mask.colors = @[
                (__bridge id)[NSColor colorWithWhite:0 alpha:1].CGColor,
                (__bridge id)[NSColor colorWithWhite:0 alpha:.9].CGColor,
                (__bridge id)[NSColor colorWithWhite:0 alpha:.6].CGColor
            ];
            l.mask = mask;
        }
        [parent addSublayer:l];
        return l;
    }
    void ensure(CALayer* root, CGFloat px) {
        if (back_ && builtScale_ == px) {
            if (back_.superlayer != root) [root addSublayer:back_];
            if (front_.superlayer != root) [root addSublayer:front_];
            return;
        }
        [back_ removeFromSuperlayer];
        [front_ removeFromSuperlayer];
        releaseImages();
        const CGFloat R = kRef;
        glowImage_ = glow(R * kGlow, std::min<CGFloat>(px, 1));  // smooth gradient: low resolution suffices
        shadowImage_ = shadow(R, px);
        archImage_ = ring(R * kArchIn, R * kArchOut, px, 0x5eed01u, 320, 0, 1.0);
        lowImage_ = ring(R * kLowIn, R * kLowOut, px, 0x5eed02u, 160, .05, .85);
        diskAImage_ = ring(R * kDiskInA, R * kDiskOutA, px, 0x5eed03u, 420, 0, 1.0);
        diskBImage_ = ring(R * kDiskInB, R * kDiskOutB, px, 0x5eed04u, 520, .25, .9);
        builtScale_ = px;

        back_ = group();
        front_ = group();
        glow_ = imageLayer(glowImage_, std::min<CGFloat>(px, 1), back_, CGPointZero);
        glow_.transform = CATransform3DMakeScale(1, kGlowSY, 1);
        const CGFloat archW = R * kArchOut * kArchSX + 2;
        CALayer* arch =
            clip(back_, CGRectMake(-archW, -R * kArchOut - 2, archW * 2, R * kArchOut + 2), false);
        archRing_ = imageLayer(archImage_, px, arch, CGPointMake(archW, R * kArchOut + 2));
        const CGFloat lowW = R * kLowOut * kLowSX + 2;
        CALayer* low = clip(back_, CGRectMake(-lowW, 0, lowW * 2, R * kLowOut + 2), false);
        low.opacity = .9f;
        lowRing_ = imageLayer(lowImage_, px, low, CGPointMake(lowW, 0));
        const CGFloat diskW = R * kDiskOutB + 2, diskH = R * kDiskOutB * kTilt + 2;
        CALayer* far = clip(back_, CGRectMake(-diskW, -diskH, diskW * 2, diskH), true);
        farOuter_ = imageLayer(diskBImage_, px, far, CGPointMake(diskW, diskH));
        farInner_ = imageLayer(diskAImage_, px, far, CGPointMake(diskW, diskH));
        shadow_ = imageLayer(shadowImage_, px, back_, CGPointZero);
        CALayer* near = clip(front_, CGRectMake(-diskW, 0, diskW * 2, diskH), true);
        nearOuter_ = imageLayer(diskBImage_, px, near, CGPointMake(diskW, 0));
        nearInner_ = imageLayer(diskAImage_, px, near, CGPointMake(diskW, 0));
        [root addSublayer:back_];
        [root addSublayer:front_];
    }
};

}  // namespace dpp::mac
