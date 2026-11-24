// Opt-in GUI integration fixture. No Desktop reads, Apple Events, permissions or screenshots.
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <cstdlib>
#include "platforms/macos/desktop_access.h"
#include "platforms/macos/diagnostics.h"
#include "platforms/macos/mirror_preview.h"
using namespace dpp;
using namespace dpp::mac;
static NSString* outputPath;
static int resultCode = 3;
static NSString* fixtureCase = @"full";
@interface DPPDesktopAccess (RetirementTests)
- (void)prepareAssisted;
- (void)returnAssisted;
- (void)action:(NSButton*)button;
- (NSDictionary*)report;
- (void)showHistory;
- (void)confirmManualRestore;
@end
@interface MirrorSmokeDelegate : NSObject <NSApplicationDelegate>
@property(strong) DPPMirrorPreview* preview;
@property(strong) NSPanel* panel;
@property(strong) NSMutableArray* checks;
@property(strong) DPPMirrorPreview* observedPreview;
@property NSUInteger observedOpens;
@property BOOL blocked;
@property(strong) NSMutableArray* menuEvents;
@end
@implementation MirrorSmokeDelegate
- (void)check:(BOOL)ok name:(NSString*)name {
    DPPMirrorPreview* preview = self.observedPreview ? self.observedPreview : self.preview;
    NSDictionary* state = [preview diagnostics];
    NSWindow* window = [preview valueForKey:@"window"];
    NSTimer* deadline = [preview valueForKey:@"deadline"];
    [self.checks addObject:@{
        @"check" : name,
        @"passed" : @(ok),
        @"app_active" : @(NSApp.active),
        @"key_window" : @(NSApp.keyWindow.windowNumber),
        @"main_window" : @(NSApp.mainWindow.windowNumber),
        @"preview_window" : @(window.windowNumber),
        @"preview_key" : @(window.keyWindow),
        @"ignores_mouse" : @(window.ignoresMouseEvents),
        @"window_level" : @(window.level),
        @"menu_active" : @(preview.menuActive),
        @"menu_timed_out" : @(preview.fixtureMenuTimedOut),
        @"opens" : @(self.observedOpens),
        @"deadline" : deadline ? @([deadline.fireDate timeIntervalSince1970]) : (id)NSNull.null,
        @"state" : state ? state : @{}
    }];
}
// Pump AppKit events on the main thread; timeout is a failure, never readiness.
- (BOOL)waitUntil:(BOOL (^)(void))ready name:(NSString*)name {
    const double end = NSProcessInfo.processInfo.systemUptime + ([fixtureCase isEqual:@"migration"] ? 4 : 3);
    while (!ready() && NSProcessInfo.processInfo.systemUptime < end) {
        NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                            untilDate:[NSDate dateWithTimeIntervalSinceNow:.01]
                                               inMode:NSDefaultRunLoopMode
                                              dequeue:YES];
        if (event) [NSApp sendEvent:event];
    }
    if (ready()) return YES;
    [self check:NO name:[@"timeout_" stringByAppendingString:name]];
    return NO;
}
- (void)writeResult:(NSDictionary*)extra {
    BOOL passed = !self.blocked;
    for (NSDictionary* check in self.checks) passed = passed && [check[@"passed"] boolValue];
    NSMutableDictionary* result = [extra mutableCopy];
    result[@"kind"] = @"opt_in_native_fixture_not_real_desktop_acceptance";
    result[@"case"] = fixtureCase;
    result[@"checks"] = self.checks;
    result[@"menu_events"] = self.menuEvents ? self.menuEvents : @[];
    result[@"passed"] = @(passed);
    result[@"status"] = self.blocked ? @"blocked" : (passed ? @"passed" : @"failed");
    result[@"os"] = NSProcessInfo.processInfo.operatingSystemVersionString;
    result[@"config"] = @DPP_BUILD_CONFIG;
    result[@"pid"] = @(NSProcessInfo.processInfo.processIdentifier);
    resultCode = writeJSON(result, outputPath) && passed ? 0 : 3;
    [self.observedPreview close:@"fixture_cleanup"];
    [self.preview close:@"fixture_cleanup"];
    [self.panel close];
    std::exit(resultCode);
}
- (MirrorSnapshot)fixture {
    NSRect f = NSScreen.screens.firstObject.frame, w = NSScreen.screens.firstObject.visibleFrame;
    MirrorSnapshot s;
    s.screen = {static_cast<float>(f.origin.x), static_cast<float>(f.origin.y),
                static_cast<float>(f.size.width), static_cast<float>(f.size.height)};
    s.workArea = {static_cast<float>(w.origin.x), static_cast<float>(w.origin.y),
                  static_cast<float>(w.size.width), static_cast<float>(w.size.height)};
    s.singleScreen = NSScreen.screens.count == 1;
    s.capturedAt = NSProcessInfo.processInfo.systemUptime;
    for (uint32_t i = 0; i < 3; ++i) {
        MirrorItem item;
        item.id = IconId{i + 17};
        item.texture = TextureId{i + 1};
        item.sceneAnchor = {150.0f + static_cast<float>(i) * 150, 120.0f + static_cast<float>(i) * 50};
        auto global = macSceneToGlobalPoints(item.sceneAnchor, s.workArea);
        item.rawDpos = macGlobalPointsToScene(global, s.screen);
        item.coordinateValidated = true;
        item.syntheticFixture = true;
        s.items.push_back(item);
    }
    return s;
}
- (NSArray*)images {
    NSImage* image = [NSImage imageWithSystemSymbolName:@"star.fill" accessibilityDescription:nil];
    NSRect r = NSMakeRect(0, 0, 48, 48);
    CGImageRef cg = [image CGImageForProposedRect:&r context:nil hints:nil];
    return cg ? @[ (__bridge id)cg, (__bridge id)cg, (__bridge id)cg ] : @[];
}
- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    (void)notification;
    @try {
        try {
            self.checks = [NSMutableArray array];
            [NSUserDefaults.standardUserDefaults removeObjectForKey:@"DPPMirrorFixtureSessionsJSONV1"];
            self.preview = [DPPMirrorPreview new];
            self.panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(500, 500, 260, 100)
                                                    styleMask:NSWindowStyleMaskTitled
                                                      backing:NSBackingStoreBuffered
                                                        defer:NO];
            self.panel.title = @"DPP mirror fixture — no Desktop reads";
            self.panel.releasedWhenClosed = NO;
            self.panel.level = NSFloatingWindowLevel;
            [self.panel makeKeyAndOrderFront:nil];
            [NSApp activateIgnoringOtherApps:YES];
            [self performSelector:@selector(beginExercises) withObject:nil afterDelay:0];
        } catch (...) {
            [NSApp terminate:nil];
        }
    } @catch (NSException*) {
        [NSApp terminate:nil];
    }
}
- (void)beginExercises {
    if (![self
            waitUntil:^BOOL {
              return NSApp.active && self.panel.keyWindow && NSScreen.screens.count == 1;
            }
                 name:@"graphical_foreground_precondition"]) {
        self.blocked = YES;
        [self writeResult:@{}];
        return;
    }
    if ([fixtureCase isEqual:@"direct"]) {
        auto scene = [self fixture];
        for (auto& item : scene.items) item.syntheticFixture = false;
        [self.preview start:scene images:[self images]];
        self.preview.allowsBackgroundSession = YES;
        self.preview.migrationProtectionValid = ^BOOL {
          return YES;
        };
        self.preview.canOpenItem = ^BOOL(uint32_t item) {
          return item == 17;
        };
        __block NSUInteger opens = 0;
        self.preview.openItem = ^(uint32_t item) {
          if (item == 17) ++opens;
        };
        self.preview.opensWithoutEndingSession = YES;
        [self.preview presentPrepared];
        NSWindow* w = [self.preview valueForKey:@"window"];
        NSDate* deadline = [[self.preview valueForKey:@"deadline"] fireDate];
        NSString* session = [self.preview valueForKey:@"sessionID"];
        [self check:self.preview.desktopReady && !w.ignoresMouseEvents && w.level < NSNormalWindowLevel
               name:@"direct_armed_at_desktop_level"];
        [self.preview fixturePointer:NSEventTypeLeftMouseDown];
        [self check:[[self.preview diagnostics][@"grabbing"] boolValue]
               name:@"first_down_without_button_grabs"];
        NSEvent* first = [self.preview valueForKey:@"lastDown"];
        NSUInteger inputs = [[self.preview valueForKey:@"inputs"] unsignedIntegerValue];
        [w.contentView mouseDown:first];
        [self check:[[self.preview valueForKey:@"inputs"] unsignedIntegerValue] == inputs
               name:@"same_down_not_dispatched_twice"];
        [self.preview fixturePointer:NSEventTypeLeftMouseUp];
        [self check:![[self.preview diagnostics][@"grabbing"] boolValue] && opens == 0
               name:@"release_does_not_open"];
        [self.preview fixtureContextMenu:YES];
        [self
            waitUntil:^BOOL {
              return opens == 1;
            }
                 name:@"first_right_click_opens_once"];
        [self check:opens == 1 && !self.preview.fixtureMenuTimedOut name:@"first_right_menu_single_open"];
        [self.preview completeOpenRequest];
        [self.panel makeKeyAndOrderFront:nil];
        [self check:self.preview.desktopReady name:@"focus_release_rearms"];
        [self.preview fixturePointer:NSEventTypeLeftMouseDown];
        [self check:[[self.preview diagnostics][@"grabbing"] boolValue] name:@"return_down_grabs"];
        [self.preview disableInteraction];
        [self.preview fixturePointer:NSEventTypeLeftMouseDown];
        [self check:w.ignoresMouseEvents && ![[self.preview diagnostics][@"grabbing"] boolValue]
               name:@"explicit_disable_blocks_down"];
        [self.preview toggleInteraction];
        [self.preview playPause];
        [self.preview fixturePointer:NSEventTypeLeftMouseDown];
        [self check:[[self.preview diagnostics][@"paused"] boolValue] &&
                    ![[self.preview diagnostics][@"grabbing"] boolValue]
               name:@"user_pause_not_resumed"];
        [self.preview disableInteraction];
        [self.preview toggleInteraction];
        [self.preview fixturePointer:NSEventTypeLeftMouseDown];
        [self check:self.preview.desktopReady && [[self.preview diagnostics][@"paused"] boolValue] &&
                    ![[self.preview diagnostics][@"grabbing"] boolValue]
               name:@"manual_reenable_preserves_user_pause"];
        NSEvent* blank = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown
                                            location:NSMakePoint(2, 2)
                                       modifierFlags:0
                                           timestamp:NSProcessInfo.processInfo.systemUptime
                                        windowNumber:w.windowNumber
                                             context:nil
                                         eventNumber:991
                                          clickCount:1
                                            pressure:1];
        [self.preview playPause];
        [w.contentView mouseDown:blank];
        [self check:![[self.preview diagnostics][@"grabbing"] boolValue] name:@"blank_no_grab"];
        NSRect smaller = w.frame;
        smaller.size.width = std::floor(smaller.size.width * .9);
        [self.preview fixtureMigrationWork:smaller scale:w.backingScaleFactor];
        [self.preview fixturePointer:NSEventTypeLeftMouseDown];
        [self check:![[self.preview diagnostics][@"grabbing"] boolValue] name:@"migration_blocks_down"];
        [self
            waitUntil:^BOOL {
              return !self.preview.migrating;
            }
                 name:@"direct_migration_commit"];
        NSEvent* stale = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown
                                            location:first.locationInWindow
                                       modifierFlags:0
                                           timestamp:first.timestamp
                                        windowNumber:w.windowNumber
                                             context:nil
                                         eventNumber:992
                                          clickCount:1
                                            pressure:1];
        [w.contentView mouseDown:stale];
        [self check:self.preview.active && ![[self.preview diagnostics][@"grabbing"] boolValue]
               name:@"old_generation_down_rejected"];
        [self check:[deadline isEqual:[[self.preview valueForKey:@"deadline"] fireDate]] &&
                    [session isEqual:[self.preview valueForKey:@"sessionID"]]
               name:@"session_deadline_unchanged"];
        [self writeResult:@{}];
    } else if ([fixtureCase isEqual:@"billiards"]) {
        [self exerciseBilliards];
        [self writeResult:@{}];
    } else if ([fixtureCase isEqual:@"migration"]) {
        [self exerciseMigration];
        [self writeResult:@{}];
    } else if ([fixtureCase isEqual:@"menus"]) {
        [self exerciseMenus];
        [self writeResult:@{}];
    } else if ([fixtureCase isEqual:@"focus"]) {
        [self.preview start:[self fixture] images:[self images]];
        [self exerciseFocus];
        [self writeResult:@{}];
    } else {
        [self exercise];
    }
}
- (void)exerciseBilliards {
    auto scene = [self fixture];
    for (auto& item : scene.items) item.syntheticFixture = false;
    [self check:[self.preview start:scene images:[self images]] name:@"billiards_fixture_start"];
    self.preview.allowsBackgroundSession = YES;
    self.preview.migrationProtectionValid = ^BOOL {
      return YES;
    };
    self.preview.showsFilenames = NO;
    [self.preview presentPrepared];
    [self check:[self.preview canEnableBilliards] && !self.preview.billiardsEnabled
           name:@"billiards_initially_disabled_and_eligible"];
    [self.preview setBilliards:YES];
    [self check:self.preview.billiardsEnabled && ![[self.preview diagnostics][@"paused"] boolValue]
           name:@"billiards_enabled_and_unpaused"];

    NSWindow* w = [self.preview valueForKey:@"window"];
    NSView* v = w.contentView;
    NSDate* deadline = [[self.preview valueForKey:@"deadline"] fireDate];

    // Margin click outside table does not shoot or grab.
    NSEvent* marginDown = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown
                                             location:[v convertPoint:NSMakePoint(4, 4) toView:nil]
                                        modifierFlags:0
                                            timestamp:NSProcessInfo.processInfo.systemUptime
                                         windowNumber:w.windowNumber
                                              context:nil
                                          eventNumber:1201
                                           clickCount:1
                                             pressure:1];
    [v mouseDown:marginDown];
    [self check:[[self.preview diagnostics][@"billiards_shots"] intValue] == 0
           name:@"billiards_margin_click_ignored"];

    // Aim, press inside table, pull back 120pt, release -> 1 shot recorded.
    const NSPoint tablePt = NSMakePoint(w.frame.size.width * 0.55, w.frame.size.height * 0.50);
    const NSPoint pullPt = NSMakePoint(tablePt.x - 120.0, tablePt.y);
    [v mouseMoved:[NSEvent mouseEventWithType:NSEventTypeMouseMoved
                                     location:[v convertPoint:tablePt toView:nil]
                                modifierFlags:0
                                    timestamp:NSProcessInfo.processInfo.systemUptime
                                 windowNumber:w.windowNumber
                                      context:nil
                                  eventNumber:1202
                                   clickCount:0
                                     pressure:0]];
    [v mouseDown:[NSEvent mouseEventWithType:NSEventTypeLeftMouseDown
                                    location:[v convertPoint:tablePt toView:nil]
                               modifierFlags:0
                                   timestamp:NSProcessInfo.processInfo.systemUptime
                                windowNumber:w.windowNumber
                                     context:nil
                                 eventNumber:1203
                                  clickCount:1
                                    pressure:1]];
    [v mouseDragged:[NSEvent mouseEventWithType:NSEventTypeLeftMouseDragged
                                       location:[v convertPoint:pullPt toView:nil]
                                  modifierFlags:0
                                      timestamp:NSProcessInfo.processInfo.systemUptime
                                   windowNumber:w.windowNumber
                                        context:nil
                                    eventNumber:1204
                                     clickCount:1
                                       pressure:1]];
    [v mouseUp:[NSEvent mouseEventWithType:NSEventTypeLeftMouseUp
                                  location:[v convertPoint:pullPt toView:nil]
                             modifierFlags:0
                                 timestamp:NSProcessInfo.processInfo.systemUptime
                              windowNumber:w.windowNumber
                                   context:nil
                               eventNumber:1205
                                clickCount:1
                                  pressure:1]];
    [self check:[[self.preview diagnostics][@"billiards_shots"] intValue] == 1 &&
                [[self.preview diagnostics][@"billiards_sound_events"] intValue] >= 1 &&
                ![[self.preview diagnostics][@"sound_muted"] boolValue]
           name:@"billiards_pull_back_shot_and_sound_recorded"];
    self.preview.soundMuted = YES;
    [self check:[[self.preview diagnostics][@"sound_muted"] boolValue] name:@"billiards_mute_toggle_applied"];
    self.preview.soundMuted = NO;

    // Reset in billiards stays unpaused so user can immediately play next rack.
    [self.preview reset];
    [self check:self.preview.billiardsEnabled && ![[self.preview diagnostics][@"paused"] boolValue]
           name:@"billiards_reset_stays_live"];

    // Live migration while in billiards re-racks without ending session or changing deadline.
    NSRect smaller = w.frame;
    smaller.size.width = std::floor(smaller.size.width * 0.88);
    smaller.size.height = std::floor(smaller.size.height * 0.88);
    [self.preview fixtureMigrationWork:smaller scale:w.backingScaleFactor];
    [self
        waitUntil:^BOOL {
          return !self.preview.migrating;
        }
             name:@"billiards_migration_commit"];
    [self check:self.preview.active && self.preview.billiardsEnabled &&
                [deadline isEqual:[[self.preview valueForKey:@"deadline"] fireDate]]
           name:@"billiards_migration_preserves_mode_and_deadline"];

    // Live rule switch to EightBall (15 + 1 = 16 bodies) and Snooker (21 + 1 = 22 bodies) with deficit
    // fillers.
    BilliardsConfig eightCfg = BilliardsConfig::desktop();
    eightCfg.rule = BilliardsRule::EightBall;
    eightCfg.showBallNumbers = true;
    [self.preview applyBilliardsConfig:eightCfg];
    [self check:self.preview.billiardsEnabled && [[self.preview diagnostics][@"bodies"] intValue] == 16
           name:@"billiards_eight_ball_deficit_fillers_applied"];

    BilliardsConfig snookerCfg = BilliardsConfig::desktop();
    snookerCfg.rule = BilliardsRule::Snooker;
    [self.preview applyBilliardsConfig:snookerCfg];
    [self check:self.preview.billiardsEnabled && [[self.preview diagnostics][@"bodies"] intValue] == 22 &&
                [self.preview playMode] == PlayMode::Billiards
           name:@"billiards_snooker_21_balls_applied"];

    // Disabling billiards via unified PlayMode selector switches back to gravity mode cleanly.
    BilliardsConfig bottomGravityCfg = BilliardsConfig::desktop();
    bottomGravityCfg.iconSelection = IconSelectionPolicy::Custom;
    bottomGravityCfg.customIconIndices = {0};
    bottomGravityCfg.extraIcons = ExtraIconsPolicy::BottomGravity;
    bottomGravityCfg.showBallBorders = true;
    [self.preview applyBilliardsConfig:bottomGravityCfg];
    BOOL bottomGravityBorderless = YES;
    for (CALayer* sub in v.layer.sublayers) {
        if (!sub.hidden && sub.borderWidth > 0) bottomGravityBorderless = NO;
    }
    [self check:bottomGravityBorderless name:@"billiards_bottom_gravity_icons_borderless"];

    [self.preview setPlayMode:PlayMode::Gravity];
    BOOL gravityIconsBorderless = YES;
    for (CALayer* sub in v.layer.sublayers) {
        if (!sub.hidden && sub.borderWidth > 0) gravityIconsBorderless = NO;
    }
    [self check:self.preview.active && !self.preview.billiardsEnabled &&
                [self.preview playMode] == PlayMode::Gravity && gravityIconsBorderless &&
                [[self.preview diagnostics][@"billiards_toggles"] intValue] == 2
           name:@"billiards_disable_returns_to_gravity"];
    [self.preview close:@"billiards_fixture_done"];
}
- (void)exerciseMigration {
    @try {
        try {
            for (BOOL paused : {YES, NO}) {
                auto scene = [self fixture];
                // Native open dispatch is intercepted below; no real target or OS open is used.
                for (auto& item : scene.items) item.syntheticFixture = false;
                [self check:[self.preview start:scene images:[self images]] name:@"migration_fixture_start"];
                self.preview.allowsBackgroundSession = YES;
                self.preview.migrationProtectionValid = ^BOOL {
                  return YES;
                };
                if (!paused) [self.preview playPause];
                NSWindow* window = [self.preview valueForKey:@"window"];
                const NSRect original = window.frame;
                NSDate* deadline = [[self.preview valueForKey:@"deadline"] fireDate];
                NSArray* items = [self.preview diagnostics][@"items"];
                const CGFloat scale = window.backingScaleFactor;
                NSRect small = original;
                small.size.width = std::floor(small.size.width * .75);
                small.size.height = std::floor(small.size.height * .75);
                [self.preview fixtureMigrationWork:small scale:scale];
                [self check:self.preview.migrating && window.ignoresMouseEvents && !self.preview.menuActive
                       name:@"migration_releases_input_and_menu"];
                [self.preview toggleInteraction];
                [self.preview playPause];
                [self.preview reset];
                [self check:[[self.preview diagnostics][@"paused"] boolValue] == paused
                       name:@"migration_blocks_user_commands"];
                // Supersede a candidate before its asynchronous commit; original freeze is retained.
                NSRect final = original;
                final.size.width = std::floor(final.size.width * .85);
                final.size.height = std::floor(final.size.height * .85);
                [self.preview fixtureMigrationWork:final scale:scale];
                [self
                    waitUntil:^BOOL {
                      return !self.preview.migrating;
                    }
                         name:@"migration_shrink_commit"];
                [self check:self.preview.active && self.preview.committedDisplayMatches &&
                            NSEqualRects(window.frame, final)
                       name:@"migration_window_backing_draw_and_physics_committed"];
                [self check:[[self.preview diagnostics][@"paused"] boolValue] == paused &&
                            [items isEqual:[self.preview diagnostics][@"items"]] &&
                            [deadline isEqual:[[self.preview valueForKey:@"deadline"] fireDate]]
                       name:@"migration_preserves_pause_binding_and_deadline"];
                [self.preview fixtureMigrationWork:original scale:scale];
                [self
                    waitUntil:^BOOL {
                      return !self.preview.migrating;
                    }
                         name:@"migration_expand_commit"];
                [self check:self.preview.active && self.preview.committedDisplayMatches
                       name:@"migration_expand_verified"];
                if (!paused && self.preview.active) {
                    __block uint32_t opened = 0;
                    self.preview.canOpenItem = ^BOOL(uint32_t item) {
                      return item == 17;
                    };
                    self.preview.openItem = ^(uint32_t item) {
                      opened = item;
                    };
                    [self.preview toggleInteraction];
                    [self
                        waitUntil:^BOOL {
                          return window.keyWindow;
                        }
                             name:@"migration_interaction_ready"];
                    [self.preview fixturePointer:NSEventTypeLeftMouseDown];
                    [self check:[[self.preview diagnostics][@"grabbing"] boolValue]
                           name:@"migration_native_grab"];
                    [self.preview fixturePointer:NSEventTypeLeftMouseUp];
                    [self.preview fixtureContextMenu:YES];
                    [self
                        waitUntil:^BOOL {
                          return opened != 0;
                        }
                             name:@"migration_native_open_dispatch"];
                    [self check:opened == 17 && !self.preview.fixtureMenuTimedOut
                           name:@"migration_open_preserves_original_binding"];
                    [self.preview completeOpenRequest];
                }
                [self.preview close:@"migration_case_done"];
            }
            [self.preview start:[self fixture] images:[self images]];
            self.preview.allowsBackgroundSession = YES;
            self.preview.migrationProtectionValid = ^BOOL {
              return YES;
            };
            NSWindow* window = [self.preview valueForKey:@"window"];
            NSRect work = window.frame;
            work.size.width *= .8;
            [self.preview fixtureMigrationWork:work scale:window.backingScaleFactor];
            self.preview.migrationProtectionValid = ^BOOL {
              return NO;
            };
            [self.preview fixtureAdvanceMigration];
            [self check:!self.preview.active && !self.preview.migrating
                   name:@"migration_helper_failure_invalidates"];
            [self.preview start:[self fixture] images:[self images]];
            self.preview.allowsBackgroundSession = YES;
            self.preview.migrationProtectionValid = ^BOOL {
              return YES;
            };
            window = [self.preview valueForKey:@"window"];
            work = window.frame;
            work.size.width *= .8;
            [self.preview fixtureMigrationWork:work scale:window.backingScaleFactor];
            [[self.preview valueForKey:@"deadline"] setFireDate:NSDate.distantPast];
            [self.preview fixtureAdvanceMigration];
            [self check:!self.preview.active && !self.preview.migrating
                   name:@"migration_preview_deadline_invalidates"];
            [self.preview start:[self fixture] images:[self images]];
            self.preview.allowsBackgroundSession = YES;
            window = [self.preview valueForKey:@"window"];
            work = window.frame;
            work.size.width *= .8;
            [self.preview fixtureMigrationWork:work scale:window.backingScaleFactor];
            [self.preview close:@"fixture_recovery_race"];
            [self.preview fixtureAdvanceMigration];
            [self check:!self.preview.active && !self.preview.migrating
                   name:@"migration_recovery_prevents_late_commit"];
            [self.preview start:[self fixture] images:[self images]];
            self.preview.allowsBackgroundSession = YES;
            window = [self.preview valueForKey:@"window"];
            [self.preview fixtureMigrationWork:window.frame scale:window.backingScaleFactor + 1];
            [self
                waitUntil:^BOOL {
                  return !self.preview.active;
                }
                     name:@"migration_native_mismatch_timeout"];
            [self check:!self.preview.active && !self.preview.migrating
                   name:@"unverified_backing_never_commits"];
            [self.preview start:[self fixture] images:[self images]];
            self.preview.allowsBackgroundSession = YES;
            window = [self.preview valueForKey:@"window"];
            work = window.frame;
            work.size = NSMakeSize(40, 40);
            [self.preview fixtureMigrationWork:work scale:window.backingScaleFactor];
            [self
                waitUntil:^BOOL {
                  return !self.preview.active;
                }
                     name:@"migration_layout_rejection"];
            [self check:!self.preview.migrating name:@"unsafe_layout_restores_instead_of_committing"];
            NSDictionary* failure = nil;
            for (NSDictionary* event in [self.preview diagnostics][@"events"])
                if ([event[@"event"] isEqual:@"display_migration_failed"]) failure = event[@"details"];
            [self check:[failure[@"stage"] isEqual:@"current_layout"] &&
                        [failure[@"reason"] isEqual:@"unsupported migration geometry"] &&
                        [failure[@"frozen_icons"] count] == 3 && failure[@"old_work_size"] &&
                        failure[@"new_work_size"] && failure[@"user_paused"]
                   name:@"migration_failure_preserves_phase_and_anonymous_frozen_geometry"];
        } catch (const std::exception& e) {
            [self check:NO name:[NSString stringWithUTF8String:e.what()]];
        }
    } @catch (NSException* e) {
        [self check:NO name:e.reason];
    }
}
- (void)exercise {
    @try {
        try {
            DPPDesktopAccess* access = [DPPDesktopAccess new];
            [access show];
            NSButton* oldButton = [NSButton new];
            oldButton.tag = 14;
            [access action:oldButton];
            [access prepareAssisted];
            [access returnAssisted];
            NSDictionary* rejected = [access report];
            [self check:![rejected[@"assisted_candidate_availability"][@"available"] boolValue] &&
                        [rejected[@"prepared_images"] intValue] == 0 &&
                        [rejected[@"snapshot_references"] intValue] == 0 &&
                        ![rejected[@"prepared_input_capture"] boolValue] &&
                        [rejected[@"pending_manual_settings_reminder"] count] == 0
                   name:@"legacy_actions_rejected_without_resources_input_or_reminder"];
            [access confirmManualRestore];
            [self check:[[access report][@"pending_manual_settings_reminder"] count] == 0
                   name:@"confirmed_absent_reminder_not_reintroduced"];
            NSDictionary* unknown = @{@"experiment_id" : @"fixture_old_unknown"};
            [NSUserDefaults.standardUserDefaults setObject:unknown forKey:@"DPPAssistedFixturePendingV1"];
            [access confirmManualRestore];
            [access prepareAssisted];
            [self check:[[access report][@"pending_manual_settings_reminder"] isEqual:unknown]
                   name:@"old_unknown_original_reminder_preserved_with_explanation"];
            [access showHistory];
            [access shutdown];
            [NSUserDefaults.standardUserDefaults removeObjectForKey:@"DPPAssistedFixturePendingV1"];
            auto s = [self fixture];
            self.preview.experimentID = @"legacy_assisted_experiment";
            NSUInteger before = self.preview.sessionSummaries.count;
            [self check:![self.preview start:s images:[self images]] && !self.preview.active &&
                        self.preview.sessionSummaries.count == before &&
                        [[self.preview diagnostics][@"start_rejection"] containsString:@"已停用"]
                   name:@"renderer_rejects_assisted_before_window_scene_input_or_journal"];
            self.preview.experimentID = nil;
            [self check:![self.preview start:s images:@[]] && !self.preview.active
                   name:@"startup_missing_cache_fails_closed"];
            [self check:[self.preview.startupFailure[@"stage"] isEqual:@"input_cache"] &&
                        [self.preview.startupFailure[@"error"] isEqual:@"missing image cache"]
                   name:@"startup_failure_retains_cache_reason"];
            auto invalidGeometry = s;
            invalidGeometry.items[0].sceneAnchor.x = -1;
            [self
                check:![self.preview start:invalidGeometry images:[self images]] &&
                      [self.preview.startupFailure[@"stage"] isEqual:@"scene_validation_and_physics"] &&
                      [self.preview.startupFailure[@"items"] count] == s.items.size() && !self.preview.active
                 name:@"startup_geometry_failure_retains_anonymous_replay_inputs"];
            for (NSUInteger stage = 1; stage <= 4; ++stage) {
                self.preview.failAfterStage = stage;
                BOOL started = [self.preview start:s images:[self images]];
                NSUInteger visible = 0;
                for (NSWindow* window in NSApp.windows)
                    if (window.visible && window != self.panel) ++visible;
                [self check:!started && !self.preview.active && visible == 0
                       name:[NSString stringWithFormat:@"startup_stage_%lu_cleans_window_scene_cache_timers",
                                                       stage]];
            }
            self.preview.failAfterStage = 0;
            [self.preview start:s images:[self images]];
            NSMutableDictionary* liveTiming = [@{
                @"protected_session_id" : @"trace-fixture",
                @"original_deadline_ms" : @100,
                @"current_owner_deadline_ms" : @100
            } mutableCopy];
            self.preview.protectionIdentity = [liveTiming copy];
            self.preview.protectionReader = ^NSDictionary*(BOOL requestHelper) {
              (void)requestHelper;
              return [liveTiming copy];
            };
            [self.preview recordEvent:@"sample_before" details:@{}];
            liveTiming[@"current_owner_deadline_ms"] = @200;
            NSTimer* liveTimer = [self.preview valueForKey:@"deadline"];
            NSDate* previousDeadline = liveTimer.fireDate;
            liveTimer.fireDate = [previousDeadline dateByAddingTimeInterval:-1];
            [self.preview recordEvent:@"sample_after" details:@{}];
            NSArray* traceRows = [self.preview diagnostics][@"events"];
            NSDictionary* beforeTiming = traceRows[traceRows.count - 2];
            NSDictionary* afterTiming = traceRows.lastObject;
            [self check:[beforeTiming[@"protection"][@"current_owner_deadline_ms"] intValue] == 100 &&
                        [afterTiming[@"protection"][@"current_owner_deadline_ms"] intValue] == 200 &&
                        [afterTiming[@"protection"][@"original_deadline_ms"] intValue] == 100 &&
                        [afterTiming[@"current_preview_deadline_epoch"] doubleValue] <
                            [afterTiming[@"initial_preview_deadline_epoch"] doubleValue]
                   name:@"trace_samples_live_owner_and_timer_not_original_copies"];
            liveTimer.fireDate = previousDeadline;
            [self.preview close:@"trace_fixture_stop"];
            [self.preview recordEvent:@"recovery_result" details:@{@"recovery_completed" : @YES}];
            [self check:[self.preview.sessionSummaries.lastObject[@"events"][0][@"protection"] count] == 0 &&
                        [[self.preview.sessionSummaries.lastObject[@"events"] lastObject][@"event"]
                            isEqual:@"recovery_result"]
                   name:@"trace_preserves_prebinding_null_and_postclose_recovery"];
            [self.preview start:s images:[self images]];
            [self check:[[self.preview diagnostics][@"protection"] count] == 0 &&
                        [[[self.preview diagnostics][@"events"] firstObject][@"protection"] count] == 0
                   name:@"new_candidate_clears_previous_protection_binding"];
            self.preview.protectionReader = nil;
            for (unsigned n = 0; n < 270; ++n) [self.preview recordEvent:@"capacity_fixture" details:@{}];
            [self.preview close:@"trace_capacity_stop"];
            NSDictionary* capped = self.preview.sessionSummaries.lastObject;
            [self check:[capped[@"events"] count] <= 260 &&
                        [capped[@"events_dropped"] unsignedIntegerValue] > 0 &&
                        [[capped[@"events"] lastObject][@"event"] isEqual:@"closed"]
                   name:@"trace_bounds_events_and_preserves_terminal_records"];
            auto full = s;
            full.fullDesktop = true;
            full.finderItemCount = 4;
            auto fourth = full.items.back();
            fourth.id = IconId{999};
            fourth.texture = TextureId{4};
            full.items.push_back(fourth);
            NSMutableArray* fullImages = [[self images] mutableCopy];
            [fullImages addObject:fullImages.firstObject];
            self.preview.experimentID = @"protected_finder";
            self.preview.labels = @[ @"fixture 1", @"fixture 2", @"fixture 3", @"fixture 4" ];
            self.preview.deferPresentation = YES;
            auto insetFixture = full;
            insetFixture.candidatePreview = true;
            insetFixture.items[0].sceneAnchor.y = 4;
            insetFixture.items[0].rawDpos = macGlobalPointsToScene(
                macSceneToGlobalPoints(insetFixture.items[0].sceneAnchor, insetFixture.workArea),
                insetFixture.screen);
            [self check:[self.preview start:insetFixture images:fullImages] &&
                        [[self.preview diagnostics][@"startup_edge_inset_count"] unsignedIntegerValue] == 1 &&
                        self.preview.fixedSceneGeometryValid && !self.preview.startupFailure
                   name:@"edge_candidate_keeps_fixed_window_and_complete_resources"];
            [self.preview close:@"edge_candidate_complete"];
            insetFixture.candidatePreview = false;
            [self check:[self.preview start:insetFixture images:fullImages] &&
                        [[self.preview diagnostics][@"startup_edge_inset_count"] unsignedIntegerValue] == 1 &&
                        self.preview.fixedSceneGeometryValid
                   name:@"edge_active_scene_matches_candidate_layout"];
            [self.preview reset];
            [self check:[[self.preview diagnostics][@"startup_edge_inset_count"] unsignedIntegerValue] == 1 &&
                        self.preview.fixedSceneGeometryValid
                   name:@"edge_reset_retains_startup_layout_and_geometry"];
            [self.preview close:@"edge_active_complete"];
            [self check:[self.preview start:full images:fullImages] &&
                        [[self.preview diagnostics][@"bodies"] intValue] == 4
                   name:@"full_resources_prepared_before_presentation"];
            NSDictionary* labelsBefore = [self.preview diagnostics];
            [self
                check:self.preview.showsFilenames && [labelsBefore[@"filename_visible_layers"] intValue] == 4
                 name:@"filenames_default_visible"];
            self.preview.showsFilenames = NO;
            [self.preview fixtureSubmitFrame:NO reset:NO];
            [self.preview reset];
            NSDictionary* labelsOff = [self.preview diagnostics];
            [self check:[labelsOff[@"filename_visible_layers"] intValue] == 0 &&
                        [labelsOff[@"filename_layer_count"] intValue] == 4 &&
                        [labelsOff[@"filename_string_writes"] isEqual:labelsBefore[@"filename_string_writes"]]
                   name:@"hidden_labels_skip_updates_and_survive_reset"];
            for (int i = 0; i < 3; ++i) {
                self.preview.showsFilenames = YES;
                self.preview.showsFilenames = NO;
            }
            self.preview.showsFilenames = YES;
            for (int i = 0; i < 5; ++i) [self.preview fixtureSubmitFrame:YES reset:NO];
            NSDictionary* labelsAfter = [self.preview diagnostics];
            [self check:[labelsAfter[@"filename_visible_layers"] intValue] == 4 &&
                        [labelsAfter[@"filename_string_writes"]
                            isEqual:labelsBefore[@"filename_string_writes"]] &&
                        [labelsAfter[@"filename_scale_writes"]
                            isEqual:labelsBefore[@"filename_scale_writes"]] &&
                        [labelsAfter[@"starts"] isEqual:labelsBefore[@"starts"]] &&
                        [labelsAfter[@"session_id"] isEqual:labelsBefore[@"session_id"]]
                   name:@"toggle_and_motion_reuse_text_and_session"];
            self.preview.showsFilenames = NO;
            self.preview.labels = @[ @"fixture renamed", @"fixture 2", @"fixture 3", @"fixture 4" ];
            self.preview.showsFilenames = YES;
            [self check:[[[self.preview diagnostics] objectForKey:@"filename_string_writes"] intValue] ==
                        [labelsBefore[@"filename_string_writes"] intValue] + 1
                   name:@"changed_label_updates_when_reshown"];
            NSUInteger fullVisible = 0;
            for (NSWindow* window in NSApp.windows)
                if (window.visible && window != self.panel) ++fullVisible;
            [self check:fullVisible == 0 name:@"deferred_full_mirror_has_no_visible_overlay"];
            [self check:[self.preview presentPrepared] name:@"full_mirror_present_prepared_resources"];
            [self.preview close:@"full_fixture_complete"];
            self.preview.showsFilenames = NO;
            self.preview.deferPresentation = YES;
            [self check:[self.preview start:full images:fullImages] &&
                        [[self.preview diagnostics][@"filename_layer_count"] intValue] == 0
                   name:@"initially_hidden_labels_allocate_no_layers"];
            self.preview.showsFilenames = YES;
            [self check:[[self.preview diagnostics][@"filename_visible_layers"] intValue] == 4
                   name:@"show_labels_after_initially_hidden_start"];
            [self.preview close:@"hidden_fixture_complete"];
            auto bottom = full;
            bottom.items[0].sceneAnchor = {80, bottom.workArea.height - 48};
            bottom.items[0].rawDpos = macGlobalPointsToScene(
                macSceneToGlobalPoints(bottom.items[0].sceneAnchor, bottom.workArea), bottom.screen);
            [self check:[self.preview start:bottom images:fullImages] name:@"bottom_label_fixture_started"];
            NSArray<CATextLayer*>* nativeLabels = [self.preview valueForKey:@"labelLayers"];
            CATextLayer* bottomLabel = nativeLabels.firstObject;
            [self check:bottomLabel && CGRectGetMaxY(bottomLabel.frame) < bottom.items[0].sceneAnchor.y &&
                        std::abs(CGRectGetMidX(bottomLabel.frame) - 80) < .01
                   name:@"bottom_filename_above_icon_and_centred"];
            [self.preview close:@"bottom_label_fixture_complete"];
            self.preview.labels = nil;
            self.preview.deferPresentation = NO;
            self.preview.experimentID = nil;
            [self check:[self.preview start:s images:[self images]] name:@"static_native_window_started"];
            NSDictionary* report = [self.preview diagnostics];
            [self check:[report[@"layer_count"] intValue] == 3 && [report[@"bodies"] intValue] == 3
                   name:@"three_cached_layers_and_bodies"];
            [self.preview playPause];
            [self
                waitUntil:^BOOL {
                  return [[self.preview diagnostics][@"ticks"] intValue] > 10;
                }
                     name:@"playback_ticks"];
            [self finish];
        } catch (...) {
            [NSApp terminate:nil];
        }
    } @catch (NSException*) {
        [NSApp terminate:nil];
    }
}
- (void)exerciseFocus {
    [self.preview reset];
    [self check:[[self.preview diagnostics][@"paused"] boolValue] name:@"reset_pauses_owned_scene"];
    [self.preview toggleInteraction];
    [self check:[[self.preview diagnostics][@"phase"] isEqual:@"interactive"]
           name:@"explicit_interaction_key_window"];
    [self.panel makeKeyAndOrderFront:nil];
    [self check:[[self.preview diagnostics][@"phase"] isEqual:@"playing"] &&
                ![[self.preview diagnostics][@"grabbing"] boolValue]
           name:@"panel_focus_releases_interaction_keeps_session"];
}
- (void)exerciseMenus {
    DPPMirrorPreview* menuPreview = [DPPMirrorPreview new];
    self.observedPreview = menuPreview;
    auto menuScene = [self fixture];
    __block NSUInteger opens = 0;
    __block NSUInteger begins = 0, ends = 0;
    self.menuEvents = [NSMutableArray array];
    NSMutableArray* observers = [NSMutableArray array];
    for (NSString* event in @[ NSMenuDidBeginTrackingNotification, NSMenuDidEndTrackingNotification ]) {
        id observer = [NSNotificationCenter.defaultCenter
            addObserverForName:event
                        object:nil
                         queue:nil
                    usingBlock:^(NSNotification* note) {
                      if (note.object != [menuPreview valueForKey:@"openMenu"]) return;
                      if ([event isEqual:NSMenuDidBeginTrackingNotification])
                          ++begins;
                      else
                          ++ends;
                      [self.menuEvents addObject:@{
                          @"event" : event,
                          @"opens" : @(opens),
                          @"app_active" : @(NSApp.active),
                          @"menu_active" : @(menuPreview.menuActive),
                          @"state" : [menuPreview diagnostics]
                      }];
                    }];
        [observers addObject:observer];
    }
    void (^cleanup)(void) = ^{
      for (id observer in observers) [NSNotificationCenter.defaultCenter removeObserver:observer];
      menuPreview.openItem = nil;
      [menuPreview close:@"menu_fixture_complete"];
    };
    NSMutableArray* targets = [NSMutableArray array];
    menuPreview.canOpenItem = ^BOOL(uint32_t) {
      return YES;
    };
    menuPreview.openItem = ^(uint32_t itemID) {
      [targets addObject:@(itemID)];
      ++opens;
      self.observedOpens = opens;
    };
    [menuPreview start:menuScene images:[self images]];
    [menuPreview toggleInteraction];
    [menuPreview fixtureContextMenu:YES];
    [self check:opens == 0 && menuPreview.active name:@"synthetic_objects_never_offer_open"];
    [menuPreview close:@"synthetic_menu_complete"];
    for (auto& item : menuScene.items) item.syntheticFixture = false;  // injected IDs only, no native targets
    menuPreview.opensWithoutEndingSession = YES;
    [menuPreview start:menuScene images:[self images]];
    [menuPreview toggleInteraction];
    NSString* sessionBefore = [menuPreview diagnostics][@"session_id"];
    NSDate* deadlineOriginal = [[menuPreview valueForKey:@"deadline"] fireDate];
    NSNumber* pausedBefore = [menuPreview diagnostics][@"paused"];
    [menuPreview fixtureContextMenu:NO];
    if (!menuPreview.active) {
        self.blocked = YES;
        [self check:NO name:@"scene_closed_during_menu_precondition"];
        cleanup();
        return;
    }
    [self
        check:opens == 0 && menuPreview.active && [[menuPreview diagnostics][@"phase"] isEqual:@"interactive"]
         name:@"menu_cancel_preserves_session_and_interaction_without_open"];
    [self check:begins == 1 && ends == 1 && !menuPreview.menuActive && !menuPreview.fixtureMenuTimedOut &&
                ![[menuPreview diagnostics][@"grabbing"] boolValue] &&
                [pausedBefore isEqual:[menuPreview diagnostics][@"paused"]] &&
                [sessionBefore isEqual:[menuPreview diagnostics][@"session_id"]] &&
                [deadlineOriginal isEqual:[[menuPreview valueForKey:@"deadline"] fireDate]]
           name:@"cancel_tracks_without_action_grab_pause_or_deadline_change"];
    [menuPreview fixtureContextMenu:YES];
    [self check:!menuPreview.fixtureMenuTimedOut && !menuPreview.menuActive && begins == 2 && ends == 2
           name:@"selected_action_ends_tracking_without_fixture_cancellation"];
    if (![self
            waitUntil:^BOOL {
              return opens >= 1 || !menuPreview.active;
            }
                 name:@"first_open_completion"] ||
        !menuPreview.active) {
        self.blocked = !menuPreview.active;
        [self check:NO name:@"first_open_not_completed_in_active_scene"];
        cleanup();
        return;
    }
    [menuPreview fixtureContextMenu:YES];
    [self check:opens == 1 && ![[menuPreview diagnostics][@"grabbing"] boolValue]
           name:@"duplicate_menu_callbacks_submit_once_after_input_release"];
    NSDate* deadlineBefore = [[menuPreview valueForKey:@"deadline"] fireDate];
    menuPreview.opensWithoutEndingSession = YES;
    menuPreview.allowsBackgroundSession = YES;
    [menuPreview completeOpenRequest];
    [menuPreview toggleInteraction];
    [NSNotificationCenter.defaultCenter postNotificationName:NSApplicationDidResignActiveNotification
                                                      object:NSApp];
    [self check:menuPreview.active && ![[menuPreview diagnostics][@"grabbing"] boolValue] &&
                [[menuPreview diagnostics][@"phase"] isEqual:@"playing"]
           name:@"explicit_open_background_handoff_keeps_scene_and_releases_input"];
    [self check:[deadlineBefore isEqual:[[menuPreview valueForKey:@"deadline"] fireDate]]
           name:@"open_handoff_does_not_extend_deadline"];
    [self check:menuPreview.desktopReady && ![[menuPreview valueForKey:@"window"] ignoresMouseEvents] &&
                [(NSWindow*)[menuPreview valueForKey:@"window"] level] ==
                    CGWindowLevelForKey(kCGDesktopIconWindowLevelKey) + 1
           name:@"open_return_accepts_next_desktop_hit_below_other_apps_without_panel"];
    [NSNotificationCenter.defaultCenter
        postNotificationName:NSApplicationDidChangeScreenParametersNotification
                      object:NSApp];
    [self check:menuPreview.active name:@"unchanged_screen_notification_preserves_session"];
    [menuPreview fixtureContextMenu:YES];
    [self
        waitUntil:^BOOL {
          return opens >= 2;
        }
             name:@"second_open_completion"];
    [self check:opens == 2 name:@"next_explicit_menu_action_can_open_after_completion"];
    [self check:begins == 3 && ends == 3 && !menuPreview.menuActive && !menuPreview.fixtureMenuTimedOut &&
                [targets isEqual:@[ @17, @17 ]] &&
                [sessionBefore isEqual:[menuPreview diagnostics][@"session_id"]] &&
                [deadlineOriginal isEqual:[[menuPreview valueForKey:@"deadline"] fireDate]]
           name:@"two_menu_actions_preserve_binding_session_and_original_deadline"];
    [menuPreview completeOpenRequest];
    NSWindow* other = [[NSWindow alloc] initWithContentRect:NSMakeRect(600, 300, 240, 160)
                                                  styleMask:NSWindowStyleMaskTitled
                                                    backing:NSBackingStoreBuffered
                                                      defer:NO];
    other.title = @"Synthetic ordinary window — no real target";
    other.releasedWhenClosed = NO;
    [other makeKeyAndOrderFront:nil];
    NSWindow* mirror = [menuPreview valueForKey:@"window"];
    [menuPreview fixturePointer:NSEventTypeLeftMouseDragged];
    [self check:![[menuPreview diagnostics][@"grabbing"] boolValue] && menuPreview.desktopReady
           name:@"desktop_return_without_new_press_does_not_resume_grab"];
    [menuPreview fixturePointer:NSEventTypeLeftMouseDown];
    [self check:[[menuPreview diagnostics][@"grabbing"] boolValue] && mirror.keyWindow
           name:@"desktop_return_fresh_press_grabs_without_panel"];
    [self check:mirror.level < NSNormalWindowLevel && !mirror.ignoresMouseEvents
           name:@"desktop_return_interaction_stays_below_ordinary_apps"];
    // Observe native WindowServer ordering, not just the requested level. No images are captured.
    [self
        waitUntil:^BOOL {
          NSArray* windows =
              CFBridgingRelease(CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID));
          NSUInteger mirrorIndex = NSNotFound, otherIndex = NSNotFound;
          for (NSUInteger i = 0; i < windows.count; ++i) {
              NSInteger number = [windows[i][(__bridge NSString*)kCGWindowNumber] integerValue];
              if (number == mirror.windowNumber) mirrorIndex = i;
              if (number == other.windowNumber) otherIndex = i;
          }
          return otherIndex != NSNotFound && mirrorIndex != NSNotFound && otherIndex < mirrorIndex;
        }
             name:@"ordinary_window_above_interacting_desktop_mirror"];
    [menuPreview fixturePointer:NSEventTypeLeftMouseUp];
    [self check:![[menuPreview diagnostics][@"grabbing"] boolValue] && mirror.level < NSNormalWindowLevel
           name:@"desktop_return_mouse_up_releases_without_covering_apps"];
    [other makeKeyAndOrderFront:nil];
    [self check:other.keyWindow && menuPreview.active && menuPreview.desktopReady &&
                ![[menuPreview diagnostics][@"grabbing"] boolValue] &&
                [sessionBefore isEqual:[menuPreview diagnostics][@"session_id"]] &&
                [deadlineOriginal isEqual:[[menuPreview valueForKey:@"deadline"] fireDate]]
           name:@"ordinary_window_focus_preserves_session_deadline_without_panel"];
    [other close];
    NSArray* anchorsBefore = [menuPreview diagnostics][@"items"];
    [menuPreview fixtureWorkAreaChange];
    [self check:menuPreview.active name:@"work_area_change_after_open_preserves_scene"];
    [self check:[anchorsBefore isEqual:[menuPreview diagnostics][@"items"]]
           name:@"work_area_change_does_not_reposition_or_rebind_replicas"];
    cleanup();
    [menuPreview start:[self fixture] images:[self images]];
    NSDate* fixedDeadline = [[menuPreview valueForKey:@"deadline"] fireDate];
    [menuPreview fixtureWorkAreaChange];
    [self check:menuPreview.active && [menuPreview fixedSceneGeometryValid] &&
                [fixedDeadline isEqual:[[menuPreview valueForKey:@"deadline"] fireDate]]
           name:@"before_open_expansion_retains_geometry_and_original_deadline"];
    [menuPreview close:@"fixed_geometry_fixture_done"];
    for (NSUInteger fault = 0; fault < 3; ++fault) {
        [menuPreview start:[self fixture] images:[self images]];
        NSWindow* owned = [menuPreview valueForKey:@"window"];
        if (fault == 0) {
            NSRect frame = owned.frame;
            frame.origin.x += 1;
            [owned setFrame:frame display:NO];
        }
        if (fault == 1) [owned.contentView setBoundsOrigin:NSMakePoint(1, 0)];
        if (fault == 2) [[menuPreview valueForKey:@"deadline"] setFireDate:[NSDate distantPast]];
        [menuPreview fixtureConfiguration:NSWindowDidChangeBackingPropertiesNotification changedGeometry:NO];
        [self check:!menuPreview.active
               name:[NSString stringWithFormat:@"invalid_fixed_geometry_or_deadline_closes_%lu", fault]];
    }
    for (NSString* event in @[
             NSWorkspaceActiveSpaceDidChangeNotification, NSWorkspaceWillSleepNotification,
             NSApplicationDidChangeScreenParametersNotification
         ]) {
        [menuPreview start:menuScene images:[self images]];
        menuPreview.allowsBackgroundSession = YES;
        [menuPreview fixtureConfiguration:event
                          changedGeometry:[event isEqual:NSApplicationDidChangeScreenParametersNotification]];
        [self check:!menuPreview.active name:[@"protection_still_closes_" stringByAppendingString:event]];
    }
}
- (void)finish {
    @try {
        try {
            NSDictionary* playback = [self.preview diagnostics];
            [self check:[playback[@"ticks"] intValue] > 10 && ![playback[@"paused"] boolValue]
                   name:@"native_timer_plays_gravity"];
            [self exerciseFocus];
            [self.preview close:@"native_fixture_close"];
            NSDictionary* firstSession = self.preview.sessionSummaries.lastObject;
            [self.preview recordHuman:@{@"fixture_report" : @"not_real_acceptance"}];
            for (int i = 0; i < 10; ++i) {
                auto s = [self fixture];
                [self check:[self.preview start:s images:[self images]] name:@"repeated_native_start"];
                [self.preview close:@"repeated_native_close"];
            }
            NSUInteger visible = 0;
            for (NSWindow* window in NSApp.windows)
                if (window.visible && window != self.panel) ++visible;
            NSDictionary* closed = [self.preview diagnostics];
            [self check:!self.preview.active && visible == 0 &&
                        [closed[@"owned_bodies_after_close"] intValue] == 0 &&
                        [closed[@"owned_images_after_close"] intValue] == 0
                   name:@"no_remaining_visible_window_scene_or_image_cache"];
            NSArray* sessions = self.preview.sessionSummaries;
            NSDictionary* retained = sessions[sessions.count - 11];
            [self check:[retained[@"session_id"] isEqual:firstSession[@"session_id"]] &&
                        [retained[@"resets"] intValue] == 1 && [sessions.lastObject[@"resets"] intValue] == 0
                   name:@"sessions_do_not_overwrite_prior_counters"];
            [self check:[retained[@"human_reports"] count] == 1 &&
                        ![retained[@"human_reports"][0][@"machine_verified"] boolValue] &&
                        [retained[@"human_reports"][0][@"session_id"] isEqual:retained[@"session_id"]]
                   name:@"human_reports_separate_and_session_linked"];
            DPPMirrorPreview* reloaded = [DPPMirrorPreview new];
            [self check:reloaded.sessionSummaries.count == sessions.count
                   name:@"bounded_journal_reload_preserves_sessions"];
            [self exerciseMenus];
            [self writeResult:@{@"playback" : playback, @"closed" : closed, @"sessions" : sessions}];
        } catch (...) {
            [self.preview close:@"fixture_exception"];
            [NSApp terminate:nil];
        }
    } @catch (NSException*) {
        [self.preview close:@"fixture_native_exception"];
        [NSApp terminate:nil];
    }
}
@end
int main(int argc, char** argv) {
    if (argc != 2 && argc != 3) return 2;
    // Cross-process exclusion also covers direct invocations outside the runner.
    const std::string lockPath = "/tmp/dpp-mirror-native-" + std::to_string(getuid()) + ".lock";
    const int lock = open(lockPath.c_str(), O_CREAT | O_RDWR, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0) return 4;
    @autoreleasepool {
        outputPath = [NSString stringWithUTF8String:argv[1]];
        if (argc == 3) fixtureCase = [NSString stringWithUTF8String:argv[2]];
        if (![@[ @"full", @"focus", @"menus", @"migration", @"direct", @"billiards" ]
                containsObject:fixtureCase])
            return 2;
        NSApplication* app = NSApplication.sharedApplication;
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        MirrorSmokeDelegate* delegate = [MirrorSmokeDelegate new];
        app.delegate = delegate;
        [app run];
    }
    close(lock);
    return resultCode;
}
