#include <gtest/gtest.h>
#include "platforms/macos/cached_draw_renderer.h"
#include "platforms/macos/desktop_layout_writeback.h"
#include "platforms/macos/full_desktop_scope.h"
#include "platforms/macos/session_trace.h"
using namespace dpp::mac;
namespace {
struct DesktopReply {
    int count = 6, missing = 0, listReads = 0;
    bool duplicate = false, changeMembership = false, localhostFirstPass = false;
    ReadResult read(NSAppleEventDescriptor* spec) {
        if ([spec descriptorForKeyword:keyAEDesiredClass].typeCodeValue == 'cobj') {
            auto list = [NSAppleEventDescriptor listDescriptor];
            ++listReads;
            const int n = count + (changeMembership && listReads > 1 ? 1 : 0);
            for (int i = 1; i <= n; ++i)
                [list
                    insertDescriptor:object('cobj', formAbsolutePosition,
                                            [NSAppleEventDescriptor descriptorWithInt32:i], property('desk'))
                             atIndex:i];
            return {list};
        }
        const auto code = [spec descriptorForKeyword:keyAEKeyData].typeCodeValue;
        if (code == 'lvis') return {[NSAppleEventDescriptor descriptorWithInt32:64]};
        const int i =
            [[spec descriptorForKeyword:keyAEContainer] descriptorForKeyword:keyAEKeyData].int32Value;
        if (i == missing) return {nil, errAEEventFailed};
        if (code == 'pURL')
            return {[NSAppleEventDescriptor
                descriptorWithString:[NSString stringWithFormat:@"file://%@/synthetic-desktop/item-%d",
                                                                localhostFirstPass && listReads == 1
                                                                    ? @"localhost"
                                                                    : @"",
                                                                duplicate ? 1 : i]]};
        if (code == 'dpos') {
            auto xy = [NSAppleEventDescriptor listDescriptor];
            [xy insertDescriptor:[NSAppleEventDescriptor descriptorWithInt32:100 + i * 70] atIndex:1];
            [xy insertDescriptor:[NSAppleEventDescriptor descriptorWithInt32:200] atIndex:2];
            return {xy};
        }
        return {nil, errAEEventFailed};
    }
    FinderSnapshot snapshot() {
        return fullDesktopSnapshot([&](NSAppleEventDescriptor* s) { return read(s); });
    }
};
}  // namespace
TEST(FullDesktop, NormalizesLocalFileURLFormsWithoutAcceptingRemoteHosts) {
    @autoreleasepool {
        NSURL* local = [NSURL URLWithString:@"file:///synthetic-desktop/a%20b"];
        NSURL* localhost = [NSURL URLWithString:@"file://localhost/synthetic-desktop/a%20b"];
        EXPECT_TRUE([fullDesktopURLIdentity(local) isEqual:fullDesktopURLIdentity(localhost)]);
        EXPECT_TRUE([fullDesktopURLIdentity(localhost.URLByDeletingLastPathComponent)
            isEqual:fullDesktopURLIdentity([NSURL fileURLWithPath:@"/synthetic-desktop" isDirectory:YES])]);
        EXPECT_FALSE([fullDesktopURLIdentity(
            [NSURL URLWithString:@"file:///different/a"].URLByDeletingLastPathComponent)
            isEqual:fullDesktopURLIdentity(local.URLByDeletingLastPathComponent)]);
        EXPECT_EQ(fullDesktopURLIdentity([NSURL URLWithString:@"file://remote-host/synthetic-desktop/a"]),
                  nil);
        EXPECT_EQ(fullDesktopURLIdentity([NSURL URLWithString:@"https://localhost/synthetic-desktop/a"]),
                  nil);
        EXPECT_EQ(fullDesktopURLIdentity([NSURL URLWithString:@"file:///synthetic-desktop/a?other=1"]), nil);
        EXPECT_EQ(fullDesktopURLIdentity([NSURL URLWithString:@"file:///synthetic-desktop/a#part"]), nil);
        DesktopReply source;
        source.localhostFirstPass = true;
        EXPECT_EQ(source.snapshot().urls.count, 6u);
    }
}
TEST(FullDesktop, ReadsEveryItemAndRechecksMembershipWithoutThreeItemTruncation) {
    @autoreleasepool {
        DesktopReply source;
        auto snapshot = source.snapshot();
        EXPECT_EQ(snapshot.urls.count, 6u);
        EXPECT_EQ(snapshot.references.count, 6u);
        EXPECT_EQ([snapshot.report[@"finder_enumerated"] intValue], 6);
        EXPECT_EQ(source.listReads, 2);
        EXPECT_TRUE(fullDesktopSnapshotsMatch(snapshot, source.snapshot()));
    }
}
TEST(FullDesktop, RejectsMissingFourthItemDuplicateIdentityAndChangingMembership) {
    @autoreleasepool {
        DesktopReply missing;
        missing.missing = 4;
        EXPECT_THROW(missing.snapshot(), std::runtime_error);
        DesktopReply duplicate;
        duplicate.duplicate = true;
        EXPECT_THROW(duplicate.snapshot(), std::runtime_error);
        DesktopReply changed;
        changed.changeMembership = true;
        EXPECT_THROW(changed.snapshot(), std::runtime_error);
        DesktopReply tooMany;
        tooMany.count = 129;
        EXPECT_THROW(tooMany.snapshot(), std::runtime_error);
        DesktopReply empty;
        empty.count = 0;
        EXPECT_THROW(empty.snapshot(), std::runtime_error);
    }
}

