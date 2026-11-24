// Explicit native-handler fixture; not a real mouse/visual acceptance test.
// Reuse the actual view implementation without running the application entry.
#define main dpp_sandbox_entry_for_drag_fixture
#include "platforms/macos/sandbox.mm"
#undef main
#include "core/scene/coordinates.h"

@interface PlaygroundView (DragFixture)
- (BOOL)fixtureGrabbing;
- (NSPoint)fixtureBodyPoint;
- (void)fixtureStep;
@end
@implementation PlaygroundView (DragFixture)
- (BOOL)fixtureGrabbing {
    return game_->mode().stats().grabbing;
}
- (NSPoint)fixtureBodyPoint {
    auto p = SceneToPhysics({1100, 700}).toScene(game_->mode().world().state(0).position);
    return NSMakePoint(p.x, p.y);
}
- (void)fixtureStep {
    game_->tick(1.0 / 120);
}
@end

int main() {
    @autoreleasepool {
        try {
            [NSApplication sharedApplication];
            NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1100, 700)
                                                           styleMask:NSWindowStyleMaskTitled
                                                             backing:NSBackingStoreBuffered
                                                               defer:NO];
            PlaygroundView* view = [[PlaygroundView alloc] initWithFrame:NSMakeRect(0, 0, 1100, 700)];
            window.contentView = view;
            auto event = [&](NSEventType type, NSPoint point) {
                return [NSEvent mouseEventWithType:type
                                          location:[view convertPoint:point toView:nil]
                                     modifierFlags:0
                                         timestamp:NSProcessInfo.processInfo.systemUptime
                                      windowNumber:window.windowNumber
                                           context:nil
                                       eventNumber:0
                                        clickCount:1
                                          pressure:1];
            };
            bool passed = true;
            auto check = [&](bool ok, const char* label) {
                std::printf("%s=%s\n", label, ok ? "passed" : "FAILED");
                passed = passed && ok;
            };
            [view mouseDown:event(NSEventTypeLeftMouseDown, [view fixtureBodyPoint])];
            check([view fixtureGrabbing], "initial_grab");
            for (int i = 0; i < 120; ++i) {
                [view mouseDragged:event(NSEventTypeLeftMouseDragged,
                                         i % 2 ? NSMakePoint(100, 100) : NSMakePoint(1000, 600))];
                [view fixtureStep];
            }
            check([view fixtureGrabbing], "rapid_moves_inside_keep_grab");
            [view mouseDragged:event(NSEventTypeLeftMouseDragged, NSMakePoint(1200, 800))];
            [view mouseExited:[NSEvent enterExitEventWithType:NSEventTypeMouseExited
                                                     location:NSMakePoint(1200, 800)
                                                modifierFlags:0
                                                    timestamp:NSProcessInfo.processInfo.systemUptime
                                                 windowNumber:window.windowNumber
                                                      context:nil
                                                  eventNumber:0
                                               trackingNumber:0
                                                     userData:nullptr]];
            check([view fixtureGrabbing], "boundary_exit_keeps_held_grab");
            [view mouseDragged:event(NSEventTypeLeftMouseDragged, NSMakePoint(500, 350))];
            check([view fixtureGrabbing], "return_without_repress_keeps_grab");
            [view mouseUp:event(NSEventTypeLeftMouseUp, NSMakePoint(500, 350))];
            check(![view fixtureGrabbing], "button_up_releases");
            [view mouseDown:event(NSEventTypeLeftMouseDown, [view fixtureBodyPoint])];
            check([view fixtureGrabbing], "regrab_before_focus_loss");
            [view setInactive:YES];
            check(![view fixtureGrabbing], "focus_loss_cancels");
            [view setInactive:NO];
            [view mouseDragged:event(NSEventTypeLeftMouseDragged, NSMakePoint(500, 350))];
            check(![view fixtureGrabbing], "focus_return_does_not_regrab");
            [view mouseDown:event(NSEventTypeLeftMouseDown, [view fixtureBodyPoint])];
            check([view fixtureGrabbing], "fresh_press_regrabs");
            [view setInactive:YES];
            [view shutdown];
            [window orderOut:nil];
            std::printf("synthetic_native_handlers_only=true visible_window=false desktop_read=false\n");
            return passed ? 0 : 1;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "fixture error: %s\n", e.what());
            return 2;
        } catch (...) {
            return 2;
        }
    }
}
