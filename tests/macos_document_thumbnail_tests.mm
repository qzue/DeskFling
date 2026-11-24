#include <gtest/gtest.h>
#include "platforms/macos/document_thumbnail.h"
using namespace dpp::mac;

TEST(DocumentThumbnail, LateCompletionCannotReplaceCancelledOrFinishedResult) {
    @autoreleasepool {
        auto cancelled = std::make_shared<ThumbnailCompletion>();
        EXPECT_EQ(cancelled->takeOrClose(), nil);
        cancelled->complete(@{@"status" : @"late"});
        EXPECT_EQ(cancelled->takeOrClose(), nil);
        auto finished = std::make_shared<ThumbnailCompletion>();
        finished->complete(@{@"status" : @"first"});
        finished->complete(@{@"status" : @"late"});
        EXPECT_TRUE([finished->takeOrClose()[@"status"] isEqual:@"first"]);
    }
}

TEST(DocumentThumbnail, MainThreadAndInvalidSizesCannotStartProvider) {
    @autoreleasepool {
        auto cancelled = std::make_shared<std::atomic_bool>(false);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        auto url = [NSURL fileURLWithPath:@"/nonexistent/document.pdf"];
        EXPECT_TRUE(
            [documentThumbnail(url, 128, 2, deadline, cancelled)[@"status"] isEqual:@"invalid_request"]);
        EXPECT_TRUE(
            [documentThumbnail(url, 257, 2, deadline, cancelled)[@"status"] isEqual:@"invalid_request"]);
        EXPECT_TRUE(
            [documentThumbnail(url, 128, NAN, deadline, cancelled)[@"status"] isEqual:@"invalid_request"]);
    }
}

TEST(DocumentThumbnail, EligibleDocumentsExcludeDirectoriesLinksAndUnrelatedFiles) {
    @autoreleasepool {
        NSURL* directory = [NSURL
            fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString]];
        ASSERT_TRUE([NSFileManager.defaultManager createDirectoryAtURL:directory
                                           withIntermediateDirectories:NO
                                                            attributes:nil
                                                                 error:nil]);
        for (NSString* name in @[ @"a.pdf", @"a.docx", @"a.xlsx", @"a.json", @"a.md", @"a.bin" ]) {
            NSURL* file = [directory URLByAppendingPathComponent:name];
            ASSERT_TRUE([[@"synthetic fixture" dataUsingEncoding:NSUTF8StringEncoding] writeToURL:file
                                                                                       atomically:YES]);
            EXPECT_EQ(documentThumbnailEligible(file), ![name isEqual:@"a.bin"]);
        }
        EXPECT_FALSE(documentThumbnailEligible(directory));
        NSURL* link = [directory URLByAppendingPathComponent:@"link.pdf"];
        ASSERT_TRUE([NSFileManager.defaultManager
            createSymbolicLinkAtURL:link
                 withDestinationURL:[directory URLByAppendingPathComponent:@"a.pdf"]
                              error:nil]);
        EXPECT_FALSE(documentThumbnailEligible(link));
        EXPECT_FALSE(documentThumbnailEligible([NSURL URLWithString:@"https://example.com/a.pdf"]));
        ASSERT_TRUE([NSFileManager.defaultManager removeItemAtURL:directory error:nil]);
    }
}
