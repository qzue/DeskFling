#pragma once
#import <AppKit/AppKit.h>
#include <cmath>
#include <cstdio>
namespace dpp::mac {
struct MirrorDisplay {
    NSUInteger count = 0;
    NSRect frame{}, work{};
    CGFloat scale = 0;
    NSNumber* id = nil;
    double sampledBegin = 0, sampledEnd = 0;
};
inline MirrorDisplay mirrorDisplayAt(NSUInteger screenIndex) {
    const double begin = NSProcessInfo.processInfo.systemUptime;
    NSArray<NSScreen*>* screens = NSScreen.screens;
    NSScreen* screen = (screenIndex < screens.count) ? screens[screenIndex] : screens.firstObject;
    return {screens.count, screen ? screen.frame : NSZeroRect, screen ? screen.visibleFrame : NSZeroRect,
            screen ? screen.backingScaleFactor : 0.0,
            screen ? screen.deviceDescription[@"NSScreenNumber"] : nil, begin,
            NSProcessInfo.processInfo.systemUptime};
}
inline MirrorDisplay mirrorDisplay() { return mirrorDisplayAt(0); }
inline bool validMirrorDisplayAnyCount(const MirrorDisplay& d) {
    const auto valid = [](NSRect r) {
        return std::isfinite(r.origin.x) && std::isfinite(r.origin.y) && std::isfinite(r.size.width) &&
               std::isfinite(r.size.height) && r.size.width > 0 && r.size.height > 0;
    };
    return d.count >= 1 && d.id && valid(d.frame) && valid(d.work) && std::isfinite(d.scale) && d.scale > 0 &&
           NSContainsRect(d.frame, d.work);
}
inline bool validMirrorDisplay(const MirrorDisplay& d) {
    return d.count == 1 && validMirrorDisplayAnyCount(d);
}
inline bool samePhysicalDisplay(const MirrorDisplay& a, const MirrorDisplay& b) {
    return validMirrorDisplay(a) && validMirrorDisplay(b) && [a.id isEqual:b.id] &&
           NSEqualRects(a.frame, b.frame) && a.scale == b.scale;
}
inline bool mirrorDisplayCompatible(const MirrorDisplay& a, const MirrorDisplay& b, bool allowWorkChange) {
    // Keep the startup scene immutable. An expanded work area contains every
    // existing draw/input/collision point; a contraction has no such proof.
    const bool retainedSceneFits = allowWorkChange && NSContainsRect(b.work, a.work);
    return samePhysicalDisplay(a, b) && (NSEqualRects(a.work, b.work) || retainedSceneFits);
}
inline void logMirrorDisplay(const MirrorDisplay& a, const MirrorDisplay& b, bool allowed) {
    std::printf("mirror_display count=%lu->%lu identity_same=%s frame_same=%s scale=%.3f->%.3f "
                "work_before=%.1f,%.1f,%.1f,%.1f work_after=%.1f,%.1f,%.1f,%.1f compatible=%s\n",
                a.count, b.count, [a.id isEqual:b.id] ? "true" : "false",
                NSEqualRects(a.frame, b.frame) ? "true" : "false", a.scale, b.scale, a.work.origin.x,
                a.work.origin.y, a.work.size.width, a.work.size.height, b.work.origin.x, b.work.origin.y,
                b.work.size.width, b.work.size.height, allowed ? "true" : "false");
    std::fflush(stdout);
}
}  // namespace dpp::mac

namespace dpp::mac {
// Bounded, main-thread event diagnostics; no per-frame I/O or desktop contents.
inline void logDisplayDecision(const MirrorDisplay& baseline, const MirrorDisplay& current,
                               NSWindow* window, NSDictionary* context, bool compatible) {
    static unsigned emitted = 0, exits = 0;
    if (compatible ? emitted++ >= 128 : exits++ >= 16) return;
    const double begin = NSProcessInfo.processInfo.systemUptime;
    auto rect = [](NSRect r) { return @[ @(r.origin.x), @(r.origin.y), @(r.size.width), @(r.size.height) ]; };
    auto display = [&](const MirrorDisplay& d) {
        return @{ @"count": @(d.count), @"id": d.id ? d.id : (id)NSNull.null,
                  @"sample_begin_uptime": @(d.sampledBegin), @"sample_end_uptime": @(d.sampledEnd),
                  @"frame": rect(d.frame), @"visibleFrame": rect(d.work), @"scale": @(d.scale) };
    };
    NSMutableDictionary* event = [context mutableCopy];
    event[@"sample_begin_uptime"] = @(begin);
    event[@"main_thread"] = @(NSThread.isMainThread);
    event[@"baseline"] = display(baseline);
    event[@"current"] = display(current);
    event[@"work_delta"] = rect(NSMakeRect(current.work.origin.x-baseline.work.origin.x,
        current.work.origin.y-baseline.work.origin.y, current.work.size.width-baseline.work.size.width,
        current.work.size.height-baseline.work.size.height));
    event[@"window_frame"] = rect(window.frame);
    event[@"content_bounds"] = rect(window.contentView.bounds);
    event[@"content_backing"] = rect([window.contentView convertRectToBacking:window.contentView.bounds]);
    event[@"window_scale"] = @(window.backingScaleFactor);
    event[@"window_screen_id"] = window.screen.deviceDescription[@"NSScreenNumber"] ? window.screen.deviceDescription[@"NSScreenNumber"] : (id)NSNull.null;
    event[@"app_active"] = @(NSApp.active);
    event[@"activation_policy"] = @(NSApp.activationPolicy);
    event[@"window_key"] = @(window.keyWindow);
    event[@"window_level"] = @(window.level);
    event[@"ignores_mouse"] = @(window.ignoresMouseEvents);
    event[@"decision"] = compatible ? @"continue_fixed_scene" : @"restore_incompatible_or_unknown";
    event[@"sample_end_uptime"] = @(NSProcessInfo.processInfo.systemUptime);
    NSData* data = [NSJSONSerialization dataWithJSONObject:event options:NSJSONWritingSortedKeys error:nil];
    if (data) std::printf("display_decision %s\n", [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding].UTF8String);
    std::fflush(stdout);
}
}
