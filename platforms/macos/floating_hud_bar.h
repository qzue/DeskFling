#pragma once
// Sleek draggable floating capsule HUD bar + pill mask helper for macOS DPP.
// Uses NSVisualEffectView.maskImage inside a clear container view so macOS
// Sonoma/Sequoia WindowServer never paints a rectangular backdrop or rim box
// outside the rounded capsule.

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#include <algorithm>
#include <cmath>
#include "core/scene/playground.h"

inline void activateDPPApp() {
    if (@available(macOS 14.0, *)) {
        [NSApp activate];
    } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        [NSApp activateIgnoringOtherApps:YES];
#pragma clang diagnostic pop
    }
}

@interface DPPKeyablePanel : NSPanel
@end

@implementation DPPKeyablePanel
- (BOOL)canBecomeKeyWindow {
    return YES;
}
@end

@interface DPPFirstMouseButton : NSButton
@end

@implementation DPPFirstMouseButton
- (BOOL)acceptsFirstMouse:(NSEvent*)event {
    (void)event;
    return YES;
}
@end

@interface DPPFirstMouseSegmentedControl : NSSegmentedControl
@end

@implementation DPPFirstMouseSegmentedControl
- (BOOL)acceptsFirstMouse:(NSEvent*)event {
    (void)event;
    return YES;
}
@end

@interface DPPFloatingGripView : NSView {
    NSPoint _dragStartMouse;
    NSPoint _dragStartOrigin;
}
@property(copy) void (^didMoveOrigin)(NSPoint origin);
@end

@implementation DPPFloatingGripView
- (BOOL)acceptsFirstMouse:(NSEvent*)event {
    (void)event;
    return YES;
}
- (BOOL)mouseDownCanMoveWindow {
    return NO;
}
- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    [[NSColor colorWithWhite:1.0 alpha:0.46] setFill];
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 2; ++c) {
            NSRect dot = NSMakeRect(9.0 + c * 6.0, 11.5 + r * 7.0, 3.2, 3.2);
            [[NSBezierPath bezierPathWithOvalInRect:dot] fill];
        }
    }
}
- (void)mouseDown:(NSEvent*)event {
    (void)event;
    _dragStartMouse = NSEvent.mouseLocation;
    _dragStartOrigin = self.window.frame.origin;
}
- (void)mouseDragged:(NSEvent*)event {
    (void)event;
    if (!self.window) return;
    const NSPoint cur = NSEvent.mouseLocation;
    NSPoint next = NSMakePoint(_dragStartOrigin.x + (cur.x - _dragStartMouse.x),
                               _dragStartOrigin.y + (cur.y - _dragStartMouse.y));
    NSScreen* scr = self.window.screen ? self.window.screen : NSScreen.screens.firstObject;
    if (scr) {
        const NSRect vis = scr.visibleFrame;
        const NSSize sz = self.window.frame.size;
        next.x = std::clamp(next.x, NSMinX(vis) + 6.0, NSMaxX(vis) - sz.width - 6.0);
        next.y = std::clamp(next.y, NSMinY(vis) + 6.0, NSMaxY(vis) - sz.height - 6.0);
    }
    [self.window setFrameOrigin:next];
    if (self.didMoveOrigin) self.didMoveOrigin(next);
}
@end

@interface DPPFloatingHudBar : NSObject
@property(strong, readonly) NSPanel* window;
@property(strong) NSSegmentedControl* modeSegments;
@property(strong) NSButton* pauseButton;
@property(strong) NSButton* muteButton;
@property(strong) NSButton* settingsButton;

@property(copy) void (^onSelectMode)(dpp::PlayMode mode);
@property(copy) void (^onTogglePause)(void);
@property(copy) void (^onReset)(void);
@property(copy) void (^onToggleMute)(void);
@property(copy) void (^onToggleSettings)(void);
@property(copy) void (^onHideCapsule)(void);
@property(copy) void (^onStopSession)(void);
@property(copy) void (^onDidMove)(NSPoint newOrigin);
@property(copy) void (^onRestorePlaygroundFocus)(void);

+ (NSImage*)maskImageWithCornerRadius:(CGFloat)radius;
- (void)showInWorkArea:(NSRect)workArea;
- (void)hide;
- (BOOL)isVisible;
- (void)syncMode:(dpp::PlayMode)mode
             paused:(BOOL)paused
              muted:(BOOL)muted
    canUseBilliards:(BOOL)canBilliards
      settingsShown:(BOOL)settingsShown;
@end

@implementation DPPFloatingHudBar {
    NSPanel* _window;
}

+ (NSImage*)maskImageWithCornerRadius:(CGFloat)radius {
    const CGFloat r = std::max<CGFloat>(4.0, std::round(radius));
    const CGFloat side = r * 2.0 + 6.0;
    NSImage* img = [NSImage imageWithSize:NSMakeSize(side, side)
                                  flipped:NO
                           drawingHandler:^BOOL(NSRect dstRect) {
                             [NSColor.clearColor setFill];
                             NSRectFill(dstRect);
                             [NSColor.blackColor setFill];
                             [[NSBezierPath bezierPathWithRoundedRect:dstRect xRadius:r yRadius:r] fill];
                             return YES;
                           }];
    img.capInsets = NSEdgeInsetsMake(r, r, r, r);
    img.resizingMode = NSImageResizingModeStretch;
    return img;
}

