#include <gtest/gtest.h>
#import "platforms/macos/open_target.h"
using namespace dpp::mac;
TEST(OpenOriginal, RestoreBeforeValidationAndOneSubmission) {
    bool consumed = false;
    std::vector<int> order;
    OpenTarget target { [NSURL fileURLWithPath:@"/synthetic-folder"], @[ @1, @2 ] };
    auto restore = [&] {
        order.push_back(1);
        return true;
    };
    auto valid = [&](const OpenTarget& t) {
        order.push_back(2);
        return [t.identity isEqual:target.identity];
    };
    auto submit = [&](const OpenTarget&) { order.push_back(3); };
    EXPECT_EQ(restoreThenOpen(consumed, target, restore, valid, submit), OpenResult::Submitted);
    EXPECT_EQ(restoreThenOpen(consumed, target, restore, valid, submit), OpenResult::Duplicate);
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
}
TEST(OpenOriginal, RecoveryFailureNeverValidatesOrOpens) {
    bool consumed = false;
    EXPECT_EQ(restoreThenOpen(
                  consumed, {}, [] { return false; },
                  [](const OpenTarget&) {
                      ADD_FAILURE();
                      return true;
                  },
                  [](const OpenTarget&) { ADD_FAILURE(); }),
              OpenResult::RecoveryFailed);
    EXPECT_TRUE(consumed);
}
TEST(OpenOriginal, InvalidTargetAfterRestoreNeverOpens) {
    bool consumed = false;
    EXPECT_EQ(restoreThenOpen(
                  consumed, {}, [] { return true; }, [](const OpenTarget&) { return false; },
                  [](const OpenTarget&) { ADD_FAILURE(); }),
              OpenResult::InvalidTarget);
}
TEST(OpenOriginal, NonFileAndMissingTargetsDisabled) {
    EXPECT_EQ(openIdentity([NSURL URLWithString:@"https://example.com"]), nil);
    EXPECT_EQ(openIdentity([NSURL fileURLWithPath:@"/dpp-nonexistent-test-object"]), nil);
}
#include "platforms/mirror_hit.h"
TEST(OpenOriginal, HitUsesCurrentRotationAndTopmostDrawing) {
    dpp::DrawList draw;
    draw.add(dpp::SpriteItem{dpp::TextureId{1}, {300, 400}, {80, 20}, 1.5707963f});
    EXPECT_EQ(dpp::mirrorHit(draw, {300, 430}), dpp::TextureId{1});
    EXPECT_EQ(dpp::mirrorHit(draw, {330, 400}), dpp::kNoTexture);
    draw.add(dpp::SpriteItem{dpp::TextureId{2}, {300, 430}, {20, 20}});
    EXPECT_EQ(dpp::mirrorHit(draw, {300, 430}), dpp::TextureId{2});
    EXPECT_EQ(dpp::mirrorHit(draw, {10, 10}), dpp::kNoTexture);
}
TEST(OpenOriginal, ReplacementAtSamePathIsNotOriginal) {
    NSURL* root =
        [[NSURL fileURLWithPath:NSTemporaryDirectory()] URLByAppendingPathComponent:NSUUID.UUID.UUIDString
                                                                        isDirectory:YES];
    ASSERT_TRUE([NSFileManager.defaultManager createDirectoryAtURL:root
                                       withIntermediateDirectories:NO
                                                        attributes:nil
                                                             error:nil]);
    NSURL* folder = [root URLByAppendingPathComponent:@"folder" isDirectory:YES];
    ASSERT_TRUE([NSFileManager.defaultManager createDirectoryAtURL:folder
                                       withIntermediateDirectories:NO
                                                        attributes:nil
                                                             error:nil]);
    NSArray* identity = openIdentity(folder);
    ASSERT_NE(identity, nil);
    ASSERT_TRUE([NSFileManager.defaultManager moveItemAtURL:folder
                                                      toURL:[root URLByAppendingPathComponent:@"original"]
                                                      error:nil]);
    ASSERT_TRUE([NSFileManager.defaultManager createDirectoryAtURL:folder
                                       withIntermediateDirectories:NO
                                                        attributes:nil
                                                             error:nil]);
    EXPECT_FALSE([openIdentity(folder) isEqual:identity]);
    EXPECT_TRUE([NSFileManager.defaultManager removeItemAtURL:root error:nil]);
}
TEST(OpenOriginal, QuitDuringRecoveryCancelsUnsubmittedRequest) {
    bool consumed = false, cancelled = false;
    EXPECT_EQ(restoreThenOpen(
                  consumed, {},
                  [&] {
                      cancelled = true;
                      return true;
                  },
                  [&](const OpenTarget&) { return !cancelled; }, [](const OpenTarget&) { ADD_FAILURE(); }),
              OpenResult::InvalidTarget);
}
TEST(OpenOriginal, AliasBindsSourceAndDestinationWithoutLaunching) {
    NSURL* root =
        [[NSURL fileURLWithPath:NSTemporaryDirectory()] URLByAppendingPathComponent:NSUUID.UUID.UUIDString
                                                                        isDirectory:YES];
    ASSERT_TRUE([NSFileManager.defaultManager createDirectoryAtURL:root
                                       withIntermediateDirectories:NO
                                                        attributes:nil
                                                             error:nil]);
    NSURL* folder = [root URLByAppendingPathComponent:@"target" isDirectory:YES];
    ASSERT_TRUE([NSFileManager.defaultManager createDirectoryAtURL:folder
                                       withIntermediateDirectories:NO
                                                        attributes:nil
                                                             error:nil]);
    NSData* bookmark = [folder bookmarkDataWithOptions:NSURLBookmarkCreationSuitableForBookmarkFile
                        includingResourceValuesForKeys:nil
                                         relativeToURL:nil
                                                 error:nil];
    ASSERT_NE(bookmark, nil);
    NSURL* alias = [root URLByAppendingPathComponent:@"alias"];
    ASSERT_TRUE([NSURL writeBookmarkData:bookmark toURL:alias options:0 error:nil]);
    const auto target = bindOpenTarget(alias);
    EXPECT_NE(target.aliasURL, nil);
    EXPECT_TRUE(validOpenTarget(target));
    EXPECT_TRUE([NSFileManager.defaultManager removeItemAtURL:alias error:nil]);
    EXPECT_FALSE(validOpenTarget(target));
    EXPECT_TRUE([NSFileManager.defaultManager removeItemAtURL:root error:nil]);
}
#include "platforms/macos/display_guard.h"
TEST(OpenDisplay, WorkAreaOnlyChangesRequireEstablishedGeometry) {
    MirrorDisplay before { 1, NSMakeRect(0, 0, 1728, 1117), NSMakeRect(0, 73, 1728, 1006), 2, @1 };
    auto after = before;
    after.work = NSMakeRect(0, 71, 1728, 1008);
    EXPECT_FALSE(mirrorDisplayCompatible(before, after, false));
    EXPECT_TRUE(mirrorDisplayCompatible(before, after, true));
    EXPECT_TRUE(NSEqualRects(before.work, NSMakeRect(0, 73, 1728, 1006)));
    after.work.size.height -= 100;
    EXPECT_FALSE(mirrorDisplayCompatible(before, after, true));
}
TEST(OpenDisplay, HandoffNeverAcceptsDisplayTopologyGeometryOrScaleChanges) {
    MirrorDisplay before { 1, NSMakeRect(0, 0, 1728, 1117), NSMakeRect(0, 73, 1728, 1006), 2, @1 };
    auto after = before;
    after.scale = 1;
    EXPECT_FALSE(mirrorDisplayCompatible(before, after, true));
    after = before;
    after.frame.size.width += 1;
    EXPECT_FALSE(mirrorDisplayCompatible(before, after, true));
    after = before;
    after.count = 2;
    EXPECT_FALSE(mirrorDisplayCompatible(before, after, true));
    after = before;
    after.id = @2;
    EXPECT_FALSE(mirrorDisplayCompatible(before, after, true));
}
#include "platforms/macos/watched_item.h"
TEST(OpenWatch, MetadataRequiresUnchangedIdentityAndContentVersion) {
    NSArray* before = @[ @1, @2, @100, @123 ];
    EXPECT_TRUE(harmlessItemMetadata(DISPATCH_VNODE_ATTRIB, before, @[ @1, @2, @100, @123 ]));
    EXPECT_TRUE(harmlessItemMetadata(DISPATCH_VNODE_WRITE, before, before));
    EXPECT_FALSE(harmlessItemMetadata(DISPATCH_VNODE_WRITE, before, @[ @1, @2, @101, @124 ]));
    EXPECT_FALSE(harmlessItemMetadata(DISPATCH_VNODE_ATTRIB, before, @[ @9, @2, @100, @123 ]));
    EXPECT_FALSE(harmlessItemMetadata(DISPATCH_VNODE_RENAME, before, before));
    EXPECT_FALSE(harmlessItemMetadata(DISPATCH_VNODE_DELETE, before, before));
    EXPECT_FALSE(harmlessItemMetadata(DISPATCH_VNODE_REVOKE, before, before));
    EXPECT_FALSE(harmlessItemMetadata(DISPATCH_VNODE_ATTRIB, before, nil));
}
#include <sys/xattr.h>
TEST(OpenWatch, NativeExtendedMetadataDoesNotInvalidateButContentWriteDoes) {
    NSURL* file =
        [[NSURL fileURLWithPath:NSTemporaryDirectory()] URLByAppendingPathComponent:NSUUID.UUID.UUIDString];
    ASSERT_TRUE([[@"fixture" dataUsingEncoding:NSUTF8StringEncoding] writeToURL:file atomically:NO]);
    NSArray* before = watchedItemVersion(file);
    ASSERT_NE(before, nil);
    const char value[] = "opened";
    ASSERT_EQ(setxattr(file.fileSystemRepresentation, "com.dpp.fixture", value, sizeof(value), 0, 0), 0);
    EXPECT_TRUE(harmlessItemMetadata(DISPATCH_VNODE_ATTRIB, before, watchedItemVersion(file)));
    ASSERT_TRUE([[@"changed file content" dataUsingEncoding:NSUTF8StringEncoding] writeToURL:file
                                                                                  atomically:NO]);
    EXPECT_FALSE(harmlessItemMetadata(DISPATCH_VNODE_WRITE, before, watchedItemVersion(file)));
    EXPECT_TRUE([NSFileManager.defaultManager removeItemAtURL:file error:nil]);
}