TEST(FullDesktop, IncludesOnlyVerifiedMountedRootsAlongsideDesktopFiles) {
    @autoreleasepool {
        NSURL* desktop = [NSURL fileURLWithPath:@"/synthetic/Desktop" isDirectory:YES];
        NSURL* file = [desktop URLByAppendingPathComponent:@"photo.png"];
        NSURL* boot = [NSURL URLWithString:@"file://localhost/"];
        NSURL* disk = [NSURL fileURLWithPath:@"/Volumes/Test Disk" isDirectory:YES];
        NSSet* mounts = desktopPathSet(@[ boot, disk ]);
        EXPECT_EQ(desktopItemScope(file, desktop, mounts, false), DesktopItemScope::directoryChild);
        EXPECT_EQ(desktopItemScope(boot, desktop, mounts, true), DesktopItemScope::mountedVolume);
        EXPECT_EQ(desktopItemScope(disk, desktop, mounts, true), DesktopItemScope::mountedVolume);
        // Root metadata alone cannot authorize an unmounted or replaced path.
        EXPECT_EQ(desktopItemScope(disk, desktop, desktopPathSet(@[ boot ]), true),
                  DesktopItemScope::unsupported);
        EXPECT_EQ(desktopItemScope(disk, desktop, mounts, false), DesktopItemScope::unsupported);
        EXPECT_EQ(
            desktopItemScope([disk URLByAppendingPathComponent:@"private-file"], desktop, mounts, false),
            DesktopItemScope::unsupported);
        EXPECT_EQ(desktopItemScope([NSURL URLWithString:@"file://remote/Volumes/Test%20Disk"], desktop,
                                   mounts, true),
                  DesktopItemScope::unsupported);
        EXPECT_EQ(desktopItemScope(desktop, desktop, mounts, false), DesktopItemScope::unsupported);
    }
}

TEST(FullDesktop, VolumeIconsDoNotMaskMissingOrAdditionalDirectoryMembers) {
    @autoreleasepool {
        NSURL* file = [NSURL fileURLWithPath:@"/synthetic/Desktop/photo.png"];
        NSURL* other = [NSURL fileURLWithPath:@"/synthetic/Desktop/new-file"];
        NSURL* boot = [NSURL fileURLWithPath:@"/" isDirectory:YES];
        NSURL* disk = [NSURL fileURLWithPath:@"/Volumes/Test Disk" isDirectory:YES];
        NSArray* snapshot = @[ file, boot, disk ];
        NSSet* volumes = desktopPathSet(@[ boot, disk ]);
        EXPECT_TRUE(desktopDirectoryMatches(snapshot, volumes, @[ file ]));
        EXPECT_FALSE(desktopDirectoryMatches(snapshot, volumes, @[]));
        EXPECT_FALSE(desktopDirectoryMatches(snapshot, volumes, @[ file, other ]));
        EXPECT_FALSE(desktopDirectoryMatches(snapshot, volumes, nil));
        EXPECT_FALSE(desktopDirectoryMatches(@[ file, boot ], volumes, @[ file ]));
        EXPECT_FALSE(desktopDirectoryMatches(snapshot, volumes, @[ file, file ]));
        EXPECT_FALSE(desktopDirectoryMatches(snapshot, [NSSet set], @[ file ]));
    }
}

TEST(FullDesktop, ChineseFailureTextPreservesReasonAndRecoveryPhase) {
    @autoreleasepool {
        NSString* text = mirrorFailureMessage("磁盘已卸载", false, true);
        EXPECT_TRUE([text containsString:@"磁盘已卸载"]);
        EXPECT_TRUE([text containsString:@"尚未隐藏原图标，无需恢复"]);
        EXPECT_TRUE([mirrorFailureMessage("恢复错误", true, false) containsString:@"恢复未完成"]);
        EXPECT_TRUE([mirrorFailureMessage("停止", true, true) containsString:@"已完成程序恢复"]);
        EXPECT_TRUE([mirrorFailureMessage("\xff", false, true) containsString:@"读取或准备失败"]);
    }
}

