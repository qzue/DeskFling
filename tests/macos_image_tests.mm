#include <gtest/gtest.h>
#include "platforms/macos/preview_image.h"
using namespace dpp::mac;
namespace {
class PreviewImageTest : public ::testing::Test {
   protected:
    NSURL* directory;
    void SetUp() override {
        directory = [NSURL
            fileURLWithPath:[NSTemporaryDirectory()
                                stringByAppendingPathComponent:[@"dpp-image-fixture-"
                                                                   stringByAppendingString:NSUUID.UUID
                                                                                               .UUIDString]]];
        ASSERT_TRUE([NSFileManager.defaultManager createDirectoryAtURL:directory
                                           withIntermediateDirectories:NO
                                                            attributes:nil
                                                                 error:nil]);
    }
    void TearDown() override { [NSFileManager.defaultManager removeItemAtURL:directory error:nil]; }
    NSURL* file(NSString* name) { return [directory URLByAppendingPathComponent:name]; }
    void writeImage(NSURL* url, int orientation) {
        CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
        CGContextRef context =
            CGBitmapContextCreate(nullptr, 320, 160, 8, 0, space, kCGImageAlphaPremultipliedLast);
        CGColorSpaceRelease(space);
        ASSERT_NE(context, nullptr);
        CGContextSetRGBFillColor(context, 1, 0, 0, 1);
        CGContextFillRect(context, CGRectMake(0, 0, 320, 160));
        CGImageRef image = CGBitmapContextCreateImage(context);
        CGImageDestinationRef destination =
            CGImageDestinationCreateWithURL((__bridge CFURLRef)url, CFSTR("public.jpeg"), 1, nullptr);
        ASSERT_NE(destination, nullptr);
        CGImageDestinationAddImage(
            destination, image,
            (__bridge CFDictionaryRef)
                @{(id)kCGImagePropertyOrientation : @(orientation)});
        ASSERT_TRUE(CGImageDestinationFinalize(destination));
        CFRelease(destination);
        CGImageRelease(image);
        CGContextRelease(context);
    }
};
}  // namespace
// Temporary synthetic files only; never accesses Desktop, preferences, or permissions.
TEST_F(PreviewImageTest, DecodesPhotoContentWithBoundedSizeAndOrientation) {
    @autoreleasepool {
        NSURL* url = file(@"synthetic.jpg");
        writeImage(url, 6);
        NSDictionary* result = previewThumbnail(url, 96);
        ASSERT_TRUE([result[@"status"] isEqual:@"decoded"]) << [result[@"status"] UTF8String];
        CGImageRef image = (__bridge CGImageRef)result[@"image"];
        EXPECT_EQ(CGImageGetWidth(image), 48u);
        EXPECT_EQ(CGImageGetHeight(image), 96u);
        // Verify actual photo pixels, not a generic document icon.
        uint8_t pixel[4] = {};
        CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
        CGContextRef context =
            CGBitmapContextCreate(pixel, 1, 1, 8, 4, space, kCGImageAlphaPremultipliedLast);
        CGContextDrawImage(context, CGRectMake(0, 0, 1, 1), image);
        EXPECT_GT(pixel[0], 240);
        EXPECT_LT(pixel[1], 15);
        EXPECT_LT(pixel[2], 15);
        CGContextRelease(context);
        CGColorSpaceRelease(space);
    }
}
TEST_F(PreviewImageTest, DamagedPhotoReportsFailureForSystemIconFallback) {
    @autoreleasepool {
        NSURL* url = file(@"broken.jpg");
        ASSERT_TRUE([[@"not an image" dataUsingEncoding:NSUTF8StringEncoding] writeToURL:url atomically:YES]);
        NSDictionary* result = previewThumbnail(url, 96);
        EXPECT_EQ(result[@"image"], NSNull.null);
        EXPECT_FALSE([result[@"status"] isEqual:@"decoded"]);
        EXPECT_EQ(previewThumbnail(url, 0)[@"image"], NSNull.null);
        EXPECT_EQ(previewThumbnail(url, 257)[@"image"], NSNull.null);
    }
}
TEST_F(PreviewImageTest, FoldersApplicationsDocumentsAndLinksKeepSystemIconRoute) {
    @autoreleasepool {
        for (NSString* name in @[ @"folder", @"Example.app" ]) {
            NSURL* url = file(name);
            ASSERT_TRUE([NSFileManager.defaultManager createDirectoryAtURL:url
                                               withIntermediateDirectories:NO
                                                                attributes:nil
                                                                     error:nil]);
            EXPECT_TRUE([previewThumbnail(url, 96)[@"status"] isEqual:@"not_regular_image_file"]);
        }
        NSURL* text = file(@"note.txt");
        ASSERT_TRUE([[@"fixture" dataUsingEncoding:NSUTF8StringEncoding] writeToURL:text atomically:YES]);
        EXPECT_TRUE([previewThumbnail(text, 96)[@"status"] isEqual:@"not_image_type"])
            << [previewThumbnail(text, 96)[@"status"] UTF8String];
        NSURL* photo = file(@"photo.jpg");
        writeImage(photo, 1);
        NSURL* link = file(@"link.jpg");
        ASSERT_TRUE([NSFileManager.defaultManager createSymbolicLinkAtURL:link
                                                       withDestinationURL:photo
                                                                    error:nil]);
        EXPECT_EQ(previewThumbnail(link, 96)[@"image"], NSNull.null);
        EXPECT_EQ(previewThumbnail(file(@"missing.jpg"), 96)[@"image"], NSNull.null);
    }
}
