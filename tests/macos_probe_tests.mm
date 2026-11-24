#include <gtest/gtest.h>
#include <limits>
#include "platforms/macos/finder_read.h"
#include "platforms/macos/native_measurement.h"
using namespace dpp::mac;

// No Desktop enumeration, Apple Events, windows or permission requests in these tests.
TEST(MacProbe, RejectsMalformedGeometryWithoutInventingZeroCoordinates) {
    @autoreleasepool {
        EXPECT_EQ(geometry(nil, 2), nil);
        EXPECT_EQ(geometry([NSAppleEventDescriptor descriptorWithString:@"0,0"], 2), nil);
        auto list = [NSAppleEventDescriptor listDescriptor];
        [list insertDescriptor:[NSAppleEventDescriptor descriptorWithDouble:20] atIndex:1];
        EXPECT_EQ(geometry(list, 2), nil);
        [list insertDescriptor:[NSAppleEventDescriptor
                                   descriptorWithDouble:std::numeric_limits<double>::infinity()]
                       atIndex:2];
        EXPECT_EQ(geometry(list, 2), nil);
        [list removeDescriptorAtIndex:2];
        [list insertDescriptor:[NSAppleEventDescriptor descriptorWithString:@"12"] atIndex:2];
        EXPECT_EQ(geometry(list, 2), nil);
    }
}
TEST(MacProbe, PreservesNegativeAndFractionalRawCoordinates) {
    @autoreleasepool {
        auto list = [NSAppleEventDescriptor listDescriptor];
        [list insertDescriptor:[NSAppleEventDescriptor descriptorWithDouble:-125.5] atIndex:1];
        [list insertDescriptor:[NSAppleEventDescriptor descriptorWithInt32:80] atIndex:2];
        NSArray* result = geometry(list, 2);
        ASSERT_NE(result, nil);
        EXPECT_DOUBLE_EQ([result[0] doubleValue], -125.5);
        EXPECT_DOUBLE_EQ([result[1] doubleValue], 80);
    }
}
TEST(MacProbe, DecodesQuickDrawGeometryAndRejectsWrongLength) {
    @autoreleasepool {
        int16_t point[] = {80, -120};
        auto p = [NSAppleEventDescriptor descriptorWithDescriptorType:typeQDPoint
                                                                bytes:point
                                                               length:sizeof(point)];
        NSArray* xy = geometry(p, 2);
        ASSERT_NE(xy, nil);
        EXPECT_EQ([xy[0] intValue], -120);
        EXPECT_EQ([xy[1] intValue], 80);
        int16_t rectangle[] = {20, -100, 80, -40};
        auto r = [NSAppleEventDescriptor descriptorWithDescriptorType:typeQDRectangle
                                                                bytes:rectangle
                                                               length:sizeof(rectangle)];
        NSArray* ltrb = geometry(r, 4);
        ASSERT_NE(ltrb, nil);
        EXPECT_TRUE([ltrb isEqual:(@[ @-100, @20, @-40, @80 ])]);
        auto bad = [NSAppleEventDescriptor descriptorWithDescriptorType:typeQDPoint bytes:point length:2];
        EXPECT_EQ(geometry(bad, 2), nil);
        EXPECT_EQ(geometry(r, 2), nil);
    }
}
TEST(MacProbe, ObjectSpecifierUsesPublicFinderPropertyAndContainer) {
    @autoreleasepool {
        auto desktop = property('desk');
        auto position = property('dpos', desktop);
        ASSERT_EQ(position.descriptorType, typeObjectSpecifier);
        EXPECT_EQ([position descriptorForKeyword:keyAEDesiredClass].typeCodeValue, typeProperty);
        EXPECT_EQ([position descriptorForKeyword:keyAEKeyForm].enumCodeValue, formPropertyID);
        EXPECT_EQ([position descriptorForKeyword:keyAEKeyData].typeCodeValue, static_cast<OSType>('dpos'));
        auto container = [position descriptorForKeyword:keyAEContainer];
        EXPECT_EQ([container descriptorForKeyword:keyAEKeyData].typeCodeValue, static_cast<OSType>('desk'));
    }
}
TEST(MacProbe, EvidenceWriterNeverOverwritesPriorReport) {
    @autoreleasepool {
        NSString* directory = [NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
        ASSERT_TRUE([NSFileManager.defaultManager createDirectoryAtPath:directory
                                            withIntermediateDirectories:NO
                                                             attributes:nil
                                                                  error:nil]);
        NSString* file = [directory stringByAppendingPathComponent:@"report.json"];
        EXPECT_TRUE(writeJSON(@{@"status" : @"unknown"}, file));
        NSData* first = [NSData dataWithContentsOfFile:file];
        EXPECT_FALSE(writeJSON(@{@"status" : @"success"}, file));
        EXPECT_TRUE([first isEqual:[NSData dataWithContentsOfFile:file]]);
        EXPECT_TRUE([NSFileManager.defaultManager removeItemAtPath:directory error:nil]);
    }
}
TEST(MacProbe, EmptyMeasurementsAreUnknownAndPercentilesStayDistinct) {
    @autoreleasepool {
        dpp::Samples samples;
        EXPECT_EQ(statistics(samples)[@"p95_ms"], NSNull.null);
        for (int i = 1; i <= 100; ++i) samples.add(i);
        NSDictionary* result = statistics(samples);
        EXPECT_EQ([result[@"samples"] intValue], 100);
        EXPECT_DOUBLE_EQ([result[@"median_ms"] doubleValue], 50);
        EXPECT_DOUBLE_EQ([result[@"p95_ms"] doubleValue], 95);
        EXPECT_DOUBLE_EQ([result[@"p99_ms"] doubleValue], 99);
    }
}

#include "core/scene/coordinates.h"
#include "platforms/macos/desktop_access_state.h"
#include "platforms/macos/finder_snapshot.h"

TEST(MacAccess, PermissionCodesNeverInventUserChoiceOrLayoutSuccess) {
    EXPECT_EQ(permissionFromStatus(-1744), Permission::NeedsConsent);
    EXPECT_EQ(permissionFromStatus(-1743), Permission::NotPermitted);
    EXPECT_EQ(permissionFromStatus(-1712), Permission::Timeout);
    EXPECT_EQ(permissionFromStatus(-600), Permission::TargetUnavailable);
    EXPECT_EQ(permissionFromStatus(-609), Permission::TargetUnavailable);
    EXPECT_EQ(permissionFromStatus(-1708), Permission::Failed);
    DesktopAccessState state;
    EXPECT_FALSE(state.hasStatus);
    EXPECT_EQ(state.begin(AccessOperation::Check), 0u);  // no startup work
    state.show();
    auto token = state.begin(AccessOperation::Check);
    ASSERT_NE(token, 0u);
    EXPECT_TRUE(state.finish(token, 0));
    EXPECT_EQ(state.permission, Permission::Granted);
    EXPECT_EQ(state.parsed, 0u);
    EXPECT_FALSE(state.canPreview());
}
TEST(MacAccess, ExplicitConsentAndSingleOutstandingOperationRequired) {
    DesktopAccessState state;
    state.show();
    EXPECT_EQ(state.begin(AccessOperation::Request), 0u);
    EXPECT_EQ(state.begin(AccessOperation::Read), 0u);
    auto token = state.begin(AccessOperation::Request, true);
    ASSERT_NE(token, 0u);
    EXPECT_EQ(state.begin(AccessOperation::Request, true), 0u);
    EXPECT_EQ(state.begin(AccessOperation::Check), 0u);
    EXPECT_TRUE(state.finish(token, -1743));
    EXPECT_FALSE(state.busy);  // no automatic retry scheduled
    EXPECT_FALSE(state.canPreview());
}
TEST(MacAccess, ClosingAndReopeningDoesNotCancelOrDuplicatePendingSystemRequest) {
    DesktopAccessState state;
    state.show();
    auto token = state.begin(AccessOperation::Request, true);
    state.close();
    state.show();
    EXPECT_TRUE(state.busy);
    EXPECT_EQ(state.begin(AccessOperation::Request, true), 0u);
    EXPECT_FALSE(state.finish(token, 0));
    EXPECT_FALSE(state.busy);
    EXPECT_EQ(state.permission, Permission::Unknown);
    auto next = state.begin(AccessOperation::Check);
    ASSERT_NE(next, 0u);
    EXPECT_FALSE(state.finish(token, 0));
    EXPECT_TRUE(state.busy);
    EXPECT_TRUE(state.finish(next, -1744));
}
TEST(MacAccess, ExitDiscardsLateResultAndPreventsRestart) {
    DesktopAccessState state;
    state.show();
    auto token = state.begin(AccessOperation::Check);
    state.shutdown();
    EXPECT_FALSE(state.finish(token, 0));
    state.show();
    EXPECT_EQ(state.begin(AccessOperation::Check), 0u);
}
TEST(MacAccess, PreviewRequiresPermissionParsedPositionsAndIndependentCoordinateEvidence) {
    DesktopAccessState state;
    state.show();
    state.parsed = 3;
    state.coordinateValidated = true;
    EXPECT_FALSE(state.startPreview());
    state.permission = Permission::Granted;
    state.coordinateValidated = false;
    EXPECT_FALSE(state.startPreview());
    state.coordinateValidated = true;
    state.failed = 1;
    EXPECT_FALSE(state.startPreview());
    state.failed = 0;
    EXPECT_TRUE(state.startPreview());
    EXPECT_FALSE(state.alignmentAccepted);
    EXPECT_TRUE(state.acceptAlignment());
    state.stopPreview();
    state.stopPreview();
    EXPECT_FALSE(state.previewActive);
    EXPECT_FALSE(state.acceptAlignment());
    EXPECT_TRUE(state.startPreview());
    EXPECT_FALSE(state.alignmentAccepted);
    state.invalidateLayout();
    EXPECT_FALSE(state.canPreview());
}

namespace {
NSAppleEventDescriptor* fakePoint(int x, int y) {
    auto list = [NSAppleEventDescriptor listDescriptor];
    [list insertDescriptor:[NSAppleEventDescriptor descriptorWithInt32:x] atIndex:1];
    [list insertDescriptor:[NSAppleEventDescriptor descriptorWithInt32:y] atIndex:2];
    return list;
}
FinderSnapshot fakeSnapshot(bool badPosition, bool changedIdentity = false) {
    NSArray<NSURL*>* selected = @[
        [NSURL fileURLWithPath:@"/mock/Desktop/private-A"],
        [NSURL fileURLWithPath:@"/mock/Desktop/private-B"]
    ];
    auto list = [NSAppleEventDescriptor listDescriptor];
    for (int i = 1; i <= 2; ++i)
        [list insertDescriptor:object('cobj', formAbsolutePosition,
                                      [NSAppleEventDescriptor descriptorWithInt32:i], property('desk'))
                       atIndex:i];
    int urlReads = 0;
    return selectedSnapshot(selected, [&](NSAppleEventDescriptor* spec) -> ReadResult {
        if ([spec descriptorForKeyword:keyAEDesiredClass].typeCodeValue == 'cobj') return {list, 0};
        const auto code = [spec descriptorForKeyword:keyAEKeyData].typeCodeValue;
        if (code == 'vers') return {[NSAppleEventDescriptor descriptorWithString:@"mock-version"], 0};
        const int i =
            [[spec descriptorForKeyword:keyAEContainer] descriptorForKeyword:keyAEKeyData].int32Value;
        if (code == 'pURL') {
            ++urlReads;
            NSString* url = changedIdentity && urlReads == 2
                                ? @"file:///mock/Desktop/renamed"
                                : selected[static_cast<NSUInteger>(i - 1)].absoluteString;
            return {[NSAppleEventDescriptor descriptorWithString:url], 0};
        }
        if (code == 'dpos') {
            if (badPosition && i == 2) return {nil, -1712, "send_transport"};
            return {fakePoint(i * 100, i * 150), 0};
        }
        if (code == 'pbnd') return {nil, -1728, "finder_reply_error"};
        return {nil, -1708};
    });
}
}  // namespace
TEST(MacAccess, GrantedPermissionDoesNotMaskReadOrDescriptorFailure) {
    @autoreleasepool {
        auto selected = @[ [NSURL fileURLWithPath:@"/mock/Desktop/a"] ];
        auto snapshot = selectedSnapshot(
            selected, [](NSAppleEventDescriptor*) -> ReadResult { return {nil, -1712, "send_transport"}; });
        EXPECT_TRUE([snapshot.report[@"request"] isEqual:@"failed"]);
        EXPECT_EQ([snapshot.report[@"osstatus"] intValue], -1712);
        EXPECT_EQ([snapshot.report[@"query_failed"] intValue], 1);
        auto malformed = selectedSnapshot(selected, [](NSAppleEventDescriptor*) -> ReadResult {
            return {[NSAppleEventDescriptor descriptorWithInt32:1], 0};
        });
        EXPECT_TRUE([malformed.report[@"parse"] isEqual:@"invalid_or_missing_text"]);
    }
}
TEST(MacAccess, PartialItemsAndUnknownRectanglesStayIndependentAndRedacted) {
    @autoreleasepool {
        auto partial = fakeSnapshot(true);
        EXPECT_EQ([partial.report[@"position_success"] intValue], 1);
        EXPECT_EQ([partial.report[@"position_failed_or_unmatched"] intValue], 1);
        auto complete = fakeSnapshot(false);
        EXPECT_EQ([complete.report[@"position_success"] intValue], 2);
        EXPECT_TRUE([complete.report[@"request"] isEqual:@"partial_failure"]);  // pbnd unsupported
        for (NSDictionary* row in complete.report[@"items"]) {
            EXPECT_EQ(row[@"image_rect"], NSNull.null);
            EXPECT_EQ(row[@"label_rect"], NSNull.null);
            EXPECT_TRUE([row[@"image_rect_validation"] isEqual:@"unknown"]);
        }
        NSData* encoded = [NSJSONSerialization dataWithJSONObject:complete.report options:0 error:nil];
        NSString* json = [[NSString alloc] initWithData:encoded encoding:NSUTF8StringEncoding];
        EXPECT_FALSE([json containsString:@"private-A"]);
        EXPECT_FALSE([json containsString:@"/mock"]);
        EXPECT_EQ(complete.urls.count, 2u);  // private identity stays in memory only
    }
}
TEST(MacAccess, SnapshotRejectsIdentityChangeDuringRead) {
    @autoreleasepool {
        auto changed = fakeSnapshot(false, true);
        EXPECT_EQ([changed.report[@"position_success"] intValue], 1);
        EXPECT_EQ(changed.report[@"items"][0][@"position_raw"], NSNull.null);
    }
}
TEST(MacAccess, PointerCalibrationUsesLogicalPointsAndRejectsRetinaAndWrongOrigin) {
    const dpp::Rect frame{0, 0, 1728, 1117};
    EXPECT_TRUE(dpp::macPointerMatchesTopLeft({1600, 200}, {1600, 917}, frame));
    EXPECT_FALSE(dpp::macPointerMatchesTopLeft({1600, 200}, {1600, 200}, frame));
    EXPECT_FALSE(dpp::macPointerMatchesTopLeft({3200, 400}, {1600, 917}, frame));
    EXPECT_FALSE(dpp::macPointerMatchesTopLeft({-1, 200}, {-1, 917}, frame));
    EXPECT_FALSE(dpp::macPointerMatchesTopLeft({NAN, 200}, {1600, 917}, frame));
    auto global = dpp::macTopLeftPointsToGlobal({1600, 200}, frame);
    auto scene = dpp::macGlobalPointsToScene(global, frame);
    EXPECT_FLOAT_EQ(scene.x, 1600);
    EXPECT_FLOAT_EQ(scene.y, 200);
}
TEST(MacProbe, EveryItemSpecifierUsesAbsoluteOrdinalNotEnumeration) {
    @autoreleasepool {
        auto spec = allItems(property('desk'));
        auto key = [spec descriptorForKeyword:keyAEKeyData];
        EXPECT_EQ(key.descriptorType, typeAbsoluteOrdinal);
        ASSERT_EQ(key.data.length, sizeof(OSType));
        OSType ordinal = 0;
        [key.data getBytes:&ordinal length:sizeof(ordinal)];
        EXPECT_EQ(ordinal, static_cast<OSType>(kAEAll));
        EXPECT_EQ([spec descriptorForKeyword:keyAEKeyForm].enumCodeValue, formAbsolutePosition);
    }
}
TEST(MacAccess, ParsedMinusOnePairAndCoincidentSamplesCannotEnableCalibration) {
    const dpp::Rect frame{0, 0, 1728, 1117};
    const std::array<dpp::Vec2, 2> actualFinderReply{{{-1, -1}, {-1, -1}}};
    EXPECT_FALSE(dpp::macTopLeftSampleSetUsable(actualFinderReply, frame));
    const std::array<dpp::Vec2, 2> coincident{{{200, 200}, {200, 200}}};
    EXPECT_FALSE(dpp::macTopLeftSampleSetUsable(coincident, frame));
    const std::array<dpp::Vec2, 2> spaced{{{200, 200}, {600, 500}}};
    EXPECT_TRUE(dpp::macTopLeftSampleSetUsable(spaced, frame));
    EXPECT_FALSE(dpp::macTopLeftSampleSetUsable({}, frame));
}
TEST(MacAccess, MirrorRecheckRejectsMovedReplacedOrMismatchedItemsWithoutEnumeration) {
    @autoreleasepool {
        auto snapshot = fakeSnapshot(false);
        ASSERT_EQ(snapshot.references.count, 2u);
        for (int failure = 0; failure <= 5; ++failure) {
            int queries = 0;
            bool identityCalled = false;
            bool matched = selectedPositionsMatch(
                snapshot,
                [&](NSAppleEventDescriptor* spec) -> ReadResult {
                    ++queries;
                    auto code = [spec descriptorForKeyword:keyAEKeyData].typeCodeValue;
                    EXPECT_TRUE(code == 'pURL' || code == 'dpos');
                    int i = [[spec descriptorForKeyword:keyAEContainer] descriptorForKeyword:keyAEKeyData]
                                .int32Value;
                    if (failure == 3) return {nil, -1712};
                    if (code == 'pURL') {
                        NSString* value = (failure == 1 || (failure == 5 && queries == 3))
                                              ? @"file:///mock/Desktop/replacement"
                                              : snapshot.urls[static_cast<NSUInteger>(i - 1)].absoluteString;
                        return {[NSAppleEventDescriptor descriptorWithString:value], 0};
                    }
                    return {fakePoint(i * 100 + (failure == 2 ? 1 : 0), i * 150), 0};
                },
                [&](NSUInteger, NSURL*) {
                    identityCalled = true;
                    return failure != 4;
                });
            EXPECT_EQ(matched, failure == 0);
            EXPECT_LE(queries, 6);
            if (failure == 0 || failure == 4) EXPECT_TRUE(identityCalled);
        }
    }
}

TEST(MacAccess, DesktopIconSizePreservesRawValueAndRejectsUnknownOrMalformedSetting) {
    @autoreleasepool {
        auto valid = desktopIconSize([](NSAppleEventDescriptor* spec) {
            EXPECT_EQ([spec descriptorForKeyword:keyAEKeyData].typeCodeValue, 'lvis');
            auto options = [spec descriptorForKeyword:keyAEContainer];
            EXPECT_EQ([options descriptorForKeyword:keyAEKeyData].typeCodeValue, 'icop');
            auto desktop = [options descriptorForKeyword:keyAEContainer];
            EXPECT_EQ([desktop descriptorForKeyword:keyAEKeyData].typeCodeValue, 'cwnd');
            auto owner = [desktop descriptorForKeyword:keyAEContainer];
            EXPECT_EQ([owner descriptorForKeyword:keyAEKeyData].typeCodeValue, 'desk');
            return ReadResult{[NSAppleEventDescriptor descriptorWithInt32:64], noErr};
        });
        EXPECT_TRUE([valid[@"usable"] boolValue]);
        EXPECT_EQ([valid[@"raw"] intValue], 64);
        for (double size : {0.0, -1.0, 64.5, 256.0}) {
            auto bad = desktopIconSize([size](NSAppleEventDescriptor*) {
                return ReadResult{[NSAppleEventDescriptor descriptorWithDouble:size], noErr};
            });
            EXPECT_FALSE([bad[@"usable"] boolValue]);
            EXPECT_EQ([bad[@"raw"] doubleValue], size);
        }
        auto error = desktopIconSize([](NSAppleEventDescriptor*) { return ReadResult{nil, -1708}; });
        EXPECT_FALSE([error[@"usable"] boolValue]);
        EXPECT_EQ(error[@"raw"], NSNull.null);
        EXPECT_EQ([error[@"osstatus"] intValue], -1708);
        auto text = desktopIconSize([](NSAppleEventDescriptor*) {
            return ReadResult{[NSAppleEventDescriptor descriptorWithString:@"64"], noErr};
        });
        EXPECT_FALSE([text[@"usable"] boolValue]);
    }
}
TEST(MacAccess, ChangedIconSizeInvalidatesSnapshotBeforePositionOrIdentityChecks) {
    @autoreleasepool {
        FinderSnapshot snapshot;
        snapshot.urls = @[ [NSURL fileURLWithPath:@"/synthetic/a"], [NSURL fileURLWithPath:@"/synthetic/b"] ];
        snapshot.references =
            @[ NSAppleEventDescriptor.nullDescriptor, NSAppleEventDescriptor.nullDescriptor ];
        snapshot.report =
            @{@"items" : @[ @{}, @{} ],
              @"desktop_icon_size" : @{@"usable" : @YES, @"raw" : @64}};
        int queries = 0;
        EXPECT_FALSE(selectedPositionsMatch(
            snapshot,
            [&](NSAppleEventDescriptor*) {
                ++queries;
                return ReadResult{[NSAppleEventDescriptor descriptorWithInt32:96], noErr};
            },
            [](NSUInteger, NSURL*) {
                ADD_FAILURE() << "must reject size change before identity check";
                return true;
            }));
        EXPECT_EQ(queries, 1);
    }
}
