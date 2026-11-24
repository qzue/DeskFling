#pragma once
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#include "platforms/macos/diagnostics.h"

@interface DPPProbeWindow : NSWindow
@end
@implementation DPPProbeWindow
- (BOOL)canBecomeKeyWindow {
    return NO;
}
- (BOOL)canBecomeMainWindow {
    return NO;
}
@end

namespace dpp::mac {
inline NSArray* windowMetadata() {
    // Metadata only: no image capture and no names/titles retained in the report.
    return CFBridgingRelease(CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID));
}
inline NSDictionary* windowReport() {
    NSScreen* screen = NSScreen.mainScreen;
    if (!screen) return @{@"status" : @"no_gui_display"};
    const pid_t before = NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier;
    const NSRect area = screen.visibleFrame;
    NSRect marker = NSMakeRect(NSMidX(area) - 80, NSMidY(area) - 40, 160, 80);
    DPPProbeWindow* overlay = [[DPPProbeWindow alloc] initWithContentRect:marker
                                                                styleMask:NSWindowStyleMaskBorderless
                                                                  backing:NSBackingStoreBuffered
                                                                    defer:NO];
    overlay.releasedWhenClosed = NO;
    overlay.opaque = NO;
    overlay.backgroundColor = NSColor.clearColor;
    overlay.hasShadow = NO;
    overlay.ignoresMouseEvents = YES;
    overlay.level = CGWindowLevelForKey(kCGDesktopIconWindowLevelKey) + 1;
    overlay.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                                 NSWindowCollectionBehaviorStationary |
                                 NSWindowCollectionBehaviorFullScreenNone;
    overlay.contentView.wantsLayer = YES;
    CALayer* shape = [CALayer layer];
    shape.frame = CGRectMake(12, 12, 30, 30);
    shape.backgroundColor = [NSColor colorWithSRGBRed:.1 green:.6 blue:1 alpha:.6].CGColor;
    [overlay.contentView.layer addSublayer:shape];
    // An owned normal-level witness checks ordering without depending on any user's window title.
    DPPProbeWindow* witness = [[DPPProbeWindow alloc] initWithContentRect:marker
                                                                styleMask:NSWindowStyleMaskBorderless
                                                                  backing:NSBackingStoreBuffered
                                                                    defer:NO];
    witness.releasedWhenClosed = NO;
    witness.backgroundColor = NSColor.darkGrayColor;
    witness.ignoresMouseEvents = YES;
    witness.level = NSNormalWindowLevel;
    [overlay orderFrontRegardless];
    [witness orderFrontRegardless];
    [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:1]];
    NSArray* list = windowMetadata();
    NSInteger overlayIndex = -1, witnessIndex = -1;
    id observedOverlayLevel = NSNull.null, observedWitnessLevel = NSNull.null;
    for (NSUInteger i = 0; i < list.count; ++i) {
        NSDictionary* entry = list[i];
        const NSInteger number = [entry[(__bridge NSString*)kCGWindowNumber] integerValue];
        if (number == overlay.windowNumber) {
            overlayIndex = static_cast<NSInteger>(i);
            observedOverlayLevel = entry[(__bridge NSString*)kCGWindowLayer];
        }
        if (number == witness.windowNumber) {
            witnessIndex = static_cast<NSInteger>(i);
            observedWitnessLevel = entry[(__bridge NSString*)kCGWindowLayer];
        }
    }
    const bool focusUnchanged = before == NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier;
    const bool noKey = !overlay.keyWindow && !witness.keyWindow;
    const NSInteger overlayNumber = overlay.windowNumber, witnessNumber = witness.windowNumber;
    [overlay close];
    [witness close];
    [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.2]];
    bool removed = true;
    for (NSDictionary* entry in windowMetadata()) {
        const NSInteger number = [entry[(__bridge NSString*)kCGWindowNumber] integerValue];
        if (number == overlayNumber || number == witnessNumber) removed = false;
    }
    const bool listed = overlayIndex >= 0 && witnessIndex >= 0;
    return @{
        @"status" : listed && removed && focusUnchanged && noKey && witnessIndex < overlayIndex
            ? @"specified_metadata_checks_passed"
            : @"metadata_check_failed",
        @"scope" : @"1 second, two owned synthetic windows; metadata only; no desktop images",
        @"overlay_listed" : @(overlayIndex >= 0),
        @"normal_witness_listed" : @(witnessIndex >= 0),
        @"overlay_level" : observedOverlayLevel,
        @"normal_witness_level" : observedWitnessLevel,
        @"normal_witness_orders_above_overlay" : @(listed && witnessIndex < overlayIndex),
        @"frontmost_application_unchanged" : @(focusUnchanged),
        @"no_key_window" : @(noKey),
        @"both_closed_and_absent" : @(removed),
        @"ignores_mouse_events_configured" : @YES,
        @"transparent_background_configured" : @YES,
        @"spaces_flags_configured" : @"canJoinAllSpaces | stationary | fullScreenNone",
        @"pixel_composition" : @"not_measured",
        @"actual_input_passthrough" : @"not_exercised",
        @"spaces_mission_control_fullscreen" : @"not_exercised",
        @"dock_menubar_lock_screen_display_changes" : @"not_exercised",
        @"original_icons" : @"untouched; no mirror alignment attempted",
        @"code" : @"platforms/macos/window_probe.h:windowReport"
    };
}
}  // namespace dpp::mac