TEST(MirrorDisplay, InvalidStartupConfigurationRejected) {
    MirrorDisplay d { 1, NSMakeRect(-1000, 0, 1000, 800), NSMakeRect(-1000, 30, 1000, 740), 1.5, @1 };
    EXPECT_TRUE(validMirrorDisplay(d));
    d.scale = 0;
    EXPECT_FALSE(validMirrorDisplay(d));
    d.scale = 2;
    d.work.size.width = 1100;
    EXPECT_FALSE(validMirrorDisplay(d));
}

TEST(MirrorDisplay, ImmutableBaselineRejectsContractionAndDrift) {
    MirrorDisplay a{1, NSMakeRect(0,0,1000,800), NSMakeRect(0,40,1000,720), 2, @1};
    EXPECT_TRUE(mirrorDisplayCompatible(a,a,false));
    auto b=a;
    b.work.origin.y -= 10; b.work.size.height += 10;
    EXPECT_TRUE(mirrorDisplayCompatible(a,b,true));
    EXPECT_FALSE(mirrorDisplayCompatible(a,b,false));
    for (int i=1;i<5;++i) {
        b=a; b.work.origin.y += i; b.work.size.height -= i;
        EXPECT_FALSE(mirrorDisplayCompatible(a,b,true));
    }
    b=a; b.work.origin.x = -0.0;
    EXPECT_TRUE(mirrorDisplayCompatible(a,b,false));
    b.scale=NAN; EXPECT_FALSE(mirrorDisplayCompatible(a,b,true));
    b=a; b.work.size.height=NAN; EXPECT_FALSE(mirrorDisplayCompatible(a,b,true));
}

TEST(OpenDisplay, FixedBaselineExpansionAndReturnBeforeOrAfterOpen) {
    MirrorDisplay initial{1, NSMakeRect(0,0,1728,1117), NSMakeRect(0,73,1728,1006), 2, @1};
    for (bool submitted : {false, true}) {
        (void)submitted; // Opening is deliberately absent from the geometry policy.
        auto current = initial;
        for (double y : {71., 70., 72., 73., 71.}) {
            current.work = NSMakeRect(0,y,1728,1079-y);
            EXPECT_TRUE(mirrorDisplayCompatible(initial,current,true));
        }
        current.work = NSMakeRect(0,74,1728,1005);
        EXPECT_FALSE(mirrorDisplayCompatible(initial,current,true));
        current = initial; current.work = NSMakeRect(0,71,1728,1008);
        EXPECT_FALSE(mirrorDisplayCompatible(initial,current,false));
        EXPECT_EQ(initial.work.origin.y,73);
    }
}
