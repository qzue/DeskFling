#include "platforms/macos/desktop_access.h"
#include <algorithm>
#include <cmath>
#include <exception>
#include <vector>
#include "core/scene/coordinates.h"
#include "platforms/macos/app_identity.h"
#include "platforms/macos/assisted_experiment.h"
#include "platforms/macos/finder_snapshot.h"
#include "platforms/macos/mirror_preview.h"
#include "platforms/macos/preview_image.h"
using namespace dpp::mac;
namespace {
template <class F>
void nativeGuard(F&& f) {
    @try {
        try {
            f();
        } catch (...) {
            NSLog(@"Desktop check callback failed; no desktop changes performed.");
        }
    } @catch (NSException*) {
        NSLog(@"Desktop check native exception; no desktop changes performed.");
    }
}
dpp::Rect frameRect(NSRect r) {
    return {static_cast<float>(r.origin.x), static_cast<float>(r.origin.y), static_cast<float>(r.size.width),
            static_cast<float>(r.size.height)};
}
dpp::Vec2 point(NSArray* values) { return {[values[0] floatValue], [values[1] floatValue]}; }
}  // namespace
static NSString* assistedPendingKey() {
#ifdef DPP_MIRROR_FAULT_TESTING
    return @"DPPAssistedFixturePendingV1";
#else
    return @"DPPAssistedPendingV1";
#endif
}

