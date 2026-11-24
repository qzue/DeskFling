#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#include <chrono>
#include <cstring>
#include <future>
#include "platforms/macos/diagnostics.h"
#include "platforms/macos/finder_report.h"
#include "platforms/macos/window_probe.h"
using namespace dpp::mac;
namespace {
NSDictionary* directoryReport() {
    NSURL* desktop = [NSFileManager.defaultManager URLsForDirectory:NSDesktopDirectory
                                                          inDomains:NSUserDomainMask]
                         .firstObject;
    NSError* error = nil;
    NSArray<NSURL*>* entries =
        [NSFileManager.defaultManager contentsOfDirectoryAtURL:desktop
                                    includingPropertiesForKeys:@[ NSURLIsUbiquitousItemKey ]
                                                       options:NSDirectoryEnumerationSkipsHiddenFiles
                                                         error:&error];
    if (error)
        return @{
            @"status" : @"denied_or_failed",
            @"error_domain" : error.domain,
            @"error_code" : @(error.code),
            @"enumerated" : NSNull.null
        };
    NSUInteger imageOK = 0, imageFailed = 0, cloud = 0, metadataFailed = 0;
    for (NSURL* entry in entries) {
        NSImage* image = [NSWorkspace.sharedWorkspace iconForFile:entry.path];
        NSRect r = NSMakeRect(0, 0, 64, 64);
        CGImageRef decoded = [image CGImageForProposedRect:&r context:nil hints:nil];
        if (decoded && CGImageGetWidth(decoded) > 0 && CGImageGetHeight(decoded) > 0)
            ++imageOK;
        else
            ++imageFailed;
        NSNumber* ubiquitous = nil;
        if ([entry getResourceValue:&ubiquitous forKey:NSURLIsUbiquitousItemKey error:nil]) {
            if (ubiquitous.boolValue) ++cloud;
        } else
            ++metadataFailed;
    }
    return @{
        @"status" : @"read",
        @"enumerated" : @(entries.count),
        @"image_decoded" : @(imageOK),
        @"image_failed" : @(imageFailed),
        @"ubiquitous_items" : @(cloud),
        @"metadata_failed" : @(metadataFailed),
        @"scope" : @"NSDesktopDirectory only; hidden entries skipped; no recursion or content download",
        @"finder_layout" : @"not_implied",
        @"finder_thumbnail_equivalence" : @"unknown",
        @"code" : @"platforms/macos/probe.mm:directoryReport"
    };
}

}  // namespace
int main(int argc, char** argv) {
    @autoreleasepool {
        bool files = false, finder = false, windows = false;
        NSString* output = nil;
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "--desktop-files") == 0)
                files = true;
            else if (std::strcmp(argv[i], "--finder-layout") == 0)
                finder = true;
            else if (std::strcmp(argv[i], "--window-test") == 0)
                windows = true;
            else if (std::strcmp(argv[i], "--output") == 0 && i + 1 < argc)
                output = [NSString stringWithUTF8String:argv[++i]];
            else {
                std::fprintf(stderr, "Usage: desktop_probe [--desktop-files] [--finder-layout] "
                                     "[--window-test] [--output NEW.json]\n");
                return 2;
            }
        }
        [NSApplication sharedApplication];
        NSDictionary* directory = files ? directoryReport() : @{@"status" : @"not_requested"};
        NSDictionary* layout = finder ? finderReport() : @{@"status" : @"not_requested"};
        NSDictionary* window = windows ? windowReport() : @{@"status" : @"not_requested"};
        NSDictionary* report = @{
            @"schema_version" : @2,
            @"environment" : environment(),
            @"screens" : screens(),
            @"accessibility_trusted" : @(AXIsProcessTrusted()),
            @"directory" : directory,
            @"finder" : layout,
            @"real_desktop_mode" : @"disabled",
            @"multiscreen_real_mode" : @"disabled",
            @"original_icon_suppression" : @"no_constraint_compliant_path_verified",
            @"private_names_or_paths_in_report" : @NO,
            @"window_test" : window
        };
        if (!writeJSON(report, output)) {
            std::fprintf(stderr, "Report creation failed (existing output is not overwritten).\n");
            return 4;
        }
        if ((files && ![directory[@"status"] isEqual:@"read"]) ||
            (finder && ![layout[@"status"] isEqual:@"read_raw_layout"]) ||
            (windows && ![window[@"status"] isEqual:@"specified_metadata_checks_passed"]))
            return 3;
    }
    return 0;
}
