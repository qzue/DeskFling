#pragma once
#import <AppKit/AppKit.h>
#include <functional>
namespace dpp::mac {
// Native value copied before preview teardown. Never persisted or replayed.
struct OpenTarget {
    NSURL* url = nil;
    NSArray* identity = nil;
    NSURL* aliasURL = nil;
    NSArray* aliasIdentity = nil;
};
inline NSArray* openIdentity(NSURL* url) {
    if (!url.isFileURL) return nil;
    [url removeAllCachedResourceValues];
    id file = nil, volume = nil;
    NSNumber *regular = nil, *directory = nil, *alias = nil, *symlink = nil;
    if (![url getResourceValue:&file forKey:NSURLFileResourceIdentifierKey error:nil] || !file ||
        ![url getResourceValue:&volume forKey:NSURLVolumeIdentifierKey error:nil] || !volume ||
        ![url getResourceValue:&regular forKey:NSURLIsRegularFileKey error:nil] ||
        ![url getResourceValue:&directory forKey:NSURLIsDirectoryKey error:nil] ||
        ![url getResourceValue:&alias forKey:NSURLIsAliasFileKey error:nil] ||
        ![url getResourceValue:&symlink forKey:NSURLIsSymbolicLinkKey error:nil] || alias.boolValue ||
        symlink.boolValue || !(regular.boolValue || directory.boolValue))
        return nil;
    return @[ file, volume ];
}
inline NSArray* aliasIdentity(NSURL* url) {
    [url removeAllCachedResourceValues];
    id file = nil, volume = nil;
    if (![url getResourceValue:&file forKey:NSURLFileResourceIdentifierKey error:nil] || !file ||
        ![url getResourceValue:&volume forKey:NSURLVolumeIdentifierKey error:nil] || !volume)
        return nil;
    return @[ file, volume ];
}
inline OpenTarget bindOpenTarget(NSURL* source) {
    if (!source.isFileURL) return {};
    NSArray* identity = openIdentity(source);
    if (identity) return {[source copy], identity};
    NSNumber* alias = nil;
    if (![source getResourceValue:&alias forKey:NSURLIsAliasFileKey error:nil] || !alias.boolValue) return {};
    NSURL* resolved = [NSURL
        URLByResolvingAliasFileAtURL:source
                             options:NSURLBookmarkResolutionWithoutUI | NSURLBookmarkResolutionWithoutMounting
                               error:nil];
    NSArray* resolvedID = openIdentity(resolved);
    NSArray* sourceID = aliasIdentity(source);
    if (!resolvedID || !sourceID) return {};
    return {[resolved copy], resolvedID, [source copy], sourceID};
}
inline bool validOpenTarget(const OpenTarget& target) {
    if (![openIdentity(target.url) isEqual:target.identity]) return false;
    if (!target.aliasURL) return true;
    const auto current = bindOpenTarget(target.aliasURL);
    return [current.aliasIdentity isEqual:target.aliasIdentity] &&
           [current.identity isEqual:target.identity] && [current.url isEqual:target.url];
}
enum class OpenResult { Duplicate, RecoveryFailed, InvalidTarget, Submitted };
// One explicit choice consumes the gate even on failure. A new snapshot owns a new gate.
inline OpenResult restoreThenOpen(bool& consumed, const OpenTarget& target,
                                  const std::function<bool()>& restore,
                                  const std::function<bool(const OpenTarget&)>& valid,
                                  const std::function<void(const OpenTarget&)>& submit) {
    if (consumed) return OpenResult::Duplicate;
    consumed = true;
    if (!restore()) return OpenResult::RecoveryFailed;
    if (!valid(target)) return OpenResult::InvalidTarget;
    submit(target);
    return OpenResult::Submitted;
}
}  // namespace dpp::mac