- (NSPanel*)window {
    return _window;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        const NSRect rect = NSMakeRect(0, 0, 596, 44);
        const CGFloat radius = 22.0;
        _window = [[NSPanel alloc]
            initWithContentRect:rect
                      styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel
                        backing:NSBackingStoreBuffered
                          defer:NO];
        _window.releasedWhenClosed = NO;
        _window.hidesOnDeactivate = NO;
        _window.becomesKeyOnlyIfNeeded = YES;
        _window.opaque = NO;
        _window.backgroundColor = NSColor.clearColor;
        _window.hasShadow = YES;
        _window.level = NSFloatingWindowLevel + 2;
        _window.collectionBehavior =
            NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorFullScreenAuxiliary;

        // Clear root container prevents WindowServer from framing the NSPanel bounds as a rectangle.
        NSView* root = [[NSView alloc] initWithFrame:rect];
        root.wantsLayer = YES;
        root.layer.backgroundColor = NSColor.clearColor.CGColor;
        _window.contentView = root;

        NSVisualEffectView* glass = [[NSVisualEffectView alloc] initWithFrame:rect];
        glass.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        glass.material = NSVisualEffectMaterialHUDWindow;
        glass.blendingMode = NSVisualEffectBlendingModeBehindWindow;
        glass.state = NSVisualEffectStateActive;
        glass.appearance = [NSAppearance appearanceNamed:NSAppearanceNameVibrantDark];
        glass.maskImage = [DPPFloatingHudBar maskImageWithCornerRadius:radius];
        [root addSubview:glass];

        // Crisp anti-aliased inner pill hairline (no rectangular CALayer border).
        CAShapeLayer* rim = [CAShapeLayer layer];
        rim.frame = rect;
        CGPathRef rimPath =
            CGPathCreateWithRoundedRect(CGRectInset(rect, 0.75, 0.75), radius - 0.75, radius - 0.75, nullptr);
        rim.path = rimPath;
        CGPathRelease(rimPath);
        rim.fillColor = nil;
        rim.strokeColor = [NSColor colorWithWhite:1.0 alpha:0.22].CGColor;
        rim.lineWidth = 1.0;
        [root.layer addSublayer:rim];

        __weak DPPFloatingHudBar* weakSelf = self;
        DPPFloatingGripView* grip = [[DPPFloatingGripView alloc] initWithFrame:NSMakeRect(6, 2, 24, 40)];
        grip.toolTip = @"按住拖动悬浮胶囊条位置";
        grip.didMoveOrigin = ^(NSPoint origin) {
          [NSUserDefaults.standardUserDefaults setDouble:origin.x forKey:@"dpp.hudOriginX"];
          [NSUserDefaults.standardUserDefaults setDouble:origin.y forKey:@"dpp.hudOriginY"];
          [NSUserDefaults.standardUserDefaults setBool:YES forKey:@"dpp.hudHasSavedOrigin"];
          DPPFloatingHudBar* strongSelf = weakSelf;
          if (strongSelf && strongSelf.onDidMove) strongSelf.onDidMove(origin);
        };
        [glass addSubview:grip];

        self.modeSegments = [DPPFirstMouseSegmentedControl
            segmentedControlWithLabels:@[ @"重力", @"黑洞", @"台球", @"弹弓", @"曲棍球" ]
                          trackingMode:NSSegmentSwitchTrackingSelectOne
                                target:self
                                action:@selector(segmentChanged:)];
        self.modeSegments.frame = NSMakeRect(32, 8, 286, 28);
        self.modeSegments.selectedSegment = 0;
        [glass addSubview:self.modeSegments];

        auto addBtn = [&](NSString* title, NSString* tip, SEL action, NSRect frame) -> NSButton* {
            NSButton* b = [DPPFirstMouseButton buttonWithTitle:title target:self action:action];
            b.bezelStyle = NSBezelStyleRecessed;
            b.showsBorderOnlyWhileMouseInside = NO;
            b.font = [NSFont systemFontOfSize:12.0 weight:NSFontWeightMedium];
            b.frame = frame;
            b.toolTip = tip;
            [glass addSubview:b];
            return b;
        };

        self.pauseButton =
            addBtn(@"暂停", @"暂停 / 继续物理模拟 (Space)", @selector(pauseClicked:), NSMakeRect(324, 8, 44, 28));
        addBtn(@"重置", @"重置当前模式场景 (R)", @selector(resetClicked:), NSMakeRect(370, 8, 44, 28));
        self.muteButton =
            addBtn(@"音效", @"开启 / 静音合成音效", @selector(muteClicked:), NSMakeRect(416, 8, 44, 28));
        self.settingsButton = addBtn(@"设置", @"展开 / 收起当前玩法专属设置", @selector(settingsClicked:),
                                     NSMakeRect(462, 8, 46, 28));
        addBtn(@"—", @"隐藏悬浮条（按 Tab 键或点顶部菜单栏 DPP 图标随时唤出）", @selector(hideClicked:),
               NSMakeRect(510, 8, 26, 28));
        NSButton* stopBtn = addBtn(@"还原", @"立即恢复原桌面图标并停止会话 (Esc)", @selector(stopClicked:),
                                   NSMakeRect(538, 8, 48, 28));
        stopBtn.contentTintColor = [NSColor colorWithSRGBRed:1.0 green:0.48 blue:0.45 alpha:1.0];
    }
    return self;
}