@interface DPPAlignmentWindow : NSWindow
@end
@implementation DPPAlignmentWindow
- (BOOL)canBecomeKeyWindow {
    return NO;
}
- (BOOL)canBecomeMainWindow {
    return NO;
}
@end
@interface DPPAlignmentView : NSView
@property(copy) NSArray* marks;
@end
@implementation DPPAlignmentView
- (BOOL)isFlipped {
    return YES;
}
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    nativeGuard([&] {
        [NSColor.systemOrangeColor setStroke];
        for (NSDictionary* mark in self.marks) {
            NSArray* xy = mark[@"scene"];
            CGFloat x = [xy[0] doubleValue], y = [xy[1] doubleValue];
            NSBezierPath* path = [NSBezierPath bezierPath];
            [path moveToPoint:NSMakePoint(x - 9, y)];
            [path lineToPoint:NSMakePoint(x + 9, y)];
            [path moveToPoint:NSMakePoint(x, y - 9)];
            [path lineToPoint:NSMakePoint(x, y + 9)];
            path.lineWidth = 2;
            [path stroke];
            [[NSString stringWithFormat:@"#%@ dpos · 人工采样 ±12pt", mark[@"id"]]
                   drawAtPoint:NSMakePoint(x - 170, y + 12)
                withAttributes:@{
                    NSFontAttributeName : [NSFont systemFontOfSize:11],
                    NSForegroundColorAttributeName : NSColor.systemOrangeColor,
                    NSBackgroundColorAttributeName : [NSColor.blackColor colorWithAlphaComponent:0.8]
                }];
        }
    });
}
@end
@interface DPPDesktopAccess () {
    DesktopAccessState _state;
    AssistedExperiment _experiment;
    double _snapshotScale;
    FinderSnapshot _snapshot;
    NSUInteger _calibrationIndex;
    BOOL _confirming, _choosing, _calibrating;
    NSRect _snapshotFrame;
    NSTimeInterval _snapshotTime;
    double _snapshotUptime, _lastVerified;
    NSRect _snapshotWorkArea;
    uint64_t _mirrorEpoch;
    BOOL _verificationBusy, _verificationInFlight;
    uint32_t _nextStableID;
    dpp::mac::MirrorSnapshot _mirrorSnapshot;
}
@property(copy) NSString* snapshotID;
@property(copy) NSString* experimentID;
@property(copy) NSDictionary* experimentReport;
@property(strong) NSPanel* panel;
@property(strong) NSTextView* text;
@property(strong) NSMutableArray<NSButton*>* buttons;
@property(copy) NSArray<NSURL*>* selected;
@property(strong) NSMutableArray* calibration;
@property(strong) NSMutableArray* history;
@property(copy) NSString* notice;
@property(copy) NSString* directoryState;
@property(copy) NSString* imageState;
@property(strong) NSArray* imageResults;
@property(strong) NSTimer* sampleTimer;
@property(strong) NSTimer* closeTimer;
@property(strong) NSTimer* expiryTimer;
@property(strong) DPPAlignmentWindow* overlay;
@property(strong) DPPMirrorPreview* mirror;
@property(strong) NSMutableArray* cachedImages;
@property(strong) NSMutableArray* identityRegistry;
@property(strong) NSMutableArray* resourceIDs;
@property(strong) NSMutableArray* volumeIDs;
@property(strong) NSTimer* verificationTimer;
@property(strong) NSTimer* verificationDeadline;
@end
@implementation DPPDesktopAccess
- (instancetype)init {
    self = [super init];
    if (self) {
        _selected = @[];
        _identityRegistry = [NSMutableArray array];
        _nextStableID = 1;
        _mirror = [DPPMirrorPreview new];
        __weak DPPDesktopAccess* weakSelf = self;
        _mirror.didClose = ^(NSString* reason) {
          DPPDesktopAccess* owner = weakSelf;
          if (!owner) return;
          ++owner->_mirrorEpoch;
          owner->_verificationBusy = NO;
          [owner.verificationTimer invalidate];
          owner.verificationTimer = nil;
          [owner.verificationDeadline invalidate];
          owner.verificationDeadline = nil;
          if (owner->_experiment.phase == AssistedExperiment::Phase::Running) {
              owner->_experiment.finish();
              owner.cachedImages = nil;
              owner.resourceIDs = nil;
              owner.volumeIDs = nil;
              owner->_snapshot = FinderSnapshot{};
              owner.selected = @[];
              owner->_mirrorSnapshot = {};
              owner->_snapshotTime = 0;
              [owner.mirror recordPreparedCleanup:owner.cachedImages.count
                                       references:owner->_snapshot.references.count];
              [owner.mirror recordHuman:owner.experimentReport ? owner.experimentReport : @{}];
          }
          owner.notice =
              [NSString stringWithFormat:@"镜像预览已关闭：%@。请检查本次手动设置；应用不自动恢复。", reason];
          if ([reason containsString:@"reacquire"] || [reason containsString:@"changed"] ||
              [reason containsString:@"failed"] || [reason containsString:@"timeout"])
              owner->_state.coordinateValidated = owner->_state.alignmentAccepted = false;
          if (owner->_state.open) [owner refresh];
        };
        _calibration = [NSMutableArray array];
        _history = [NSMutableArray array];
        _directoryState = @"not_requested";
        _imageState = @"not_requested";
        _imageResults = @[];
        _notice = @"启动不扫描桌面、不检查或申请 Finder 权限。请先检查当前权限。";
        if ([NSUserDefaults.standardUserDefaults objectForKey:assistedPendingKey()])
            _notice = @"上次实验设置可能尚未恢复。请检查本次实际改动；这只是提醒，不是系统状态备份。";
        [NSNotificationCenter.defaultCenter addObserver:self
                                               selector:@selector(screenChanged:)
                                                   name:NSApplicationDidChangeScreenParametersNotification
                                                 object:nil];
        [NSWorkspace.sharedWorkspace.notificationCenter
            addObserver:self
               selector:@selector(screenChanged:)
                   name:NSWorkspaceActiveSpaceDidChangeNotification
                 object:nil];
    }
    return self;
}
- (void)show {
    nativeGuard([&] {
        if (!_panel) {
            _panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 820, 660)
                                                styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                  backing:NSBackingStoreBuffered
                                                    defer:NO];
            _panel.title = @"桌面接入检查 · 镜像重力开发预览";
            _panel.level = NSFloatingWindowLevel;
            _panel.releasedWhenClosed = NO;
            _panel.delegate = self;
            _buttons = [NSMutableArray array];
            NSArray* titles = @[
                @"检查当前权限", @"申请 Finder 布局读取权限", @"选择项目并读取布局",
                @"采样下一项目中心（5 秒）", @"开启只读对齐预览", @"关闭预览", @"标记对齐一致", @"标记不一致",
                @"导出脱敏诊断", @"普通桌面镜像预览（静态）", @"开始／暂停重力", @"重置副本",
                @"进入／退出交互", @"关闭重力预览", @"用户辅助隐藏实验（已停用）", @"查看历史会话",
                @"手动恢复说明／确认", @"记录最后会话人工观察"
            ];
            for (NSUInteger i = 0; i < titles.count; ++i) {
                NSButton* b = [NSButton buttonWithTitle:titles[i] target:self action:@selector(action:)];
                b.tag = static_cast<NSInteger>(i);
                b.frame = NSMakeRect(16 + (i % 3) * 266, 612 - (i / 3) * 40, 258, 32);
                [_panel.contentView addSubview:b];
                [_buttons addObject:b];
            }
            NSScrollView* scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(16, 16, 788, 366)];
            scroll.hasVerticalScroller = YES;
            _text = [[NSTextView alloc] initWithFrame:scroll.bounds];
            _text.editable = NO;
            _text.font = [NSFont systemFontOfSize:13];
            _text.textContainerInset = NSMakeSize(10, 10);
            _text.autoresizingMask = NSViewWidthSizable;
            _text.textContainer.widthTracksTextView = YES;
            scroll.documentView = _text;
            [_panel.contentView addSubview:scroll];
            [_panel center];
        }
        _state.show();
        [self refresh];
        [_panel makeKeyAndOrderFront:nil];
    });
}
- (BOOL)snapshotFresh {
    return _snapshotTime > 0 && NSDate.timeIntervalSinceReferenceDate - _snapshotTime < 300 &&
           NSScreen.screens.count == 1 && NSScreen.screens.firstObject.backingScaleFactor == _snapshotScale &&
           NSEqualRects(NSScreen.screens.firstObject.frame, _snapshotFrame) &&
           NSEqualRects(NSScreen.screens.firstObject.visibleFrame, _snapshotWorkArea);
}
- (BOOL)candidateCoordinatesUsable {
    std::vector<dpp::Vec2> points;
    for (NSDictionary* row in _snapshot.report[@"items"]) {
        if (![row[@"position_raw"] isKindOfClass:NSArray.class]) return NO;
        points.push_back(point(row[@"position_raw"]));
    }
    return NSScreen.screens.count == 1 && dpp::macTopLeftSampleSetUsable(points, frameRect(_snapshotFrame));
}
- (void)refresh {
    const bool idle = !_mirror.active && !_verificationBusy && !_verificationInFlight && !_state.busy &&
                      !_confirming && !_choosing && !_calibrating &&
                      _experiment.phase != AssistedExperiment::Phase::Waiting &&
                      _experiment.phase != AssistedExperiment::Phase::Verified;
    _buttons[0].enabled = idle;
    _buttons[1].enabled = idle && _state.permission != Permission::Granted;
    _buttons[2].enabled = idle && _state.permission == Permission::Granted;
    _buttons[3].enabled = idle && _state.parsed >= 2 && _state.failed == 0 && [self snapshotFresh] &&
                          [self candidateCoordinatesUsable] && !_state.previewActive;
    NSArray* pendingRows = _snapshot.report[@"items"];
    NSUInteger next = _calibrationIndex < pendingRows.count ? _calibrationIndex : 0;
    _buttons[3].title =
        pendingRows.count ? [NSString stringWithFormat:@"采样 #%@ 图标中心（5 秒）", pendingRows[next][@"id"]]
                          : @"采样下一项目中心（5 秒）";
    _buttons[4].enabled = idle && _state.canPreview() && [self snapshotFresh] && !_state.previewActive;
    _buttons[5].enabled = _overlay != nil;
    _buttons[6].enabled = _state.previewActive;
    _buttons[7].enabled = _state.previewActive;
    _buttons[8].enabled = !_confirming && !_choosing;
    _buttons[9].enabled = idle && _state.canPreview() && _state.alignmentAccepted && [self snapshotFresh];
    _buttons[10].enabled = _mirror.active;
    _buttons[11].enabled = _mirror.active;
    _buttons[12].enabled = _mirror.active;
    _buttons[13].enabled = _mirror.active || _verificationBusy || _experiment.preparedImages > 0;
    _buttons[14].enabled = YES;  // Explanation only; legacy action is guarded too.
    _buttons[15].enabled = YES;
    _buttons[16].enabled = !_mirror.active && !_experiment.preparedImages;
    _buttons[17].enabled = !_mirror.active && _mirror.sessionSummaries.count > 0;
    NSMutableString* value = [NSMutableString
        stringWithFormat:
            @"%@\n\nFinder Automation：%@（OSStatus %@）\nDesktop 目录访问：%@\n系统图标读取：%@\nFinder "
            @"请求：%@\n原始解析：%@；成功 %zu，失败/未匹配 "
            @"%zu\n坐标转换：%@\n图像矩形：未知；标签矩形：未知\n只读预览：%@；人工对齐验收：%@"
            @"\n无双影：阻塞；真实桌面模式：禁用\n\n",
            _notice, permissionName(_state.permission), _state.hasStatus ? @(_state.rawStatus) : @"未检查",
            _directoryState, _imageState, _snapshot.report[@"request"], _snapshot.report[@"parse"],
            _state.parsed, _state.failed,
            _state.coordinateValidated ? @"本快照单屏多点人工采样符合 ±12pt" : @"未验证",
            _state.previewActive ? @"显示中（60 秒自动关闭）" : @"已关闭",
            _state.alignmentAccepted ? @"用户报告一致" : @"未通过/未执行"];
    if (_snapshot.report[@"stage"]) {
        [value appendFormat:@"读取阶段：%@；OSStatus：%@；返回类型：%@\n查询成功 %@，失败 %@\n\n",
                            _snapshot.report[@"stage"],
                            _snapshot.report[@"osstatus"] ? _snapshot.report[@"osstatus"] : @"见逐项诊断",
                            _snapshot.report[@"descriptor_type"] ? _snapshot.report[@"descriptor_type"]
                                                                 : @"见逐项诊断",
                            _snapshot.report[@"query_succeeded"], _snapshot.report[@"query_failed"]];
    }
    [value appendString:
               @"读取仅限你选择的 2–3 个 Desktop 直接子项；文件选择器不等于已获得整个 Desktop "
               @"访问权。\n授权是 macOS Finder "
               @"自动化权限，本阶段仅执行只读操作。\n若不允许发送，请人工查看：系统设置 → 隐私与安全性 → "
               @"自动化 → Desktop Physics Sandbox → Finder，再点击检查。-1743 本身不能证明用户拒绝。\n\n"];
    NSArray* rows = _snapshot.report[@"items"];
    for (NSDictionary* row in rows) {
        NSUInteger index = [row[@"id"] unsignedIntegerValue] - 1;
        NSString* name = index < _selected.count ? _selected[index].lastPathComponent : @"?";
        [value appendFormat:@"#%@ %@：dpos=%@，pbnd=%@\n", row[@"id"], name, row[@"position_raw"],
                            row[@"item_bounds_raw_ltrb"]];
    }
    [value appendString:
               @"\n采样方法：点击采样后，在 5 "
               @"秒内将鼠标指向提示项目的图像中心并保持，不要点击或移动文件。至少两个相距 80pt "
               @"的项目须符合“主屏左上角、points、Y 向下”的规则，才允许十字预览。失败就停在坐标未验证；不乘 "
               @"Retina 倍率、不推测矩形。\n预览仅单屏、仅当前快照，60 秒自动关闭；切换 "
               @"Space、屏幕变化或关闭面板立即清理。快照 5 分钟失效。Spaces、全屏、Dock "
               @"与实际鼠标穿透仍待人工验收。\n诊断不含本页文件名称或路径；不会导出截图。"];
    NSDictionary* mirrorReport = [_mirror diagnostics];
    [value
        insertString:
            [NSString stringWithFormat:
                          @"普通桌面镜像预览：%@。开发预览，原图标保留，可能出现重复显示。\n仅 "
                          @"2–3 项；橙色轮廓为副本；绘制框 %.0fpt（%@），DPP 碰撞 %.1fpt，标签不绘制。\n静态/"
                          @"播放鼠标穿透；进入交互后可抓取抛掷；点击面板退出交互，切换"
                          @"其他应用关闭预览。\n关闭按钮和 DPP 菜单始终可用，120 "
                          @"秒自动关闭；每 2 秒复查所选位置，超时停止。\n状态：%@\n\n",
                          mirrorReport[@"phase"], [self mirrorDrawSize],
                          [_snapshot.report[@"desktop_icon_size"][@"usable"] boolValue]
                              ? @"Finder 当前尺寸配置；非图像边界测量"
                              : @"尺寸未取得，DPP 默认值",
                          [self mirrorDrawSize] * .90,
                          mirrorReport[@"reason"] ? mirrorReport[@"reason"] : @"运行中"]
             atIndex:0];
    [value
        insertString:[NSString
                         stringWithFormat:@"用户辅助隐藏实验：%@\n正式真实桌面模式：未完成，不可启动。\n\n",
                                          [NSString stringWithUTF8String:assistedUnavailableReason]]
             atIndex:0];
    _text.string = value;
}
- (void)clearLayout {
    [self closeMirror];
    _cachedImages = nil;
    _resourceIDs = nil;
    _volumeIDs = nil;
    _mirrorSnapshot = {};
    [self stopPreview];
    [_sampleTimer invalidate];
    _sampleTimer = nil;
    _calibrating = NO;
    [_expiryTimer invalidate];
    _expiryTimer = nil;
    _state.invalidateLayout();
    _snapshot = FinderSnapshot{};
    _snapshotTime = 0;
    [_calibration removeAllObjects];
    _calibrationIndex = 0;
    _imageState = @"not_requested";
    _imageResults = @[];
}
- (void)runOperation:(AccessOperation)operation consent:(BOOL)consent {
    const auto token = _state.begin(operation, consent);
    if (!token) return;
    [self clearLayout];
    _notice = operation == AccessOperation::Request ? @"等待系统授权结果；可关闭面板，无法取消系统授权操作。"
                                                    : @"正在后台检查/读取；界面可继续使用。";
    [self refresh];
    NSArray<NSURL*>* selected = [_selected copy];
    const CGFloat snapshotScale = NSScreen.screens.firstObject.backingScaleFactor;
    __weak DPPDesktopAccess* weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      @autoreleasepool {
          OSStatus status = errAEEventFailed;
          FinderSnapshot snapshot;
          NSMutableArray* thumbnails = [NSMutableArray array];
          @try {
              try {
                  status = finderPermission(operation == AccessOperation::Request && consent);
                  if (status == noErr) {
                      auto target =
                          [NSAppleEventDescriptor descriptorWithBundleIdentifier:@"com.apple.finder"];
                      if (operation == AccessOperation::Read)
                          snapshot = selectedSnapshot(selected, [target](NSAppleEventDescriptor* spec) {
                              return readFinder(target, spec);
                          });
                      NSDictionary* iconSize = desktopIconSize(
                          [target](NSAppleEventDescriptor* spec) { return readFinder(target, spec); });
                      NSMutableDictionary* report = [snapshot.report mutableCopy];
                      report[@"desktop_icon_size"] = iconSize;
                      snapshot.report = report;
                      const double drawSize =
                          [iconSize[@"usable"] boolValue] ? [iconSize[@"raw"] doubleValue] : 48;
                      const NSUInteger thumbnailPixels = static_cast<NSUInteger>(
                          std::clamp(std::ceil(drawSize * snapshotScale), 64.0, 256.0));
                      for (NSUInteger i = 0; i < snapshot.urls.count; ++i) {
                          const BOOL stable = [snapshot.report[@"items"][i][@"identity_stable"] boolValue];
                          [thumbnails
                              addObject:stable ? previewThumbnail(snapshot.urls[i], thumbnailPixels)
                                               : @{@"image" : NSNull.null, @"status" : @"unstable_identity"}];
                      }
                  }
              } catch (...) {
                  status = errAEEventFailed;
              }
          } @catch (NSException*) {
              status = errAEEventFailed;
          }
          dispatch_async(dispatch_get_main_queue(), ^{
            nativeGuard([&] {
                DPPDesktopAccess* owner = weakSelf;
                if (!owner) return;
                const bool accepted = owner->_state.finish(token, status);
                [owner.history addObject:@{
                    @"stage" : operation == AccessOperation::Request ? @"permission_request"
                                                                     : @"permission_preflight",
                    @"osstatus" : @(status),
                    @"ask_user_if_needed" : @(operation == AccessOperation::Request && consent),
                    @"delivery" : accepted ? @"accepted" : @"discarded_after_close_or_exit",
                    @"system_prompt_visible" : @"not_observable_by_API",
                    @"user_choice" : @"not_observable_by_API"
                }];
                if (!accepted) {
                    if (owner->_state.open) [owner refresh];
                    return;
                }
                owner->_snapshot = snapshot;
                owner->_state.parsed = [snapshot.report[@"position_success"] unsignedIntegerValue];
                owner->_state.failed =
                    [snapshot.report[@"position_failed_or_unmatched"] unsignedIntegerValue];
                if (operation == AccessOperation::Read && status == noErr) {
                    [owner decodeSelectedImages:thumbnails];
                    owner->_snapshotTime = NSDate.timeIntervalSinceReferenceDate;
                    owner->_snapshotFrame = NSScreen.screens.firstObject.frame;
                    owner->_snapshotWorkArea = NSScreen.screens.firstObject.visibleFrame;
                    owner->_snapshotUptime = NSProcessInfo.processInfo.systemUptime;
                    owner->_snapshotScale = NSScreen.screens.firstObject.backingScaleFactor;
                    owner->_snapshotID = NSUUID.UUID.UUIDString;
                    owner.notice = owner->_state.parsed
                                       ? ([owner candidateCoordinatesUsable]
                                              ? @"已读取原始布局字段，坐标契约尚未验证；可开始人工采样。"
                                              : @"读取成功，坐标契约未验证。原始点不满足单屏候选范围/"
                                                @"样本距离；采样与预览禁用，不以 pbnd 替代。")
                                       : @"权限检查通过，但布局读取/解析未成功；请导出诊断。";
                    owner.expiryTimer =
                        [NSTimer scheduledTimerWithTimeInterval:300
                                                        repeats:NO
                                                          block:^(NSTimer*) {
                                                            nativeGuard([&] { [weakSelf expire]; });
                                                          }];
                } else {
                    owner.notice = status == noErr ? @"对应权限检查通过，已尝试只读获取图标尺寸；尚未读取项目"
                                                     @"布局。点击选择项目并读取布局。"
                                                   : @"权限检查/申请未通过。保留原始状态；不会自动再次申请。";
                }
                [owner refresh];
            });
          });
      }
    });
}
- (CGFloat)mirrorDrawSize {
    NSDictionary* size = _snapshot.report[@"desktop_icon_size"];
    return [size[@"usable"] boolValue] ? [size[@"raw"] doubleValue] : 48;
}
- (void)decodeSelectedImages:(NSArray*)thumbnails {
    NSMutableArray* results = [NSMutableArray array];
    NSUInteger decoded = 0, thumbnailCount = 0;
    _cachedImages = [NSMutableArray array];
    _resourceIDs = [NSMutableArray array];
    _volumeIDs = [NSMutableArray array];
    for (NSUInteger i = 0; i < _snapshot.urls.count; ++i) {
        const bool stable = [_snapshot.report[@"items"][i][@"identity_stable"] boolValue];
        NSDictionary* thumbnail = i < thumbnails.count ? thumbnails[i] : nil;
        id thumbnailImage = stable ? thumbnail[@"image"] : nil;
        const BOOL hasThumbnail = thumbnailImage && thumbnailImage != NSNull.null;
        NSImage* icon =
            stable && !hasThumbnail ? [NSWorkspace.sharedWorkspace iconForFile:_snapshot.urls[i].path] : nil;
        const CGFloat pixels =
            std::clamp([self mirrorDrawSize] * NSScreen.screens.firstObject.backingScaleFactor, 64.0, 256.0);
        NSRect rect = NSMakeRect(0, 0, pixels, pixels);
        CGImageRef cg = hasThumbnail ? (__bridge CGImageRef)thumbnailImage
                                     : [icon CGImageForProposedRect:&rect context:nil hints:nil];
        if (hasThumbnail) ++thumbnailCount;
        const bool ok = cg && CGImageGetWidth(cg) > 0 && CGImageGetHeight(cg) > 0;
        if (ok) {
            ++decoded;
            [_cachedImages addObject:(__bridge id)cg];
        }
        id resource = nil, volume = nil;
        [_snapshot.urls[i] getResourceValue:&resource forKey:NSURLFileResourceIdentifierKey error:nil];
        [_snapshot.urls[i] getResourceValue:&volume forKey:NSURLVolumeIdentifierKey error:nil];
        [_resourceIDs addObject:resource ? resource : NSNull.null];
        [_volumeIDs addObject:volume ? volume : NSNull.null];
        [results addObject:@{
            @"id" : _snapshot.report[@"items"][i][@"id"],
            @"decoded" : @(ok),
            @"source" : hasThumbnail ? @"ImageIO_selected_image_thumbnail"
                                     : (ok ? @"NSWorkspace_system_icon" : @"unavailable"),
            @"thumbnail_status" : thumbnail[@"status"] ? thumbnail[@"status"] : @"not_available",
            @"pixel_width" : ok ? @(CGImageGetWidth(cg)) : NSNull.null,
            @"pixel_height" : ok ? @(CGImageGetHeight(cg)) : NSNull.null
        }];
    }
    _imageResults = results;
    _imageState = [NSString stringWithFormat:@"%lu/%lu decoded; %lu 图片缩略图，%lu 系统图标", decoded,
                                             _snapshot.urls.count, thumbnailCount, decoded - thumbnailCount];
}
- (void)requestPermission {
    if (_state.busy || _confirming || _choosing) return;
    _confirming = YES;
    [self refresh];
    const auto generation = _state.generation;
    NSAlert* alert = [NSAlert new];
    alert.messageText = @"申请 Finder 自动化权限";
    alert.informativeText =
        @"用于读取 Finder 中桌面项目的位置及必要元数据。当前功能不会移动、删除文件或改变桌面布局。\n\nmacOS "
        @"授予的是 Finder "
        @"自动化权限；本应用在本阶段只执行只读操作。这不是操作系统提供的“只读权限”。系统操作可能等待用户交互"
        @"，关闭本面板不会取消它。";
    [alert addButtonWithTitle:@"继续申请"];
    [alert addButtonWithTitle:@"取消"];
    [alert beginSheetModalForWindow:_panel
                  completionHandler:^(NSModalResponse response) {
                    nativeGuard([&] {
                        self->_confirming = NO;
                        if (response == NSAlertFirstButtonReturn && self->_state.open &&
                            self->_state.generation == generation)
                            [self runOperation:AccessOperation::Request consent:YES];
                        [self refresh];
                    });
                  }];
}
- (void)selectAndRead {
    if (_state.busy || _choosing || _confirming || _state.permission != Permission::Granted) return;
    [self clearLayout];
    _choosing = YES;
    [self refresh];
    const auto generation = _state.generation;
    NSOpenPanel* chooser = [NSOpenPanel openPanel];
    NSURL* desktop = [NSFileManager.defaultManager URLsForDirectory:NSDesktopDirectory
                                                          inDomains:NSUserDomainMask]
                         .firstObject;
    chooser.directoryURL = desktop;
    chooser.canChooseDirectories = YES;
    chooser.canChooseFiles = YES;
    chooser.allowsMultipleSelection = YES;
    chooser.resolvesAliases = NO;
    chooser.message =
        @"请选择 2–3 个相距较远的 Desktop 直接子项。图片会读取内容生成本机缩略图；其他项目读取系统图标。";
    [chooser beginSheetModalForWindow:_panel
                    completionHandler:^(NSModalResponse response) {
                      nativeGuard([&] {
                          self->_choosing = NO;
                          if (!self->_state.open || self->_state.generation != generation) return;
                          if (response == NSModalResponseOK) {
                              BOOL valid = chooser.URLs.count >= 2 && chooser.URLs.count <= 3;
                              for (NSURL* url in chooser.URLs)
                                  valid = valid && [urlIdentity(url.URLByDeletingLastPathComponent)
                                                       isEqual:urlIdentity(desktop)];
                              if (valid) {
                                  self.selected = chooser.URLs;
                                  self.directoryState =
                                      @"user_selected_direct_children; full_directory_access_not_tested";
                                  [self runOperation:AccessOperation::Read consent:NO];
                              } else
                                  self.notice = @"请选择 2–3 个 Desktop 直接子项；不递归、不按名称猜测对应。";
                          }
                          [self refresh];
                      });
                    }];
}
- (void)calibrate {
    NSArray* rows = _snapshot.report[@"items"];
    if (![self snapshotFresh] || ![self candidateCoordinatesUsable] || _state.parsed < 2 || _state.failed ||
        _calibrating || _state.busy)
        return;
    if (_calibrationIndex >= rows.count) {
        _calibrationIndex = 0;
        [_calibration removeAllObjects];
    }
    _state.coordinateValidated = _state.alignmentAccepted = false;
    NSDictionary* row = rows[_calibrationIndex];
    NSUInteger selectedIndex = [row[@"id"] unsignedIntegerValue] - 1;
    _notice = [NSString stringWithFormat:@"5 秒内将鼠标指向 #%@ %@ 的图像中心，保持不动；不要点击。",
                                         row[@"id"], _selected[selectedIndex].lastPathComponent];
    _calibrating = YES;
    [self refresh];
    __weak DPPDesktopAccess* weakSelf = self;
    _sampleTimer = [NSTimer scheduledTimerWithTimeInterval:5
                                                   repeats:NO
                                                     block:^(NSTimer*) {
                                                       nativeGuard([&] { [weakSelf finishCalibration]; });
                                                     }];
}
- (void)finishCalibration {
    _calibrating = NO;
    _sampleTimer = nil;
    if (!_state.open || ![self snapshotFresh]) {
        [self expire];
        return;
    }
    NSArray* rows = _snapshot.report[@"items"];
    NSDictionary* row = rows[_calibrationIndex];
    NSPoint mouse = NSEvent.mouseLocation;
    const bool match = dpp::macPointerMatchesTopLeft(
        point(row[@"position_raw"]), {static_cast<float>(mouse.x), static_cast<float>(mouse.y)},
        frameRect(_snapshotFrame));
    [_calibration addObject:@{
        @"id" : row[@"id"],
        @"pointer_global_points" : @[ @(mouse.x), @(mouse.y) ],
        @"matched_within_12pt" : @(match)
    }];
    ++_calibrationIndex;
    bool all = _calibration.count == rows.count;
    for (NSDictionary* sample in _calibration) all = all && [sample[@"matched_within_12pt"] boolValue];
    bool separated = false;
    for (NSDictionary* a in rows)
        for (NSDictionary* b in rows) {
            auto x = point(a[@"position_raw"]), y = point(b[@"position_raw"]);
            if (std::hypot(x.x - y.x, x.y - y.y) >= 80) separated = true;
        }
    _state.coordinateValidated = all && separated;
    _notice = _state.coordinateValidated
                  ? @"本快照多点采样符合候选坐标规则，可开启十字预览并人工核对。矩形仍未知。"
                  : (_calibrationIndex < rows.count
                         ? @"已采样；继续采样下一项目。"
                         : @"坐标验证未通过或样本距离不足。请重新读取/采样，不开启预览。");
    [self refresh];
}
- (void)startPreview {
    if (![self snapshotFresh] || !_state.startPreview()) return;
    _overlay = [[DPPAlignmentWindow alloc] initWithContentRect:_snapshotFrame
                                                     styleMask:NSWindowStyleMaskBorderless
                                                       backing:NSBackingStoreBuffered
                                                         defer:NO];
    _overlay.releasedWhenClosed = NO;
    _overlay.opaque = NO;
    _overlay.backgroundColor = NSColor.clearColor;
    _overlay.hasShadow = NO;
    _overlay.ignoresMouseEvents = YES;
    _overlay.level = CGWindowLevelForKey(kCGDesktopIconWindowLevelKey) + 1;
    _overlay.collectionBehavior =
        NSWindowCollectionBehaviorStationary | NSWindowCollectionBehaviorFullScreenNone;
    DPPAlignmentView* view = [[DPPAlignmentView alloc]
        initWithFrame:NSMakeRect(0, 0, _snapshotFrame.size.width, _snapshotFrame.size.height)];
    NSMutableArray* marks = [NSMutableArray array];
    for (NSDictionary* row in _snapshot.report[@"items"]) {
        auto global = dpp::macTopLeftPointsToGlobal(point(row[@"position_raw"]), frameRect(_snapshotFrame));
        auto scene = dpp::macGlobalPointsToScene(global, frameRect(_snapshotFrame));
        [marks addObject:@{@"id" : row[@"id"], @"scene" : @[ @(scene.x), @(scene.y) ]}];
    }
    view.marks = marks;
    _overlay.contentView = view;
    [_overlay orderFront:nil];
    __weak DPPDesktopAccess* weakSelf = self;
    _closeTimer = [NSTimer scheduledTimerWithTimeInterval:60
                                                  repeats:NO
                                                    block:^(NSTimer*) {
                                                      nativeGuard([&] {
                                                          [weakSelf stopPreview];
                                                          [weakSelf refresh];
                                                      });
                                                    }];
    _notice = @"预览仅绘制十字和编号。请人工核对多个项目，然后报告一致或不一致；60 秒自动关闭。";
    [self refresh];
}
- (void)stopPreview {
    [_closeTimer invalidate];
    _closeTimer = nil;
    [_overlay orderOut:nil];
    [_overlay close];
    _overlay = nil;
    _state.stopPreview();
}
- (void)expire {
    [self stopPreview];
    [self closeMirror];
    [_sampleTimer invalidate];
    _sampleTimer = nil;
    _calibrating = NO;
    _state.coordinateValidated = _state.alignmentAccepted = false;
    _snapshotTime = 0;
    _notice = @"快照已过期或屏幕/Space 已改变，请重新读取并验证坐标。";
    if (_state.open) [self refresh];
}
- (void)screenChanged:(NSNotification*)note {
    (void)note;
    nativeGuard([&] {
        if (_snapshotTime > 0 || _state.previewActive || _calibrating) [self expire];
    });
}
- (void)closeMirror {
    if (_experiment.preparedImages) {
        _experiment.finish();
        _cachedImages = nil;
        _resourceIDs = nil;
        _volumeIDs = nil;
        _snapshot = FinderSnapshot{};
        _selected = @[];
        _mirrorSnapshot = {};
        _snapshotTime = 0;
        _notice = @"实验取消/停止，请检查本次实际改动的设置。应用不自动恢复。";
    }
    ++_mirrorEpoch;
    _verificationBusy = NO;
    [_verificationTimer invalidate];
    _verificationTimer = nil;
    [_verificationDeadline invalidate];
    _verificationDeadline = nil;
    [_mirror close:@"explicit_close"];
}
- (BOOL)mirrorControlValid {
    if (![self snapshotFresh] || NSProcessInfo.processInfo.systemUptime - _lastVerified > 6) {
        [_mirror close:@"layout_verification_expired_reacquire_snapshot"];
        return NO;
    }
    return _mirror.active;
}
- (void)beginMirror {
    if (!_state.canPreview() || !_state.alignmentAccepted || ![self snapshotFresh] || _verificationBusy ||
        _verificationInFlight)
        return;
    [self stopPreview];
    [self closeMirror];
    _notice = @"正在复查所选项目身份及原始位置；不会更改桌面。";
    [self verifyMirror:YES];
    [self refresh];
}
- (BOOL)prepareMirror {
    if (_experiment.phase != AssistedExperiment::Phase::Idle &&
        _experiment.phase != AssistedExperiment::Phase::Finished && !assistedCandidateAvailable()) {
        _notice = [NSString stringWithUTF8String:assistedUnavailableReason];
        return NO;
    }
    NSArray* rows = _snapshot.report[@"items"];
    if (_cachedImages.count != rows.count || _resourceIDs.count != rows.count || ![self snapshotFresh])
        return NO;
    _mirrorSnapshot = {};
    _mirrorSnapshot.screen = frameRect(_snapshotFrame);
    _mirrorSnapshot.workArea = frameRect(_snapshotWorkArea);
    _mirrorSnapshot.singleScreen = NSScreen.screens.count == 1;
    _mirrorSnapshot.capturedAt = _snapshotUptime;
    for (NSUInteger i = 0; i < rows.count; ++i) {
        if (_resourceIDs[i] == NSNull.null || _volumeIDs[i] == NSNull.null ||
            ![rows[i][@"identity_stable"] boolValue])
            return NO;
        NSNumber* stableID = nil;
        for (NSDictionary* entry in _identityRegistry)
            if ([entry[@"resource"] isEqual:_resourceIDs[i]] && [entry[@"volume"] isEqual:_volumeIDs[i]]) {
                stableID = entry[@"id"];
                break;
            }
        if (!stableID) {
            stableID = @(_nextStableID++);
            [_identityRegistry
                addObject:@{@"resource" : _resourceIDs[i], @"volume" : _volumeIDs[i], @"id" : stableID}];
        }
        dpp::mac::MirrorItem item;
        item.id = dpp::IconId{stableID.unsignedIntValue};
        item.texture = dpp::TextureId{static_cast<uint32_t>(i + 1)};
        item.rawDpos = point(rows[i][@"position_raw"]);
        item.sceneAnchor = dpp::macGlobalPointsToScene(
            dpp::macTopLeftPointsToGlobal(item.rawDpos, _mirrorSnapshot.screen), _mirrorSnapshot.workArea);
        const float drawSize = static_cast<float>([self mirrorDrawSize]);
        item.finderIconSize = [_snapshot.report[@"desktop_icon_size"][@"usable"] boolValue] ? drawSize : 0;
        item.dppDrawSize = {drawSize, drawSize};
        item.dppCollisionBox = {drawSize * .90f, drawSize * .90f};
        item.coordinateValidated = _state.coordinateValidated && _state.alignmentAccepted;
        _mirrorSnapshot.items.push_back(item);
    }
    _mirror.snapshotID = _snapshotID;
    _mirror.experimentID = _experiment.phase == AssistedExperiment::Phase::Verified ? _experimentID : nil;
    const BOOL assisted = _experiment.phase == AssistedExperiment::Phase::Verified;
    if (assisted && !_experiment.start()) return NO;
    BOOL started = [_mirror start:_mirrorSnapshot images:_cachedImages];
    if (assisted && !started) {
        _experiment.finish();
        _cachedImages = nil;
        _mirrorSnapshot = {};
    }
    return started;
}
- (void)verifyMirror:(BOOL)starting {
    if (_experiment.phase != AssistedExperiment::Phase::Idle &&
        _experiment.phase != AssistedExperiment::Phase::Finished && !assistedCandidateAvailable()) {
        _notice = [NSString stringWithUTF8String:assistedUnavailableReason];
        return;
    }
    if (_verificationBusy || _verificationInFlight || (!starting && !_mirror.active)) return;
    if (![self snapshotFresh]) {
        [self expire];
        return;
    }
    _verificationBusy = _verificationInFlight = YES;
    const uint64_t epoch = _mirrorEpoch;
    FinderSnapshot snapshot = _snapshot;
    NSArray* resources = [_resourceIDs copy];
    NSArray* volumes = [_volumeIDs copy];
    __weak DPPDesktopAccess* weakSelf = self;
    // Bounded selected-property checks, never per-frame enumeration. Late replies discarded.
    _verificationDeadline =
        [NSTimer scheduledTimerWithTimeInterval:6
                                        repeats:NO
                                          block:^(NSTimer*) {
                                            DPPDesktopAccess* owner = weakSelf;
                                            if (!owner || owner->_mirrorEpoch != epoch) return;
                                            [owner closeMirror];
                                            owner->_state.coordinateValidated =
                                                owner->_state.alignmentAccepted = false;
                                            owner.notice = @"位置复查超时，已关闭预览。请重新读取快照。";
                                            [owner refresh];
                                          }];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      @autoreleasepool {
          __block BOOL valid = snapshot.references.count == snapshot.urls.count && snapshot.urls.count >= 2;
          @try {
              try {
                  auto target = [NSAppleEventDescriptor descriptorWithBundleIdentifier:@"com.apple.finder"];
                  valid = selectedPositionsMatch(
                      snapshot, [target](NSAppleEventDescriptor* spec) { return readFinder(target, spec); },
                      [&](NSUInteger i, NSURL* current) {
                          id resource = nil, volume = nil;
                          [current removeAllCachedResourceValues];
                          [current getResourceValue:&resource
                                             forKey:NSURLFileResourceIdentifierKey
                                              error:nil];
                          [current getResourceValue:&volume forKey:NSURLVolumeIdentifierKey error:nil];
                          return [resource isEqual:resources[i]] && [volume isEqual:volumes[i]];
                      });
              } catch (...) {
                  valid = NO;
              }
          } @catch (NSException*) {
              valid = NO;
          }
          dispatch_async(dispatch_get_main_queue(), ^{
            DPPDesktopAccess* owner = weakSelf;
            if (!owner) return;
            owner->_verificationInFlight = NO;
            if (owner->_mirrorEpoch != epoch) {
                if (owner->_state.open) [owner refresh];
                return;
            }
            [owner.verificationDeadline invalidate];
            owner.verificationDeadline = nil;
            owner->_verificationBusy = NO;
            if (!valid || ![owner snapshotFresh]) {
                [owner closeMirror];
                owner->_state.coordinateValidated = owner->_state.alignmentAccepted = false;
                owner.notice = @"项目身份、位置或显示配置已变化／读取失败。预览已关闭，请重新读取快照。";
            } else {
                owner->_lastVerified = NSProcessInfo.processInfo.systemUptime;
                if (starting) {
                    BOOL started = NO;
                    @try {
                        try {
                            started = [owner prepareMirror];
                        } catch (...) {
                        }
                    } @catch (NSException*) {
                    }
                    if (!started) {
                        [owner closeMirror];
                        owner.notice =
                            @"启动失败，资源已清理。需完整图像、稳定身份及工作区内留出半个绘制框的有效锚点。";
                    } else {
                        owner.notice =
                            owner->_experiment.phase == AssistedExperiment::Phase::Running
                                ? @"实验静态副本已显示：隐藏是用户报告，程序未验证无双影。先检查静态，再开始"
                                  @"重力；异常立即关闭。"
                                : @"静态副本已显示。原图标保留，可能出现重复图标。可以开始重力或进入交互。";
                        owner.verificationTimer = [NSTimer
                            scheduledTimerWithTimeInterval:2
                                                   repeats:YES
                                                     block:^(NSTimer*) {
                                                       nativeGuard([&] { [weakSelf verifyMirror:NO]; });
                                                     }];
                    }
                }
            }
            [owner refresh];
          });
      }
    });
}
- (NSDictionary*)report {
    return @{
        @"schema_version" : @5,
        @"assisted_candidate_availability" : @{
            @"available" : @(assistedCandidateAvailable()),
            @"reason" : [NSString stringWithUTF8String:assistedUnavailableReason],
            @"source" : @"human_observation_not_machine_detection",
            @"session_id" : @"A2791BEC-3FEF-4874-A425-134FA4FF619A",
            @"evidence" : @"assisted-real-observation-1-acceptance.json"
        },
        @"mirror_sessions" : [_mirror sessionSummaries],
        @"prepared_images" : @(_experiment.preparedImages),
        @"cached_snapshot_images" : @(_cachedImages.count),
        @"snapshot_references" : @(_snapshot.references.count),
        @"prepared_input_capture" : @NO,
        @"assisted_experiment" : _experimentReport ? _experimentReport : @{},
        @"pending_manual_settings_reminder" :
                [NSUserDefaults.standardUserDefaults objectForKey:assistedPendingKey()]
            ? [NSUserDefaults.standardUserDefaults objectForKey:assistedPendingKey()]
            : @{},
        @"mirror_development_preview" : [_mirror diagnostics],
        @"environment" : environment(),
        @"identity" : appIdentity(),
        @"screens" : screens(),
        @"target" : @"com.apple.finder",
        @"event_class" : @"core",
        @"event_id" : @"getd",
        @"desktop_directory_access" : _directoryState,
        @"system_icon_read" : _imageState,
        @"images" : _imageResults,
        @"permission" : permissionName(_state.permission),
        @"permission_osstatus" : _state.hasStatus ? @(_state.rawStatus) : static_cast<id>(NSNull.null),
        @"operations" : _history,
        @"finder" : _snapshot.report,
        @"coordinate_samples" : _calibration,
        @"coordinate_candidate_usable" : @([self candidateCoordinatesUsable]),
        @"coordinate_validation" : _state.coordinateValidated
            ? @"manual_pointer_samples_single_screen_within_12pt"
            : @"unverified",
        @"coordinate_source" : @"dpos; candidate primary-top-left logical points; no Retina scaling",
        @"image_rectangle" : NSNull.null,
        @"label_rectangle" : NSNull.null,
        @"preview_active" : @(_state.previewActive),
        @"preview_opened" : @(_state.previewEverOpened),
        @"preview_acceptance" : _state.alignmentAccepted ? @"user_reported_aligned"
                                                         : @"not_accepted_or_not_tested",
        @"no_double_image" : @"blocked",
        @"real_desktop_mode" : @"disabled",
        @"spaces_fullscreen_dock_input_passthrough" : @"unverified",
        @"system_prompt_and_choice" : @"requires_human_record; API_does_not_report_prompt_visibility",
        @"restart_authorization_persistence" : @"requires_manual_recheck",
        @"private_names_or_paths" : @NO
    };
}
- (void)exportReport {
    NSSavePanel* save = [NSSavePanel savePanel];
    save.directoryURL = [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory
                                                             inDomains:NSUserDomainMask]
                            .firstObject;
    save.nameFieldStringValue = [NSString stringWithFormat:@"dpp-access-%@.json", NSUUID.UUID.UUIDString];
    [save beginSheetModalForWindow:_panel
                 completionHandler:^(NSModalResponse response) {
                   nativeGuard([&] {
                       if (response == NSModalResponseOK) {
                           self.notice = writeJSON([self report], save.URL.path)
                                             ? @"脱敏诊断已新建。"
                                             : @"导出失败；已有文件不会覆盖。";
                           [self refresh];
                       }
                   });
                 }];
}
// Only explicit panel actions enter this flow. No system settings API is used.
- (void)prepareAssisted {
    if (!assistedCandidateAvailable()) {
        _notice = [NSString stringWithUTF8String:assistedUnavailableReason];
        if (_panel) [self refresh];
        return;
    }
    if ([NSUserDefaults.standardUserDefaults objectForKey:assistedPendingKey()] || _mirror.active ||
        _verificationBusy || _verificationInFlight || _cachedImages.count != 3 || !_state.alignmentAccepted ||
        ![self snapshotFresh])
        return;
    NSAlert* alert = [NSAlert new];
    alert.messageText = @"用户辅助无双影实验（尚待验证）";
    alert.informativeText = @"系统设置可能影响全部原图标，本次只显示选定三个副本。程序不会修改或恢复设置；关"
                            @"闭应用不等于设置已恢复。可取消并使用普通镜像预览。请先自行查看“桌面与程序坞 → "
                            @"显示项目 → 在桌面上”，报告原状态。只改变这一项，不改变其他设置或布局。";
    [alert addButtonWithTitle:@"取消"];
    [alert addButtonWithTitle:@"原来显示，准备后手动隐藏"];
    [alert addButtonWithTitle:@"原来已隐藏，不改变设置"];
    NSInteger response = [alert runModal];
    if (response == NSAlertFirstButtonReturn) return;
    auto original = response == NSAlertSecondButtonReturn ? AssistedExperiment::Original::Visible
                                                          : AssistedExperiment::Original::Hidden;
    if (!_experiment.prepare(true, [self snapshotFresh], original)) return;
    [self stopPreview];
    _experimentID = NSUUID.UUID.UUIDString;
    _experimentReport = @{
        @"experiment_id" : _experimentID,
        @"original_setting_source" : @"user_report",
        @"original_on_desktop" : @(original == AssistedExperiment::Original::Visible),
        @"setting" : @"Desktop & Dock / Show Items / On Desktop",
        @"machine_verified_visibility" : @NO,
        @"candidate_result" : @"pending_manual_validation"
    };
    // Written before leaving: a possible-change reminder, never a backup or recovery promise.
    if (original == AssistedExperiment::Original::Visible)
        [NSUserDefaults.standardUserDefaults setObject:_experimentReport forKey:assistedPendingKey()];
    _notice = @"等待用户设置：没有活动预览或输入捕获。请自行调整后返回核验；五分钟快照期限仍有效。";
    [self refresh];
}
- (void)returnAssisted {
    if (!assistedCandidateAvailable()) {
        _notice = [NSString stringWithUTF8String:assistedUnavailableReason];
        if (_panel) [self refresh];
        return;
    }
    if (_experiment.phase != AssistedExperiment::Phase::Waiting) return;
    NSAlert* alert = [NSAlert new];
    alert.messageText = @"确认原图标当前不可见";
    alert.informativeText =
        @"确认只按准备时说明改变了“在桌面上”（原来隐藏则未改变），没有更改布局、对象、Space "
        @"或其他设置。随后程序将严格重新读取身份和位置；无法读取即停止，不复用旧坐标冒充成功。";
    [alert addButtonWithTitle:@"取消实验"];
    [alert addButtonWithTitle:@"原图标不可见且无其他改动"];
    BOOL confirmed = [alert runModal] == NSAlertSecondButtonReturn;
    BOOL changed = _experiment.original == AssistedExperiment::Original::Visible;
    if (!_experiment.verify([self snapshotFresh], confirmed, confirmed, changed)) {
        _cachedImages = nil;
        _resourceIDs = nil;
        _volumeIDs = nil;
        _snapshot = FinderSnapshot{};
        _selected = @[];
        _mirrorSnapshot = {};
        _snapshotTime = 0;
        _notice = @"未确认或快照失效，已取消。请检查并恢复本次实际改动。";
    } else {
        NSMutableDictionary* report = [_experimentReport mutableCopy];
        report[@"original_icons_hidden_user_report"] = @YES;
        report[@"changed_on_desktop_user_report"] = @(changed);
        _experimentReport = report;
        [self verifyMirror:YES];
    }
    [self refresh];
}
- (void)confirmManualRestore {
    NSDictionary* pending = [NSUserDefaults.standardUserDefaults dictionaryForKey:assistedPendingKey()];
    if (!pending || ![pending[@"original_on_desktop"] isKindOfClass:NSNumber.class]) {
        _notice = pending ? @"原始设置未知：仅检查本次实际改动，不能确认系统已恢复。提醒保留。"
                          : @"当前没有待确认恢复提醒。历史确认仍为用户确认，不是机器验证。仅恢复本次实际改动"
                            @"；原本隐藏且未改动时无需改成显示。";
        if (_panel) [self refresh];
        return;
    }
    NSAlert* alert = [NSAlert new];
    alert.messageText = @"检查本次手动设置";
    alert.informativeText =
        [pending[@"original_on_desktop"] boolValue]
            ? @"仅恢复本次实际改动：“显示项目 → "
              @"在桌面上”的原用户报告值为显示。若没有改变则无需调整。确认正常桌面操作。此确认不"
              @"是机器验证系统恢复成功。"
            : @"原用户报告值为隐藏。仅恢复本次实际改动；若未改变，无需改成显示。确认只作为用户报告，不是机器"
              @"验证。";
    [alert addButtonWithTitle:@"尚未完成"];
    [alert addButtonWithTitle:@"我已检查并恢复本次改动"];
    if ([alert runModal] == NSAlertSecondButtonReturn) {
        NSMutableDictionary* report = [pending mutableCopy];
        report[@"user_confirmed_restored"] = @YES;
        report[@"machine_verified_system_recovery"] = @NO;
        _experimentReport = report;
        // Match experiment, never attribute restoration to an unrelated later session.
        NSDictionary* last = _mirror.sessionSummaries.lastObject;
        if ([last[@"experiment_id"] isEqual:pending[@"experiment_id"]]) [_mirror recordHuman:report];
        [NSUserDefaults.standardUserDefaults removeObjectForKey:assistedPendingKey()];
        _notice = @"用户已确认检查/恢复本次改动；系统恢复未由程序验证。";
    }
    [self refresh];
}
- (void)showHistory {
    NSData* data = [NSJSONSerialization dataWithJSONObject:_mirror.sessionSummaries
                                                   options:NSJSONWritingPrettyPrinted
                                                     error:nil];
    _notice = [NSString stringWithFormat:@"历史会话（最多 32 段；人工报告与机器计数独立）：\n%@",
                                         [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding]];
    if (_panel) [self refresh];
}
- (void)recordObservation {
    NSAlert* alert = [NSAlert new];
    NSDictionary* last = _mirror.sessionSummaries.lastObject;
    alert.messageText = [NSString stringWithFormat:@"人工观察：会话 %@", last[@"session_id"]];
    alert.informativeText = @"填写步骤及结果：静态副本、重力离位、抓取抛掷、暂停重置、面板、空白点击、应用切"
                            @"换、返回、关闭、桌面操作。区分运行时与关闭后；若运行中出现原件，请写触发动作及"
                            @"失败。未操作请写待验证。不会转换为机器计数。";
    NSView* form = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 440, 100)];
    NSPopUpButton* step = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(0, 65, 440, 28) pullsDown:NO];
    [step addItemsWithTitles:@[
        @"静态副本位置与重复显示", @"重力离开原位后是否留下原件", @"抓取抛掷", @"暂停与重置", @"控制面板",
        @"游戏空白点击", @"切换普通应用停止及输入释放", @"返回后重新确认", @"关闭预览清理",
        @"恢复设置后桌面操作"
    ]];
    NSPopUpButton* result = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(0, 33, 440, 28) pullsDown:NO];
    [result
        addItemsWithTitles:@[ @"待验证", @"用户观察通过", @"失败：运行中原图标出现", @"失败：其他行为异常" ]];
    NSPopUpButton* scope = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(0, 0, 440, 28) pullsDown:NO];
    [scope addItemsWithTitles:@[ @"游戏运行期间", @"安全关闭或切换应用后" ]];
    [form addSubview:step];
    [form addSubview:result];
    [form addSubview:scope];
    alert.accessoryView = form;
    [alert addButtonWithTitle:@"取消"];
    [alert addButtonWithTitle:@"保存人工报告"];
    if ([alert runModal] == NSAlertSecondButtonReturn) {
        [_mirror recordHuman:@{
            @"step_or_trigger" : step.titleOfSelectedItem,
            @"result" : result.titleOfSelectedItem,
            @"scope" : scope.titleOfSelectedItem
        }];
    }
}
- (void)action:(NSButton*)button {
    nativeGuard([&] {
        switch (button.tag) {
            case 0:
                [self runOperation:AccessOperation::Check consent:NO];
                break;
            case 1:
                [self requestPermission];
                break;
            case 2:
                [self selectAndRead];
                break;
            case 3:
                [self calibrate];
                break;
            case 4:
                [self startPreview];
                break;
            case 5:
                [self stopPreview];
                [self refresh];
                break;
            case 6:
                _state.acceptAlignment();
                [self stopPreview];
                [self refresh];
                break;
            case 7:
                _state.coordinateValidated = _state.alignmentAccepted = false;
                [self stopPreview];
                [self refresh];
                break;
            case 8:
                [self exportReport];
                break;
            case 9:
                [self beginMirror];
                break;
            case 10:
                if ([self mirrorControlValid]) [_mirror playPause];
                [self refresh];
                break;
            case 11:
                if ([self mirrorControlValid]) [_mirror reset];
                [self refresh];
                break;
            case 12:
                if ([self mirrorControlValid]) [_mirror toggleInteraction];
                [self refresh];
                break;
            case 14:
                [self prepareAssisted];
                break;
            case 15:
                [self showHistory];
                break;
            case 16:
                [self confirmManualRestore];
                break;
            case 17:
                [self recordObservation];
                break;
            case 13:
                [self closeMirror];
                [self refresh];
                break;
        }
    });
}
- (void)windowWillClose:(NSNotification*)note {
    (void)note;
    nativeGuard([&] {
        if (_panel.attachedSheet) [_panel endSheet:_panel.attachedSheet returnCode:NSModalResponseCancel];
        _state.close();
        [self clearLayout];
        _selected = @[];
    });
}
- (void)shutdown {
    nativeGuard([&] {
        _state.shutdown();
        [self clearLayout];
        [_panel close];
    });
}
- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
    [NSWorkspace.sharedWorkspace.notificationCenter removeObserver:self];
}
@end
