#pragma once
#include <chrono>
#include <functional>
#include "platforms/macos/desktop_access_state.h"
#include "platforms/macos/finder_read.h"
namespace dpp::mac {
inline NSString* permissionName(Permission state) {
    switch (state) {
        case Permission::Unknown:
            return @"not_checked";
        case Permission::Granted:
            return @"granted_for_core_getd";
        case Permission::NeedsConsent:
            return @"needs_user_consent";
        case Permission::NotPermitted:
            return @"not_permitted_check_configuration_and_user_choice";
        case Permission::Timeout:
            return @"timeout";
        case Permission::TargetUnavailable:
            return @"target_unavailable";
        case Permission::Failed:
            return @"configuration_or_other_failure";
    }
}
inline OSStatus finderPermission(bool ask) {
    if ([NSRunningApplication runningApplicationsWithBundleIdentifier:@"com.apple.finder"].count == 0)
        return procNotFound;
    auto target = [NSAppleEventDescriptor descriptorWithBundleIdentifier:@"com.apple.finder"];
    return AEDeterminePermissionToAutomateTarget(target.aeDesc, kAECoreSuite, kAEGetData, ask);
}
// URL identity, never display-name identity. Retained URLs are in-memory only.
inline NSString* urlIdentity(NSURL* url) { return url.isFileURL ? url.standardizedURL.absoluteString : nil; }
using FinderReader = std::function<ReadResult(NSAppleEventDescriptor*)>;
// Finder.sdef: desktop -> container window -> icon view options -> icon size. Read only.
// A view setting is not an image/label rectangle or a measured pixel boundary.
inline NSDictionary* desktopIconSize(const FinderReader& read) {
    auto window = property('cwnd', property('desk'));
    auto result = read(property('lvis', property('icop', window)));
    const bool numeric = !result.error && result.value && numericType(result.value.descriptorType) &&
                         std::isfinite(result.value.doubleValue);
    const double size = numeric ? result.value.doubleValue : 0;
    return @{
        @"raw" : numeric ? @(size) : static_cast<id>(NSNull.null),
        @"usable" : @(numeric && size >= 16 && size <= 128 && std::floor(size) == size),
        @"osstatus" : @(result.error),
        @"descriptor_type" : fourCC(result.value.descriptorType),
        @"source" : @"Finder_desk_cwnd_icop_lvis",
        @"draw_contract" : @"configuration_value_used_as_AppKit_points; visual_size_unverified",
        @"system_image_rect" : NSNull.null
    };
}
struct FinderSnapshot {
    NSDictionary* report = @{@"request" : @"not_requested", @"parse" : @"not_attempted"};
    NSArray<NSURL*>* urls = @[];
    NSArray<NSAppleEventDescriptor*>* references = @[];
};
// Injected reader permits complete offline reply/error tests. Production sends only core/getd.
inline FinderSnapshot selectedSnapshotImpl(NSArray<NSURL*>* selected, const FinderReader& read) {
    if (selected.count < 1 || selected.count > 3) return {
            @{@"request" : @"invalid_selection", @"parse" : @"not_attempted"}, @[]
        };
    ReadResult version = read(property('vers'));
    if (version.error ||
        !(version.value.descriptorType == typeUnicodeText || version.value.descriptorType == typeUTF8Text ||
          version.value.descriptorType == typeChar) ||
        !version.value.stringValue)
        return {
            @{
                @"request" : @"failed",
                @"stage" : @"minimal_version_query",
                @"osstatus" : @(version.error),
                @"descriptor_type" : fourCC(version.value.descriptorType),
                @"parse" : @"invalid_or_missing_text"
            },
                @[]
        };
    ReadResult items = read(allItems(property('desk')));
    if (items.error || items.value.descriptorType != typeAEList) return {
            @{
                @"request" : @"failed",
                @"stage" : @"desktop_item_list",
                @"osstatus" : @(items.error),
                @"descriptor_type" : fourCC(items.value.descriptorType),
                @"parse" : @"invalid_or_missing_list"
            },
                @[]
        };
    NSMutableArray* rows = [NSMutableArray array];
    NSMutableArray<NSURL*>* urls = [NSMutableArray array];
    NSMutableArray* references = [NSMutableArray array];
    NSMutableSet* found = [NSMutableSet set];
    NSUInteger inspected = 0, success = 0, requestFailures = 0, urlFailures = 0;
    const auto start = std::chrono::steady_clock::now();
    for (NSInteger i = 1; i <= items.value.numberOfItems && i <= 128; ++i) {
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() > 15) break;
        ++inspected;
        auto item = [items.value descriptorAtIndex:i];
        if (item.descriptorType != typeObjectSpecifier && item.descriptorType != typeAlias &&
            item.descriptorType != typeFileURL) {
            ++urlFailures;
            continue;
        }
        ReadResult url = read(property('pURL', item));
        NSURL* file = !url.error && url.value.stringValue ? [NSURL URLWithString:url.value.stringValue] : nil;
        NSString* identity = urlIdentity(file);
        if (!identity) {
            ++urlFailures;
            continue;
        }
        NSUInteger selectedIndex = NSNotFound;
        for (NSUInteger j = 0; j < selected.count; ++j)
            if ([identity isEqual:urlIdentity(selected[j])]) {
                selectedIndex = j;
                break;
            }
        if (selectedIndex == NSNotFound || [found containsObject:identity]) continue;
        [found addObject:identity];
        ReadResult position = read(property('dpos', item));
        ReadResult bounds = read(property('pbnd', item));
        ReadResult afterURL = read(property('pURL', item));
        NSURL* after = !afterURL.error && afterURL.value.stringValue
                           ? [NSURL URLWithString:afterURL.value.stringValue]
                           : nil;
        const bool stable = [identity isEqual:urlIdentity(after)];
        NSArray* xy = !position.error && stable ? geometry(position.value, 2) : nil;
        NSArray* ltrb = !bounds.error && stable ? geometry(bounds.value, 4) : nil;
        if (xy) ++success;
        if (position.error) ++requestFailures;
        if (bounds.error) ++requestFailures;
        if (afterURL.error) ++requestFailures;
        [urls addObject:file];
        [references addObject:item];
        [rows addObject:@{
            @"id" : @(selectedIndex + 1),
            @"position_source" : @"system_returned_dpos",
            @"position_raw" : xy ? xy : static_cast<id>(NSNull.null),
            @"item_bounds_raw_ltrb" : ltrb ? ltrb : static_cast<id>(NSNull.null),
            @"position_descriptor_type" : fourCC(position.value.descriptorType),
            @"bounds_descriptor_type" : fourCC(bounds.value.descriptorType),
            @"position_osstatus" : @(position.error),
            @"bounds_osstatus" : @(bounds.error),
            @"identity_recheck_osstatus" : @(afterURL.error),
            @"identity_stable" : @(stable),
            @"image_rect" : NSNull.null,
            @"label_rect" : NSNull.null,
            @"image_rect_validation" : @"unknown",
            @"label_rect_validation" : @"unknown"
        }];
        if (found.count == selected.count) break;
    }
    return {
        @{
            @"request" : requestFailures || urlFailures ? @"partial_failure" : @"read",
            @"parse" : success == selected.count ? @"selected_positions_parsed" : @"partial_or_failed",
            @"stage" : @"selected_item_properties",
            @"selected" : @(selected.count),
            @"finder_enumerated" : @(items.value.numberOfItems),
            @"url_inspected" : @(inspected),
            @"url_failed" : @(urlFailures),
            @"matched" : @(rows.count),
            @"position_success" : @(success),
            @"position_failed_or_unmatched" : @(selected.count - success),
            @"property_request_failures" : @(requestFailures),
            @"items" : rows,
            @"coverage" : @"selected_direct_children_only; not Finder-visible-count verification",
            @"coordinate_contract" : @"unverified; dpos unit/origin/anchor not specified by Finder.sdef",
            @"image_rect" : NSNull.null,
            @"label_rect" : NSNull.null,
            @"bounds_semantics" : @"item pbnd only; never window/image/label bounds"
        },
            urls, references
    };
}
// Revalidate the same references, not a new enumeration or name/ordinal association.
// identityCheck supplies native file-resource identity verification; offline tests inject it.
inline bool selectedPositionsMatch(const FinderSnapshot& snapshot, const FinderReader& read,
                                   const std::function<bool(NSUInteger, NSURL*)>& identityCheck) {
    NSArray* rows = snapshot.report[@"items"];
    if (snapshot.urls.count < 2 || snapshot.urls.count > 3 ||
        snapshot.references.count != snapshot.urls.count || rows.count != snapshot.urls.count)
        return false;
    NSDictionary* size = snapshot.report[@"desktop_icon_size"];
    if ([size[@"usable"] boolValue]) {
        NSDictionary* current = desktopIconSize(read);
        if (![current[@"usable"] boolValue] || ![current[@"raw"] isEqual:size[@"raw"]]) return false;
    }
    for (NSUInteger i = 0; i < snapshot.urls.count; ++i) {
        auto before = read(property('pURL', snapshot.references[i]));
        if (before.error || !before.value.stringValue) return false;
        NSURL* url = [NSURL URLWithString:before.value.stringValue];
        if (![urlIdentity(url) isEqual:urlIdentity(snapshot.urls[i])]) return false;
        auto position = read(property('dpos', snapshot.references[i]));
        if (position.error || ![geometry(position.value, 2) isEqual:rows[i][@"position_raw"]]) return false;
        auto after = read(property('pURL', snapshot.references[i]));
        if (after.error || !after.value.stringValue ||
            ![urlIdentity([NSURL URLWithString:after.value.stringValue]) isEqual:urlIdentity(url)] ||
            !identityCheck(i, url))
            return false;
    }
    return true;
}
inline FinderSnapshot selectedSnapshot(NSArray<NSURL*>* selected, const FinderReader& read) {
    NSUInteger succeeded = 0, failed = 0;
    NSMutableArray* errors = [NSMutableArray array];
    FinderSnapshot snapshot = selectedSnapshotImpl(selected, [&](NSAppleEventDescriptor* spec) {
        ReadResult result = read(spec);
        if (result.error) {
            ++failed;
            [errors addObject:@{
                @"property_code" : fourCC([spec descriptorForKeyword:keyAEKeyData].typeCodeValue),
                @"osstatus" : @(result.error),
                @"phase" : [NSString stringWithUTF8String:result.phase]
            }];
        } else
            ++succeeded;
        return result;
    });
    NSMutableDictionary* report = [snapshot.report mutableCopy];
    report[@"query_succeeded"] = @(succeeded);
    report[@"query_failed"] = @(failed);
    report[@"query_errors"] = errors;
    snapshot.report = report;
    return snapshot;
}
}  // namespace dpp::mac