TEST(MacSessionTrace, OwnerSampleReadsCurrentControlInsteadOfCopyingInitialDeadline) {
    NSDictionary* original = @{ @"session_id": @"fixture", @"started_ms": @1000 };
    NSMutableDictionary* current = [original mutableCopy];
    auto before = ownerTimingSample(original, current, true, 2000, 180000);
    current[@"started_ms"] = @4000;
    auto after = ownerTimingSample(original, current, true, 5000, 180000);
    EXPECT_EQ([before[@"current_owner_deadline_ms"] unsignedLongLongValue], 181000u);
    EXPECT_EQ([after[@"original_deadline_ms"] unsignedLongLongValue], 181000u);
    EXPECT_EQ([after[@"current_owner_deadline_ms"] unsignedLongLongValue], 184000u);
    EXPECT_EQ([after[@"sampled_ms"] unsignedLongLongValue], 5000u);
    EXPECT_EQ(ownerTimingSample(nil, nil, false, 5000, 180000).count, 0u);
}
TEST(MacSessionTrace, HelperDatagramsSampleIndependentControlWithoutRefreshingHeartbeat) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_DGRAM, 0, sockets), 0);
    NSDictionary* control = @{ @"session_id": @"helper-fixture", @"started_ms": @7000, @"limit_ms": @180000 };
    const unsigned long long heartbeat = 9000;
    uint64_t sequence = 7;
    ASSERT_EQ(send(sockets[0], &sequence, sizeof(sequence), MSG_DONTWAIT), sizeof(sequence));
    unsigned processed = 0;
    serviceHelperTimingRequests(sockets[1], control, heartbeat, true, 10000, processed);
    char bytes[4096];
    auto count = recv(sockets[0], bytes, sizeof(bytes), MSG_DONTWAIT);
    ASSERT_GT(count, 0);
    NSDictionary* sample = [NSJSONSerialization JSONObjectWithData:
        [NSData dataWithBytes:bytes length:static_cast<NSUInteger>(count)] options:0 error:nil];
    EXPECT_EQ([sample[@"helper_sample_request"] unsignedLongLongValue], 7u);
    EXPECT_EQ([sample[@"current_helper_deadline_ms"] unsignedLongLongValue], 187000u);
    EXPECT_EQ([sample[@"current_heartbeat_deadline_ms"] unsignedLongLongValue], 14000u);
    EXPECT_EQ(heartbeat, 9000u);
    EXPECT_EQ(processed, 1u);
    // Empty queue returns immediately; capped service cannot change control state.
    serviceHelperTimingRequests(sockets[1], control, heartbeat, true, 12000, processed);
    EXPECT_EQ(processed, 1u);
    processed = 260;
    serviceHelperTimingRequests(sockets[1], control, heartbeat, true, 12000, processed);
    EXPECT_EQ(processed, 260u);
    close(sockets[0]); close(sockets[1]);
}