- (void)showInWorkArea:(NSRect)workArea {
    const NSSize sz = _window.frame.size;
    NSPoint origin = NSMakePoint(NSMidX(workArea) - sz.width * 0.5, NSMaxY(workArea) - sz.height - 10.0);
    if ([NSUserDefaults.standardUserDefaults boolForKey:@"dpp.hudHasSavedOrigin"]) {
        const double sx = [NSUserDefaults.standardUserDefaults doubleForKey:@"dpp.hudOriginX"];
        const double sy = [NSUserDefaults.standardUserDefaults doubleForKey:@"dpp.hudOriginY"];
        if (std::isfinite(sx) && std::isfinite(sy)) {
            origin.x = std::clamp<CGFloat>(sx, NSMinX(workArea) + 6.0, NSMaxX(workArea) - sz.width - 6.0);
            origin.y = std::clamp<CGFloat>(sy, NSMinY(workArea) + 6.0, NSMaxY(workArea) - sz.height - 6.0);
        }
    }
    [_window setFrameOrigin:origin];
    [_window orderFrontRegardless];
    [_window invalidateShadow];
}

- (void)hide {
    [_window orderOut:nil];
}

- (BOOL)isVisible {
    return _window.visible;
}

- (void)syncMode:(dpp::PlayMode)mode
             paused:(BOOL)paused
              muted:(BOOL)muted
    canUseBilliards:(BOOL)canBilliards
      settingsShown:(BOOL)settingsShown {
    NSInteger seg = 0;
    switch (mode) {
        case dpp::PlayMode::Gravity:
            seg = 0;
            break;
        case dpp::PlayMode::BlackHole:
            seg = 1;
            break;
        case dpp::PlayMode::Billiards:
            seg = 2;
            break;
        case dpp::PlayMode::Slingshot:
            seg = 3;
            break;
        case dpp::PlayMode::AirHockey:
            seg = 4;
            break;
    }
    self.modeSegments.selectedSegment = seg;
    [self.modeSegments setEnabled:(mode == dpp::PlayMode::Billiards || canBilliards) forSegment:2];
    self.pauseButton.title = paused ? @"继续" : @"暂停";
    self.muteButton.title = muted ? @"静音" : @"音效";
    self.settingsButton.title = settingsShown ? @"收起" : @"设置";
}

- (void)segmentChanged:(id)sender {
    (void)sender;
    if (!self.onSelectMode) return;
    activateDPPApp();
    const NSInteger s = self.modeSegments.selectedSegment;
    const dpp::PlayMode pm =
        s == 1 ? dpp::PlayMode::BlackHole
               : (s == 2 ? dpp::PlayMode::Billiards
                         : (s == 3 ? dpp::PlayMode::Slingshot
                                   : (s == 4 ? dpp::PlayMode::AirHockey : dpp::PlayMode::Gravity)));
    self.onSelectMode(pm);
    if (self.onRestorePlaygroundFocus) self.onRestorePlaygroundFocus();
}
- (void)pauseClicked:(id)sender {
    (void)sender;
    activateDPPApp();
    if (self.onTogglePause) self.onTogglePause();
    if (self.onRestorePlaygroundFocus) self.onRestorePlaygroundFocus();
}
- (void)resetClicked:(id)sender {
    (void)sender;
    activateDPPApp();
    if (self.onReset) self.onReset();
    if (self.onRestorePlaygroundFocus) self.onRestorePlaygroundFocus();
}
- (void)muteClicked:(id)sender {
    (void)sender;
    activateDPPApp();
    if (self.onToggleMute) self.onToggleMute();
    if (self.onRestorePlaygroundFocus) self.onRestorePlaygroundFocus();
}
- (void)settingsClicked:(id)sender {
    (void)sender;
    activateDPPApp();
    if (self.onToggleSettings) self.onToggleSettings();
}
- (void)hideClicked:(id)sender {
    (void)sender;
    activateDPPApp();
    [self hide];
    if (self.onHideCapsule) self.onHideCapsule();
    if (self.onRestorePlaygroundFocus) self.onRestorePlaygroundFocus();
}
- (void)stopClicked:(id)sender {
    (void)sender;
    if (self.onStopSession) self.onStopSession();
}
@end
