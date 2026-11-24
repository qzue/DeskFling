#pragma once
#import <Foundation/Foundation.h>
#include <dispatch/dispatch.h>
namespace dpp::mac {
// Identity and content version only. Opening may update access/extended metadata.
inline NSArray* watchedItemVersion(NSURL* url) {
    [url removeAllCachedResourceValues];
    id resource = nil, volume = nil, size = nil, modified = nil;
    if (![url getResourceValue:&resource forKey:NSURLFileResourceIdentifierKey error:nil] || !resource ||
        ![url getResourceValue:&volume forKey:NSURLVolumeIdentifierKey error:nil] || !volume ||
        ![url getResourceValue:&size forKey:NSURLFileSizeKey error:nil] || !size ||
        ![url getResourceValue:&modified forKey:NSURLContentModificationDateKey error:nil] || !modified)
        return nil;
    return @[ resource, volume, size, modified ];
}
inline bool harmlessItemMetadata(unsigned long flags, NSArray* before, NSArray* after) {
    const auto destructive = DISPATCH_VNODE_DELETE | DISPATCH_VNODE_RENAME | DISPATCH_VNODE_REVOKE;
    return !(flags & destructive) && before && after && [before isEqual:after];
}
}  // namespace dpp::mac