TEST(FullDesktop, OptInLayoutWritebackBuildsExactFinderPositionsRollsBackOnFailureAndUndoes) {
    @autoreleasepool {
        DesktopReply source;
        auto snap = source.snapshot();
        ASSERT_EQ(snap.urls.count, 6u);

        const dpp::Rect primaryFrame{0.0f, 0.0f, 1728.0f, 1117.0f};
        const dpp::Rect visibleFrame{0.0f, 0.0f, 1728.0f, 1092.0f};  // 25pt menu bar at top
        std::vector<dpp::Vec2> newSceneCenters;
        for (int i = 0; i < 6; ++i) {
            newSceneCenters.push_back({240.0f + static_cast<float>(i) * 110.0f, 520.0f});
        }
        NSArray<NSDictionary*>* plan =
            buildMacLayoutWritebackPlan(snap, newSceneCenters, visibleFrame, primaryFrame);
        ASSERT_NE(plan, nil);
        ASSERT_EQ(plan.count, 6u);
        EXPECT_EQ([plan[0][@"orig_x"] intValue], 170);
        EXPECT_EQ([plan[0][@"orig_y"] intValue], 200);
        EXPECT_EQ([plan[0][@"new_x"] intValue], 240);
        EXPECT_EQ([plan[0][@"new_y"] intValue], 545);  // 25pt menu bar + 520pt scene Y

        // 1. Mid-batch writer failure on 4th item rolls back items 0..2 to original dpos!
        std::vector<std::pair<int, int>> mockPositions(6);
        for (int i = 0; i < 6; ++i) mockPositions[static_cast<std::size_t>(i)] = {170 + i * 70, 200};
        int callCount = 0;
        NSUInteger appliedCount = 0;
        NSString* errReason = nil;
        const bool partialOk = applyFinderDesktopWritebackPlan(
            plan, /*restoreOriginal=*/false, [&](NSAppleEventDescriptor* s) { return source.read(s); },
            [&](NSAppleEventDescriptor* itemRef, int x, int y) -> OSStatus {
                const int idx1 = [itemRef descriptorForKeyword:keyAEKeyData].int32Value;
                ++callCount;
                if (callCount == 4) return errAEEventFailed;
                mockPositions[static_cast<std::size_t>(idx1 - 1)] = {x, y};
                return noErr;
            },
            &appliedCount, &errReason);
        EXPECT_FALSE(partialOk);
        for (int i = 0; i < 6; ++i) {
            EXPECT_EQ(mockPositions[static_cast<std::size_t>(i)].first, 170 + i * 70);
            EXPECT_EQ(mockPositions[static_cast<std::size_t>(i)].second, 200);
        }

        // 2. Full opt-in write-back succeeds, and Undo (`restoreOriginal=true`) restores exact originals!
        ASSERT_TRUE(applyFinderDesktopWritebackPlan(
            plan, /*restoreOriginal=*/false, [&](NSAppleEventDescriptor* s) { return source.read(s); },
            [&](NSAppleEventDescriptor* itemRef, int x, int y) -> OSStatus {
                const int idx1 = [itemRef descriptorForKeyword:keyAEKeyData].int32Value;
                mockPositions[static_cast<std::size_t>(idx1 - 1)] = {x, y};
                return noErr;
            },
            &appliedCount, &errReason));
        EXPECT_EQ(appliedCount, 6u);
        for (int i = 0; i < 6; ++i) {
            EXPECT_EQ(mockPositions[static_cast<std::size_t>(i)].first, 240 + i * 110);
            EXPECT_EQ(mockPositions[static_cast<std::size_t>(i)].second, 545);
        }

        ASSERT_TRUE(applyFinderDesktopWritebackPlan(
            plan, /*restoreOriginal=*/true, [&](NSAppleEventDescriptor* s) { return source.read(s); },
            [&](NSAppleEventDescriptor* itemRef, int x, int y) -> OSStatus {
                const int idx1 = [itemRef descriptorForKeyword:keyAEKeyData].int32Value;
                mockPositions[static_cast<std::size_t>(idx1 - 1)] = {x, y};
                return noErr;
            },
            &appliedCount, &errReason));
        EXPECT_EQ(appliedCount, 6u);
        for (int i = 0; i < 6; ++i) {
            EXPECT_EQ(mockPositions[static_cast<std::size_t>(i)].first, 170 + i * 70);
            EXPECT_EQ(mockPositions[static_cast<std::size_t>(i)].second, 200);
        }
    }
}

TEST(FullDesktop, CachedDrawRendererRendersIconsWithoutRectangularBorderWhenUnoutlined) {
    @autoreleasepool {
        NSImage* image = [NSImage imageWithSystemSymbolName:@"doc.fill" accessibilityDescription:nil];
        NSRect rect = NSMakeRect(0, 0, 64, 64);
        CGImageRef cg = [image CGImageForProposedRect:&rect context:nil hints:nil];
        ASSERT_NE(cg, nullptr);
        CachedDrawRenderer renderer(@[ (__bridge id)cg ]);
        CALayer* root = [CALayer layer];
        dpp::DrawList draw;
        dpp::SpriteItem rectIcon{dpp::TextureId{1}, {120.0f, 300.0f}, {64.0f, 64.0f}, 0.25f};
        rectIcon.clipCircle = false;
        draw.add(rectIcon);
        dpp::SpriteItem ballIcon{dpp::TextureId{1}, {240.0f, 200.0f}, {36.0f, 36.0f}, 0.0f};
        ballIcon.clipCircle = true;
        draw.add(ballIcon);

        renderer.render(draw, root, CGRectMake(0, 0, 800, 600), 2.0, /*outlined=*/false);
        NSUInteger visibleLayers = 0;
        for (CALayer* sub in root.sublayers) {
            if (!sub.hidden) {
                ++visibleLayers;
                EXPECT_EQ(sub.borderWidth, 0.0);
            }
        }
        EXPECT_EQ(visibleLayers, 2u);
    }
}
