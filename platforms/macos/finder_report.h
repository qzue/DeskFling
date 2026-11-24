#pragma once
#include <chrono>
#include <future>
#include "platforms/macos/finder_snapshot.h"
namespace dpp::mac {
inline NSDictionary* finderReport() {
    if ([NSRunningApplication runningApplicationsWithBundleIdentifier:@"com.apple.finder"].count == 0)
        return @{@"status" : @"finder_not_running", @"launched_finder" : @NO};
    NSAppleEventDescriptor* target =
        [NSAppleEventDescriptor descriptorWithBundleIdentifier:@"com.apple.finder"];
    // Even permission preflight runs off the main thread; askUserIfNeeded=false.
    OSStatus permission =
        std::async(std::launch::async, [target] {
            return AEDeterminePermissionToAutomateTarget(target.aeDesc, kAECoreSuite, kAEGetData, false);
        }).get();
    if (permission != noErr)
        return @{
            @"status" : permissionName(permissionFromStatus(permission)),
            @"permission_osstatus" : @(permission),
            @"prompted" : @NO,
            @"layout" : NSNull.null
        };
    NSAppleEventDescriptor* desktop = property('desk');
    ReadResult items = readFinder(target, allItems(desktop));
    if (items.error || items.value.descriptorType != typeAEList)
        return @{
            @"status" : @"enumeration_failed",
            @"osstatus" : @(items.error),
            @"descriptor_type" : fourCC(items.value.descriptorType)
        };
    NSAppleEventDescriptor* window =
        object('dktw', formAbsolutePosition, [NSAppleEventDescriptor descriptorWithInt32:1]);
    ReadResult windowBounds = readFinder(target, property('pbnd', window));
    ReadResult iconSize = readFinder(target, property('lvis', property('icop', window)));
    ReadResult arrangement = readFinder(target, property('iarr', property('icop', window)));
    NSMutableArray* rows = [NSMutableArray array];
    NSUInteger positions = 0, bounds = 0, images = 0, volumes = 0;
    auto start = std::chrono::steady_clock::now();
    for (NSInteger i = 1; i <= items.value.numberOfItems && i <= 128; ++i) {
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() > 15) break;
        NSAppleEventDescriptor* item = [items.value descriptorAtIndex:i];
        ReadResult position = readFinder(target, property('dpos', item));
        ReadResult rectangle = readFinder(target, property('pbnd', item));
        ReadResult cls = readFinder(target, property('pcls', item));
        ReadResult url = readFinder(target, property('pURL', item));
        NSArray* xy = geometry(position.value, 2);
        NSArray* ltrb = geometry(rectangle.value, 4);
        if (xy) ++positions;
        if (ltrb) ++bounds;
        const bool volume = cls.value.typeCodeValue == 'cdis';
        if (volume) ++volumes;
        bool imageOK = false;
        // Finder supplies the URL for this desktop item. Keep it in memory; never persist paths/names.
        NSURL* fileURL = url.error ? nil : [NSURL URLWithString:url.value.stringValue];
        if (fileURL.isFileURL) {
            NSImage* image = [NSWorkspace.sharedWorkspace iconForFile:fileURL.path];
            NSRect rect = NSMakeRect(0, 0, 64, 64);
            CGImageRef decoded = [image CGImageForProposedRect:&rect context:nil hints:nil];
            imageOK = decoded && CGImageGetWidth(decoded) > 0;
            if (imageOK) ++images;
        }
        [rows addObject:@{
            @"id" : @(i),
            @"finder_class_code" : fourCC(cls.value.typeCodeValue),
            @"volume" : @(volume),
            @"desktop_position_raw" : xy ? xy : static_cast<id>(NSNull.null),
            @"item_bounds_raw_ltrb" : ltrb ? ltrb : static_cast<id>(NSNull.null),
            @"position_error" : @(position.error),
            @"bounds_error" : @(rectangle.error),
            @"class_error" : @(cls.error),
            @"url_error" : @(url.error),
            @"position_descriptor_type" : fourCC(position.value.descriptorType),
            @"icon_image_decoded" : @(imageOK)
        }];
    }
    NSMutableDictionary* preferences = [NSMutableDictionary dictionary];
    for (NSNumber* code in @[
             @(static_cast<unsigned>('pdhd')), @(static_cast<unsigned>('pehd')),
             @(static_cast<unsigned>('pdrm')), @(static_cast<unsigned>('pdsv'))
         ]) {
        ReadResult result = readFinder(target, property(code.unsignedIntValue, property('pfrp')));
        preferences[fourCC(code.unsignedIntValue)] = @{
            @"value" : result.error ? static_cast<id>(NSNull.null) : @(result.value.booleanValue),
            @"error" : @(result.error)
        };
    }
    return @{
        @"status" : rows.count == static_cast<NSUInteger>(items.value.numberOfItems) &&
                positions == rows.count
            ? @"read_raw_layout"
            : @"partial_or_failed_layout",
        @"permission_osstatus" : @(permission),
        @"prompted" : @NO,
        @"enumerated" : @(items.value.numberOfItems),
        @"inspected" : @(rows.count),
        @"truncated" : @(rows.count < static_cast<NSUInteger>(items.value.numberOfItems)),
        @"position_success" : @(positions),
        @"position_failed" : @(rows.count - positions),
        @"bounds_success" : @(bounds),
        @"image_decoded" : @(images),
        @"volumes" : @(volumes),
        @"desktop_window_bounds_raw_ltrb" : geometry(windowBounds.value, 4) ? geometry(windowBounds.value, 4)
                                                                            : static_cast<id>(NSNull.null),
        @"window_bounds_error" : @(windowBounds.error),
        @"icon_size_raw" : iconSize.error ? static_cast<id>(NSNull.null) : @(iconSize.value.int32Value),
        @"icon_size_error" : @(iconSize.error),
        @"arrangement_code" : arrangement.error ? static_cast<id>(NSNull.null)
                                                : fourCC(arrangement.value.enumCodeValue),
        @"arrangement_error" : @(arrangement.error),
        @"volume_visibility_preferences_read_only" : preferences,
        @"items" : rows,
        @"coordinate_contract" : @"Finder.sdef desktop position and item bounds, raw values; "
                                 @"unit/origin/anchor require independent validation",
        @"image_rect" : @"unknown: item bounds are not assumed to be image-only bounds",
        @"label_rect" : @"unknown",
        @"stacks" : @"not exposed in inspected Finder.sdef; display correspondence unverified",
        @"code" : @"platforms/macos/finder_read.h + probe.mm:finderReport"
    };
}
}  // namespace dpp::mac
