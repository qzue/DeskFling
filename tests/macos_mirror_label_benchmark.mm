// Synthetic CPU submission benchmark. No Desktop reads, permissions, screenshots or takeover.
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#include "platforms/macos/mirror_preview.h"
#include "platforms/macos/native_measurement.h"
using namespace dpp;
using namespace dpp::mac;
int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc < 2 || argc > 3) return 2;
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
        [NSApp finishLaunching];
        NSMutableArray* cases = [NSMutableArray array];
        for (NSUInteger count : {18U, 50U, 128U}) {
            for (NSUInteger repetition = 0; repetition < 3; ++repetition) {
                @autoreleasepool {
                    DPPMirrorPreview* preview = [DPPMirrorPreview new];
                    preview.experimentID = @"protected_finder";
                    preview.deferPresentation = YES;
                    if (argc == 3) [preview setValue:@NO forKey:@"showsFilenames"];
                    NSRect f = NSScreen.screens.firstObject.frame;
                    NSRect w = NSScreen.screens.firstObject.visibleFrame;
                    MirrorSnapshot s;
                    s.screen = {float(f.origin.x), float(f.origin.y), float(f.size.width),
                                float(f.size.height)};
                    s.workArea = {float(w.origin.x), float(w.origin.y), float(w.size.width),
                                  float(w.size.height)};
                    s.singleScreen = true;
                    s.fullDesktop = true;
                    s.finderItemCount = count;
                    s.capturedAt = NSProcessInfo.processInfo.systemUptime;
                    NSImage* icon = [NSImage imageWithSystemSymbolName:@"doc.text.fill"
                                              accessibilityDescription:nil];
                    NSRect rect = NSMakeRect(0, 0, 64, 64);
                    CGImageRef image = [icon CGImageForProposedRect:&rect context:nil hints:nil];
                    if (!image) return 3;
                    NSMutableArray* images = [NSMutableArray array];
                    NSMutableArray* labels = [NSMutableArray array];
                    for (NSUInteger i = 0; i < count; ++i) {
                        MirrorItem item;
                        item.id = IconId{uint32_t(i + 1)};
                        item.texture = TextureId{uint32_t(i + 1)};
                        item.sceneAnchor = {100.0f + float(i % 16) * 90, 100.0f + float(i / 16) * 90};
                        item.rawDpos = macGlobalPointsToScene(
                            macSceneToGlobalPoints(item.sceneAnchor, s.workArea), s.screen);
                        item.coordinateValidated = true;
                        item.syntheticFixture = true;
                        s.items.push_back(item);
                        [images addObject:(__bridge id)image];
                        [labels
                            addObject:[NSString stringWithFormat:@"合成文件-%lu-Document-preview.pdf", i]];
                    }
                    preview.labels = labels;
                    if (![preview start:s images:images]) return 4;
                    Samples stationary, moving;
                    for (NSUInteger frame = 0; frame < 390; ++frame) {
                        @autoreleasepool {
                            const double ms = [preview fixtureSubmitFrame:NO reset:NO];
                            if (frame >= 30) stationary.add(ms);
                            [CATransaction flush];
                        }
                    }
                    NSDictionary* still = [preview diagnostics];
                    for (NSUInteger frame = 0; frame < 390; ++frame) {
                        @autoreleasepool {
                            const double ms = [preview fixtureSubmitFrame:YES reset:frame % 120 == 0];
                            if (frame >= 30) moving.add(ms);
                            [CATransaction flush];
                        }
                    }
                    NSDictionary* motion = [preview diagnostics];
                    [preview close:@"synthetic_label_benchmark"];
                    NSDictionary* cleanup = preview.sessionSummaries.lastObject;
                    if ([cleanup[@"owned_layers_after_close"] intValue] ||
                        [cleanup[@"owned_bodies_after_close"] intValue])
                        return 5;
                    [cases addObject:@{
                        @"items" : @(count),
                        @"repetition" : @(repetition),
                        @"labels_requested" : @(argc == 2),
                        @"stationary_submit" : statistics(stationary),
                        @"moving_submit" : statistics(moving),
                        @"stationary_string_writes" : still[@"filename_string_writes"],
                        @"total_string_writes" : motion[@"filename_string_writes"],
                        @"total_scale_writes" : motion[@"filename_scale_writes"],
                        @"total_geometry_writes" : motion[@"filename_geometry_writes"],
                        @"cleanup_layers" : cleanup[@"owned_layers_after_close"],
                        @"cleanup_bodies" : cleanup[@"owned_bodies_after_close"]
                    }];
                }
            }
        }
        NSDictionary* report = @{
            @"classification" : @"synthetic_cpu_layer_submission_microbenchmark_deferred_window",
            @"frames_per_phase" : @390,
            @"warmup_frames_per_phase" : @30,
            @"gpu_ms" : NSNull.null,
            @"presented_fps" : NSNull.null,
            @"finder_events_sent" : @NO,
            @"desktop_settings_written" : @NO,
            @"scale" : @(NSScreen.screens.firstObject.backingScaleFactor),
            @"cases" : cases
        };
        NSData* data = [NSJSONSerialization dataWithJSONObject:report
                                                       options:NSJSONWritingPrettyPrinted
                                                         error:nil];
        NSString* output = [NSString stringWithUTF8String:argv[1]];
        return data && [data writeToFile:output options:NSDataWritingWithoutOverwriting error:nil] ? 0 : 6;
    }
}
