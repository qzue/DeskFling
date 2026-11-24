#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#include <mach/mach.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include "core/scene/playground.h"
#include "platforms/macos/cached_draw_renderer.h"
#include "platforms/macos/desktop_access.h"
#include "platforms/macos/native_measurement.h"
using namespace dpp;
static double measureSeconds = 0;
static NSString* measurePath = nil;
static int processExitCode = 0;
static std::uint64_t residentBytes() {
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    return task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) ==
                   KERN_SUCCESS
               ? info.resident_size
               : 0;
}
static double nowSeconds() {
    return std::chrono::duration<double>(SteadyClock::now().time_since_epoch()).count();
}
@interface PlaygroundView : NSView {
    std::unique_ptr<Playground> game_;
    std::unique_ptr<dpp::mac::CachedDrawRenderer> renderer_;
    NSMutableArray* textures_;
    NSTimer* timer_;
    NSTimer* measurementTimer_;
    std::unique_ptr<dpp::mac::NativeMeasurement> measurement_;
    double previous_;
    BOOL inactive_;
    std::uint64_t initialResident_, peakResident_;
    double memorySampleTime_;
}
- (void)command:(NSInteger)tag;
- (void)setInactive:(BOOL)value;
- (void)shutdown;
@end
@implementation PlaygroundView
- (BOOL)isFlipped {
    return YES;
}
- (BOOL)acceptsFirstResponder {
    return YES;
}
- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        game_ = std::make_unique<Playground>(syntheticScene());
        self.wantsLayer = YES;
        self.layer.backgroundColor = [NSColor colorWithSRGBRed:.055 green:.085 blue:.12 alpha:1].CGColor;
        textures_ = [NSMutableArray array];
        // Six synthetic assets, cached once. No Desktop files are read by this app.
        NSArray<NSString*>* names = @[
            @"doc.fill", @"folder.fill", @"photo.fill", @"music.note", @"terminal.fill", @"archivebox.fill"
        ];
        for (NSString* name in names) {
            NSImage* symbol = [NSImage imageWithSystemSymbolName:name accessibilityDescription:nil];
            NSImage* raster = [NSImage imageWithSize:NSMakeSize(64, 64)
                                             flipped:NO
                                      drawingHandler:^BOOL(NSRect rect) {
                                        [[NSColor colorWithSRGBRed:.21 green:.48 blue:.77 alpha:1] setFill];
                                        [[NSBezierPath bezierPathWithRoundedRect:rect xRadius:13
                                                                         yRadius:13] fill];
                                        [symbol drawInRect:NSInsetRect(rect, 12, 12)];
                                        return YES;
                                      }];
            NSRect imageRect = NSMakeRect(0, 0, 64, 64);
            CGImageRef image = [raster CGImageForProposedRect:&imageRect context:nil hints:nil];
            if (!image) throw std::runtime_error("Unable to rasterize sandbox resource");
            [textures_ addObject:(__bridge id)image];
        }
        renderer_ = std::make_unique<dpp::mac::CachedDrawRenderer>(textures_);
        previous_ = nowSeconds();
        initialResident_ = peakResident_ = residentBytes();
        memorySampleTime_ = previous_;
        __weak PlaygroundView* weakSelf = self;
        timer_ = [NSTimer timerWithTimeInterval:1.0 / 60
                                        repeats:YES
                                          block:^(NSTimer*) {
                                            [weakSelf tick];
                                          }];
        timer_.tolerance = .002;
        [NSRunLoop.mainRunLoop addTimer:timer_ forMode:NSRunLoopCommonModes];
        NSTrackingArea* tracking =
            [[NSTrackingArea alloc] initWithRect:self.bounds
                                         options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                                                 NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
                                           owner:self
                                        userInfo:nil];
        [self addTrackingArea:tracking];
        if (measureSeconds > 0) {
            measurement_ = std::make_unique<dpp::mac::NativeMeasurement>(measureSeconds, measurePath);
            measurementTimer_ = [NSTimer timerWithTimeInterval:1
                                                       repeats:YES
                                                         block:^(NSTimer*) {
                                                           [weakSelf measure];
                                                         }];
            [NSRunLoop.mainRunLoop addTimer:measurementTimer_ forMode:NSRunLoopCommonModes];
        }
    }
    return self;
}
- (void)measure {
    try {
        BOOL visible = self.window.visible && (self.window.occlusionState & NSWindowOcclusionStateVisible);
        if (measurement_->sample(*game_, textures_.count, renderer_->layerCount(), !inactive_ && NSApp.active,
                                 visible, self.window.backingScaleFactor)) {
            const bool ok = measurement_->finish(*game_, true);
            processExitCode = ok ? 0 : 3;
            std::fprintf(stderr, "native_measurement=%s\n", ok ? "completed" : "report_write_failed");
            [NSApp terminate:nil];
        }
    } catch (...) {
        processExitCode = 3;
        [self shutdown];
        [NSApp terminate:nil];
    }
}
- (void)shutdown {
    [measurementTimer_ invalidate];
    measurementTimer_ = nil;
    try {
        if (measurement_ && !measurement_->finish(*game_, false)) processExitCode = 3;
    } catch (...) {
        processExitCode = 3;
    }
    [timer_ invalidate];
    timer_ = nil;
}
- (void)dealloc {
    [measurementTimer_ invalidate];
    [timer_ invalidate];
}
- (void)fail:(const char*)message {
    processExitCode = 3;
    [self shutdown];
    NSAlert* alert = [NSAlert new];
    alert.messageText = @"Sandbox stopped safely";
    alert.informativeText = [NSString stringWithUTF8String:message];
    [alert runModal];
    [NSApp terminate:nil];
}
- (void)tick {
    try {
        const double now = nowSeconds(), dt = now - previous_;
        previous_ = now;
        if (inactive_) return;
        if (now - memorySampleTime_ >= 1) {
            peakResident_ = std::max(peakResident_, residentBytes());
            memorySampleTime_ = now;
        }
        if (measurement_) measurement_->exercise(*game_);
        if (game_->tick(dt)) [self render];
        timer_.fireDate = [NSDate
            dateWithTimeIntervalSinceNow:game_->idle() ? .2
                                                       : std::max(0.001, 1.0 / 60 - (nowSeconds() - now))];
    } catch (const std::exception& e) {
        [self fail:e.what()];
    } catch (...) {
        [self fail:"Unknown C++ error"];
    }
}
- (void)render {
    const auto start = SteadyClock::now();
    renderer_->render(game_->drawList(), self.layer, self.bounds, self.window.backingScaleFactor);
    game_->perf.submit.add(elapsedMs(start));
    const auto stats = game_->mode().stats();
    self.window.title = [NSString stringWithFormat:@"DEVELOPMENT SANDBOX · %s · %d objects / %d awake%@ · "
                                                   @"Space pause · R reset · B mode · P stats · Esc exit",
                                                   modeName(game_->mode().kind()), stats.bodies,
                                                   stats.awakeBodies, game_->paused() ? @" · PAUSED" : @""];
}
- (void)sendPointer:(NSEvent*)event type:(InputType)type {
    try {
        if (measurement_ && type == InputType::PointerDown) measurement_->userInteraction();
        NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
        if (game_->idle()) previous_ = nowSeconds();
        const bool alt = (event.modifierFlags & NSEventModifierFlagOption) != 0;
        game_->input(
            {type, {static_cast<float>(p.x), static_cast<float>(p.y)}, nowSeconds(), kNoTexture, alt});
        timer_.fireDate = NSDate.date;
    } catch (const std::exception& e) {
        [self fail:e.what()];
    } catch (...) {
        [self fail:"Input error"];
    }
}
- (void)mouseDown:(NSEvent*)e {
    [self sendPointer:e type:InputType::PointerDown];
}
- (void)mouseDragged:(NSEvent*)e {
    [self sendPointer:e type:InputType::PointerMove];
}
- (void)mouseMoved:(NSEvent*)e {
    [self sendPointer:e type:InputType::PointerMove];
}
- (void)mouseExited:(NSEvent*)e {
    (void)e;
    // A hover boundary is not a button release. Continue the AppKit drag
    // sequence until mouseUp; focus loss still cancels through setInactive.
}
- (void)mouseUp:(NSEvent*)e {
    [self sendPointer:e type:InputType::PointerUp];
}
- (void)setInactive:(BOOL)value {
    if (value && measurement_) measurement_->focusLost();
    inactive_ = value;
    try {
        game_->input({InputType::PointerCancel, {}, nowSeconds()});
    } catch (...) {
        [self shutdown];
    }
    previous_ = nowSeconds();
    timer_.fireDate = value ? NSDate.distantFuture : NSDate.date;
}
- (void)command:(NSInteger)tag {
    try {
        if (measurement_) measurement_->userInteraction();
        if (tag == 1) game_->input({InputType::TogglePause, {}, 0});
        if (tag == 2) game_->reset();
        if (tag == 3)
            game_->switchMode(game_->mode().kind() == ModeKind::Gravity ? ModeKind::Billiards
                                                                        : ModeKind::Gravity);
        if (tag == 4) {
            const auto& p = game_->perf;
            NSAlert* a = [NSAlert new];
            a.messageText = @"Local performance · rolling samples";
            a.informativeText = [NSString
                stringWithFormat:
                    @"Physics P95 %.3f ms / P99 %.3f ms\nRules P95 %.3f ms\nDraw data P95 %.3f ms\nCA "
                    @"submission P95 %.3f ms\nActive callback interval P95 %.2f ms / P99 %.2f ms\nStalls "
                    @"(>33.34 ms): %zu\nCached synthetic images: %lu; layer pool: %lu\nResident MiB start / "
                    @"current / sampled peak: %.1f / %.1f / %.1f\nGPU / presented FPS / CPU %%: not "
                    @"measured\nIdle callbacks use 5 Hz; inactive timer is stopped.",
                    p.physics.percentile(.95), p.physics.percentile(.99), p.rules.percentile(.95),
                    p.draw.percentile(.95), p.submit.percentile(.95), p.interval.percentile(.95),
                    p.interval.percentile(.99), p.stalls, (unsigned long)textures_.count,
                    (unsigned long)(renderer_->layerCount()), static_cast<double>(initialResident_) / 1048576,
                    static_cast<double>(residentBytes()) / 1048576,
                    static_cast<double>(peakResident_) / 1048576];
            [a runModal];
            previous_ = nowSeconds();
        }
        timer_.fireDate = NSDate.date;
    } catch (const std::exception& e) {
        [self fail:e.what()];
    } catch (...) {
        [self fail:"Command error"];
    }
}
- (void)keyDown:(NSEvent*)event {
    if (event.isARepeat) return;
    if (event.keyCode == 53) {
        [NSApp terminate:nil];
        return;
    }
    NSString* key = event.charactersIgnoringModifiers.lowercaseString;
    if ([key isEqualToString:@" "])
        [self command:1];
    else if ([key isEqualToString:@"r"])
        [self command:2];
    else if ([key isEqualToString:@"b"])
        [self command:3];
    else if ([key isEqualToString:@"p"])
        [self command:4];
    else
        [super keyDown:event];
}
@end
@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property(strong) NSWindow* window;
@property(strong) PlaygroundView* playground;
@property(strong) NSStatusItem* status;
@property(strong) DPPDesktopAccess* desktopAccess;
@end
@implementation AppDelegate
- (void)applicationDidFinishLaunching:(NSNotification*)note {
    (void)note;
    try {
        self.window =
            [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1100, 700)
                                        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                  NSWindowStyleMaskMiniaturizable
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
        self.window.delegate = self;
        self.window.releasedWhenClosed = NO;
        self.playground = [[PlaygroundView alloc] initWithFrame:NSMakeRect(0, 0, 1100, 700)];
        self.window.contentView = self.playground;
        NSMenu* menu = [NSMenu new];
        for (NSString* title in
             @[ @"Pause / Resume", @"Reset", @"Switch Gravity / Billiards", @"Performance" ]) {
            NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:title
                                                          action:@selector(action:)
                                                   keyEquivalent:@""];
            item.tag = menu.numberOfItems + 1;
            item.target = self;
            [menu addItem:item];
        }
        NSMenuItem* access = [[NSMenuItem alloc] initWithTitle:@"桌面接入检查"
                                                        action:@selector(showDesktopAccess:)
                                                 keyEquivalent:@""];
        access.target = self;
        [menu addItem:access];
        NSMenuItem* closeMirror = [[NSMenuItem alloc] initWithTitle:@"关闭桌面重力开发预览"
                                                             action:@selector(closeMirror:)
                                                      keyEquivalent:@""];
        closeMirror.target = self;
        [menu addItem:closeMirror];
        [menu addItem:NSMenuItem.separatorItem];
        NSMenuItem* disabled = [[NSMenuItem alloc] initWithTitle:@"Real desktop mode unavailable"
                                                          action:nil
                                                   keyEquivalent:@""];
        [menu addItem:disabled];
        [menu addItemWithTitle:@"Quit Sandbox" action:@selector(terminate:) keyEquivalent:@"q"];
        NSMenu* main = [NSMenu new];
        NSMenuItem* root = [NSMenuItem new];
        root.submenu = menu;
        [main addItem:root];
        NSApp.mainMenu = main;
        self.status = [NSStatusBar.systemStatusBar statusItemWithLength:NSVariableStatusItemLength];
        self.status.button.title = @"DPP";
        self.status.menu = [menu copy];
        [self.window center];
        [self.window makeKeyAndOrderFront:nil];
        [self.window makeFirstResponder:self.playground];
        [NSApp activateIgnoringOtherApps:YES];
        // Safe finite smoke run: opens only this sandbox and exits normally.
        if ([NSProcessInfo.processInfo.arguments containsObject:@"--smoke-test"])
            [NSTimer scheduledTimerWithTimeInterval:3
                                            repeats:NO
                                              block:^(NSTimer*) {
                                                std::puts("sandbox_smoke=completed");
                                                [NSApp terminate:nil];
                                              }];
    } catch (const std::exception& e) {
        processExitCode = 3;
        std::fprintf(stderr, "startup: %s\n", e.what());
        [NSApp terminate:nil];
    } catch (...) {
        processExitCode = 3;
        std::fprintf(stderr, "startup failed\n");
        [NSApp terminate:nil];
    }
}
- (void)showDesktopAccess:(id)sender {
    (void)sender;
    @try {
        try {
            if (!self.desktopAccess) self.desktopAccess = [DPPDesktopAccess new];
            [self.desktopAccess show];
        } catch (...) {
            NSLog(@"Could not open desktop access panel.");
        }
    } @catch (NSException*) {
        NSLog(@"Could not open desktop access panel.");
    }
}
- (void)closeMirror:(id)sender {
    (void)sender;
    [self.desktopAccess closeMirror];
}
- (void)action:(NSMenuItem*)item {
    [self.playground command:item.tag];
}
- (void)windowDidResignKey:(NSNotification*)n {
    (void)n;
    [self.playground setInactive:YES];
}
- (void)windowDidBecomeKey:(NSNotification*)n {
    (void)n;
    [self.playground setInactive:NO];
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)app {
    (void)app;
    return YES;
}
- (void)applicationWillTerminate:(NSNotification*)note {
    (void)note;
    [self.desktopAccess shutdown];
    [self.playground shutdown];
    [NSStatusBar.systemStatusBar removeStatusItem:self.status];
}
@end
int main(int argc, char** argv) {
    @autoreleasepool {
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "--measure-seconds") == 0 && i + 1 < argc) {
                char* end = nullptr;
                measureSeconds = std::strtod(argv[++i], &end);
                if (!end || *end || !std::isfinite(measureSeconds) || measureSeconds < 10 ||
                    measureSeconds > 3600)
                    return 2;
            } else if (std::strcmp(argv[i], "--report") == 0 && i + 1 < argc)
                measurePath = [NSString stringWithUTF8String:argv[++i]];
            else if (std::strcmp(argv[i], "--smoke-test") != 0)
                return 2;
        }
        if ((measureSeconds > 0) != static_cast<bool>(measurePath)) return 2;
        NSApplication* app = NSApplication.sharedApplication;
        if (NSScreen.screens.count == 0) {
            std::fprintf(stderr, "No GUI display available; run in a logged-in macOS session.\n");
            return 2;
        }
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        AppDelegate* delegate = [AppDelegate new];
        app.delegate = delegate;
        [app run];
    }
    return processExitCode;
}
