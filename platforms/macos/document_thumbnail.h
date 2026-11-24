#pragma once
#import <QuickLookThumbnailing/QuickLookThumbnailing.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include "platforms/macos/preview_image.h"

namespace dpp::mac {
inline NSDictionary* thumbnailUnavailable(NSString* status) {
    return @{@"image" : NSNull.null, @"status" : status};
}

inline bool documentThumbnailEligible(NSURL* url) {
    if (!url.fileURL) return false;
    NSDictionary* values = [url resourceValuesForKeys:@[
        NSURLIsRegularFileKey, NSURLIsSymbolicLinkKey, NSURLIsAliasFileKey, NSURLFileSizeKey
    ]
                                                error:nil];
    if (![values[NSURLIsRegularFileKey] boolValue] || [values[NSURLIsSymbolicLinkKey] boolValue] ||
        [values[NSURLIsAliasFileKey] boolValue] || !values[NSURLFileSizeKey] ||
        [values[NSURLFileSizeKey] unsignedLongLongValue] > 64 * 1024 * 1024)
        return false;
    UTType* type = nil;
    [url getResourceValue:&type forKey:NSURLContentTypeKey error:nil];
    if ([type conformsToType:UTTypeImage]) return true;
    // Content previews only for these document types. Keep apps, volumes,
    // folders, aliases and unrelated file types on the existing icon route.
    return [@[
        @"pdf", @"doc", @"docx", @"xls", @"xlsx", @"ppt", @"pptx", @"txt", @"md", @"markdown", @"json",
        @"csv", @"rtf"
    ] containsObject:url.pathExtension.lowercaseString];
}

struct ThumbnailCompletion {
    std::mutex mutex;
    bool closed = false;
    __strong NSDictionary* result = nil;
    dispatch_semaphore_t ready = dispatch_semaphore_create(0);
    void complete(NSDictionary* value) {
        std::lock_guard lock(mutex);
        if (closed) return;
        result = value;
        closed = true;
        dispatch_semaphore_signal(ready);
    }
    NSDictionary* takeOrClose() {
        std::lock_guard lock(mutex);
        closed = true;
        return result;
    }
};

// Worker only; bounded request time and batch deadline. Never block the UI or
// read Shell per frame. A late provider callback owns only its completion state.
inline NSDictionary* documentThumbnail(NSURL* url, NSUInteger pixels, CGFloat scale,
                                       std::chrono::steady_clock::time_point batchDeadline,
                                       const std::shared_ptr<std::atomic_bool>& cancelled) {
    if (NSThread.isMainThread || !pixels || pixels > 256 || !std::isfinite(scale) || scale <= 0)
        return thumbnailUnavailable(@"invalid_request");
    if (!documentThumbnailEligible(url)) return thumbnailUnavailable(@"system_icon_route");
    if (cancelled->load()) return thumbnailUnavailable(@"cancelled");
    using Clock = std::chrono::steady_clock;
    auto deadline = std::min(batchDeadline, Clock::now() + std::chrono::seconds(3));
    if (Clock::now() >= deadline) return thumbnailUnavailable(@"batch_timeout");
    auto state = std::make_shared<ThumbnailCompletion>();
    QLThumbnailGenerationRequest* request = [[QLThumbnailGenerationRequest alloc]
          initWithFileAtURL:url
                       size:CGSizeMake(pixels / scale, pixels / scale)
                      scale:scale
        representationTypes:QLThumbnailGenerationRequestRepresentationTypeThumbnail];
    request.iconMode = YES;
    @try {
        [QLThumbnailGenerator.sharedGenerator
            generateBestRepresentationForRequest:request
                               completionHandler:^(QLThumbnailRepresentation* representation, NSError*) {
                                 @autoreleasepool {
                                     @try {
                                         CGImageRef image = representation.CGImage;
                                         if (representation.type == QLThumbnailRepresentationTypeThumbnail &&
                                             image && CGImageGetWidth(image) > 0 &&
                                             CGImageGetHeight(image) > 0 &&
                                             CGImageGetWidth(image) <= pixels &&
                                             CGImageGetHeight(image) <= pixels)
                                             state->complete(@{
                                                 @"image" : (__bridge id)image,
                                                 @"status" : @"quicklook_content"
                                             });
                                         else
                                             state->complete(thumbnailUnavailable(@"quicklook_unavailable"));
                                     } @catch (NSException*) {
                                         state->complete(thumbnailUnavailable(@"quicklook_exception"));
                                     }
                                 }
                               }];
        while (!cancelled->load() && Clock::now() < deadline) {
            if (dispatch_semaphore_wait(state->ready, dispatch_time(DISPATCH_TIME_NOW, 50 * NSEC_PER_MSEC)) ==
                0)
                break;
        }
        NSDictionary* result = state->takeOrClose();
        [QLThumbnailGenerator.sharedGenerator cancelRequest:request];
        if (cancelled->load()) return thumbnailUnavailable(@"cancelled");
        if (result) return result;
        return thumbnailUnavailable(@"quicklook_timeout");
    } @catch (NSException*) {
        state->takeOrClose();
        @try {
            [QLThumbnailGenerator.sharedGenerator cancelRequest:request];
        } @catch (NSException*) {
        }
        return thumbnailUnavailable(@"quicklook_exception");
    }
}
}  // namespace dpp::mac
