#pragma once
#include <stdexcept>
#include "platforms/macos/finder_snapshot.h"
namespace dpp::mac {
// Finder may return file://localhost while NSOpenPanel returns file:///.
// Only local file URLs may normalize to a filesystem identity; reject remote hosts.
inline NSString* fullDesktopURLIdentity(NSURL* url) {
    if (!url.fileURL ||
        (url.host.length && [url.host caseInsensitiveCompare:@"localhost"] != NSOrderedSame) || url.user ||
        url.password || url.port || url.query || url.fragment || !url.path.isAbsolutePath)
        return nil;
    return url.path.stringByStandardizingPath;
}
// All-or-nothing, bounded Finder desktop enumeration. No files/positions are modified.
inline FinderSnapshot fullDesktopSnapshot(const FinderReader& read) {
    const auto start = std::chrono::steady_clock::now();
    auto require = [](bool ok, const char* reason) {
        if (!ok) throw std::runtime_error(reason);
    };
    auto checked = [&](NSAppleEventDescriptor* spec) {
        require(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < 15,
                "full desktop snapshot timed out");
        ReadResult r = read(spec);
        require(!r.error && r.value, "full desktop property unavailable; takeover refused");
        return r.value;
    };
    auto list = checked(allItems(property('desk')));
    require(list.descriptorType == typeAEList && list.numberOfItems > 0 && list.numberOfItems <= 128,
            "desktop must contain 1 to 128 fully readable items");
    NSMutableArray* urls = [NSMutableArray array];
    NSMutableArray* references = [NSMutableArray array];
    NSMutableArray* rows = [NSMutableArray array];
    NSMutableSet* identities = [NSMutableSet set];
    for (NSInteger i = 1; i <= list.numberOfItems; ++i) {
        auto item = [list descriptorAtIndex:i];
        require(item.descriptorType == typeObjectSpecifier || item.descriptorType == typeFileURL ||
                    item.descriptorType == typeAlias,
                "unmappable desktop item");
        auto urlValue = checked(property('pURL', item));
        NSURL* url = urlValue.stringValue ? [NSURL URLWithString:urlValue.stringValue] : nil;
        NSString* identity = fullDesktopURLIdentity(url);
        require(identity && ![identities containsObject:identity], "missing or duplicate desktop identity");
        auto position = checked(property('dpos', item));
        NSArray* xy = geometry(position, 2);
        auto afterURL = checked(property('pURL', item));
        NSURL* after = afterURL.stringValue ? [NSURL URLWithString:afterURL.stringValue] : nil;
        require(xy && [identity isEqual:fullDesktopURLIdentity(after)],
                "desktop item moved or cannot be positioned");
        [identities addObject:identity];
        [urls addObject:url];
        [references addObject:item];
        [rows addObject:@{@"id" : @(i), @"identity_stable" : @YES, @"position_raw" : xy}];
    }
    // Re-read the list and URL membership; no early exit after three samples.
    auto finalList = checked(allItems(property('desk')));
    require(finalList.descriptorType == typeAEList && finalList.numberOfItems == list.numberOfItems,
            "desktop membership changed during snapshot");
    NSMutableSet* finalIDs = [NSMutableSet set];
    for (NSInteger i = 1; i <= finalList.numberOfItems; ++i) {
        auto value = checked(property('pURL', [finalList descriptorAtIndex:i]));
        NSURL* url = value.stringValue ? [NSURL URLWithString:value.stringValue] : nil;
        NSString* identity = fullDesktopURLIdentity(url);
        require(identity != nil, "desktop membership recheck failed");
        [finalIDs addObject:identity];
    }
    require([identities isEqualToSet:finalIDs], "desktop membership changed; takeover refused");
    NSDictionary* size = desktopIconSize(read);
    require([size[@"usable"] boolValue], "Finder icon size unavailable; no guessed takeover size");
    return {
        @{
            @"coverage" : @"full_Finder_desktop_item_list",
            @"finder_enumerated" : @(list.numberOfItems),
            @"items" : rows,
            @"desktop_icon_size" : size
        },
            urls, references
    };
}
inline bool fullDesktopSnapshotsMatch(const FinderSnapshot& a, const FinderSnapshot& b) {
    if (a.urls.count != b.urls.count || a.urls.count == 0 ||
        ![a.report[@"desktop_icon_size"][@"raw"] isEqual:b.report[@"desktop_icon_size"][@"raw"]])
        return false;
    NSMutableDictionary* positions = [NSMutableDictionary dictionary];
    for (NSUInteger i = 0; i < a.urls.count; ++i)
        positions[fullDesktopURLIdentity(a.urls[i])] = a.report[@"items"][i][@"position_raw"];
    for (NSUInteger i = 0; i < b.urls.count; ++i)
        if (![positions[fullDesktopURLIdentity(b.urls[i])] isEqual:b.report[@"items"][i][@"position_raw"]])
            return false;
    return true;
}
}  // namespace dpp::mac
