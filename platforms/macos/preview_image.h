#pragma once
#import <AppKit/AppKit.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

namespace dpp::mac {
// Snapshot worker only. Does not enumerate folders, follow aliases, or request permissions.
// The returned image is owned by the dictionary; no file URLs enter exported diagnostics.
inline NSDictionary* previewThumbnail(NSURL* url, NSUInteger maxPixels) {
    auto unavailable = [](NSString* reason) { return @{@"image" : NSNull.null, @"status" : reason}; };
    if (!url.fileURL || maxPixels < 1 || maxPixels > 256) return unavailable(@"invalid_request");
    NSDictionary* values = [url resourceValuesForKeys:@[
        NSURLIsRegularFileKey, NSURLIsSymbolicLinkKey, NSURLIsAliasFileKey, NSURLFileSizeKey
    ]
                                                error:nil];
    if (!values) return unavailable(@"file_metadata_unavailable");
    if (![values[NSURLIsRegularFileKey] boolValue] || [values[NSURLIsSymbolicLinkKey] boolValue] ||
        [values[NSURLIsAliasFileKey] boolValue])
        return unavailable(@"not_regular_image_file");
    UTType* type = nil;
    if (![url getResourceValue:&type forKey:NSURLContentTypeKey error:nil] || !type)
        return unavailable(@"image_type_unavailable");
    if (![type conformsToType:UTTypeImage]) return unavailable(@"not_image_type");
    NSNumber* size = values[NSURLFileSizeKey];
    if (!size || size.unsignedLongLongValue > 64 * 1024 * 1024)
        return unavailable(@"image_size_limit_or_unknown");
    CGImageSourceRef source =
        CGImageSourceCreateWithURL((__bridge CFURLRef)url, (__bridge CFDictionaryRef)
                                                               @{(id)kCGImageSourceShouldCache : @NO});
    if (!source) return unavailable(@"image_source_failed");
    CGImageRef image = CGImageSourceCreateThumbnailAtIndex(
        source, 0, (__bridge CFDictionaryRef) @{
            (id)kCGImageSourceCreateThumbnailFromImageAlways : @YES,
            (id)kCGImageSourceCreateThumbnailWithTransform : @YES,
            (id)kCGImageSourceThumbnailMaxPixelSize : @(maxPixels),
            (id)kCGImageSourceShouldCacheImmediately : @YES
        });
    CFRelease(source);
    if (!image) return unavailable(@"thumbnail_decode_failed");
    return @{@"image" : CFBridgingRelease(image), @"status" : @"decoded"};
}
}  // namespace dpp::mac
