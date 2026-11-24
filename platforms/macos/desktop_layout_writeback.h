#pragma once
// Opt-in Finder Desktop icon position write-back & undo backup helper.
// Never invoked during normal session stop, Esc, timeout, crash recovery, or unit tests.
// Uses injected FinderReader + FinderPositionWriter for 100% offline unit testability.

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#include <cmath>
#include <functional>
#include <vector>

#include "core/scene/coordinates.h"
#include "platforms/macos/full_desktop_snapshot.h"

namespace dpp::mac {

using FinderPositionWriter = std::function<OSStatus(NSAppleEventDescriptor* itemSpec, int x, int y)>;

inline NSString* const kSavedLayoutBackupDefaultsKey = @"dpp.savedDesktopLayoutBackup.v1";

inline OSStatus nativeFinderPositionWriter(NSAppleEventDescriptor* itemSpec, int x, int y) {
    if (!itemSpec) return paramErr;
    if ([NSRunningApplication runningApplicationsWithBundleIdentifier:@"com.apple.finder"].count != 1) {
        return procNotFound;
    }
    NSAppleEventDescriptor* target =
        [NSAppleEventDescriptor descriptorWithBundleIdentifier:@"com.apple.finder"];
    NSAppleEventDescriptor* event =
        [NSAppleEventDescriptor appleEventWithEventClass:kAECoreSuite
                                                 eventID:kAESetData
                                        targetDescriptor:target
                                                returnID:kAutoGenerateReturnID
                                           transactionID:kAnyTransactionID];
    NSAppleEventDescriptor* prop = property('dpos', itemSpec);
    NSAppleEventDescriptor* ptList = [NSAppleEventDescriptor listDescriptor];
    [ptList insertDescriptor:[NSAppleEventDescriptor descriptorWithInt32:x] atIndex:1];
    [ptList insertDescriptor:[NSAppleEventDescriptor descriptorWithInt32:y] atIndex:2];
    [event setParamDescriptor:prop forKeyword:keyDirectObject];
    [event setParamDescriptor:ptList forKeyword:keyAEData];

    AppleEvent reply{typeNull, nullptr};
    const OSStatus sendErr =
        AESendMessage(event.aeDesc, &reply, kAEWaitReply | kAENeverInteract | kAEDoNotPromptForUserConsent, 180);
    if (sendErr != noErr) {
        AEDisposeDesc(&reply);
        return sendErr;
    }
    DescType typeCode = typeNull;
    Size actualSize = 0;
    SInt32 handlerErr = 0;
    if (AEGetParamPtr(&reply, keyErrorNumber, typeSInt32, &typeCode, &handlerErr, sizeof(handlerErr),
                      &actualSize) == noErr &&
        handlerErr != 0) {
        AEDisposeDesc(&reply);
        return static_cast<OSStatus>(handlerErr);
    }
    AEDisposeDesc(&reply);
    return noErr;
}

inline NSArray<NSDictionary*>* buildMacLayoutWritebackPlan(const FinderSnapshot& originalSnapshot,
                                                           const std::vector<Vec2>& currentSceneCenters,
                                                           const Rect& visibleFrame,
                                                           const Rect& primaryScreenFrame) {
    NSArray* origRows = [originalSnapshot.report[@"items"] isKindOfClass:NSArray.class]
                            ? originalSnapshot.report[@"items"]
                            : nil;
    if (!origRows || originalSnapshot.urls.count == 0 ||
        originalSnapshot.urls.count != origRows.count ||
        currentSceneCenters.size() != originalSnapshot.urls.count) {
        return nil;
    }
    NSMutableArray<NSDictionary*>* plan = [NSMutableArray arrayWithCapacity:originalSnapshot.urls.count];
    for (NSUInteger i = 0; i < originalSnapshot.urls.count; ++i) {
        NSString* pathId = fullDesktopURLIdentity(originalSnapshot.urls[i]);
        NSArray* rawXY = [origRows[i][@"position_raw"] isKindOfClass:NSArray.class]
                             ? origRows[i][@"position_raw"]
                             : nil;
        if (!pathId || !rawXY || rawXY.count < 2) return nil;
        const Vec2 topLeft = macSceneToTopLeftPoints(currentSceneCenters[i], visibleFrame, primaryScreenFrame);
        const int ox = static_cast<int>(std::lround([rawXY[0] doubleValue]));
        const int oy = static_cast<int>(std::lround([rawXY[1] doubleValue]));
        const int nx = std::clamp(static_cast<int>(std::lround(topLeft.x)), 16,
                                  std::max(16, static_cast<int>(std::lround(primaryScreenFrame.width)) - 16));
        const int ny = std::clamp(static_cast<int>(std::lround(topLeft.y)), 28,
                                  std::max(28, static_cast<int>(std::lround(primaryScreenFrame.height)) - 16));
        [plan addObject:@{
            @"path" : pathId,
            @"orig_x" : @(ox),
            @"orig_y" : @(oy),
            @"new_x" : @(nx),
            @"new_y" : @(ny),
        }];
    }
    return plan;
}

inline NSArray<NSDictionary*>* buildMacLayoutWritebackPlanSubset(
    const FinderSnapshot& originalSnapshot, const std::vector<std::size_t>& activeOrigIndices,
    const std::vector<Vec2>& currentSceneCenters, const Rect& visibleFrame, const Rect& primaryScreenFrame,
    const Rect& clampGlobalBounds) {
    NSArray* origRows = [originalSnapshot.report[@"items"] isKindOfClass:NSArray.class]
                            ? originalSnapshot.report[@"items"]
                            : nil;
    if (!origRows || originalSnapshot.urls.count == 0 ||
        originalSnapshot.urls.count != origRows.count || activeOrigIndices.empty() ||
        currentSceneCenters.size() != activeOrigIndices.size()) {
        return nil;
    }
    NSMutableArray<NSDictionary*>* plan =
        [NSMutableArray arrayWithCapacity:activeOrigIndices.size()];
    for (std::size_t k = 0; k < activeOrigIndices.size(); ++k) {
        const std::size_t i = activeOrigIndices[k];
        if (i >= originalSnapshot.urls.count) return nil;
        NSString* pathId = fullDesktopURLIdentity(originalSnapshot.urls[i]);
        NSArray* rawXY = [origRows[i][@"position_raw"] isKindOfClass:NSArray.class]
                             ? origRows[i][@"position_raw"]
                             : nil;
        if (!pathId || !rawXY || rawXY.count < 2) return nil;
        Vec2 globalPt = macSceneToGlobalPoints(currentSceneCenters[k], visibleFrame);
        if (clampGlobalBounds.width > 48.0f && clampGlobalBounds.height > 48.0f) {
            globalPt.x = std::clamp(globalPt.x, clampGlobalBounds.x + 16.0f,
                                    clampGlobalBounds.x + clampGlobalBounds.width - 16.0f);
            globalPt.y = std::clamp(globalPt.y, clampGlobalBounds.y + 16.0f,
                                    clampGlobalBounds.y + clampGlobalBounds.height - 28.0f);
        }
        const Vec2 clampedScene = macGlobalPointsToScene(globalPt, visibleFrame);
        const Vec2 topLeft = macSceneToTopLeftPoints(clampedScene, visibleFrame, primaryScreenFrame);
        const int ox = static_cast<int>(std::lround([rawXY[0] doubleValue]));
        const int oy = static_cast<int>(std::lround([rawXY[1] doubleValue]));
        const int nx = static_cast<int>(std::lround(topLeft.x));
        const int ny = static_cast<int>(std::lround(topLeft.y));
        [plan addObject:@{
            @"path" : pathId,
            @"orig_x" : @(ox),
            @"orig_y" : @(oy),
            @"new_x" : @(nx),
            @"new_y" : @(ny),
        }];
    }
    return plan;
}

inline bool applyFinderDesktopWritebackPlan(NSArray<NSDictionary*>* plan, bool restoreOriginal,
                                            const FinderReader& read, const FinderPositionWriter& write,
                                            NSUInteger* outAppliedCount = nullptr,
                                            NSString** outError = nullptr) {
    if (outAppliedCount) *outAppliedCount = 0;
    if (!plan || plan.count == 0) {
        if (outError) *outError = @"无可写回的桌面图标记录";
        return false;
    }
    NSMutableDictionary<NSString*, NSDictionary*>* byPath =
        [NSMutableDictionary dictionaryWithCapacity:plan.count];
    for (NSDictionary* entry in plan) {
        if (![entry isKindOfClass:NSDictionary.class]) continue;
        NSString* p = [entry[@"path"] isKindOfClass:NSString.class] ? entry[@"path"] : nil;
        if (p.length) byPath[p] = entry;
    }
    if (byPath.count == 0) {
        if (outError) *outError = @"桌面布局备份格式无效";
        return false;
    }

    ReadResult listRes = read(allItems(property('desk')));
    if (listRes.error != noErr || !listRes.value || listRes.value.descriptorType != typeAEList) {
        if (outError) *outError = @"无法枚举 Finder 桌面项目（请确认桌面已恢复显示且已授权自动化）";
        return false;
    }

    struct AppliedItem {
        NSAppleEventDescriptor* spec = nil;
        int origX = 0;
        int origY = 0;
    };
    std::vector<AppliedItem> applied;
    applied.reserve(byPath.count);

    const NSInteger total = std::min<NSInteger>(listRes.value.numberOfItems, 256);
    for (NSInteger i = 1; i <= total; ++i) {
        NSAppleEventDescriptor* item = [listRes.value descriptorAtIndex:i];
        if (!item) continue;
        ReadResult urlRes = read(property('pURL', item));
        if (urlRes.error != noErr || !urlRes.value.stringValue) continue;
        NSURL* url = [NSURL URLWithString:urlRes.value.stringValue];
        NSString* pathId = fullDesktopURLIdentity(url);
        if (!pathId) continue;
        NSDictionary* match = byPath[pathId];
        if (!match) continue;

        const int ox = [match[@"orig_x"] intValue];
        const int oy = [match[@"orig_y"] intValue];
        const int tx = restoreOriginal ? ox : [match[@"new_x"] intValue];
        const int ty = restoreOriginal ? oy : [match[@"new_y"] intValue];

        const OSStatus st = write(item, tx, ty);
        if (st != noErr) {
            if (!restoreOriginal) {
                for (const auto& prev : applied) {
                    (void)write(prev.spec, prev.origX, prev.origY);
                }
            }
            if (outError) {
                *outError = [NSString
                    stringWithFormat:@"Finder 拒绝设置图标位置 (OSStatus %d)，已回滚本次修改", static_cast<int>(st)];
            }
            return false;
        }
        applied.push_back({item, ox, oy});
    }

    if (applied.empty()) {
        if (outError) *outError = @"未匹配到原桌面项目，未移动任何图标";
        return false;
    }
    if (outAppliedCount) *outAppliedCount = applied.size();
    return true;
}

}  // namespace dpp::mac
