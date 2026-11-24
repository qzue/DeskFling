#import <AppKit/AppKit.h>
#include <fcntl.h>
#include <libproc.h>
#include <unistd.h>
#include <map>
#include "core/scene/user_prefs.h"
#include "platforms/macos/billiards_lan.h"
#include "platforms/macos/desktop_layout_writeback.h"
#include "platforms/macos/display_guard.h"
#include "platforms/macos/document_thumbnail.h"
#include "platforms/macos/floating_hud_bar.h"
#include "platforms/macos/full_desktop_scope.h"
#include "platforms/macos/mirror_preview.h"
#include "platforms/macos/open_target.h"
#include "platforms/macos/protected_mirror_session.h"
#include "platforms/macos/session_trace.h"
#include "platforms/macos/watched_item.h"
using namespace dpp;
using namespace dpp::mac;
namespace {
void requireMirror(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
dpp::Rect mirrorRect(NSRect r) {
    return {static_cast<float>(r.origin.x), static_cast<float>(r.origin.y), static_cast<float>(r.size.width),
            static_cast<float>(r.size.height)};
}
NSDictionary* mirrorFinderIdentity() {
    auto apps = [NSRunningApplication runningApplicationsWithBundleIdentifier:@"com.apple.finder"];
    requireMirror(apps.count == 1, "Finder host unavailable");
    const pid_t pid = apps.firstObject.processIdentifier;
    proc_bsdinfo p{};
    requireMirror(proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &p, sizeof(p)) == sizeof(p) && p.pbi_uid == getuid(),
                  "Finder process identity unavailable");
    return @{
        @"pid" : @(pid),
        @"seconds" : @(p.pbi_start_tvsec),
        @"microseconds" : @(p.pbi_start_tvusec),
        @"uid" : @(p.pbi_uid)
    };
}
NSArray* fileIdentity(NSURL* url) {
    [url removeAllCachedResourceValues];
    id resource = nil, volume = nil;
    requireMirror([url getResourceValue:&resource forKey:NSURLFileResourceIdentifierKey error:nil] &&
                      resource && [url getResourceValue:&volume forKey:NSURLVolumeIdentifierKey error:nil] &&
                      volume,
                  "file identity unavailable; no partial desktop takeover");
    return @[ resource, volume ];
}
NSSet<NSString*>* mountedRootPaths() {
    NSSet* paths =
        desktopPathSet([NSFileManager.defaultManager mountedVolumeURLsIncludingResourceValuesForKeys:nil
                                                                                             options:0]);
    requireMirror(paths != nil, "mounted volume inventory unavailable");
    return paths;
}
bool isVolumeRoot(NSURL* url) {
    [url removeAllCachedResourceValues];
    NSNumber* value = nil;
    return [url getResourceValue:&value forKey:NSURLIsVolumeKey error:nil] && value.boolValue;
}

NSUInteger owningScreenIndexForGlobalPoint(NSPoint pt, NSArray<NSScreen*>* screens) {
    if (screens.count <= 1) return 0;
    for (NSUInteger i = 0; i < screens.count; ++i) {
        if (NSPointInRect(pt, screens[i].frame)) return i;
    }
    NSUInteger bestIdx = 0;
    double bestDist2 = 1e30;
    for (NSUInteger i = 0; i < screens.count; ++i) {
        const NSRect f = screens[i].frame;
        const double dx = pt.x < NSMinX(f) ? (NSMinX(f) - pt.x) : (pt.x > NSMaxX(f) ? (pt.x - NSMaxX(f)) : 0.0);
        const double dy = pt.y < NSMinY(f) ? (NSMinY(f) - pt.y) : (pt.y > NSMaxY(f) ? (pt.y - NSMaxY(f)) : 0.0);
        const double d2 = dx * dx + dy * dy;
        if (d2 < bestDist2) {
            bestDist2 = d2;
            bestIdx = i;
        }
    }
    return bestIdx;
}
}  // namespace

@interface DPPLauncherDragView : NSView
@end
@implementation DPPLauncherDragView
- (BOOL)mouseDownCanMoveWindow {
    return YES;
}
@end

@interface DPPLauncherPrimaryButton : NSButton {
    BOOL _hovered;
    NSTrackingArea* _trackingArea;
}
@end
@implementation DPPLauncherPrimaryButton
- (BOOL)isFlipped {
    return YES;
}
- (BOOL)mouseDownCanMoveWindow {
    return NO;
}
- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (_trackingArea) [self removeTrackingArea:_trackingArea];
    _trackingArea = [[NSTrackingArea alloc]
        initWithRect:self.bounds
             options:NSTrackingMouseEnteredAndExited | NSTrackingActiveAlways | NSTrackingInVisibleRect
               owner:self
            userInfo:nil];
    [self addTrackingArea:_trackingArea];
}
- (void)mouseEntered:(NSEvent*)event {
    (void)event;
    _hovered = YES;
    self.needsDisplay = YES;
}
- (void)mouseExited:(NSEvent*)event {
    (void)event;
    _hovered = NO;
    self.needsDisplay = YES;
}
- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    const NSRect b = NSInsetRect(self.bounds, 0.5, 0.5);
    NSBezierPath* path = [NSBezierPath bezierPathWithRoundedRect:b xRadius:12.0 yRadius:12.0];
    const BOOL down = self.cell.isHighlighted && self.enabled;
    NSColor* topCol = !self.enabled
                          ? [NSColor colorWithSRGBRed:0.12 green:0.42 blue:0.82 alpha:0.42]
                          : (down ? [NSColor colorWithSRGBRed:0.04 green:0.44 blue:0.90 alpha:1.0]
                                  : (_hovered ? [NSColor colorWithSRGBRed:0.16 green:0.60 blue:1.0 alpha:1.0]
                                              : [NSColor colorWithSRGBRed:0.06 green:0.53 blue:1.0 alpha:1.0]));
    NSColor* botCol = !self.enabled
                          ? [NSColor colorWithSRGBRed:0.08 green:0.32 blue:0.70 alpha:0.42]
                          : (down ? [NSColor colorWithSRGBRed:0.02 green:0.35 blue:0.80 alpha:1.0]
                                  : (_hovered ? [NSColor colorWithSRGBRed:0.05 green:0.46 blue:0.96 alpha:1.0]
                                              : [NSColor colorWithSRGBRed:0.02 green:0.40 blue:0.90 alpha:1.0]));
    NSGradient* grad = [[NSGradient alloc] initWithStartingColor:topCol endingColor:botCol];
    [grad drawInBezierPath:path angle:90.0];
    [[NSColor colorWithWhite:1.0 alpha:(self.enabled ? 0.28 : 0.10)] setStroke];
    path.lineWidth = 1.0;
    [path stroke];

    NSDictionary* attrs = @{
        NSFontAttributeName : [NSFont systemFontOfSize:15.5 weight:NSFontWeightSemibold],
        NSForegroundColorAttributeName : [NSColor colorWithWhite:1.0 alpha:(self.enabled ? 1.0 : 0.55)]
    };
    NSString* txt = self.title.length ? self.title : @"▶ 开始游玩";
    const NSSize ts = [txt sizeWithAttributes:attrs];
    [txt drawAtPoint:NSMakePoint(NSMidX(b) - ts.width * 0.5, NSMidY(b) - ts.height * 0.5 - 0.5)
      withAttributes:attrs];
}
@end

@interface DPPLauncherModePill : NSButton {
    BOOL _hovered;
    NSTrackingArea* _trackingArea;
}
@property BOOL pillSelected;
@end
@implementation DPPLauncherModePill
- (BOOL)isFlipped {
    return YES;
}
- (BOOL)mouseDownCanMoveWindow {
    return NO;
}
- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (_trackingArea) [self removeTrackingArea:_trackingArea];
    _trackingArea = [[NSTrackingArea alloc]
        initWithRect:self.bounds
             options:NSTrackingMouseEnteredAndExited | NSTrackingActiveAlways | NSTrackingInVisibleRect
               owner:self
            userInfo:nil];
    [self addTrackingArea:_trackingArea];
}
- (void)mouseEntered:(NSEvent*)event {
    (void)event;
    _hovered = YES;
    self.needsDisplay = YES;
}
- (void)mouseExited:(NSEvent*)event {
    (void)event;
    _hovered = NO;
    self.needsDisplay = YES;
}
- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    const NSRect b = NSInsetRect(self.bounds, 0.5, 0.5);
    NSBezierPath* path = [NSBezierPath bezierPathWithRoundedRect:b xRadius:9.5 yRadius:9.5];
    NSColor* fill = _pillSelected
                        ? [NSColor colorWithSRGBRed:0.05 green:0.52 blue:1.0 alpha:0.92]
                        : (_hovered ? [NSColor colorWithWhite:1.0 alpha:0.15]
                                    : [NSColor colorWithWhite:1.0 alpha:0.08]);
    [fill setFill];
    [path fill];
    NSColor* rim = _pillSelected ? [NSColor colorWithWhite:1.0 alpha:0.35]
                                 : [NSColor colorWithWhite:1.0 alpha:0.14];
    [rim setStroke];
    path.lineWidth = 1.0;
    [path stroke];

    NSDictionary* attrs = @{
        NSFontAttributeName : [NSFont systemFontOfSize:12.5
                                                weight:(_pillSelected ? NSFontWeightSemibold : NSFontWeightMedium)],
        NSForegroundColorAttributeName :
            (_pillSelected ? NSColor.whiteColor : [NSColor colorWithWhite:0.86 alpha:1.0])
    };
    NSString* txt = self.title.length ? self.title : @"";
    const NSSize ts = [txt sizeWithAttributes:attrs];
    [txt drawAtPoint:NSMakePoint(NSMidX(b) - ts.width * 0.5, NSMidY(b) - ts.height * 0.5)
      withAttributes:attrs];
}
@end

@interface DPPFlippedPassiveView : NSView
@end
@implementation DPPFlippedPassiveView
- (BOOL)isFlipped {
    return YES;
}
@end

@interface DPPProtectedMirrorPanel : NSObject <NSWindowDelegate, NSApplicationDelegate> {
    FinderSnapshot _snapshot;
    std::map<uint32_t, OpenTarget> _openTargets;
    bool _openConsumed;
    bool _cancelOpen;
    std::shared_ptr<std::atomic_bool> _thumbnailCancelled;
    MirrorSnapshot _scene;
    std::unique_ptr<DPPProtectedMirrorSession> _protectedSession;
    MirrorDisplay _displaySnapshot;
    std::vector<MirrorDisplay> _allDisplaysSnapshot;
    NSRect _screenFrame, _workArea, _hudWorkArea;
    CGFloat _scale;
    int _displayMode;  // 0..15 = single screen index, -1 = all-screens connected span
    std::vector<std::size_t> _activeOrigIndices;
    uint64_t _epoch;
    BOOL _loading, _transition, _cancelRequested, _activeSession;
    BOOL _savedGravityFilenames;
    BOOL _quickAutoStart;
    BOOL _suppressPrefSave;
    BOOL _systemSleeping;
    BOOL _screenLocked;
    BOOL _menuTrackingActive;
    BOOL _modalAlertActive;
    dpp::PlayMode _savedPlayMode;
    dpp::SlingshotProgress _slingshotProgress;
    std::vector<dpp::SlingshotCustomPiece> _slingshotCustomPieces;
    std::vector<std::size_t> _customIconIndices;
    std::uint32_t _randomSeed;
}
@property BOOL syntheticCheckMode;
@property(strong) id<NSObject> appNapActivity;
@property(strong) NSPopUpButton* displayScopePopup;
@property(strong) NSArray<DPPLauncherModePill*>* launcherModePills;
@property(copy) NSArray* activeImages;
@property(copy) NSArray<NSString*>* activeLabels;
@property(strong) NSPopUpButton* inGameDisplayPopup;
@property(strong) NSMutableArray<NSWindow*>* passiveScreenWindows;
@property(strong) NSMutableArray<CATextLayer*>* passiveLabelLayers;
@property(strong) NSButton* saveLayoutButton;
@property(strong) NSButton* undoLayoutButton;
@property(strong) NSButton* slingEditButton;
@property(strong) NSButton* lanScanButton;
@property(strong) NSPanel* panel;
@property(strong) DPPKeyablePanel* modeSettingsPanel;
@property(strong) NSVisualEffectView* modeSettingsGlass;
@property(strong) CAShapeLayer* modeSettingsRim;
@property(strong) NSTextField* modeSettingsTitle;
@property(strong) NSView* modeSettingsDivider;
@property(strong) NSButton* modeSettingsCloseBtn;
@property(strong) NSArray<NSView*>* modeSettingLabels;
@property(strong) NSStatusItem* statusItem;
@property(strong) DPPFloatingHudBar* hudBar;
@property(strong) NSButton* quickStartButton;
@property(strong) NSTextField* status;
@property(strong) NSButton* readButton;
@property(strong) NSButton* confirmed;
@property(strong) NSButton* filenameToggle;
@property(strong) NSPopUpButton* gravityPresetPopup;
@property(strong) NSPopUpButton* gravityDirectionPopup;
@property(strong) NSSlider* gravityAccelSlider;
@property(strong) NSTextField* gravityAccelValueLabel;
@property(strong) NSSlider* gravityRestitutionSlider;
@property(strong) NSTextField* gravityRestitutionValueLabel;
@property(strong) NSButton* gravityResetButton;
@property(strong) NSPopUpButton* playModePopup;
@property(strong) NSPopUpButton* rulePopup;
@property(strong) NSPopUpButton* matchPopup;
@property(strong) NSPopUpButton* aiDifficultyPopup;
@property(strong) NSPopUpButton* tableThemePopup;
@property(strong) NSButton* ballBordersToggle;
@property(strong) NSButton* ballNumbersToggle;
@property(strong) NSButton* aimGuideToggle;
@property(strong) NSButton* aiIntentToggle;
@property(strong) NSPopUpButton* slingPresetPopup;
@property(strong) NSPopUpButton* slingAmmoPopup;
@property(strong) NSButton* slingTrajToggle;
@property(strong) NSPopUpButton* hockeyMatchPopup;
@property(strong) NSPopUpButton* hockeyDiffPopup;
@property(strong) NSPopUpButton* hockeyGoalsPopup;
@property(strong) NSButton* hockeyChromeToggle;
@property(strong) NSPopUpButton* iconSelPopup;
@property(strong) NSButton* pickIconsButton;
@property(strong) NSPopUpButton* extraIconsPopup;
@property(strong) NSTextField* lanAddressField;
@property(strong) NSButton* lanHostButton;
@property(strong) NSButton* lanJoinButton;
@property(strong) NSButton* lanStopButton;
@property(strong) NSTextField* lanStatusLabel;
@property(strong) NSPanel* iconPickerSheet;
@property(strong) NSArray<NSButton*>* iconCheckboxes;
@property(strong) NSButton* muteToggle;
@property(strong) NSSlider* blackHoleSize;
@property(strong) NSSlider* blackHoleElevation;
@property(strong) NSSlider* blackHoleTilt;
@property(strong) NSTextField* elevationValueLabel;
@property(strong) NSTextField* tiltValueLabel;
@property(strong) NSButton* blackHoleAngleReset;
@property(strong) NSButton* blackHoleDevour;
@property(strong) NSButton* startButton;
@property(strong) NSButton* startupUndoButton;
@property(strong) NSArray<NSButton*>* controls;
@property(strong) DPPMirrorPreview* preview;
@property(copy) NSArray* images;
@property(copy) NSString* imageSummary;
@property(copy) NSArray* identities;
@property(copy) NSArray<NSString*>* labels;
@property(copy) NSSet<NSString*>* volumePaths;
@property(copy) NSSet<NSString*>* mountedRoots;
@property(copy) NSDictionary* finderIdentity;
@property(strong) NSURL* desktopURL;
@property(strong) NSTimer* timer;
@property(strong) id escapeMonitor;
@property(strong) id settingsLocalMouseMonitor;
@property(strong) id settingsGlobalMouseMonitor;
@property(strong) NSOpenPanel* desktopChooser;
@property BOOL scoped;
@property(strong) NSMutableArray* fileWatchers;
@property(strong) NSMutableArray* volumeWatchers;
- (void)show;
- (void)setupModeSettingsCard;
- (void)layoutModeSettingsForMode:(dpp::PlayMode)pm;
- (void)repositionModeSettingsCard;
- (void)setupStatusBarItem;
- (void)rebuildStatusMenu;
- (void)switchToPlayMode:(dpp::PlayMode)pm;
- (dpp::SlingshotConfig)currentSlingshotConfig;
- (dpp::AirHockeyConfig)currentAirHockeyConfig;
- (void)guard:(void (^)(void))action;
- (BOOL)finish:(NSString*)reason;
@end
@implementation DPPProtectedMirrorPanel
- (NSButton*)button:(NSString*)title action:(SEL)action frame:(NSRect)frame {
    NSButton* b = [NSButton buttonWithTitle:title target:self action:action];
    b.frame = frame;
    [self.panel.contentView addSubview:b];
    return b;
}
- (NSPopUpButton*)popup:(NSArray<NSString*>*)titles action:(SEL)action frame:(NSRect)frame {
    NSPopUpButton* p = [[NSPopUpButton alloc] initWithFrame:frame pullsDown:NO];
    [p addItemsWithTitles:titles];
    p.target = self;
    p.action = action;
    [self.panel.contentView addSubview:p];
    return p;
}
- (NSButton*)cardButton:(NSString*)title action:(SEL)action frame:(NSRect)frame {
    NSButton* b = [NSButton buttonWithTitle:title target:self action:action];
    b.frame = frame;
    [self.modeSettingsGlass addSubview:b];
    return b;
}
- (NSPopUpButton*)cardPopup:(NSArray<NSString*>*)titles action:(SEL)action frame:(NSRect)frame {
    NSPopUpButton* p = [[NSPopUpButton alloc] initWithFrame:frame pullsDown:NO];
    [p addItemsWithTitles:titles];
    p.target = self;
    p.action = action;
    [self.modeSettingsGlass addSubview:p];
    return p;
}
- (NSTextField*)cardLabel:(NSString*)text {
    NSTextField* l = [NSTextField labelWithString:text];
    l.font = [NSFont systemFontOfSize:12.5 weight:NSFontWeightMedium];
    l.textColor = [NSColor colorWithWhite:0.92 alpha:1.0];
    [self.modeSettingsGlass addSubview:l];
    return l;
}
- (void)setupModeSettingsCard {
    const NSRect initRect = NSMakeRect(0, 0, 420, 200);
    self.modeSettingsPanel = [[DPPKeyablePanel alloc]
        initWithContentRect:initRect
                  styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel
                    backing:NSBackingStoreBuffered
                      defer:NO];
    self.modeSettingsPanel.releasedWhenClosed = NO;
    self.modeSettingsPanel.hidesOnDeactivate = NO;
    self.modeSettingsPanel.becomesKeyOnlyIfNeeded = YES;
    self.modeSettingsPanel.opaque = NO;
    self.modeSettingsPanel.backgroundColor = NSColor.clearColor;
    self.modeSettingsPanel.hasShadow = YES;
    self.modeSettingsPanel.level = NSFloatingWindowLevel + 2;
    self.modeSettingsPanel.collectionBehavior =
        NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorFullScreenAuxiliary;

    NSView* root = [[NSView alloc] initWithFrame:initRect];
    root.wantsLayer = YES;
    root.layer.backgroundColor = NSColor.clearColor.CGColor;
    self.modeSettingsPanel.contentView = root;

    self.modeSettingsGlass = [[NSVisualEffectView alloc] initWithFrame:initRect];
    self.modeSettingsGlass.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    self.modeSettingsGlass.material = NSVisualEffectMaterialHUDWindow;
    self.modeSettingsGlass.blendingMode = NSVisualEffectBlendingModeBehindWindow;
    self.modeSettingsGlass.state = NSVisualEffectStateActive;
    self.modeSettingsGlass.appearance = [NSAppearance appearanceNamed:NSAppearanceNameVibrantDark];
    self.modeSettingsGlass.maskImage = [DPPFloatingHudBar maskImageWithCornerRadius:16.0];
    [root addSubview:self.modeSettingsGlass];

    self.modeSettingsRim = [CAShapeLayer layer];
    self.modeSettingsRim.fillColor = nil;
    self.modeSettingsRim.strokeColor = [NSColor colorWithWhite:1.0 alpha:0.20].CGColor;
    self.modeSettingsRim.lineWidth = 1.0;
    [root.layer addSublayer:self.modeSettingsRim];

    self.modeSettingsTitle = [NSTextField labelWithString:@"普通重力 · 专属设置"];
    self.modeSettingsTitle.font = [NSFont systemFontOfSize:13.5 weight:NSFontWeightSemibold];
    self.modeSettingsTitle.textColor = NSColor.whiteColor;
    [self.modeSettingsGlass addSubview:self.modeSettingsTitle];

    self.modeSettingsCloseBtn = [NSButton buttonWithTitle:@"✕"
                                                   target:self
                                                   action:@selector(closeModeSettingsCard:)];
    self.modeSettingsCloseBtn.bezelStyle = NSBezelStyleRecessed;
    self.modeSettingsCloseBtn.showsBorderOnlyWhileMouseInside = YES;
    self.modeSettingsCloseBtn.font = [NSFont systemFontOfSize:12.5 weight:NSFontWeightBold];
    [self.modeSettingsGlass addSubview:self.modeSettingsCloseBtn];

    self.modeSettingsDivider = [[NSView alloc] initWithFrame:NSMakeRect(16, 160, 388, 1)];
    self.modeSettingsDivider.wantsLayer = YES;
    self.modeSettingsDivider.layer.backgroundColor = [NSColor colorWithWhite:1.0 alpha:0.12].CGColor;
    [self.modeSettingsGlass addSubview:self.modeSettingsDivider];

    // Gravity controls
    NSTextField* gravEnvLbl = [self cardLabel:@"天体环境"];
    self.gravityPresetPopup = [self cardPopup:@[
        @"太空失重 (0.00 m/s²)",
        @"冥王星 (0.62 m/s² · 0.06g)",
        @"月球表面 (1.62 m/s² · 0.17g)",
        @"火星表面 (3.72 m/s² · 0.38g)",
        @"地球标准 (9.80 m/s² · 1.00g)",
        @"木星强重 (24.79 m/s² · 2.53g)",
        @"超重离心 (39.20 m/s² · 4.00g)",
        @"自定义加速度"
    ]
                                       action:@selector(gravityPresetChanged:)
                                        frame:NSZeroRect];
    [self.gravityPresetPopup selectItemAtIndex:4];
    self.gravityDirectionPopup = [self cardPopup:@[ @"向下 ↓", @"向上 ↑", @"向左 ←", @"向右 →" ]
                                          action:@selector(gravityOptionsChanged:)
                                           frame:NSZeroRect];
    [self.gravityDirectionPopup selectItemAtIndex:0];

    NSTextField* gravAccLbl = [self cardLabel:@"重力 g"];
    self.gravityAccelSlider = [NSSlider sliderWithValue:dpp::kEarthGravityMps2
                                               minValue:dpp::kMinGravityMps2
                                               maxValue:dpp::kMaxGravityMps2
                                                 target:self
                                                 action:@selector(gravitySliderChanged:)];
    self.gravityAccelSlider.continuous = YES;
    [self.modeSettingsGlass addSubview:self.gravityAccelSlider];
    self.gravityAccelValueLabel = [self cardLabel:@"9.80 m/s² (1.00g)"];
    self.gravityAccelValueLabel.font =
        [NSFont monospacedDigitSystemFontOfSize:11.5 weight:NSFontWeightRegular];

    NSTextField* gravRestLbl = [self cardLabel:@"碰撞弹性"];
    self.gravityRestitutionSlider = [NSSlider sliderWithValue:dpp::kDefaultGravityRestitution
                                                     minValue:dpp::kMinGravityRestitution
                                                     maxValue:dpp::kMaxGravityRestitution
                                                       target:self
                                                       action:@selector(gravityOptionsChanged:)];
    self.gravityRestitutionSlider.continuous = YES;
    [self.modeSettingsGlass addSubview:self.gravityRestitutionSlider];
    self.gravityRestitutionValueLabel = [self cardLabel:@"e = 0.15"];
    self.gravityRestitutionValueLabel.font =
        [NSFont monospacedDigitSystemFontOfSize:11.5 weight:NSFontWeightRegular];

    self.filenameToggle = [NSButton checkboxWithTitle:@"显示桌面图标文件名"
                                               target:self
                                               action:@selector(filenameVisibilityChanged:)];
    self.filenameToggle.state = NSControlStateValueOn;
    [self.modeSettingsGlass addSubview:self.filenameToggle];
    self.gravityResetButton = [self cardButton:@"标准地球重力"
                                        action:@selector(resetEarthGravity:)
                                         frame:NSZeroRect];
    self.saveLayoutButton = [self cardButton:@"保存当前位置到桌面…"
                                      action:@selector(saveLayoutWritebackClicked:)
                                       frame:NSZeroRect];
    self.undoLayoutButton = [self cardButton:@"撤销写回"
                                      action:@selector(undoLayoutWritebackClicked:)
                                       frame:NSZeroRect];
    NSTextField* gravHint = [NSTextField
        wrappingLabelWithString:
            @"提示：加速度遵循真实 SI 物理单位 (s = ½gt²)；默认还原不改动真实桌面，仅主动点击保存时写回。"];
    gravHint.font = [NSFont systemFontOfSize:11.5];
    gravHint.textColor = [NSColor colorWithWhite:0.74 alpha:1.0];
    [self.modeSettingsGlass addSubview:gravHint];

    // Black Hole controls
    self.blackHoleDevour = [NSButton checkboxWithTitle:@"黑洞吞噬全部图标副本"
                                                target:self
                                                action:@selector(blackHoleDevourChanged:)];
    [self.modeSettingsGlass addSubview:self.blackHoleDevour];
    self.blackHoleAngleReset = [self cardButton:@"默认 3D 角度"
                                         action:@selector(resetBlackHoleAngles:)
                                          frame:NSZeroRect];
    NSTextField* bhSizeLbl = [self cardLabel:@"黑洞大小"];
    self.blackHoleSize = [NSSlider sliderWithValue:1.0
                                          minValue:dpp::BlackHoleState::kMinScale
                                          maxValue:dpp::BlackHoleState::kMaxScale
                                            target:self
                                            action:@selector(blackHoleSizeChanged:)];
    self.blackHoleSize.continuous = YES;
    [self.modeSettingsGlass addSubview:self.blackHoleSize];

    NSTextField* bhPitchLbl = [self cardLabel:@"俯仰角度"];
    self.blackHoleElevation = [NSSlider sliderWithValue:dpp::BlackHoleState::kDefaultElevation * 180.0 / M_PI
                                               minValue:-85.0
                                               maxValue:85.0
                                                 target:self
                                                 action:@selector(blackHoleAnglesChanged:)];
    self.blackHoleElevation.continuous = YES;
    [self.modeSettingsGlass addSubview:self.blackHoleElevation];
    self.elevationValueLabel = [self cardLabel:@"+3°"];
    self.elevationValueLabel.font = [NSFont monospacedDigitSystemFontOfSize:12.0 weight:NSFontWeightRegular];

    NSTextField* bhRollLbl = [self cardLabel:@"倾斜角度"];
    self.blackHoleTilt = [NSSlider sliderWithValue:0.0
                                          minValue:-180.0
                                          maxValue:180.0
                                            target:self
                                            action:@selector(blackHoleAnglesChanged:)];
    self.blackHoleTilt.continuous = YES;
    [self.modeSettingsGlass addSubview:self.blackHoleTilt];
    self.tiltValueLabel = [self cardLabel:@"0°"];
    self.tiltValueLabel.font = [NSFont monospacedDigitSystemFontOfSize:12.0 weight:NSFontWeightRegular];
    NSTextField* bhHint =
        [NSTextField wrappingLabelWithString:@"提示：拖拽视界移动黑洞；按住 Option 拖拽自由旋转 3D 吸积盘。"];
    bhHint.font = [NSFont systemFontOfSize:11.5];
    bhHint.textColor = [NSColor colorWithWhite:0.74 alpha:1.0];
    [self.modeSettingsGlass addSubview:bhHint];

    // Billiards controls
    NSTextField* ruleLbl = [self cardLabel:@"台球规则"];
    self.rulePopup = [self cardPopup:@[ @"自由清台", @"国际黑八(15球)", @"国际斯诺克(21球)" ]
                              action:@selector(billiardsOptionsChanged:)
                               frame:NSZeroRect];
    NSTextField* matchLbl = [self cardLabel:@"对局模式"];
    self.matchPopup = [self cardPopup:@[ @"单人挑战", @"人机对战", @"双人同屏轮流", @"局域网对战" ]
                               action:@selector(billiardsOptionsChanged:)
                                frame:NSZeroRect];
    self.aiDifficultyPopup = [self cardPopup:@[ @"简单", @"标准", @"大师", @"球王" ]
                                      action:@selector(billiardsOptionsChanged:)
                                       frame:NSZeroRect];
    [self.aiDifficultyPopup selectItemAtIndex:1];

    NSTextField* tableThemeLbl = [self cardLabel:@"球桌外观"];
    self.tableThemePopup = [self cardPopup:@[ @"拟真球桌与球管", @"极简线条球桌", @"完全无边框" ]
                                    action:@selector(billiardsOptionsChanged:)
                                     frame:NSZeroRect];

    self.ballBordersToggle = [NSButton checkboxWithTitle:@"显示球边框/彩环"
                                                  target:self
                                                  action:@selector(billiardsOptionsChanged:)];
    self.ballBordersToggle.state = NSControlStateValueOn;
    [self.modeSettingsGlass addSubview:self.ballBordersToggle];

    self.ballNumbersToggle = [NSButton checkboxWithTitle:@"球心显示球号"
                                                  target:self
                                                  action:@selector(billiardsOptionsChanged:)];
    self.ballNumbersToggle.state = NSControlStateValueOff;
    [self.modeSettingsGlass addSubview:self.ballNumbersToggle];

    self.aimGuideToggle = [NSButton checkboxWithTitle:@"瞄准折射辅助线"
                                               target:self
                                               action:@selector(billiardsOptionsChanged:)];
    self.aimGuideToggle.state = NSControlStateValueOn;
    [self.modeSettingsGlass addSubview:self.aimGuideToggle];

    self.aiIntentToggle = [NSButton checkboxWithTitle:@"显示人机战术意图提示"
                                               target:self
                                               action:@selector(billiardsOptionsChanged:)];
    self.aiIntentToggle.state = NSControlStateValueOn;
    [self.modeSettingsGlass addSubview:self.aiIntentToggle];

    // Shared icon selection & extra icons controls
    NSTextField* iconSelLbl = [self cardLabel:@"出场图标"];
    self.iconSelPopup = [self cardPopup:@[ @"取前 N 个", @"随机抽取", @"自选图标…" ]
                                 action:@selector(iconSelectionChanged:)
                                  frame:NSZeroRect];
    self.pickIconsButton = [self cardButton:@"挑选图标…" action:@selector(pickIcons:) frame:NSZeroRect];

    NSTextField* extraLbl = [self cardLabel:@"其余图标"];
    self.extraIconsPopup = [self cardPopup:@[ @"玩法期间隐藏", @"底部重力 / 场边观众" ]
                                    action:@selector(extraIconsChanged:)
                                     frame:NSZeroRect];

    // Slingshot controls
    NSTextField* slingLbl = [self cardLabel:@"攻城关卡"];
    self.slingPresetPopup = [self
        cardPopup:@[ @"经典双塔", @"金字塔要塞", @"冰晶悬廊", @"巨石碉堡", @"随机无尽关卡", @"自建关卡工坊" ]
           action:@selector(slingshotOptionsChanged:)
            frame:NSZeroRect];
    self.slingEditButton = [self cardButton:@"关卡工坊…"
                                     action:@selector(slingshotEditClicked:)
                                      frame:NSZeroRect];
    NSTextField* slingAmmoLbl = [self cardLabel:@"每关弹药"];
    self.slingAmmoPopup = [self cardPopup:@[ @"每关 3 发", @"每关 4 发", @"每关 5 发", @"每关 6 发" ]
                                   action:@selector(slingshotOptionsChanged:)
                                    frame:NSZeroRect];
    [self.slingAmmoPopup selectItemAtIndex:1];
    self.slingTrajToggle = [NSButton checkboxWithTitle:@"显示抛物线弹道预测点"
                                                target:self
                                                action:@selector(slingshotOptionsChanged:)];
    self.slingTrajToggle.state = NSControlStateValueOn;
    [self.modeSettingsGlass addSubview:self.slingTrajToggle];
    NSTextField* slingHint = [NSTextField
        wrappingLabelWithString:@"提示：按住左侧皮兜后拉发射；飞行途中再次点击屏幕触发二段加速冲刺！"];
    slingHint.font = [NSFont systemFontOfSize:11.5];
    slingHint.textColor = [NSColor colorWithWhite:0.74 alpha:1.0];
    [self.modeSettingsGlass addSubview:slingHint];

    // Air Hockey controls
    NSTextField* hkMatchLbl = [self cardLabel:@"对局模式"];
    self.hockeyMatchPopup = [self cardPopup:@[ @"单人挑战 AI", @"双人同屏(鼠+键)", @"局域网 1v1" ]
                                     action:@selector(airHockeyOptionsChanged:)
                                      frame:NSZeroRect];
    self.hockeyDiffPopup = [self cardPopup:@[ @"休闲 AI", @"标准 AI", @"大师 AI" ]
                                    action:@selector(airHockeyOptionsChanged:)
                                     frame:NSZeroRect];
    [self.hockeyDiffPopup selectItemAtIndex:1];
    NSTextField* hkGoalLbl = [self cardLabel:@"获胜条件"];
    self.hockeyGoalsPopup = [self cardPopup:@[ @"先得 5 分胜", @"先得 7 分胜", @"先得 11 分胜" ]
                                     action:@selector(airHockeyOptionsChanged:)
                                      frame:NSZeroRect];
    [self.hockeyGoalsPopup selectItemAtIndex:1];
    self.hockeyChromeToggle = [NSButton checkboxWithTitle:@"显示霓虹圆角球台外框"
                                                   target:self
                                                   action:@selector(airHockeyOptionsChanged:)];
    self.hockeyChromeToggle.state = NSControlStateValueOn;
    [self.modeSettingsGlass addSubview:self.hockeyChromeToggle];

    // Shared LAN controls
    NSTextField* lanLbl = [self cardLabel:@"联机地址"];
    const std::string localIp = BilliardsLan::primaryLanIPv4();
    NSString* defaultAddr = [NSString stringWithFormat:@"%s:%u", localIp.c_str(), BilliardsLan::kDefaultPort];
    self.lanAddressField = [NSTextField textFieldWithString:defaultAddr];
    self.lanAddressField.placeholderString = @"房主IP:39457";
    [self.modeSettingsGlass addSubview:self.lanAddressField];
    self.lanHostButton = [self cardButton:@"建房" action:@selector(lanHostClicked:) frame:NSZeroRect];
    self.lanScanButton = [self cardButton:@"搜房加入" action:@selector(lanScanJoinClicked:) frame:NSZeroRect];
    self.lanJoinButton = [self cardButton:@"连IP" action:@selector(lanJoinClicked:) frame:NSZeroRect];
    self.lanStopButton = [self cardButton:@"断开" action:@selector(lanStopClicked:) frame:NSZeroRect];
    self.lanStatusLabel = [NSTextField labelWithString:@"局域网未开启"];
    self.lanStatusLabel.font = [NSFont systemFontOfSize:11.5];
    self.lanStatusLabel.textColor = [NSColor colorWithWhite:0.78 alpha:1.0];
    self.lanStatusLabel.lineBreakMode = NSLineBreakByTruncatingTail;
    [self.modeSettingsGlass addSubview:self.lanStatusLabel];

    NSTextField* dispScopeCardLbl = [self cardLabel:@"显示屏幕"];
    self.inGameDisplayPopup = [self cardPopup:@[] action:@selector(inGameDisplayScopeChanged:) frame:NSZeroRect];
    [self refreshDisplayScopePopup];

    self.modeSettingLabels = @[
        gravHint, bhSizeLbl, bhPitchLbl, bhRollLbl, bhHint, ruleLbl, matchLbl, iconSelLbl, extraLbl, slingLbl,
        slingAmmoLbl, slingHint, hkMatchLbl, hkGoalLbl, lanLbl, gravEnvLbl, gravAccLbl, gravRestLbl,
        tableThemeLbl, dispScopeCardLbl
    ];

    [self layoutModeSettingsForMode:dpp::PlayMode::Gravity];
}

- (void)layoutModeSettingsForMode:(dpp::PlayMode)pm {
    if (!self.modeSettingsPanel) return;
    NSArray<NSView*>* allControls = @[
        self.inGameDisplayPopup,
        self.gravityPresetPopup,      self.gravityDirectionPopup,   self.gravityAccelSlider,
        self.gravityAccelValueLabel,  self.gravityRestitutionSlider, self.gravityRestitutionValueLabel,
        self.gravityResetButton,      self.filenameToggle,          self.saveLayoutButton,
        self.undoLayoutButton,        self.blackHoleDevour,         self.blackHoleAngleReset,
        self.blackHoleSize,           self.blackHoleElevation,      self.elevationValueLabel,
        self.blackHoleTilt,           self.tiltValueLabel,          self.rulePopup,
        self.matchPopup,              self.aiDifficultyPopup,       self.tableThemePopup,
        self.ballBordersToggle,       self.ballNumbersToggle,       self.aimGuideToggle,
        self.aiIntentToggle,          self.iconSelPopup,            self.pickIconsButton,
        self.extraIconsPopup,         self.slingPresetPopup,        self.slingEditButton,
        self.slingAmmoPopup,          self.slingTrajToggle,         self.hockeyMatchPopup,
        self.hockeyDiffPopup,         self.hockeyGoalsPopup,        self.hockeyChromeToggle,
        self.lanAddressField,         self.lanHostButton,           self.lanScanButton,
        self.lanJoinButton,           self.lanStopButton,           self.lanStatusLabel
    ];
    for (NSView* v in allControls) v.hidden = YES;
    for (NSView* v in self.modeSettingLabels) v.hidden = YES;

    const BOOL showMultiDisplayRow = (!_syntheticCheckMode && NSScreen.screens.count > 1);
    if (showMultiDisplayRow) [self refreshDisplayScopePopup];

    const CGFloat W = 420.0;
    CGFloat H = 154.0;
    if (pm == dpp::PlayMode::Gravity) {
        self.modeSettingsTitle.stringValue = @"普通重力 · 科学物理设置";
        H = 216.0;
    } else if (pm == dpp::PlayMode::BlackHole) {
        self.modeSettingsTitle.stringValue = @"3D 拟真黑洞 · 专属设置";
        H = 182.0;
    } else if (pm == dpp::PlayMode::Billiards) {
        self.modeSettingsTitle.stringValue = @"桌面台球 · 专属设置";
        const BOOL showLan = (self.matchPopup.indexOfSelectedItem == 3);
        H = showLan ? 360.0 : 292.0;
    } else if (pm == dpp::PlayMode::Slingshot) {
        self.modeSettingsTitle.stringValue = @"弹弓攻城 · 专属设置";
        H = 176.0;
    } else {
        self.modeSettingsTitle.stringValue = @"空气曲棍球 · 专属设置";
        const BOOL showLan = (self.hockeyMatchPopup.indexOfSelectedItem == 2);
        H = showLan ? 268.0 : 200.0;
    }
    if (showMultiDisplayRow) H += 34.0;

    NSRect f = self.modeSettingsPanel.frame;
    f.size = NSMakeSize(W, H);
    [self.modeSettingsPanel setFrame:f display:YES];

    const NSRect bounds = NSMakeRect(0, 0, W, H);
    self.modeSettingsRim.frame = bounds;
    CGPathRef rimPath = CGPathCreateWithRoundedRect(CGRectInset(bounds, 0.75, 0.75), 15.25, 15.25, nullptr);
    self.modeSettingsRim.path = rimPath;
    CGPathRelease(rimPath);

    self.modeSettingsTitle.frame = NSMakeRect(18, H - 30, W - 64, 20);
    self.modeSettingsCloseBtn.frame = NSMakeRect(W - 38, H - 33, 24, 24);
    self.modeSettingsDivider.frame = NSMakeRect(16, H - 40, W - 32, 1);

    CGFloat y = H - 74.0;
    auto L = [&](NSUInteger idx) -> NSView* {
        NSView* v = self.modeSettingLabels[idx];
        v.hidden = NO;
        return v;
    };

    if (showMultiDisplayRow) {
        L(19).frame = NSMakeRect(18, y + 4, 62, 20);
        self.inGameDisplayPopup.hidden = NO;
        self.inGameDisplayPopup.frame = NSMakeRect(84, y, W - 102, 26);
        y -= 34.0;
    }

    if (pm == dpp::PlayMode::Gravity) {
        L(15).frame = NSMakeRect(18, y + 4, 62, 20);
        self.gravityPresetPopup.hidden = NO;
        self.gravityPresetPopup.frame = NSMakeRect(84, y, 222, 26);
        self.gravityDirectionPopup.hidden = NO;
        self.gravityDirectionPopup.frame = NSMakeRect(312, y, 90, 26);
        y -= 34.0;
        L(16).frame = NSMakeRect(18, y + 2, 62, 20);
        self.gravityAccelSlider.hidden = NO;
        self.gravityAccelSlider.frame = NSMakeRect(84, y, 202, 24);
        self.gravityAccelValueLabel.hidden = NO;
        self.gravityAccelValueLabel.frame = NSMakeRect(292, y + 2, 114, 20);
        y -= 30.0;
        L(17).frame = NSMakeRect(18, y + 2, 62, 20);
        self.gravityRestitutionSlider.hidden = NO;
        self.gravityRestitutionSlider.frame = NSMakeRect(84, y, 242, 24);
        self.gravityRestitutionValueLabel.hidden = NO;
        self.gravityRestitutionValueLabel.frame = NSMakeRect(332, y + 2, 74, 20);
        y -= 30.0;
        self.filenameToggle.hidden = NO;
        self.filenameToggle.frame = NSMakeRect(18, y, 186, 24);
        self.gravityResetButton.hidden = NO;
        self.gravityResetButton.frame = NSMakeRect(W - 122, y - 1, 104, 26);
        y -= 32.0;
        self.saveLayoutButton.hidden = NO;
        self.saveLayoutButton.frame = NSMakeRect(18, y, 178, 26);
        self.undoLayoutButton.hidden = NO;
        self.undoLayoutButton.frame = NSMakeRect(204, y, 96, 26);
    } else if (pm == dpp::PlayMode::BlackHole) {
        self.blackHoleDevour.hidden = NO;
        self.blackHoleDevour.frame = NSMakeRect(18, y, 200, 24);
        self.blackHoleAngleReset.hidden = NO;
        self.blackHoleAngleReset.frame = NSMakeRect(W - 124, y - 2, 106, 26);
        y -= 34.0;
        L(1).frame = NSMakeRect(18, y + 2, 62, 20);
        self.blackHoleSize.hidden = NO;
        self.blackHoleSize.frame = NSMakeRect(86, y, W - 104, 24);
        y -= 32.0;
        L(2).frame = NSMakeRect(18, y + 2, 62, 20);
        self.blackHoleElevation.hidden = NO;
        self.blackHoleElevation.frame = NSMakeRect(86, y, W - 156, 24);
        self.elevationValueLabel.hidden = NO;
        self.elevationValueLabel.frame = NSMakeRect(W - 62, y + 2, 46, 20);
        y -= 32.0;
        L(3).frame = NSMakeRect(18, y + 2, 62, 20);
        self.blackHoleTilt.hidden = NO;
        self.blackHoleTilt.frame = NSMakeRect(86, y, W - 156, 24);
        self.tiltValueLabel.hidden = NO;
        self.tiltValueLabel.frame = NSMakeRect(W - 62, y + 2, 46, 20);
    } else if (pm == dpp::PlayMode::Billiards) {
        L(5).frame = NSMakeRect(18, y + 4, 62, 20);
        self.rulePopup.hidden = NO;
        self.rulePopup.frame = NSMakeRect(84, y, W - 102, 26);
        y -= 34.0;
        L(6).frame = NSMakeRect(18, y + 4, 62, 20);
        self.matchPopup.hidden = NO;
        const BOOL vsAi = (self.matchPopup.indexOfSelectedItem == 1);
        self.matchPopup.frame = NSMakeRect(84, y, vsAi ? 186 : (W - 102), 26);
        if (vsAi) {
            self.aiDifficultyPopup.hidden = NO;
            self.aiDifficultyPopup.frame = NSMakeRect(278, y, W - 296, 26);
        }
        y -= 34.0;
        L(18).frame = NSMakeRect(18, y + 4, 62, 20);
        self.tableThemePopup.hidden = NO;
        self.tableThemePopup.frame = NSMakeRect(84, y, W - 102, 26);
        y -= 32.0;
        self.ballBordersToggle.hidden = NO;
        self.ballBordersToggle.frame = NSMakeRect(18, y, 178, 22);
        self.ballNumbersToggle.hidden = NO;
        self.ballNumbersToggle.frame = NSMakeRect(204, y, 198, 22);
        y -= 26.0;
        self.aimGuideToggle.hidden = NO;
        self.aimGuideToggle.frame = NSMakeRect(18, y, 178, 22);
        if (vsAi) {
            self.aiIntentToggle.hidden = NO;
            self.aiIntentToggle.frame = NSMakeRect(204, y, 198, 22);
        }
        y -= 34.0;
        L(7).frame = NSMakeRect(18, y + 4, 62, 20);
        self.iconSelPopup.hidden = NO;
        const BOOL customSel = (self.iconSelPopup.indexOfSelectedItem == 2);
        self.iconSelPopup.frame = NSMakeRect(84, y, customSel ? 196 : (W - 102), 26);
        if (customSel) {
            self.pickIconsButton.hidden = NO;
            self.pickIconsButton.frame = NSMakeRect(288, y, W - 306, 26);
        }
        y -= 32.0;
        L(8).frame = NSMakeRect(18, y + 4, 62, 20);
        self.extraIconsPopup.hidden = NO;
        self.extraIconsPopup.frame = NSMakeRect(84, y, W - 102, 26);
        if (self.matchPopup.indexOfSelectedItem == 3) {
            y -= 34.0;
            L(14).frame = NSMakeRect(18, y + 2, 62, 20);
            self.lanAddressField.hidden = NO;
            self.lanAddressField.frame = NSMakeRect(84, y, 118, 24);
            self.lanHostButton.hidden = NO;
            self.lanHostButton.frame = NSMakeRect(206, y - 1, 46, 26);
            self.lanScanButton.hidden = NO;
            self.lanScanButton.frame = NSMakeRect(254, y - 1, 66, 26);
            self.lanJoinButton.hidden = NO;
            self.lanJoinButton.frame = NSMakeRect(322, y - 1, 42, 26);
            self.lanStopButton.hidden = NO;
            self.lanStopButton.frame = NSMakeRect(366, y - 1, 42, 26);
            self.lanStatusLabel.hidden = NO;
            self.lanStatusLabel.frame = NSMakeRect(84, 10, W - 102, 18);
        }
    } else if (pm == dpp::PlayMode::Slingshot) {
        L(9).frame = NSMakeRect(18, y + 4, 62, 20);
        self.slingPresetPopup.hidden = NO;
        self.slingPresetPopup.frame = NSMakeRect(84, y, 214, 26);
        self.slingEditButton.hidden = NO;
        self.slingEditButton.frame = NSMakeRect(306, y, W - 324, 26);
        y -= 34.0;
        L(10).frame = NSMakeRect(18, y + 4, 62, 20);
        self.slingAmmoPopup.hidden = NO;
        self.slingAmmoPopup.frame = NSMakeRect(84, y, 128, 26);
        self.slingTrajToggle.hidden = NO;
        self.slingTrajToggle.frame = NSMakeRect(222, y + 2, 182, 22);
        y -= 34.0;
        L(7).frame = NSMakeRect(18, y + 4, 62, 20);
        self.iconSelPopup.hidden = NO;
        const BOOL customSel = (self.iconSelPopup.indexOfSelectedItem == 2);
        self.iconSelPopup.frame = NSMakeRect(84, y, customSel ? 196 : (W - 102), 26);
        if (customSel) {
            self.pickIconsButton.hidden = NO;
            self.pickIconsButton.frame = NSMakeRect(288, y, W - 306, 26);
        }
    } else {
        L(12).frame = NSMakeRect(18, y + 4, 62, 20);
        self.hockeyMatchPopup.hidden = NO;
        const BOOL soloAi = (self.hockeyMatchPopup.indexOfSelectedItem == 0);
        self.hockeyMatchPopup.frame = NSMakeRect(84, y, soloAi ? 186 : (W - 102), 26);
        if (soloAi) {
            self.hockeyDiffPopup.hidden = NO;
            self.hockeyDiffPopup.frame = NSMakeRect(278, y, W - 296, 26);
        }
        y -= 34.0;
        L(13).frame = NSMakeRect(18, y + 4, 62, 20);
        self.hockeyGoalsPopup.hidden = NO;
        self.hockeyGoalsPopup.frame = NSMakeRect(84, y, 146, 26);
        self.hockeyChromeToggle.hidden = NO;
        self.hockeyChromeToggle.frame = NSMakeRect(240, y + 2, 166, 22);
        y -= 34.0;
        L(8).frame = NSMakeRect(18, y + 4, 62, 20);
        self.extraIconsPopup.hidden = NO;
        self.extraIconsPopup.frame = NSMakeRect(84, y, W - 102, 26);
        if (self.hockeyMatchPopup.indexOfSelectedItem == 2) {
            y -= 34.0;
            L(14).frame = NSMakeRect(18, y + 2, 62, 20);
            self.lanAddressField.hidden = NO;
            self.lanAddressField.frame = NSMakeRect(84, y, 118, 24);
            self.lanHostButton.hidden = NO;
            self.lanHostButton.frame = NSMakeRect(206, y - 1, 46, 26);
            self.lanScanButton.hidden = NO;
            self.lanScanButton.frame = NSMakeRect(254, y - 1, 66, 26);
            self.lanJoinButton.hidden = NO;
            self.lanJoinButton.frame = NSMakeRect(322, y - 1, 42, 26);
            self.lanStopButton.hidden = NO;
            self.lanStopButton.frame = NSMakeRect(366, y - 1, 42, 26);
            self.lanStatusLabel.hidden = NO;
            self.lanStatusLabel.frame = NSMakeRect(84, 10, W - 102, 18);
        }
    }
    [self repositionModeSettingsCard];
    [self.modeSettingsPanel invalidateShadow];
}

- (void)repositionModeSettingsCard {
    if (!self.modeSettingsPanel) return;
    const NSRect fallbackVis =
        NSScreen.screens.firstObject ? NSScreen.screens.firstObject.visibleFrame : _workArea;
    const NSRect vis = (_activeSession && _hudWorkArea.size.width > 0) ? _hudWorkArea : fallbackVis;
    const NSSize sz = self.modeSettingsPanel.frame.size;
    NSPoint origin = NSMakePoint(NSMidX(vis) - sz.width * 0.5, NSMaxY(vis) - sz.height - 62.0);
    if (self.hudBar && self.hudBar.isVisible) {
        const NSRect hf = self.hudBar.window.frame;
        origin.x =
            std::clamp<CGFloat>(NSMidX(hf) - sz.width * 0.5, NSMinX(vis) + 8.0, NSMaxX(vis) - sz.width - 8.0);
        CGFloat belowY = NSMinY(hf) - sz.height - 8.0;
        if (belowY < NSMinY(vis) + 8.0) {
            belowY = std::min<CGFloat>(NSMaxY(vis) - sz.height - 8.0, NSMaxY(hf) + 8.0);
        }
        origin.y = belowY;
    }
    [self.modeSettingsPanel setFrameOrigin:origin];
}

- (BOOL)canAutoDismissModeSettingsCard {
    return self.modeSettingsPanel && self.modeSettingsPanel.visible && !_menuTrackingActive &&
           !_modalAlertActive && !self.iconPickerSheet && !self.desktopChooser;
}
- (void)menuTrackingBegan:(NSNotification*)notification {
    (void)notification;
    _menuTrackingActive = YES;
}
- (void)menuTrackingEnded:(NSNotification*)notification {
    (void)notification;
    _menuTrackingActive = NO;
}
- (void)closeModeSettingsCard:(id)sender {
    (void)sender;
    [self.modeSettingsPanel orderOut:nil];
    if (_activeSession && !_transition && self.preview.active) {
        [self.preview ensureInteractiveIfAllowed];
    }
    [self refreshButtons];
}

- (void)extraIconsChanged:(id)sender {
    (void)sender;
    [self guard:^{
      if (self.preview.airHockeyEnabled) {
          [self airHockeyOptionsChanged:nil];
      } else {
          [self billiardsOptionsChanged:nil];
      }
    }];
}

- (void)syncLauncherModePills {
    const NSInteger activeIdx =
        _savedPlayMode == dpp::PlayMode::BlackHole
            ? 1
            : (_savedPlayMode == dpp::PlayMode::Billiards
                   ? 2
                   : (_savedPlayMode == dpp::PlayMode::Slingshot
                          ? 3
                          : (_savedPlayMode == dpp::PlayMode::AirHockey ? 4 : 0)));
    for (NSUInteger i = 0; i < self.launcherModePills.count; ++i) {
        DPPLauncherModePill* pill = self.launcherModePills[i];
        pill.pillSelected = (static_cast<NSInteger>(i) == activeIdx);
        pill.needsDisplay = YES;
    }
}

- (void)launcherModePillClicked:(DPPLauncherModePill*)sender {
    const NSInteger idx = sender.tag;
    _savedPlayMode =
        idx == 1 ? dpp::PlayMode::BlackHole
                 : (idx == 2 ? dpp::PlayMode::Billiards
                             : (idx == 3 ? dpp::PlayMode::Slingshot
                                         : (idx == 4 ? dpp::PlayMode::AirHockey : dpp::PlayMode::Gravity)));
    [self syncLauncherModePills];
    [self saveUserPreferences];
}

- (void)refreshDisplayScopePopup {
    NSArray<NSScreen*>* screens = NSScreen.screens;
    auto populatePopup = [&](NSPopUpButton* popup) {
        if (!popup) return;
        [popup removeAllItems];
        if (screens.count <= 1) {
            NSScreen* scr = screens.firstObject;
            NSString* title = scr ? [NSString stringWithFormat:@"🖥  主显示器（%.0f × %.0f）",
                                                               scr.frame.size.width, scr.frame.size.height]
                                  : @"🖥  主显示器";
            [popup addItemWithTitle:title];
            popup.lastItem.tag = 0;
            [popup selectItemAtIndex:0];
            return;
        }
        for (NSUInteger i = 0; i < screens.count; ++i) {
            NSScreen* scr = screens[i];
            NSString* role = (i == 0) ? @"主屏" : @"副屏";
            NSString* title = [NSString stringWithFormat:@"🖥  仅屏幕 %lu（%@ · %.0f × %.0f）",
                                                         static_cast<unsigned long>(i + 1), role,
                                                         scr.frame.size.width, scr.frame.size.height];
            [popup addItemWithTitle:title];
            popup.lastItem.tag = static_cast<NSInteger>(i);
        }
        NSString* spanTitle =
            [NSString stringWithFormat:@"🌐  多屏跨屏显示（共 %lu 块屏幕铺满）",
                                       static_cast<unsigned long>(screens.count)];
        [popup addItemWithTitle:spanTitle];
        popup.lastItem.tag = -1;

        if (_displayMode < 0) {
            [popup selectItemWithTag:-1];
        } else {
            const NSInteger clamped =
                std::clamp<NSInteger>(_displayMode, 0, static_cast<NSInteger>(screens.count) - 1);
            [popup selectItemWithTag:clamped];
        }
    };
    if (screens.count <= 1 && _displayMode < 0) _displayMode = 0;
    populatePopup(self.displayScopePopup);
    populatePopup(self.inGameDisplayPopup);
}

- (void)applyDisplayModeSelection:(int)newMode {
    if (_loading || _transition) return;
    if (newMode == _displayMode && !_activeSession) {
        [self refreshDisplayScopePopup];
        [self rebuildStatusMenu];
        return;
    }
    _displayMode = newMode;
    [self refreshDisplayScopePopup];
    [self saveUserPreferences];
    if (_activeSession && _protectedSession && self.images.count > 0) {
        [self guard:^{
          NSArray<NSScreen*>* screens = NSScreen.screens;
          if (screens.count < 1) return;
          const dpp::PlayMode activePm = [self.preview playMode];
          [self closePassiveScreenWindows];

          self->_allDisplaysSnapshot.clear();
          CGFloat maxBackingScale = 1.0;
          for (NSUInteger i = 0; i < screens.count; ++i) {
              const auto d = mirrorDisplayAt(i);
              self->_allDisplaysSnapshot.push_back(d);
              maxBackingScale = std::max(maxBackingScale, d.scale);
          }
          self->_displaySnapshot = self->_allDisplaysSnapshot.front();
          self->_screenFrame = screens.firstObject.frame;

          const BOOL spanAllScreens = (self->_displayMode < 0 && screens.count > 1);
          const NSUInteger selectedScreenIdx =
              spanAllScreens ? 0u
                             : std::min<NSUInteger>(static_cast<NSUInteger>(std::max(0, self->_displayMode)),
                                                    screens.count - 1);
          if (spanAllScreens) {
              NSRect unionVis = screens.firstObject.visibleFrame;
              NSMutableArray<NSValue*>* vps = [NSMutableArray arrayWithCapacity:screens.count];
              for (NSScreen* scr in screens) {
                  unionVis = NSUnionRect(unionVis, scr.visibleFrame);
                  [vps addObject:[NSValue valueWithRect:scr.visibleFrame]];
              }
              self->_workArea = unionVis;
              self->_hudWorkArea = screens.firstObject.visibleFrame;
              self->_scale = maxBackingScale;
              self.preview.multiScreenMode = YES;
              self.preview.targetScreenIndex = 0;
              self.preview.multiScreenViewports = vps;
          } else {
              self->_workArea = screens[selectedScreenIdx].visibleFrame;
              self->_hudWorkArea = self->_workArea;
              self->_scale = screens[selectedScreenIdx].backingScaleFactor;
              self.preview.multiScreenMode = (screens.count > 1);
              self.preview.targetScreenIndex = selectedScreenIdx;
              self.preview.multiScreenViewports = nil;
          }

          auto callback = self.preview.didClose;
          self.preview.didClose = nil;
          [self.preview close:@"display_scope_switched"];
          self.preview.didClose = callback;

          self->_scene = [self scene:NO];
          self->_openTargets.clear();
          self->_openConsumed = false;
          self->_cancelOpen = false;
          for (std::size_t k = 0; k < self->_scene.items.size() && k < self->_activeOrigIndices.size(); ++k) {
              const std::size_t origIdx = self->_activeOrigIndices[k];
              if (origIdx >= self->_snapshot.urls.count) continue;
              NSURL* url = [self->_snapshot.urls[origIdx] copy];
              const auto target = bindOpenTarget(url);
              NSArray* sourceIdentity = target.aliasURL ? target.aliasIdentity : target.identity;
              if (target.url && [sourceIdentity isEqual:self.identities[origIdx]])
                  self->_openTargets.emplace(static_cast<uint32_t>(self->_scene.items[k].id), target);
          }
          self.preview.labels = self.activeLabels;
          self.preview.deferPresentation = YES;
          self.preview.continuousSession = YES;
          if (![self.preview start:self->_scene images:self.activeImages]) {
              [self finish:@"display_scope_switch_failed"];
              return;
          }
          self.preview.allowsBackgroundSession = YES;
          self.preview.protectionIdentity = self->_protectedSession->diagnosticIdentity();
          if (![self.preview presentPrepared]) {
              [self finish:@"display_scope_present_failed"];
              return;
          }
          if (self->_displayMode >= 0 && screens.count > 1) {
              [self presentPassiveScreenWindowsForSelectedScreen:selectedScreenIdx];
          }
          [self.preview playPause];
          if (activePm != dpp::PlayMode::Gravity) {
              [self switchToPlayMode:activePm];
          }
          const NSRect hudVis = (self->_hudWorkArea.size.width > 0) ? self->_hudWorkArea : self->_workArea;
          if (self.hudBar.isVisible) [self.hudBar showInWorkArea:hudVis];
          if (self.modeSettingsPanel && self.modeSettingsPanel.visible) {
              [self layoutModeSettingsForMode:[self.preview playMode]];
              [self repositionModeSettingsCard];
              [self.modeSettingsPanel orderFrontRegardless];
          } else {
              [self.preview ensureInteractiveIfAllowed];
          }
          [self refreshButtons];
        }];
        return;
    }
    [self rebuildStatusMenu];
}

- (void)displayScopeChanged:(id)sender {
    (void)sender;
    if (!self.displayScopePopup || !self.displayScopePopup.selectedItem) return;
    [self applyDisplayModeSelection:static_cast<int>(self.displayScopePopup.selectedItem.tag)];
}

- (void)inGameDisplayScopeChanged:(id)sender {
    (void)sender;
    if (!self.inGameDisplayPopup || !self.inGameDisplayPopup.selectedItem) return;
    [self applyDisplayModeSelection:static_cast<int>(self.inGameDisplayPopup.selectedItem.tag)];
}

- (void)screenParametersChanged:(NSNotification*)notification {
    (void)notification;
    if (!_activeSession && !_loading && !_transition) {
        [self refreshDisplayScopePopup];
        [self rebuildStatusMenu];
    }
}

- (void)closeLauncherClicked:(id)sender {
    (void)sender;
    if ([self windowShouldClose:self.panel]) {
        [self.panel orderOut:nil];
    }
}

- (void)show {
    _randomSeed = 0x20261122u;
    _displayMode = 0;
    const CGFloat W = 540.0;
    const CGFloat H = 286.0;
    const NSRect initRect = NSMakeRect(160, 120, W, H);
    self.panel = [[DPPKeyablePanel alloc]
        initWithContentRect:initRect
                  styleMask:NSWindowStyleMaskBorderless
                    backing:NSBackingStoreBuffered
                      defer:NO];
    const NSRect work = NSScreen.screens.firstObject ? NSScreen.screens.firstObject.visibleFrame
                                                     : NSMakeRect(0, 0, 1440, 900);
    NSRect panelFrame = self.panel.frame;
    panelFrame.origin.x = NSMinX(work) + std::max<CGFloat>(0, (NSWidth(work) - NSWidth(panelFrame)) / 2);
    panelFrame.origin.y = NSMinY(work) + std::max<CGFloat>(0, (NSHeight(work) - NSHeight(panelFrame)) / 2);
    [self.panel setFrame:panelFrame display:NO];
    self.panel.releasedWhenClosed = NO;
    self.panel.hidesOnDeactivate = NO;
    self.panel.delegate = self;
    self.panel.opaque = NO;
    self.panel.backgroundColor = NSColor.clearColor;
    self.panel.hasShadow = YES;
    self.panel.movableByWindowBackground = YES;
    self.panel.level = NSFloatingWindowLevel;
    self.panel.title = @"桌面游乐场 · DeskFling";

    DPPLauncherDragView* root = [[DPPLauncherDragView alloc] initWithFrame:NSMakeRect(0, 0, W, H)];
    root.wantsLayer = YES;
    root.layer.backgroundColor = NSColor.clearColor.CGColor;
    self.panel.contentView = root;

    NSVisualEffectView* glass = [[NSVisualEffectView alloc] initWithFrame:NSMakeRect(0, 0, W, H)];
    glass.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    glass.material = NSVisualEffectMaterialHUDWindow;
    glass.blendingMode = NSVisualEffectBlendingModeBehindWindow;
    glass.state = NSVisualEffectStateActive;
    glass.appearance = [NSAppearance appearanceNamed:NSAppearanceNameVibrantDark];
    glass.maskImage = [DPPFloatingHudBar maskImageWithCornerRadius:20.0];
    [root addSubview:glass];

    CAShapeLayer* rim = [CAShapeLayer layer];
    rim.frame = NSMakeRect(0, 0, W, H);
    CGPathRef rimPath =
        CGPathCreateWithRoundedRect(CGRectInset(NSMakeRect(0, 0, W, H), 0.75, 0.75), 19.25, 19.25, nullptr);
    rim.path = rimPath;
    CGPathRelease(rimPath);
    rim.fillColor = nil;
    rim.strokeColor = [NSColor colorWithWhite:1.0 alpha:0.22].CGColor;
    rim.lineWidth = 1.0;
    [root.layer addSublayer:rim];

    // Header: AppIcon + Vertically Centered Brand Title + Close button
    NSImageView* iconView = [[NSImageView alloc] initWithFrame:NSMakeRect(28, H - 68, 52, 52)];
    NSImage* namedIcon = [NSImage imageNamed:@"AppIcon"];
    NSImage* appIcon = namedIcon ? namedIcon : NSApp.applicationIconImage;
    iconView.image = appIcon;
    iconView.imageScaling = NSImageScaleProportionallyUpOrDown;
    [glass addSubview:iconView];

    NSTextField* brandTitle = [NSTextField labelWithString:@"桌面游乐场 · DeskFling"];
    brandTitle.font = [NSFont systemFontOfSize:19.5 weight:NSFontWeightBold];
    brandTitle.textColor = NSColor.whiteColor;
    brandTitle.frame = NSMakeRect(92, H - 55, W - 144, 26);
    [glass addSubview:brandTitle];

    NSButton* closeBtn = [NSButton buttonWithTitle:@"✕" target:self action:@selector(closeLauncherClicked:)];
    closeBtn.bezelStyle = NSBezelStyleRecessed;
    closeBtn.showsBorderOnlyWhileMouseInside = YES;
    closeBtn.font = [NSFont systemFontOfSize:13.0 weight:NSFontWeightBold];
    closeBtn.frame = NSMakeRect(W - 44, H - 48, 26, 26);
    [glass addSubview:closeBtn];

    NSView* headerDivider = [[NSView alloc] initWithFrame:NSMakeRect(26, H - 82, W - 52, 1)];
    headerDivider.wantsLayer = YES;
    headerDivider.layer.backgroundColor = [NSColor colorWithWhite:1.0 alpha:0.12].CGColor;
    [glass addSubview:headerDivider];

    // Row 1: Display selection
    NSTextField* dispLbl = [NSTextField labelWithString:@"游玩屏幕"];
    dispLbl.font = [NSFont systemFontOfSize:13.0 weight:NSFontWeightMedium];
    dispLbl.textColor = [NSColor colorWithWhite:0.90 alpha:1.0];
    dispLbl.frame = NSMakeRect(28, H - 126, 68, 22);
    [glass addSubview:dispLbl];

    self.displayScopePopup =
        [[NSPopUpButton alloc] initWithFrame:NSMakeRect(104, H - 129, W - 132, 28) pullsDown:NO];
    self.displayScopePopup.target = self;
    self.displayScopePopup.action = @selector(displayScopeChanged:);
    self.displayScopePopup.appearance = [NSAppearance appearanceNamed:NSAppearanceNameVibrantDark];
    [glass addSubview:self.displayScopePopup];
    [self refreshDisplayScopePopup];

    // Row 2: Initial PlayMode pills
    NSTextField* modeLbl = [NSTextField labelWithString:@"初始玩法"];
    modeLbl.font = [NSFont systemFontOfSize:13.0 weight:NSFontWeightMedium];
    modeLbl.textColor = [NSColor colorWithWhite:0.90 alpha:1.0];
    modeLbl.frame = NSMakeRect(28, H - 178, 68, 22);
    [glass addSubview:modeLbl];

    NSArray<NSString*>* pillTitles = @[ @"重力", @"3D 黑洞", @"台球", @"弹弓", @"曲棍球" ];
    NSMutableArray<DPPLauncherModePill*>* pills = [NSMutableArray arrayWithCapacity:5];
    const CGFloat pillStartX = 104.0;
    const CGFloat pillGap = 7.0;
    const CGFloat pillW = std::floor((W - 28.0 - pillStartX - pillGap * 4.0) / 5.0);
    for (NSInteger i = 0; i < 5; ++i) {
        DPPLauncherModePill* pill = [[DPPLauncherModePill alloc]
            initWithFrame:NSMakeRect(pillStartX + i * (pillW + pillGap), H - 185, pillW, 34)];
        pill.title = pillTitles[static_cast<NSUInteger>(i)];
        pill.tag = i;
        pill.bordered = NO;
        pill.target = self;
        pill.action = @selector(launcherModePillClicked:);
        [glass addSubview:pill];
        [pills addObject:pill];
    }
    self.launcherModePills = pills;
    [self syncLauncherModePills];

    // Primary CTA Button
    DPPLauncherPrimaryButton* startCta =
        [[DPPLauncherPrimaryButton alloc] initWithFrame:NSMakeRect(28, 36, W - 56, 48)];
    startCta.title = @"▶  开始游玩";
    startCta.bordered = NO;
    startCta.target = self;
    startCta.action = @selector(quickStartClicked:);
    startCta.keyEquivalent = @"\r";
    [glass addSubview:startCta];
    self.quickStartButton = startCta;

    self.startupUndoButton = [NSButton buttonWithTitle:@"↺ 撤销上次写回"
                                                target:self
                                                action:@selector(undoLayoutWritebackClicked:)];
    self.startupUndoButton.bezelStyle = NSBezelStyleRecessed;
    self.startupUndoButton.appearance = [NSAppearance appearanceNamed:NSAppearanceNameVibrantDark];
    self.startupUndoButton.font = [NSFont systemFontOfSize:11.5 weight:NSFontWeightMedium];
    self.startupUndoButton.frame = NSMakeRect(W - 134, 6, 106, 24);
    self.startupUndoButton.hidden = YES;
    [glass addSubview:self.startupUndoButton];

    self.status = [NSTextField wrappingLabelWithString:@""];
    self.status.font = [NSFont systemFontOfSize:11.5 weight:NSFontWeightRegular];
    self.status.textColor = [NSColor colorWithRed:1.0 green:0.78 blue:0.42 alpha:1.0];
    self.status.frame = NSMakeRect(28, 6, W - 56, 24);
    self.status.hidden = !_syntheticCheckMode;
    [glass addSubview:self.status];

    _savedGravityFilenames = YES;
    // Hidden compatibility controls retained for --mirror-panel-check synthetic state assertions
    self.readButton = [self button:@"重选桌面目录…"
                            action:@selector(readAll:)
                             frame:NSMakeRect(-900, 0, 120, 28)];
    self.confirmed = [NSButton checkboxWithTitle:@"已核对副本对应"
                                          target:self
                                          action:@selector(confirmationChanged:)];
    self.confirmed.frame = NSMakeRect(-900, 0, 118, 24);
    self.confirmed.enabled = NO;
    [self.panel.contentView addSubview:self.confirmed];
    self.startButton = [self button:@"启动实验"
                             action:@selector(startSession:)
                              frame:NSMakeRect(-900, 0, 76, 28)];
    if (!_syntheticCheckMode) {
        self.readButton.hidden = YES;
        self.confirmed.hidden = YES;
        self.startButton.hidden = YES;
    }

    self.playModePopup = [self popup:@[ @"普通重力", @"玩具黑洞", @"台球模式", @"弹弓攻城", @"空气曲棍球" ]
                              action:@selector(playModeChanged:)
                               frame:NSMakeRect(-900, 0, 100, 24)];
    self.playModePopup.hidden = YES;
    NSButton* play = [self button:@"暂停" action:@selector(play:) frame:NSMakeRect(-900, 0, 60, 24)];
    NSButton* reset = [self button:@"重置" action:@selector(resetScene:) frame:NSMakeRect(-900, 0, 60, 24)];
    NSButton* interact = [self button:@"交互" action:@selector(interact:) frame:NSMakeRect(-900, 0, 60, 24)];
    NSButton* disable = [self button:@"关交互"
                              action:@selector(disableInteraction:)
                               frame:NSMakeRect(-900, 0, 60, 24)];
    play.hidden = reset.hidden = interact.hidden = disable.hidden = YES;
    self.muteToggle = [NSButton checkboxWithTitle:@"静音音效" target:self action:@selector(muteChanged:)];
    self.muteToggle.frame = NSMakeRect(-900, 0, 80, 24);
    self.muteToggle.hidden = YES;
    [self.panel.contentView addSubview:self.muteToggle];
    self.controls = @[ play, reset, interact, disable ];

    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(screenParametersChanged:)
                                               name:NSApplicationDidChangeScreenParametersNotification
                                             object:nil];

    [self setupModeSettingsCard];
    self.preview = [DPPMirrorPreview new];
    self.preview.experimentID = @"protected_finder";
    self.preview.opensWithoutEndingSession = YES;
    __weak DPPProtectedMirrorPanel* weak = self;
    self.preview.protectionReader = ^NSDictionary*(BOOL requestHelper) {
      DPPProtectedMirrorPanel* owner = weak;
      if (!owner || !owner->_protectedSession) return @{};
      return requestHelper ? owner->_protectedSession->sampleTiming()
                           : owner->_protectedSession->diagnosticIdentity();
    };
    self.preview.canOpenItem = ^BOOL(uint32_t itemID) {
      DPPProtectedMirrorPanel* owner = weak;
      return owner && owner->_activeSession && !owner->_transition && !owner->_openConsumed &&
             owner->_protectedSession && owner->_protectedSession->remainingMs() > 0 &&
             owner->_openTargets.contains(itemID);
    };
    self.preview.openItem = ^(uint32_t itemID) {
      [weak guard:^{
        [weak openOriginal:itemID];
      }];
    };
    self.preview.didClose = ^(NSString* reason) {
      DPPProtectedMirrorPanel* owner = weak;
      if (owner)
          [owner guard:^{
            if (owner->_transition)
                owner->_cancelRequested = YES;
            else
                [owner finish:reason];
          }];
    };
    self.preview.blackHoleAnglesDidChange = ^(double elevation, double tilt) {
      DPPProtectedMirrorPanel* owner = weak;
      if (!owner) return;
      const double elDeg = elevation * 180.0 / M_PI, tiltDeg = tilt * 180.0 / M_PI;
      owner.blackHoleElevation.doubleValue = elDeg;
      owner.blackHoleTilt.doubleValue = tiltDeg;
      owner.elevationValueLabel.stringValue = [NSString stringWithFormat:@"%+.0f°", elDeg];
      owner.tiltValueLabel.stringValue = [NSString stringWithFormat:@"%+.0f°", tiltDeg];
      [owner saveUserPreferences];
    };
    self.preview.lanStatusChanged = ^(NSString* lanStatus, BOOL activeOrWaiting, BOOL isHost) {
      (void)activeOrWaiting;
      (void)isHost;
      DPPProtectedMirrorPanel* owner = weak;
      if (!owner) return;
      owner.lanStatusLabel.stringValue = lanStatus.length ? lanStatus : @"局域网未连接";
      [owner refreshButtons];
    };
    self.preview.billiardsAimGuideDidChange = ^(BOOL enabled) {
      DPPProtectedMirrorPanel* owner = weak;
      if (!owner) return;
      owner.aimGuideToggle.state = enabled ? NSControlStateValueOn : NSControlStateValueOff;
      [owner saveUserPreferences];
    };
    self.preview.billiardsAiIntentDidChange = ^(BOOL enabled) {
      DPPProtectedMirrorPanel* owner = weak;
      if (!owner) return;
      owner.aiIntentToggle.state = enabled ? NSControlStateValueOn : NSControlStateValueOff;
      [owner saveUserPreferences];
    };
    self.preview.slingshotTrajectoryDidChange = ^(BOOL enabled) {
      DPPProtectedMirrorPanel* owner = weak;
      if (!owner) return;
      owner.slingTrajToggle.state = enabled ? NSControlStateValueOn : NSControlStateValueOff;
      [owner saveUserPreferences];
    };
    self.preview.slingshotPersistenceDidChange = ^(const dpp::SlingshotProgress& progress,
                                                   const std::vector<dpp::SlingshotCustomPiece>& pieces) {
      DPPProtectedMirrorPanel* owner = weak;
      if (!owner) return;
      owner->_slingshotProgress = progress;
      if (!pieces.empty()) {
          owner->_slingshotCustomPieces = pieces;
      }
      [owner.slingPresetPopup
          selectItemAtIndex:std::clamp<NSInteger>(static_cast<NSInteger>(owner.preview.slingshotConfig.preset),
                                                  0, 5)];
      [owner saveUserPreferences];
    };
    self.preview.lanRoomsUpdated = ^(NSArray<NSString*>* labels) {
      DPPProtectedMirrorPanel* owner = weak;
      if (!owner || labels.count == 0) return;
      NSString* firstEp = [owner.preview discoveredLanRoomEndpointAtIndex:0];
      if (firstEp.length && !owner.preview.lanActive) {
          owner.lanAddressField.stringValue = firstEp;
          owner.lanStatusLabel.stringValue = [NSString stringWithFormat:@"已发现房间：%@", labels.firstObject];
      }
      [owner refreshButtons];
    };
    self.preview.airHockeyOptionsDidChange = ^(dpp::AirHockeyDifficulty diff, BOOL showChrome) {
      DPPProtectedMirrorPanel* owner = weak;
      if (!owner) return;
      [owner.hockeyDiffPopup selectItemAtIndex:std::clamp<NSInteger>(static_cast<NSInteger>(diff), 0, 2)];
      owner.hockeyChromeToggle.state = showChrome ? NSControlStateValueOn : NSControlStateValueOff;
      [owner saveUserPreferences];
    };
    self.timer = [NSTimer scheduledTimerWithTimeInterval:.25
                                                 repeats:YES
                                                   block:^(NSTimer*) {
                                                     [weak guard:^{
                                                       [weak heartbeat];
                                                     }];
                                                   }];
    [NSRunLoop.mainRunLoop addTimer:self.timer forMode:NSRunLoopCommonModes];
    self.escapeMonitor =
        [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                              handler:^NSEvent*(NSEvent* event) {
                                                DPPProtectedMirrorPanel* strong = weak;
                                                if (!strong) return event;
                                                if (event.keyCode == 53 && !strong.desktopChooser &&
                                                    !strong.preview.menuActive) {
                                                    [strong guard:^{
                                                      [strong finish:@"escape"];
                                                    }];
                                                    return nil;
                                                }
                                                if (event.keyCode == 48 && strong->_activeSession &&
                                                    !strong.modeSettingsPanel.visible &&
                                                    !strong.iconPickerSheet && !strong.desktopChooser &&
                                                    !strong.preview.menuActive) {
                                                    [strong guard:^{
                                                      [strong toggleHudCapsule:nil];
                                                    }];
                                                    return nil;
                                                }
                                                return event;
                                              }];
    const NSEventMask anyMouseDownMask =
        NSEventMaskLeftMouseDown | NSEventMaskRightMouseDown | NSEventMaskOtherMouseDown;
    self.settingsLocalMouseMonitor =
        [NSEvent addLocalMonitorForEventsMatchingMask:anyMouseDownMask
                                              handler:^NSEvent*(NSEvent* event) {
                                                DPPProtectedMirrorPanel* strong = weak;
                                                if (!strong || ![strong canAutoDismissModeSettingsCard])
                                                    return event;
                                                NSWindow* hitWin = event.window;
                                                if ((strong.preview.overlayWindow &&
                                                     hitWin == strong.preview.overlayWindow) ||
                                                    (strong.panel && hitWin == strong.panel)) {
                                                    [strong guard:^{
                                                      [strong closeModeSettingsCard:nil];
                                                    }];
                                                }
                                                return event;
                                              }];
    self.settingsGlobalMouseMonitor =
        [NSEvent addGlobalMonitorForEventsMatchingMask:anyMouseDownMask
                                               handler:^(NSEvent*) {
                                                 DPPProtectedMirrorPanel* strong = weak;
                                                 if (!strong || ![strong canAutoDismissModeSettingsCard])
                                                     return;
                                                 const NSPoint screenPt = NSEvent.mouseLocation;
                                                 if (NSPointInRect(screenPt, strong.modeSettingsPanel.frame))
                                                     return;
                                                 if (strong.hudBar.isVisible &&
                                                     NSPointInRect(screenPt, strong.hudBar.window.frame))
                                                     return;
                                                 [strong guard:^{
                                                   [strong closeModeSettingsCard:nil];
                                                 }];
                                               }];
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(menuTrackingBegan:)
                                               name:NSMenuDidBeginTrackingNotification
                                             object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(menuTrackingEnded:)
                                               name:NSMenuDidEndTrackingNotification
                                             object:nil];
    for (NSNotificationName name in @[
             NSWorkspaceDidMountNotification, NSWorkspaceWillUnmountNotification,
             NSWorkspaceDidUnmountNotification, NSWorkspaceDidRenameVolumeNotification
         ])
        [NSWorkspace.sharedWorkspace.notificationCenter addObserver:self
                                                           selector:@selector(volumeChanged:)
                                                               name:name
                                                             object:nil];
    for (NSNotificationName sname in @[
             NSWorkspaceWillSleepNotification, NSWorkspaceDidWakeNotification,
             NSWorkspaceScreensDidSleepNotification, NSWorkspaceScreensDidWakeNotification
         ])
        [NSWorkspace.sharedWorkspace.notificationCenter addObserver:self
                                                           selector:@selector(powerOrScreenStateChanged:)
                                                               name:sname
                                                             object:nil];
    [NSDistributedNotificationCenter.defaultCenter addObserver:self
                                                      selector:@selector(powerOrScreenStateChanged:)
                                                          name:@"com.apple.screenIsLocked"
                                                        object:nil];
    [NSDistributedNotificationCenter.defaultCenter addObserver:self
                                                      selector:@selector(powerOrScreenStateChanged:)
                                                          name:@"com.apple.screenIsUnlocked"
                                                        object:nil];

    self.hudBar = [DPPFloatingHudBar new];
    self.hudBar.onSelectMode = ^(dpp::PlayMode pm) {
      [weak guard:^{
        [weak switchToPlayMode:pm];
      }];
    };
    self.hudBar.onTogglePause = ^{
      DPPProtectedMirrorPanel* owner = weak;
      if (!owner) return;
      [owner guard:^{
        if (owner->_activeSession && !owner->_transition) {
            [owner.preview playPause];
            [owner refreshButtons];
        }
      }];
    };
    self.hudBar.onReset = ^{
      DPPProtectedMirrorPanel* owner = weak;
      if (!owner) return;
      [owner guard:^{
        if (owner->_activeSession && !owner->_transition) {
            [owner.preview reset];
            [owner refreshButtons];
        }
      }];
    };
    self.hudBar.onToggleMute = ^{
      [weak guard:^{
        if (!weak) return;
        const BOOL nextMuted = !weak.preview.soundMuted;
        weak.muteToggle.state = nextMuted ? NSControlStateValueOn : NSControlStateValueOff;
        weak.preview.soundMuted = nextMuted;
        [weak saveUserPreferences];
        [weak refreshButtons];
      }];
    };
    self.hudBar.onToggleSettings = ^{
      [weak guard:^{
        [weak toggleSettingsPanel:nil];
      }];
    };
    self.hudBar.onHideCapsule = ^{
      [weak guard:^{
        if (weak.modeSettingsPanel.visible) {
            [weak closeModeSettingsCard:nil];
        }
        [weak rebuildStatusMenu];
      }];
    };
    self.hudBar.onStopSession = ^{
      [weak guard:^{
        [weak finish:@"user_stop"];
      }];
    };
    self.hudBar.onDidMove = ^(NSPoint newOrigin) {
      (void)newOrigin;
      DPPProtectedMirrorPanel* owner = weak;
      if (owner && owner.modeSettingsPanel.visible) {
          [owner repositionModeSettingsCard];
      }
    };
    self.hudBar.onRestorePlaygroundFocus = ^{
      DPPProtectedMirrorPanel* owner = weak;
      if (owner && owner->_activeSession && !owner->_transition && !owner.modeSettingsPanel.visible) {
          [owner.preview ensureInteractiveIfAllowed];
      }
    };
    self.preview.onToggleHudShortcut = ^{
      DPPProtectedMirrorPanel* owner = weak;
      if (owner && owner->_activeSession && !owner->_transition) {
          [owner toggleHudCapsule:nil];
      }
    };

    if (!_syntheticCheckMode) {
        [self loadUserPreferences];
        [self setupStatusBarItem];
    }

    [self refreshButtons];
    activateDPPApp();
    [self.panel makeKeyAndOrderFront:nil];
}

- (void)loadUserPreferences {
    if (_syntheticCheckMode) return;
    NSString* raw = [NSUserDefaults.standardUserDefaults stringForKey:@"dpp.userPrefsV2"];
    if (!raw.length) return;
    _suppressPrefSave = YES;
    const dpp::UserPreferences p = dpp::UserPreferences::parseIni(raw.UTF8String, /*forColdBoot=*/true);
    _savedPlayMode = p.lastPlayMode;
    _savedGravityFilenames = p.showFilenames;
    _displayMode = p.displayMode;
    [self refreshDisplayScopePopup];
    [self syncLauncherModePills];

    self.filenameToggle.state = p.showFilenames ? NSControlStateValueOn : NSControlStateValueOff;
    self.preview.showsFilenames = p.showFilenames;

    self.gravityAccelSlider.doubleValue = p.gravityMagnitude;
    self.gravityRestitutionSlider.doubleValue = p.gravityRestitution;
    [self.gravityDirectionPopup
        selectItemAtIndex:std::clamp<NSInteger>(static_cast<NSInteger>(p.gravityDirection), 0, 3)];
    const dpp::GravityPreset matchedPreset = dpp::matchGravityPreset(p.gravityMagnitude);
    [self.gravityPresetPopup
        selectItemAtIndex:std::clamp<NSInteger>(static_cast<NSInteger>(matchedPreset), 0, 7)];
    [self syncGravityReadoutLabels];
    [self.preview applyGravityMagnitude:p.gravityMagnitude
                              direction:p.gravityDirection
                            restitution:p.gravityRestitution];

    self.muteToggle.state = p.soundMuted ? NSControlStateValueOn : NSControlStateValueOff;
    self.preview.soundMuted = p.soundMuted;

    self.blackHoleSize.doubleValue = p.blackHoleScale;
    [self.preview setToyBlackHoleScale:p.blackHoleScale];
    const double elDeg = p.blackHoleElevation * 180.0 / M_PI;
    const double tiltDeg = p.blackHoleTilt * 180.0 / M_PI;
    self.blackHoleElevation.doubleValue = elDeg;
    self.blackHoleTilt.doubleValue = tiltDeg;
    self.elevationValueLabel.stringValue = [NSString stringWithFormat:@"%+.0f°", elDeg];
    self.tiltValueLabel.stringValue =
        std::abs(tiltDeg) < 0.5 ? @"0°" : [NSString stringWithFormat:@"%+.0f°", tiltDeg];
    [self.preview setToyBlackHoleElevation:p.blackHoleElevation tilt:p.blackHoleTilt];

    [self.rulePopup selectItemAtIndex:std::clamp<NSInteger>(static_cast<NSInteger>(p.billiardsRule), 0, 2)];
    const NSInteger mIdx = p.billiardsMatchMode == dpp::BilliardsMatchMode::VsAi
                               ? 1
                               : (p.billiardsMatchMode == dpp::BilliardsMatchMode::Hotseat
                                      ? 2
                                      : (p.billiardsMatchMode == dpp::BilliardsMatchMode::Lan ? 3 : 0));
    [self.matchPopup selectItemAtIndex:mIdx];
    [self.aiDifficultyPopup
        selectItemAtIndex:std::clamp<NSInteger>(static_cast<NSInteger>(p.billiardsAiDifficulty), 0, 3)];
    [self.iconSelPopup
        selectItemAtIndex:std::clamp<NSInteger>(static_cast<NSInteger>(p.iconSelection), 0, 2)];
    [self.extraIconsPopup
        selectItemAtIndex:std::clamp<NSInteger>(static_cast<NSInteger>(p.extraIcons), 0, 1)];
    self.ballBordersToggle.state = p.showBallBorders ? NSControlStateValueOn : NSControlStateValueOff;
    self.ballNumbersToggle.state = p.showBallNumbers ? NSControlStateValueOn : NSControlStateValueOff;
    [self.tableThemePopup
        selectItemAtIndex:std::clamp<NSInteger>(static_cast<NSInteger>(p.tableTheme), 0, 2)];
    self.aimGuideToggle.state = p.showAimGuide ? NSControlStateValueOn : NSControlStateValueOff;
    self.aiIntentToggle.state = p.showAiIntent ? NSControlStateValueOn : NSControlStateValueOff;

    [self.slingPresetPopup
        selectItemAtIndex:std::clamp<NSInteger>(static_cast<NSInteger>(p.slingshotPreset), 0, 5)];
    [self.slingAmmoPopup selectItemAtIndex:std::clamp<NSInteger>(p.slingshotAmmoCount - 3, 0, 3)];
    self.slingTrajToggle.state = p.slingshotShowTrajectory ? NSControlStateValueOn : NSControlStateValueOff;
    _slingshotProgress = p.slingshotProgress;
    _slingshotCustomPieces = p.slingshotCustomPieces;
    self.preview.slingshotHighScore = p.slingshotHighScore;
    [self.preview applySlingshotProgress:_slingshotProgress];

    [self.hockeyMatchPopup
        selectItemAtIndex:std::clamp<NSInteger>(static_cast<NSInteger>(p.hockeyMatchMode), 0, 2)];
    [self.hockeyDiffPopup
        selectItemAtIndex:std::clamp<NSInteger>(static_cast<NSInteger>(p.hockeyDifficulty), 0, 2)];
    [self.hockeyGoalsPopup
        selectItemAtIndex:(p.hockeyTargetGoals == 5 ? 0 : (p.hockeyTargetGoals == 11 ? 2 : 1))];
    self.hockeyChromeToggle.state = p.hockeyShowRinkChrome ? NSControlStateValueOn : NSControlStateValueOff;

    if (!p.lastLanEndpoint.empty()) {
        NSString* ep = [NSString stringWithUTF8String:p.lastLanEndpoint.c_str()];
        if (ep.length) self.lanAddressField.stringValue = ep;
    }

    [self.preview applyBilliardsConfig:[self currentBilliardsConfig]];
    [self.preview applySlingshotConfig:[self currentSlingshotConfig]];
    [self.preview applyAirHockeyConfig:[self currentAirHockeyConfig]];
    _suppressPrefSave = NO;
}

- (void)saveUserPreferences {
    if (_syntheticCheckMode || _suppressPrefSave) return;
    dpp::UserPreferences p;
    p.lastPlayMode = _activeSession ? [self.preview playMode] : _savedPlayMode;
    p.displayMode = _displayMode;
    p.soundMuted = self.preview.soundMuted;
    const BOOL inArena = _activeSession && (self.preview.billiardsEnabled || self.preview.slingshotEnabled ||
                                            self.preview.airHockeyEnabled);
    p.showFilenames = inArena ? static_cast<bool>(_savedGravityFilenames)
                              : (self.filenameToggle.state == NSControlStateValueOn);
    p.gravityMagnitude = static_cast<float>(self.gravityAccelSlider.doubleValue);
    p.gravityDirection = static_cast<dpp::GravityDirection>(
        std::clamp<NSInteger>(self.gravityDirectionPopup.indexOfSelectedItem, 0, 3));
    p.gravityRestitution = static_cast<float>(self.gravityRestitutionSlider.doubleValue);
    p.blackHoleScale = static_cast<float>(self.blackHoleSize.doubleValue);
    p.blackHoleElevation = static_cast<float>(self.blackHoleElevation.doubleValue * M_PI / 180.0);
    p.blackHoleTilt = static_cast<float>(self.blackHoleTilt.doubleValue * M_PI / 180.0);
    p.fromBilliardsConfig([self currentBilliardsConfig]);
    p.fromSlingshotConfig([self currentSlingshotConfig]);
    p.slingshotProgress = [self.preview slingshotProgress];
    p.slingshotCustomPieces = _slingshotCustomPieces;
    p.slingshotHighScore = std::max(self.preview.slingshotHighScore, p.slingshotProgress.highScore);
    p.fromAirHockeyConfig([self currentAirHockeyConfig]);
    if (self.lanAddressField.stringValue.length) {
        p.lastLanEndpoint = self.lanAddressField.stringValue.UTF8String;
    }
    const std::string text = p.serializeIni();
    if (!text.empty()) {
        NSString* ns = [[NSString alloc] initWithBytes:text.data()
                                                length:text.size()
                                              encoding:NSUTF8StringEncoding];
        if (ns) [NSUserDefaults.standardUserDefaults setObject:ns forKey:@"dpp.userPrefsV2"];
    }
}

- (void)setupStatusBarItem {
    if (self.statusItem) return;
    self.statusItem = [NSStatusBar.systemStatusBar statusItemWithLength:NSVariableStatusItemLength];
    NSStatusBarButton* btn = self.statusItem.button;
    NSImage* sym = [NSImage imageWithSystemSymbolName:@"sparkles.rectangle.stack"
                             accessibilityDescription:@"DeskFling"];
    if (sym) {
        [sym setTemplate:YES];
        btn.image = sym;
        btn.imagePosition = NSImageLeft;
        btn.title = @" DeskFling";
    } else {
        btn.title = @"DeskFling";
    }
    btn.toolTip = @"桌面游乐场 · DeskFling 控制台";
    [self rebuildStatusMenu];
}

- (void)rebuildStatusMenu {
    if (!self.statusItem) return;
    NSMenu* menu = [[NSMenu alloc] initWithTitle:@"DeskFling"];
    menu.autoenablesItems = NO;

    if (!_activeSession) {
        NSMenuItem* qs = [menu addItemWithTitle:@"▶ 开始游玩"
                                         action:@selector(quickStartClicked:)
                                  keyEquivalent:@""];
        qs.target = self;
        qs.enabled = !_loading && !_transition;
        NSMenuItem* showPanel = [menu addItemWithTitle:@"打开启动器窗口…"
                                                action:@selector(showMainPanelFromMenu:)
                                         keyEquivalent:@""];
        showPanel.target = self;

        NSArray<NSScreen*>* screens = NSScreen.screens;
        if (screens.count > 1) {
            NSMenuItem* dispRoot = [menu addItemWithTitle:@"游玩屏幕" action:nil keyEquivalent:@""];
            NSMenu* dispSub = [[NSMenu alloc] initWithTitle:@"游玩屏幕"];
            dispSub.autoenablesItems = NO;
            const NSInteger effIdx =
                (_displayMode < 0)
                    ? -1
                    : std::clamp<NSInteger>(_displayMode, 0, static_cast<NSInteger>(screens.count) - 1);
            for (NSUInteger i = 0; i < screens.count; ++i) {
                NSScreen* scr = screens[i];
                NSString* t =
                    [NSString stringWithFormat:@"屏幕 %lu（%@ · %.0f×%.0f）", static_cast<unsigned long>(i + 1),
                                               (i == 0 ? @"主屏" : @"副屏"), scr.frame.size.width,
                                               scr.frame.size.height];
                NSMenuItem* si = [dispSub addItemWithTitle:t
                                                    action:@selector(statusMenuDisplayScopeSelected:)
                                             keyEquivalent:@""];
                si.target = self;
                si.tag = static_cast<NSInteger>(i);
                si.state = (effIdx == static_cast<NSInteger>(i)) ? NSControlStateValueOn : NSControlStateValueOff;
                si.enabled = !_loading && !_transition;
            }
            NSMenuItem* spanItem =
                [dispSub addItemWithTitle:[NSString stringWithFormat:@"多屏跨屏打通（共 %lu 屏）",
                                                                     static_cast<unsigned long>(screens.count)]
                                   action:@selector(statusMenuDisplayScopeSelected:)
                            keyEquivalent:@""];
            spanItem.target = self;
            spanItem.tag = -1;
            spanItem.state = (effIdx == -1) ? NSControlStateValueOn : NSControlStateValueOff;
            spanItem.enabled = !_loading && !_transition;
            dispRoot.submenu = dispSub;
        }
    } else {
        NSString* hudTitle = self.hudBar.isVisible ? @"隐藏顶部悬浮胶囊条" : @"显示顶部悬浮胶囊条";
        NSMenuItem* toggleHud = [menu addItemWithTitle:hudTitle
                                                action:@selector(toggleHudCapsule:)
                                         keyEquivalent:@""];
        toggleHud.target = self;

        NSString* setTitle = self.modeSettingsPanel.visible ? @"收起当前玩法设置" : @"打开当前玩法专属设置…";
        NSMenuItem* toggleSet = [menu addItemWithTitle:setTitle
                                                action:@selector(toggleSettingsPanel:)
                                         keyEquivalent:@""];
        toggleSet.target = self;

        NSArray<NSScreen*>* screens = NSScreen.screens;
        if (screens.count > 1) {
            NSMenuItem* dispRoot = [menu addItemWithTitle:@"游玩屏幕 / 多屏显示" action:nil keyEquivalent:@""];
            NSMenu* dispSub = [[NSMenu alloc] initWithTitle:@"游玩屏幕"];
            dispSub.autoenablesItems = NO;
            const NSInteger effIdx =
                (_displayMode < 0)
                    ? -1
                    : std::clamp<NSInteger>(_displayMode, 0, static_cast<NSInteger>(screens.count) - 1);
            for (NSUInteger i = 0; i < screens.count; ++i) {
                NSScreen* scr = screens[i];
                NSString* t =
                    [NSString stringWithFormat:@"仅屏幕 %lu（%@ · %.0f×%.0f）",
                                               static_cast<unsigned long>(i + 1), (i == 0 ? @"主屏" : @"副屏"),
                                               scr.frame.size.width, scr.frame.size.height];
                NSMenuItem* si = [dispSub addItemWithTitle:t
                                                    action:@selector(statusMenuDisplayScopeSelected:)
                                             keyEquivalent:@""];
                si.target = self;
                si.tag = static_cast<NSInteger>(i);
                si.state = (effIdx == static_cast<NSInteger>(i)) ? NSControlStateValueOn : NSControlStateValueOff;
                si.enabled = !_loading && !_transition;
            }
            NSMenuItem* spanItem =
                [dispSub addItemWithTitle:[NSString stringWithFormat:@"🌐 多屏跨屏显示（共 %lu 屏铺满）",
                                                                     static_cast<unsigned long>(screens.count)]
                                   action:@selector(statusMenuDisplayScopeSelected:)
                            keyEquivalent:@""];
            spanItem.target = self;
            spanItem.tag = -1;
            spanItem.state = (effIdx == -1) ? NSControlStateValueOn : NSControlStateValueOff;
            spanItem.enabled = !_loading && !_transition;
            dispRoot.submenu = dispSub;
        }

        [menu addItem:NSMenuItem.separatorItem];

        const auto pm = [self.preview playMode];
        const auto cfg = [self currentBilliardsConfig];
        const BOOL canBilliards = self.preview.billiardsEnabled ||
                                  [self.preview canEnableBilliardsWithConfig:cfg] ||
                                  self.preview.canEnableAnyBilliards;
        NSArray<NSString*>* names =
            @[ @"普通重力", @"玩具黑洞 (3D)", @"桌面台球", @"弹弓攻城", @"空气曲棍球" ];
        for (NSInteger i = 0; i < 5; ++i) {
            NSMenuItem* mi = [menu addItemWithTitle:names[static_cast<NSUInteger>(i)]
                                             action:@selector(statusMenuModeSelected:)
                                      keyEquivalent:@""];
            mi.target = self;
            mi.tag = i;
            mi.state = (static_cast<NSInteger>(pm) == i) ? NSControlStateValueOn : NSControlStateValueOff;
            mi.enabled = !_transition && (i != 2 || canBilliards);
        }

        [menu addItem:NSMenuItem.separatorItem];

        const BOOL paused = [[self.preview diagnostics][@"user_paused"] boolValue];
        NSMenuItem* pauseItem = [menu addItemWithTitle:(paused ? @"继续物理运行" : @"暂停物理运行")
                                                action:@selector(play:)
                                         keyEquivalent:@""];
        pauseItem.target = self;

        NSMenuItem* resetItem = [menu addItemWithTitle:@"重置当前场景"
                                                action:@selector(resetScene:)
                                         keyEquivalent:@""];
        resetItem.target = self;

        NSMenuItem* muteItem = [menu addItemWithTitle:@"静音合成音效"
                                               action:@selector(toggleMuteFromMenu:)
                                        keyEquivalent:@""];
        muteItem.target = self;
        muteItem.state = self.preview.soundMuted ? NSControlStateValueOn : NSControlStateValueOff;

        [menu addItem:NSMenuItem.separatorItem];

        NSMenuItem* stopItem = [menu addItemWithTitle:@"立即恢复原桌面并停止 (Esc)"
                                               action:@selector(stopSession:)
                                        keyEquivalent:@""];
        stopItem.target = self;

        NSMenuItem* savePosItem = [menu addItemWithTitle:@"保存当前图标布局到桌面并还原…"
                                                  action:@selector(saveLayoutWritebackClicked:)
                                           keyEquivalent:@""];
        savePosItem.target = self;
        savePosItem.enabled = !_transition;
    }

    NSArray* savedBackup =
        [NSUserDefaults.standardUserDefaults arrayForKey:kSavedLayoutBackupDefaultsKey];
    if ([savedBackup isKindOfClass:NSArray.class] && savedBackup.count > 0 && !_activeSession) {
        NSMenuItem* undoPosItem = [menu addItemWithTitle:@"撤销上次写回（恢复原桌面坐标）"
                                                  action:@selector(undoLayoutWritebackClicked:)
                                           keyEquivalent:@""];
        undoPosItem.target = self;
        undoPosItem.enabled = !_loading && !_transition;
    }

    [menu addItem:NSMenuItem.separatorItem];
    NSMenuItem* repick = [menu addItemWithTitle:@"重新选择桌面目录…"
                                         action:@selector(repickDesktopFromMenu:)
                                  keyEquivalent:@""];
    repick.target = self;
    repick.enabled = !_loading && !_transition;

    if (!_activeSession) {
        NSMenuItem* authFinder = [menu addItemWithTitle:@"🔑 授权访达(Finder)自动化权限…"
                                                 action:@selector(authorizeFinderFromMenu:)
                                          keyEquivalent:@""];
        authFinder.target = self;
        authFinder.enabled = !_loading && !_transition;

        NSMenuItem* emergencyRecover = [menu addItemWithTitle:@"🛠 紧急恢复原桌面（检查并修复残留）"
                                                       action:@selector(recoverResidualFromMenu:)
                                                keyEquivalent:@""];
        emergencyRecover.target = self;
        emergencyRecover.enabled = !_loading && !_transition;
    }

    NSMenuItem* openLogs = [menu addItemWithTitle:@"📂 打开运行日志目录…"
                                           action:@selector(openLogDirectoryFromMenu:)
                                    keyEquivalent:@""];
    openLogs.target = self;

    [menu addItem:NSMenuItem.separatorItem];
    NSMenuItem* quitItem = [menu addItemWithTitle:@"退出 DPP（自动还原原桌面）"
                                           action:@selector(quitAppFromMenu:)
                                    keyEquivalent:@"q"];
    quitItem.target = self;

    self.statusItem.menu = menu;
}

- (void)authorizeFinderFromMenu:(id)sender {
    (void)sender;
    if (_activeSession || _loading || _transition) return;
    [self guard:^{
      activateDPPApp();
      NSAlert* alert = [[NSAlert alloc] init];
      alert.alertStyle = NSAlertStyleInformational;
      alert.messageText = @"请求访达 (Finder) 自动化控制权限";
      alert.informativeText =
          @"点击“继续请求授权”后，macOS 将弹出系统权限确认框（用于读取桌面图标坐标并在会话结束时恢复原桌面）。本步骤不会退出访达，也不会修改任何桌面文件。";
      [alert addButtonWithTitle:@"继续请求授权"];
      [alert addButtonWithTitle:@"取消"];
      self->_modalAlertActive = YES;
      const NSModalResponse resp = [alert runModal];
      self->_modalAlertActive = NO;
      if (resp != NSAlertFirstButtonReturn) return;
      const int rc = requestFinderAuthorizationFromUI();
      if (rc == 0) {
          self.status.stringValue = @"访达自动化权限已就绪！现在可直接点击「⚡ 一键进入桌面游乐场」。";
      } else {
          self.status.stringValue =
              @"未授予访达自动化权限：请前往「系统设置 → 隐私与安全性 → 自动化」勾选允许控制 Finder。";
      }
      [self refreshButtons];
    }];
}

- (void)recoverResidualFromMenu:(id)sender {
    (void)sender;
    if (_activeSession || _loading || _transition) return;
    [self guard:^{
      const bool ok = recoverProtectedMirrorJournalFromUI();
      self.status.stringValue = ok ? @"紧急恢复检查完成：原桌面显示状态已确认恢复正常。"
                                   : @"紧急恢复未完成，请打开运行日志目录查看详情或运行 恢复桌面.command。";
      [self refreshButtons];
    }];
}

- (void)openLogDirectoryFromMenu:(id)sender {
    (void)sender;
    NSString* logDir = dppAppLogDirectoryPath();
    [NSFileManager.defaultManager createDirectoryAtPath:logDir
                            withIntermediateDirectories:YES
                                             attributes:@{NSFilePosixPermissions : @0700}
                                                  error:nil];
    [NSWorkspace.sharedWorkspace openURL:[NSURL fileURLWithPath:logDir isDirectory:YES]];
}

- (void)statusMenuDisplayScopeSelected:(NSMenuItem*)sender {
    if (_loading || _transition) return;
    [self applyDisplayModeSelection:static_cast<int>(sender.tag)];
}

- (BOOL)applicationShouldHandleReopen:(NSApplication*)sender hasVisibleWindows:(BOOL)flag {
    (void)sender;
    (void)flag;
    if (_activeSession) {
        const NSRect hudVis = (_hudWorkArea.size.width > 0) ? _hudWorkArea : _workArea;
        if (!self.hudBar.isVisible) {
            [self.hudBar showInWorkArea:hudVis];
        }
        if (!self.modeSettingsPanel.visible) {
            [self toggleSettingsPanel:nil];
        }
    } else {
        [self showMainPanelFromMenu:nil];
    }
    return YES;
}

- (void)showMainPanelFromMenu:(id)sender {
    (void)sender;
    activateDPPApp();
    [self.panel makeKeyAndOrderFront:nil];
    [self refreshButtons];
}
- (void)toggleHudCapsule:(id)sender {
    (void)sender;
    if (!_activeSession) return;
    if (self.hudBar.isVisible) {
        [self.hudBar hide];
        if (self.modeSettingsPanel.visible) {
            [self closeModeSettingsCard:nil];
        }
    } else {
        const NSRect hudVis = (_hudWorkArea.size.width > 0) ? _hudWorkArea : _workArea;
        [self.hudBar showInWorkArea:hudVis];
    }
    if (!self.modeSettingsPanel.visible) {
        [self.preview ensureInteractiveIfAllowed];
    }
    [self rebuildStatusMenu];
}
- (void)toggleSettingsPanel:(id)sender {
    (void)sender;
    if (!_activeSession) {
        activateDPPApp();
        [self.panel makeKeyAndOrderFront:nil];
        [self refreshButtons];
        return;
    }
    if (self.modeSettingsPanel.visible) {
        [self.modeSettingsPanel orderOut:nil];
        [self.preview ensureInteractiveIfAllowed];
    } else {
        [self layoutModeSettingsForMode:[self.preview playMode]];
        [self repositionModeSettingsCard];
        [self.modeSettingsPanel orderFrontRegardless];
        [self.modeSettingsPanel invalidateShadow];
    }
    [self refreshButtons];
}
- (void)statusMenuModeSelected:(NSMenuItem*)sender {
    const NSInteger tag = sender.tag;
    const dpp::PlayMode pm =
        tag == 1 ? dpp::PlayMode::BlackHole
                 : (tag == 2 ? dpp::PlayMode::Billiards
                             : (tag == 3 ? dpp::PlayMode::Slingshot
                                         : (tag == 4 ? dpp::PlayMode::AirHockey : dpp::PlayMode::Gravity)));
    [self guard:^{
      [self switchToPlayMode:pm];
    }];
}
- (void)toggleMuteFromMenu:(id)sender {
    (void)sender;
    [self guard:^{
      const BOOL nextMuted = !self.preview.soundMuted;
      self.muteToggle.state = nextMuted ? NSControlStateValueOn : NSControlStateValueOff;
      self.preview.soundMuted = nextMuted;
      [self saveUserPreferences];
      [self refreshButtons];
    }];
}
- (void)repickDesktopFromMenu:(id)sender {
    (void)sender;
    [self guard:^{
      [NSUserDefaults.standardUserDefaults removeObjectForKey:@"dpp.desktopBookmarkV1"];
      if (self->_activeSession) {
          if (![self finish:@"repick_desktop"]) return;
      }
      activateDPPApp();
      [self.panel makeKeyAndOrderFront:nil];
      self->_quickAutoStart = YES;
      [self readAll:nil];
    }];
}
- (void)quitAppFromMenu:(id)sender {
    (void)sender;
    [NSApp terminate:nil];
}
- (void)guard:(void (^)(void))action {
    @try {
        try {
            action();
        } catch (const std::exception& error) {
            const bool attempted = self->_protectedSession != nullptr;
            self->_transition = NO;
            const BOOL restored = [self finish:@"native_failure"];
            self.status.stringValue = mirrorFailureMessage(error.what(), attempted, restored);
            std::fprintf(stderr, "mirror_error=%s takeover_attempted=%s recovery_completed=%s\n",
                         error.what(), attempted ? "true" : "false", restored ? "true" : "false");
        }
    } @catch (NSException*) {
        const bool attempted = self->_protectedSession != nullptr;
        self->_transition = NO;
        const BOOL restored = [self finish:@"native_exception"];
        self.status.stringValue = !attempted ? @"读取/准备时发生异常，尚未隐藏原图标，无需恢复。"
                                             : (restored ? @"原生调用异常；程序恢复已完成，请检查桌面。"
                                                         : @"恢复未完成，请使用同目录独立恢复入口。");
    }
}
- (BOOL)displayMatches {
    if (_activeSession && self.preview.active) return [self.preview checkDisplayForMigration];
    if (_allDisplaysSnapshot.size() > 1) {
        NSArray<NSScreen*>* screens = NSScreen.screens;
        if (screens.count != _allDisplaysSnapshot.size()) return NO;
        for (NSUInteger i = 0; i < screens.count; ++i) {
            const auto cur = mirrorDisplayAt(i);
            const auto& exp = _allDisplaysSnapshot[i];
            if (!validMirrorDisplayAnyCount(cur) || ![cur.id isEqual:exp.id] ||
                std::abs(cur.frame.size.width - exp.frame.size.width) > .001 ||
                std::abs(cur.frame.size.height - exp.frame.size.height) > .001 ||
                std::abs(cur.scale - exp.scale) > .001 ||
                std::abs(cur.work.size.width - exp.work.size.width) > 96.0 ||
                std::abs(cur.work.size.height - exp.work.size.height) > 96.0) {
                return NO;
            }
        }
        return !self.preview.active || [self.preview fixedSceneGeometryValid];
    }
    const auto current = mirrorDisplay();
    const bool compatible =
        mirrorDisplayCompatible(_displaySnapshot, current, [self.preview fixedSceneGeometryValid]) &&
        (!self.preview.active || [self.preview fixedSceneGeometryValid]);
    NSMutableDictionary* context =
        _protectedSession ? [_protectedSession->diagnosticIdentity() mutableCopy] : [NSMutableDictionary new];
    context[@"source"] = @"panel_display_recheck";
    context[@"phase"] = _activeSession ? @"protected_active" : @"preparation";
    logDisplayDecision(_displaySnapshot, current, self.panel, context, compatible);
    if (!compatible) logMirrorDisplay(_displaySnapshot, current, false);
    return compatible;
}
- (dpp::BilliardsConfig)currentBilliardsConfig {
    dpp::BilliardsConfig cfg = dpp::BilliardsConfig::desktop();
    const NSInteger rIdx = self.rulePopup.indexOfSelectedItem;
    cfg.rule = rIdx == 1 ? dpp::BilliardsRule::EightBall
                         : (rIdx == 2 ? dpp::BilliardsRule::Snooker : dpp::BilliardsRule::Free);
    const NSInteger mIdx = self.matchPopup.indexOfSelectedItem;
    cfg.matchMode =
        mIdx == 1 ? dpp::BilliardsMatchMode::VsAi
                  : (mIdx == 2 ? dpp::BilliardsMatchMode::Hotseat
                               : (mIdx == 3 ? dpp::BilliardsMatchMode::Lan : dpp::BilliardsMatchMode::Solo));
    const NSInteger dIdx = self.aiDifficultyPopup.indexOfSelectedItem;
    cfg.aiDifficulty = dIdx == 0 ? dpp::BilliardsAiDifficulty::Easy
                                 : (dIdx == 2 ? dpp::BilliardsAiDifficulty::Master
                                              : (dIdx == 3 ? dpp::BilliardsAiDifficulty::Goat
                                                           : dpp::BilliardsAiDifficulty::Medium));
    const NSInteger iIdx = self.iconSelPopup.indexOfSelectedItem;
    cfg.iconSelection =
        iIdx == 1 ? dpp::IconSelectionPolicy::Random
                  : (iIdx == 2 ? dpp::IconSelectionPolicy::Custom : dpp::IconSelectionPolicy::FirstN);
    cfg.extraIcons = self.extraIconsPopup.indexOfSelectedItem == 1 ? dpp::ExtraIconsPolicy::BottomGravity
                                                                   : dpp::ExtraIconsPolicy::Hide;
    cfg.showBallBorders = self.ballBordersToggle.state == NSControlStateValueOn;
    cfg.showBallNumbers = self.ballNumbersToggle.state == NSControlStateValueOn;
    const NSInteger tIdx = self.tableThemePopup.indexOfSelectedItem;
    cfg.tableTheme =
        tIdx == 1 ? dpp::BilliardsTableTheme::Minimal
                  : (tIdx == 2 ? dpp::BilliardsTableTheme::Borderless : dpp::BilliardsTableTheme::Full);
    cfg.showTableChrome = (cfg.tableTheme == dpp::BilliardsTableTheme::Full);
    cfg.showAimGuide = self.aimGuideToggle.state == NSControlStateValueOn;
    cfg.showAiIntent = self.aiIntentToggle.state == NSControlStateValueOn;
    cfg.randomSeed = _randomSeed;
    cfg.freeModeMaxRackBalls = (cfg.rule == dpp::BilliardsRule::Free) ? 21u : 0u;
    cfg.customIconIndices = _customIconIndices;
    return cfg;
}
- (dpp::SlingshotConfig)currentSlingshotConfig {
    dpp::SlingshotConfig cfg;
    const NSInteger pIdx = self.slingPresetPopup.indexOfSelectedItem;
    cfg.preset = static_cast<dpp::SlingshotPreset>(std::clamp<NSInteger>(pIdx, 0, 5));
    cfg.ammoCount =
        3 + static_cast<int>(std::clamp<NSInteger>(self.slingAmmoPopup.indexOfSelectedItem, 0, 3));
    cfg.showTrajectory = self.slingTrajToggle.state == NSControlStateValueOn;
    const NSInteger iIdx = self.iconSelPopup.indexOfSelectedItem;
    cfg.iconSelection =
        iIdx == 1 ? dpp::IconSelectionPolicy::Random
                  : (iIdx == 2 ? dpp::IconSelectionPolicy::Custom : dpp::IconSelectionPolicy::FirstN);
    cfg.randomSeed = _randomSeed;
    cfg.customIconIndices = _customIconIndices;
    cfg.customPieces = _slingshotCustomPieces;
    return cfg;
}
- (dpp::AirHockeyConfig)currentAirHockeyConfig {
    dpp::AirHockeyConfig cfg;
    const NSInteger mIdx = self.hockeyMatchPopup.indexOfSelectedItem;
    cfg.matchMode = mIdx == 1 ? dpp::AirHockeyMatchMode::Hotseat2P
                              : (mIdx == 2 ? dpp::AirHockeyMatchMode::Lan : dpp::AirHockeyMatchMode::SoloAI);
    const NSInteger dIdx = self.hockeyDiffPopup.indexOfSelectedItem;
    cfg.difficulty =
        dIdx == 0 ? dpp::AirHockeyDifficulty::Casual
                  : (dIdx == 2 ? dpp::AirHockeyDifficulty::Master : dpp::AirHockeyDifficulty::Standard);
    const NSInteger gIdx = self.hockeyGoalsPopup.indexOfSelectedItem;
    cfg.targetGoals = gIdx == 0 ? 5 : (gIdx == 2 ? 11 : 7);
    cfg.showRinkChrome = self.hockeyChromeToggle.state == NSControlStateValueOn;
    cfg.showSpectators = self.extraIconsPopup.indexOfSelectedItem == 1;
    return cfg;
}
- (void)switchToPlayMode:(dpp::PlayMode)pm {
    if (!_activeSession || _transition) {
        [self.playModePopup selectItemAtIndex:0];
        [self refreshButtons];
        return;
    }
    if (self.preview.active && [self.preview playMode] == pm) {
        activateDPPApp();
        [self.preview ensureInteractiveIfAllowed];
        [self refreshButtons];
        return;
    }
    const BOOL wasArena =
        self.preview.billiardsEnabled || self.preview.slingshotEnabled || self.preview.airHockeyEnabled;
    if (pm == dpp::PlayMode::Billiards) {
        const auto cfg = [self currentBilliardsConfig];
        [self.preview applyBilliardsConfig:cfg];
        if (![self.preview canEnableBilliardsWithConfig:cfg] && !self.preview.canEnableAnyBilliards) {
            self.status.stringValue = @"当前工作区尺寸过小，无法容纳标准台球桌。";
            [self refreshButtons];
            return;
        }
        if (!wasArena) {
            _savedGravityFilenames = self.filenameToggle.state == NSControlStateValueOn;
            self.filenameToggle.state = NSControlStateValueOff;
            self.preview.showsFilenames = NO;
        }
        [self.preview setPlayMode:dpp::PlayMode::Billiards];
        [self.rulePopup
            selectItemAtIndex:std::clamp<NSInteger>(static_cast<NSInteger>(self.preview.billiardsConfig.rule),
                                                    0, 2)];
        self.status.stringValue =
            @"台球模式运行中：移动鼠标瞄准，按住往后拉调力度，左上角白球调加塞；右键图标球可打开原件。";
    } else if (pm == dpp::PlayMode::Slingshot) {
        if (!wasArena) {
            _savedGravityFilenames = self.filenameToggle.state == NSControlStateValueOn;
            self.filenameToggle.state = NSControlStateValueOff;
            self.preview.showsFilenames = NO;
        }
        [self.preview applySlingshotConfig:[self currentSlingshotConfig]];
        [self.preview setPlayMode:dpp::PlayMode::Slingshot];
        self.status.stringValue =
            @"弹弓攻城运行中：按住左侧弹兜后拉发射，空中再次点击触发二段冲刺，轰塌右侧城堡！";
    } else if (pm == dpp::PlayMode::AirHockey) {
        if (!wasArena) {
            _savedGravityFilenames = self.filenameToggle.state == NSControlStateValueOn;
            self.filenameToggle.state = NSControlStateValueOff;
            self.preview.showsFilenames = NO;
        }
        [self.preview applyAirHockeyConfig:[self currentAirHockeyConfig]];
        [self.preview setPlayMode:dpp::PlayMode::AirHockey];
        self.status.stringValue =
            @"空气曲棍球运行中：移动鼠标握持左半场球槌直接抽射；双人模式右方可用 WASD / 方向键控制。";
    } else if (pm == dpp::PlayMode::BlackHole) {
        if (wasArena) {
            [self.preview setPlayMode:dpp::PlayMode::Gravity];
            self.filenameToggle.state =
                _savedGravityFilenames ? NSControlStateValueOn : NSControlStateValueOff;
            self.preview.showsFilenames = _savedGravityFilenames;
        }
        [self.preview setPlayMode:dpp::PlayMode::BlackHole];
        self.status.stringValue = @"玩具黑洞运行中：可拖拽视界边缘移动，按住 Option 拖拽旋转三维视角。";
    } else {
        [self.preview setPlayMode:dpp::PlayMode::Gravity];
        if (wasArena) {
            self.filenameToggle.state =
                _savedGravityFilenames ? NSControlStateValueOn : NSControlStateValueOff;
            self.preview.showsFilenames = _savedGravityFilenames;
            self.status.stringValue = @"已切回普通重力模式：所有副本从当前位置自由坠落。";
        } else {
            self.status.stringValue = @"普通重力模式运行中：直接拖拽图标副本，右键可打开原件。";
        }
    }
    _savedPlayMode = [self.preview playMode];
    [self saveUserPreferences];
    activateDPPApp();
    [self.preview ensureInteractiveIfAllowed];
    if (self.modeSettingsPanel && self.modeSettingsPanel.visible) {
        [self layoutModeSettingsForMode:[self.preview playMode]];
    }
    [self refreshButtons];
}
- (void)playModeChanged:(id)sender {
    (void)sender;
    [self guard:^{
      const NSInteger idx = self.playModePopup.indexOfSelectedItem;
      const dpp::PlayMode pm =
          idx == 1 ? dpp::PlayMode::BlackHole
                   : (idx == 2 ? dpp::PlayMode::Billiards
                               : (idx == 3 ? dpp::PlayMode::Slingshot
                                           : (idx == 4 ? dpp::PlayMode::AirHockey : dpp::PlayMode::Gravity)));
      [self switchToPlayMode:pm];
    }];
}
- (void)syncGravityReadoutLabels {
    const double g = std::clamp<double>(self.gravityAccelSlider.doubleValue, dpp::kMinGravityMps2,
                                        dpp::kMaxGravityMps2);
    const double gRatio = g / static_cast<double>(dpp::kEarthGravityMps2);
    self.gravityAccelValueLabel.stringValue =
        [NSString stringWithFormat:@"%.2f m/s² (%.2fg)", g, gRatio];
    const double e = std::clamp<double>(self.gravityRestitutionSlider.doubleValue,
                                        dpp::kMinGravityRestitution, dpp::kMaxGravityRestitution);
    self.gravityRestitutionValueLabel.stringValue = [NSString stringWithFormat:@"e = %.2f", e];
}
- (void)applyGravityControlsToPreview {
    const float g = static_cast<float>(self.gravityAccelSlider.doubleValue);
    const auto dir = static_cast<dpp::GravityDirection>(
        std::clamp<NSInteger>(self.gravityDirectionPopup.indexOfSelectedItem, 0, 3));
    const float e = static_cast<float>(self.gravityRestitutionSlider.doubleValue);
    [self syncGravityReadoutLabels];
    [self.preview applyGravityMagnitude:g direction:dir restitution:e];
    [self saveUserPreferences];
}
- (void)gravityPresetChanged:(id)sender {
    (void)sender;
    [self guard:^{
      const NSInteger idx = std::clamp<NSInteger>(self.gravityPresetPopup.indexOfSelectedItem, 0, 7);
      const auto preset = static_cast<dpp::GravityPreset>(idx);
      if (preset != dpp::GravityPreset::Custom) {
          self.gravityAccelSlider.doubleValue = dpp::gravityPresetMps2(preset);
      }
      [self applyGravityControlsToPreview];
    }];
}
- (void)gravitySliderChanged:(id)sender {
    (void)sender;
    [self guard:^{
      const float g = static_cast<float>(self.gravityAccelSlider.doubleValue);
      const auto matched = dpp::matchGravityPreset(g);
      [self.gravityPresetPopup
          selectItemAtIndex:std::clamp<NSInteger>(static_cast<NSInteger>(matched), 0, 7)];
      [self applyGravityControlsToPreview];
    }];
}
- (void)gravityOptionsChanged:(id)sender {
    (void)sender;
    [self guard:^{
      [self applyGravityControlsToPreview];
    }];
}
- (void)resetEarthGravity:(id)sender {
    (void)sender;
    [self guard:^{
      [self.gravityPresetPopup selectItemAtIndex:static_cast<NSInteger>(dpp::GravityPreset::Earth)];
      [self.gravityDirectionPopup selectItemAtIndex:0];
      self.gravityAccelSlider.doubleValue = dpp::kEarthGravityMps2;
      self.gravityRestitutionSlider.doubleValue = dpp::kDefaultGravityRestitution;
      [self applyGravityControlsToPreview];
    }];
}
- (void)slingshotOptionsChanged:(id)sender {
    (void)sender;
    [self guard:^{
      [self.preview applySlingshotConfig:[self currentSlingshotConfig]];
      [self saveUserPreferences];
      [self refreshButtons];
    }];
}
- (void)airHockeyOptionsChanged:(id)sender {
    (void)sender;
    [self guard:^{
      const auto cfg = [self currentAirHockeyConfig];
      [self.preview applyAirHockeyConfig:cfg];
      if (cfg.matchMode == dpp::AirHockeyMatchMode::Lan && !self.preview.lanActive) {
          (void)[self.preview startLanDiscovery];
          self.lanStatusLabel.stringValue = @"正在自动搜索局域网房间… 也可点“建房”或“搜房加入”";
      } else if (cfg.matchMode != dpp::AirHockeyMatchMode::Lan &&
                 self.matchPopup.indexOfSelectedItem != 3) {
          [self.preview stopLanDiscovery];
      }
      [self saveUserPreferences];
      if (self.modeSettingsPanel && self.modeSettingsPanel.visible) {
          [self layoutModeSettingsForMode:[self.preview playMode]];
      }
      [self refreshButtons];
    }];
}
- (void)billiardsOptionsChanged:(id)sender {
    (void)sender;
    [self guard:^{
      const auto cfg = [self currentBilliardsConfig];
      [self.preview applyBilliardsConfig:cfg];
      if (cfg.matchMode != dpp::BilliardsMatchMode::Lan) {
          if (self.hockeyMatchPopup.indexOfSelectedItem != 2) {
              [self.preview stopLanDiscovery];
          }
          self.lanStatusLabel.stringValue = @"局域网未开启";
      } else if (!self.preview.lanActive) {
          (void)[self.preview startLanDiscovery];
          self.lanStatusLabel.stringValue = @"正在自动搜索局域网房间… 也可点“建房”或“搜房加入”";
      }
      [self saveUserPreferences];
      if (self.modeSettingsPanel && self.modeSettingsPanel.visible) {
          [self layoutModeSettingsForMode:[self.preview playMode]];
      }
      [self refreshButtons];
    }];
}
- (void)iconSelectionChanged:(id)sender {
    (void)sender;
    [self guard:^{
      NSArray<NSString*>* curLabels = self.activeLabels.count ? self.activeLabels : self.labels;
      if (self.iconSelPopup.indexOfSelectedItem == 1) {
          self->_randomSeed = arc4random();
      } else if (self.iconSelPopup.indexOfSelectedItem == 2 && self->_customIconIndices.empty() &&
                 curLabels.count > 0) {
          const NSUInteger defCount = std::min<NSUInteger>(curLabels.count, 15u);
          for (NSUInteger i = 0; i < defCount; ++i) self->_customIconIndices.push_back(i);
      }
      if (self.preview.slingshotEnabled) {
          [self slingshotOptionsChanged:nil];
      } else {
          [self billiardsOptionsChanged:nil];
      }
      if (self.modeSettingsPanel && self.modeSettingsPanel.visible) {
          [self layoutModeSettingsForMode:[self.preview playMode]];
      }
      if (self.iconSelPopup.indexOfSelectedItem == 2 && curLabels.count > 0 && !self.iconPickerSheet) {
          [self pickIcons:nil];
      }
    }];
}
- (void)pickIcons:(id)sender {
    (void)sender;
    [self guard:^{
      NSArray<NSString*>* curLabels = self.activeLabels.count ? self.activeLabels : self.labels;
      if (!curLabels.count || self->_loading || self->_transition || self.desktopChooser ||
          self.iconPickerSheet)
          return;
      NSPanel* sheet = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 440, 360)
                                                  styleMask:NSWindowStyleMaskTitled
                                                    backing:NSBackingStoreBuffered
                                                      defer:NO];
      sheet.releasedWhenClosed = NO;
      NSTextField* header =
          [NSTextField labelWithString:@"勾选要作为台球上桌的桌面项目（不足规则球数将自动用标准彩球补齐）："];
      header.frame = NSMakeRect(16, 326, 408, 20);
      [sheet.contentView addSubview:header];

      NSScrollView* scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(16, 54, 408, 264)];
      scroll.hasVerticalScroller = YES;
      scroll.borderType = NSBezelBorder;
      const CGFloat rowH = 24.0;
      const CGFloat docH = std::max<CGFloat>(260.0, static_cast<CGFloat>(curLabels.count) * rowH + 8.0);
      NSView* doc = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 386, docH)];
      NSMutableArray<NSButton*>* boxes = [NSMutableArray arrayWithCapacity:curLabels.count];
      for (NSUInteger i = 0; i < curLabels.count; ++i) {
          const bool checked = std::find(self->_customIconIndices.begin(), self->_customIconIndices.end(),
                                         static_cast<std::size_t>(i)) != self->_customIconIndices.end();
          NSString* title = [NSString stringWithFormat:@"%lu. %@", i + 1, curLabels[i]];
          NSButton* cb = [NSButton checkboxWithTitle:title target:nil action:nil];
          cb.state = checked ? NSControlStateValueOn : NSControlStateValueOff;
          cb.lineBreakMode = NSLineBreakByTruncatingTail;
          cb.frame = NSMakeRect(8, docH - static_cast<CGFloat>(i + 1) * rowH - 2.0, 370, rowH);
          [doc addSubview:cb];
          [boxes addObject:cb];
      }
      scroll.documentView = doc;
      [sheet.contentView addSubview:scroll];
      self.iconCheckboxes = boxes;
      self.iconPickerSheet = sheet;

      NSButton* allBtn = [NSButton buttonWithTitle:@"全选"
                                            target:self
                                            action:@selector(selectAllCustomIcons:)];
      allBtn.frame = NSMakeRect(16, 14, 72, 28);
      [sheet.contentView addSubview:allBtn];

      NSButton* clearBtn = [NSButton buttonWithTitle:@"清空"
                                              target:self
                                              action:@selector(clearAllCustomIcons:)];
      clearBtn.frame = NSMakeRect(94, 14, 72, 28);
      [sheet.contentView addSubview:clearBtn];

      NSButton* cancelBtn = [NSButton buttonWithTitle:@"取消"
                                               target:self
                                               action:@selector(cancelCustomIcons:)];
      cancelBtn.frame = NSMakeRect(256, 14, 80, 28);
      [sheet.contentView addSubview:cancelBtn];

      NSButton* okBtn = [NSButton buttonWithTitle:@"应用所选"
                                           target:self
                                           action:@selector(confirmCustomIcons:)];
      okBtn.keyEquivalent = @"\r";
      okBtn.frame = NSMakeRect(342, 14, 86, 28);
      [sheet.contentView addSubview:okBtn];

      [self refreshButtons];
      NSWindow* hostWin =
          (self.modeSettingsPanel && self.modeSettingsPanel.visible) ? self.modeSettingsPanel : self.panel;
      if (hostWin.visible) {
          [hostWin beginSheet:sheet completionHandler:nil];
      } else {
          [sheet center];
          [sheet makeKeyAndOrderFront:nil];
      }
    }];
}
- (void)selectAllCustomIcons:(id)sender {
    (void)sender;
    for (NSButton* cb in self.iconCheckboxes) cb.state = NSControlStateValueOn;
}
- (void)clearAllCustomIcons:(id)sender {
    (void)sender;
    for (NSButton* cb in self.iconCheckboxes) cb.state = NSControlStateValueOff;
}
- (void)cancelCustomIcons:(id)sender {
    (void)sender;
    if (!self.iconPickerSheet) return;
    NSPanel* sheet = self.iconPickerSheet;
    self.iconPickerSheet = nil;
    self.iconCheckboxes = nil;
    if (sheet.sheetParent) [sheet.sheetParent endSheet:sheet returnCode:NSModalResponseCancel];
    [sheet orderOut:nil];
    [self refreshButtons];
}
- (void)confirmCustomIcons:(id)sender {
    (void)sender;
    if (!self.iconPickerSheet) return;
    _customIconIndices.clear();
    for (NSUInteger i = 0; i < self.iconCheckboxes.count; ++i) {
        if (self.iconCheckboxes[i].state == NSControlStateValueOn) {
            _customIconIndices.push_back(static_cast<std::size_t>(i));
        }
    }
    NSPanel* sheet = self.iconPickerSheet;
    self.iconPickerSheet = nil;
    self.iconCheckboxes = nil;
    if (sheet.sheetParent) [sheet.sheetParent endSheet:sheet returnCode:NSModalResponseOK];
    [sheet orderOut:nil];
    [self.iconSelPopup selectItemAtIndex:2];
    if (self.preview.slingshotEnabled) {
        [self slingshotOptionsChanged:nil];
    } else {
        [self billiardsOptionsChanged:nil];
    }
}
- (void)lanHostClicked:(id)sender {
    (void)sender;
    [self guard:^{
      if (self.preview.airHockeyEnabled) {
          [self.hockeyMatchPopup selectItemAtIndex:2];
          [self airHockeyOptionsChanged:nil];
      } else {
          [self.matchPopup selectItemAtIndex:3];
          [self billiardsOptionsChanged:nil];
      }
      const std::string ip = BilliardsLan::primaryLanIPv4();
      self.lanAddressField.stringValue =
          [NSString stringWithFormat:@"%s:%u", ip.c_str(), BilliardsLan::kDefaultPort];
      if (![self.preview startLanHost:BilliardsLan::kDefaultPort]) {
          self.lanStatusLabel.stringValue = [self.preview lanStatusString];
      }
      [self refreshButtons];
    }];
}
- (void)lanScanJoinClicked:(id)sender {
    (void)sender;
    [self guard:^{
      if (self.preview.airHockeyEnabled) {
          [self.hockeyMatchPopup selectItemAtIndex:2];
          [self airHockeyOptionsChanged:nil];
      } else {
          [self.matchPopup selectItemAtIndex:3];
          [self billiardsOptionsChanged:nil];
      }
      if (![self.preview quickScanAndJoinLan]) {
          self.lanStatusLabel.stringValue = [self.preview lanStatusString];
      } else {
          self.lanStatusLabel.stringValue = [self.preview lanStatusString];
      }
      [self refreshButtons];
    }];
}
- (void)slingshotEditClicked:(id)sender {
    (void)sender;
    [self guard:^{
      [self.slingPresetPopup selectItemAtIndex:5];
      if (!self->_activeSession) return;
      [self.preview enterSlingshotEditor];
      [self saveUserPreferences];
      [self closeModeSettingsCard:nil];
    }];
}
- (void)saveLayoutWritebackClicked:(id)sender {
    (void)sender;
    [self guard:^{
      if (!self->_activeSession || self->_transition || !self.preview.active) return;
      const auto centers = [self.preview currentIconSceneCenters];
      NSRect globalClamp = self->_workArea;
      for (NSScreen* scr in NSScreen.screens) globalClamp = NSUnionRect(globalClamp, scr.frame);
      NSArray<NSDictionary*>* plan =
          self->_activeOrigIndices.empty()
              ? buildMacLayoutWritebackPlan(self->_snapshot, centers, mirrorRect(self->_workArea),
                                            mirrorRect(self->_screenFrame))
              : buildMacLayoutWritebackPlanSubset(self->_snapshot, self->_activeOrigIndices, centers,
                                                  mirrorRect(self->_workArea), mirrorRect(self->_screenFrame),
                                                  mirrorRect(globalClamp));
      if (!plan.count) {
          self.status.stringValue = @"当前所选屏幕无可写回的桌面图标，未修改系统桌面。";
          return;
      }
      NSAlert* alert = [[NSAlert alloc] init];
      alert.alertStyle = NSAlertStyleInformational;
      alert.messageText = @"将当前物理图标布局写回真实桌面？";
      alert.informativeText = [NSString
          stringWithFormat:@"即将正常退出本轮游乐场，并将 %lu 个桌面图标的当前屏幕位置同步到 Finder "
                           @"桌面。系统会自动保存写回前的原始坐标，您可随时点击“撤销写回”一键恢复。",
                           static_cast<unsigned long>(plan.count)];
      [alert addButtonWithTitle:@"确认写回并还原桌面"];
      [alert addButtonWithTitle:@"取消"];
      self->_modalAlertActive = YES;
      const NSModalResponse resp = [alert runModal];
      self->_modalAlertActive = NO;
      if (resp != NSAlertFirstButtonReturn) return;

      [NSUserDefaults.standardUserDefaults setObject:plan forKey:kSavedLayoutBackupDefaultsKey];
      if (![self finish:@"user_layout_writeback"]) return;

      auto target = [NSAppleEventDescriptor descriptorWithBundleIdentifier:@"com.apple.finder"];
      NSUInteger applied = 0;
      NSString* errText = nil;
      const bool ok = applyFinderDesktopWritebackPlan(
          plan, false, [target](NSAppleEventDescriptor* spec) { return readFinder(target, spec); },
          nativeFinderPositionWriter, &applied, &errText);
      self.status.stringValue =
          ok ? [NSString stringWithFormat:@"已将 %lu 个图标的新位置写回 Finder 桌面（可随时点“撤销写回”恢复原布局）。",
                                          static_cast<unsigned long>(applied)]
             : (errText.length ? errText : @"桌面图标位置写回未完成。");
      [self refreshButtons];
    }];
}
- (void)undoLayoutWritebackClicked:(id)sender {
    (void)sender;
    [self guard:^{
      NSArray<NSDictionary*>* plan =
          [NSUserDefaults.standardUserDefaults arrayForKey:kSavedLayoutBackupDefaultsKey];
      if (![plan isKindOfClass:NSArray.class] || plan.count == 0) {
          self.status.stringValue = @"暂无可撤销的桌面坐标写回记录。";
          return;
      }
      if (self->_activeSession) {
          if (![self finish:@"undo_layout_writeback"]) return;
      }
      auto target = [NSAppleEventDescriptor descriptorWithBundleIdentifier:@"com.apple.finder"];
      NSUInteger restoredCount = 0;
      NSString* errText = nil;
      const bool ok = applyFinderDesktopWritebackPlan(
          plan, true, [target](NSAppleEventDescriptor* spec) { return readFinder(target, spec); },
          nativeFinderPositionWriter, &restoredCount, &errText);
      if (ok) {
          [NSUserDefaults.standardUserDefaults removeObjectForKey:kSavedLayoutBackupDefaultsKey];
          self.status.stringValue = [NSString
              stringWithFormat:@"已撤销写回，成功恢复 %lu 个桌面图标的原始坐标。",
                               static_cast<unsigned long>(restoredCount)];
      } else {
          self.status.stringValue = errText.length ? errText : @"恢复原始桌面坐标未完成。";
      }
      [self refreshButtons];
    }];
}
- (void)powerOrScreenStateChanged:(NSNotification*)note {
    [self guard:^{
      NSString* name = note.name;
      if ([name isEqual:NSWorkspaceWillSleepNotification]) {
          self->_systemSleeping = YES;
          if (self->_activeSession && self->_protectedSession) {
              (void)self->_protectedSession->notifySystemSleep(true);
              [self.preview setSystemSuspended:YES];
          }
          return;
      }
      if ([name isEqual:NSWorkspaceDidWakeNotification]) {
          self->_systemSleeping = NO;
          if (self->_activeSession && self->_protectedSession) {
              (void)self->_protectedSession->notifySystemSleep(false);
              if (!self->_screenLocked) {
                  [self.preview setSystemSuspended:NO];
              }
          }
          return;
      }
      if ([name isEqual:NSWorkspaceScreensDidSleepNotification] ||
          [name isEqual:@"com.apple.screenIsLocked"]) {
          self->_screenLocked = YES;
          if (self->_activeSession && self.preview.active) {
              [self.preview setSystemSuspended:YES];
          }
          return;
      }
      if ([name isEqual:NSWorkspaceScreensDidWakeNotification] ||
          [name isEqual:@"com.apple.screenIsUnlocked"]) {
          self->_screenLocked = NO;
          if (self->_activeSession && self->_protectedSession) {
              (void)self->_protectedSession->tick();
              if (!self->_systemSleeping) {
                  [self.preview setSystemSuspended:NO];
              }
          }
          return;
      }
    }];
}
- (void)lanJoinClicked:(id)sender {
    (void)sender;
    [self guard:^{
      if (self.preview.airHockeyEnabled) {
          [self.hockeyMatchPopup selectItemAtIndex:2];
          [self airHockeyOptionsChanged:nil];
      } else {
          [self.matchPopup selectItemAtIndex:3];
          [self billiardsOptionsChanged:nil];
      }
      if (![self.preview joinLanEndpoint:self.lanAddressField.stringValue]) {
          self.lanStatusLabel.stringValue = [self.preview lanStatusString];
      }
      [self saveUserPreferences];
      [self refreshButtons];
    }];
}
- (void)lanStopClicked:(id)sender {
    (void)sender;
    [self guard:^{
      [self.preview stopLan];
      self.lanStatusLabel.stringValue = @"局域网已断开";
      [self refreshButtons];
    }];
}
- (void)blackHoleDevourChanged:(id)sender {
    [self guard:^{
      [self.preview setToyBlackHoleDevour:self.blackHoleDevour.state == NSControlStateValueOn];
    }];
}
- (void)blackHoleSizeChanged:(id)sender {
    [self guard:^{
      // Remembered even before a session starts; applied on the next enable.
      [self.preview setToyBlackHoleScale:self.blackHoleSize.doubleValue];
      [self saveUserPreferences];
    }];
}
- (void)blackHoleAnglesChanged:(id)sender {
    (void)sender;
    [self guard:^{
      const double elDeg = self.blackHoleElevation.doubleValue;
      const double tiltDeg = self.blackHoleTilt.doubleValue;
      self.elevationValueLabel.stringValue = [NSString stringWithFormat:@"%+.0f°", elDeg];
      self.tiltValueLabel.stringValue =
          std::abs(tiltDeg) < 0.5 ? @"0°" : [NSString stringWithFormat:@"%+.0f°", tiltDeg];
      [self.preview setToyBlackHoleElevation:elDeg * M_PI / 180.0 tilt:tiltDeg * M_PI / 180.0];
      [self saveUserPreferences];
    }];
}
- (void)resetBlackHoleAngles:(id)sender {
    (void)sender;
    [self guard:^{
      const double elDeg = dpp::BlackHoleState::kDefaultElevation * 180.0 / M_PI;
      self.blackHoleElevation.doubleValue = elDeg;
      self.blackHoleTilt.doubleValue = 0.0;
      self.elevationValueLabel.stringValue = [NSString stringWithFormat:@"%+.0f°", elDeg];
      self.tiltValueLabel.stringValue = @"0°";
      [self.preview setToyBlackHoleElevation:dpp::BlackHoleState::kDefaultElevation tilt:0.0];
      [self saveUserPreferences];
    }];
}
- (void)filenameVisibilityChanged:(id)sender {
    (void)sender;
    [self guard:^{
      const BOOL visible = self.filenameToggle.state == NSControlStateValueOn;
      self.preview.showsFilenames = visible;
      for (CATextLayer* tl in self.passiveLabelLayers) tl.hidden = !visible;
      if (!self.preview.billiardsEnabled && !self.preview.slingshotEnabled &&
          !self.preview.airHockeyEnabled) {
          self->_savedGravityFilenames = visible;
      }
      [self saveUserPreferences];
    }];
}
- (void)muteChanged:(id)sender {
    (void)sender;
    [self guard:^{
      self.preview.soundMuted = self.muteToggle.state == NSControlStateValueOn;
      [self saveUserPreferences];
    }];
}
- (void)refreshButtons {
    const BOOL inBilliards = _activeSession && self.preview.billiardsEnabled;
    const BOOL inAirHockey = _activeSession && self.preview.airHockeyEnabled;
    const BOOL inArena = inBilliards || (_activeSession && self.preview.slingshotEnabled) || inAirHockey;
    const auto cfg = [self currentBilliardsConfig];
    const BOOL canBilliards =
        _activeSession &&
        ([self.preview canEnableBilliardsWithConfig:cfg] || self.preview.canEnableAnyBilliards);
    self.playModePopup.enabled = _activeSession && !_transition;
    if (!_activeSession) {
        [self.playModePopup selectItemAtIndex:0];
        [self.playModePopup itemAtIndex:2].enabled = YES;
        [self.playModePopup itemAtIndex:2].title = @"台球模式";
    } else {
        const auto pm = [self.preview playMode];
        const NSInteger selIdx =
            pm == dpp::PlayMode::Billiards
                ? 2
                : (pm == dpp::PlayMode::Slingshot
                       ? 3
                       : (pm == dpp::PlayMode::AirHockey ? 4 : (pm == dpp::PlayMode::BlackHole ? 1 : 0)));
        [self.playModePopup selectItemAtIndex:selIdx];
        [self.playModePopup itemAtIndex:2].enabled = inBilliards || canBilliards;
        [self.playModePopup itemAtIndex:2].title =
            (!inBilliards && !canBilliards) ? @"台球模式(图标超限)" : @"台球模式";
    }

    const BOOL allowOptions = !_loading && !_transition && !self.desktopChooser && !self.iconPickerSheet;
    self.rulePopup.enabled = allowOptions;
    self.matchPopup.enabled = allowOptions;
    const BOOL vsAiSelected = self.matchPopup.indexOfSelectedItem == 1;
    self.aiDifficultyPopup.enabled = allowOptions && vsAiSelected;
    self.ballBordersToggle.enabled = allowOptions;
    self.ballNumbersToggle.enabled = allowOptions;
    self.tableThemePopup.enabled = allowOptions;
    self.aimGuideToggle.enabled = allowOptions;
    self.aiIntentToggle.enabled = allowOptions && vsAiSelected;
    self.iconSelPopup.enabled = allowOptions;
    NSArray<NSString*>* curLabels = self.activeLabels.count ? self.activeLabels : self.labels;
    self.pickIconsButton.enabled = allowOptions && curLabels.count > 0;
    self.extraIconsPopup.enabled = allowOptions;

    self.slingPresetPopup.enabled = allowOptions;
    self.slingEditButton.enabled = allowOptions && _activeSession;
    self.slingAmmoPopup.enabled = allowOptions;
    self.slingTrajToggle.enabled = allowOptions;
    self.hockeyMatchPopup.enabled = allowOptions;
    self.hockeyDiffPopup.enabled = allowOptions && (self.hockeyMatchPopup.indexOfSelectedItem == 0);
    self.hockeyGoalsPopup.enabled = allowOptions;
    self.hockeyChromeToggle.enabled = allowOptions;

    const BOOL billiardsLanSel = self.matchPopup.indexOfSelectedItem == 3;
    const BOOL hockeyLanSel = self.hockeyMatchPopup.indexOfSelectedItem == 2;
    const BOOL lanSelected = billiardsLanSel || hockeyLanSel;
    const BOOL lanModeActive = (inBilliards && billiardsLanSel) || (inAirHockey && hockeyLanSel);
    const BOOL lanConnectedOrWaiting = self.preview.lanActive;
    self.lanAddressField.enabled = allowOptions && lanSelected && !lanConnectedOrWaiting;
    self.lanHostButton.enabled = allowOptions && _activeSession && lanModeActive && !lanConnectedOrWaiting;
    self.lanScanButton.enabled = allowOptions && _activeSession && lanModeActive && !lanConnectedOrWaiting;
    self.lanJoinButton.enabled = allowOptions && _activeSession && lanModeActive && !lanConnectedOrWaiting;
    self.lanStopButton.enabled = allowOptions && lanConnectedOrWaiting;

    NSArray* savedBackup =
        [NSUserDefaults.standardUserDefaults arrayForKey:kSavedLayoutBackupDefaultsKey];
    const BOOL hasUndoBackup = [savedBackup isKindOfClass:NSArray.class] && savedBackup.count > 0;
    self.saveLayoutButton.enabled = allowOptions && _activeSession;
    self.undoLayoutButton.enabled = allowOptions && hasUndoBackup;
    if (self.startupUndoButton) {
        const BOOL showUndo = !_activeSession && hasUndoBackup;
        self.startupUndoButton.hidden = !showUndo;
        self.startupUndoButton.enabled = allowOptions && showUndo;
        if (self.status && !_syntheticCheckMode) {
            NSString* st = self.status.stringValue ? self.status.stringValue : @"";
            const BOOL isAlert = [st containsString:@"未"] || [st containsString:@"失败"] ||
                                 [st containsString:@"异常"] || [st containsString:@"无法"];
            self.status.hidden = !isAlert;
            self.status.frame = NSMakeRect(28, 6, showUndo ? (540.0 - 172.0) : (540.0 - 56.0), 24);
        }
    }
    const BOOL canEditLauncher =
        !_loading && !_transition && !_activeSession && !self.desktopChooser && !self.iconPickerSheet;
    if (self.displayScopePopup) self.displayScopePopup.enabled = canEditLauncher;
    if (self.inGameDisplayPopup) self.inGameDisplayPopup.enabled = !_loading && !_transition;
    for (DPPLauncherModePill* pill in self.launcherModePills) pill.enabled = canEditLauncher;
    if (!_activeSession) [self syncLauncherModePills];

    self.blackHoleSize.enabled = !_transition && !inArena;
    self.blackHoleElevation.enabled = !_transition && !inArena;
    self.blackHoleTilt.enabled = !_transition && !inArena;
    self.blackHoleAngleReset.enabled = !_transition && !inArena;
    self.blackHoleDevour.enabled = !_transition && !inArena;
    self.filenameToggle.enabled = !_transition;
    self.muteToggle.enabled = !_transition;
    self.quickStartButton.enabled = canEditLauncher;
    self.quickStartButton.needsDisplay = YES;
    self.readButton.enabled = canEditLauncher;
    self.confirmed.enabled = !_loading && !_transition && !_activeSession && self.preview.active;
    self.startButton.enabled = self.confirmed.enabled && self.confirmed.state == NSControlStateValueOn;
    for (NSButton* b in self.controls) b.enabled = _activeSession && !_transition;

    if (self.hudBar) {
        const BOOL paused = _activeSession && [[self.preview diagnostics][@"user_paused"] boolValue];
        const BOOL settingsVis = self.modeSettingsPanel != nil && self.modeSettingsPanel.visible;
        [self.hudBar syncMode:(_activeSession ? [self.preview playMode] : dpp::PlayMode::Gravity)
                       paused:paused
                        muted:self.preview.soundMuted
              canUseBilliards:canBilliards
                settingsShown:settingsVis];
    }
    [self rebuildStatusMenu];
}
- (void)confirmationChanged:(id)sender {
    (void)sender;
    [self refreshButtons];
}
- (void)beginSnapshotWithDesktopURL:(NSURL*)selectedURL saveBookmark:(BOOL)saveBookmark {
    NSURL* desktop = [NSFileManager.defaultManager URLsForDirectory:NSDesktopDirectory
                                                          inDomains:NSUserDomainMask]
                         .firstObject;
    requireMirror([fullDesktopURLIdentity(selectedURL) isEqual:fullDesktopURLIdentity(desktop)],
                  "select the system Desktop directory");
    const BOOL keepQuick = _quickAutoStart;
    if (![self finish:@"reacquire_snapshot"]) return;
    _quickAutoStart = keepQuick;
    self.desktopURL = selectedURL;
    self.scoped = [self.desktopURL startAccessingSecurityScopedResource];
    if (saveBookmark) {
        NSData* bm = [self.desktopURL bookmarkDataWithOptions:NSURLBookmarkCreationWithSecurityScope
                               includingResourceValuesForKeys:nil
                                                relativeToURL:nil
                                                        error:nil];
        if (bm) {
            [NSUserDefaults.standardUserDefaults setObject:bm forKey:@"dpp.desktopBookmarkV1"];
        }
    }
    NSArray<NSScreen*>* screens = NSScreen.screens;
    requireMirror(screens.count >= 1, "display unavailable");
    _allDisplaysSnapshot.clear();
    CGFloat maxBackingScale = 1.0;
    for (NSUInteger i = 0; i < screens.count; ++i) {
        const auto d = mirrorDisplayAt(i);
        requireMirror(validMirrorDisplayAnyCount(d), "unknown display geometry or backing");
        _allDisplaysSnapshot.push_back(d);
        maxBackingScale = std::max(maxBackingScale, d.scale);
    }
    _displaySnapshot = _allDisplaysSnapshot.front();
    if (screens.count == 1) {
        logMirrorDisplay(_displaySnapshot, _displaySnapshot, true);
    }
    const BOOL spanAllScreens = (_displayMode < 0 && screens.count > 1);
    const NSUInteger selectedScreenIdx =
        spanAllScreens ? 0u
                       : std::min<NSUInteger>(static_cast<NSUInteger>(std::max(0, _displayMode)),
                                              screens.count - 1);
    _screenFrame = screens.firstObject.frame;  // Finder dpos is always relative to primary screen top-left
    if (spanAllScreens) {
        NSRect unionVis = screens.firstObject.visibleFrame;
        NSMutableArray<NSValue*>* vps = [NSMutableArray arrayWithCapacity:screens.count];
        for (NSScreen* scr in screens) {
            unionVis = NSUnionRect(unionVis, scr.visibleFrame);
            [vps addObject:[NSValue valueWithRect:scr.visibleFrame]];
        }
        _workArea = unionVis;
        _hudWorkArea = screens.firstObject.visibleFrame;
        _scale = maxBackingScale;
        self.preview.multiScreenMode = YES;
        self.preview.targetScreenIndex = 0;
        self.preview.multiScreenViewports = vps;
    } else {
        _workArea = screens[selectedScreenIdx].visibleFrame;
        _hudWorkArea = _workArea;
        _scale = screens[selectedScreenIdx].backingScaleFactor;
        self.preview.multiScreenMode = (screens.count > 1);
        self.preview.targetScreenIndex = selectedScreenIdx;
        self.preview.multiScreenViewports = nil;
    }
    const auto& display = _allDisplaysSnapshot[selectedScreenIdx];
    std::printf("startup_display id=%u screens=%lu mode=%d frame=%.1f,%.1f,%.1f,%.1f backing=%.3f "
                "scene=AppKit_points calibration=Finder_top_left\n",
                display.id.unsignedIntValue, static_cast<unsigned long>(screens.count), _displayMode,
                display.frame.origin.x, display.frame.origin.y, display.frame.size.width,
                display.frame.size.height, _scale);
    self.finderIdentity = mirrorFinderIdentity();
    [self acquire:NO];
}
- (void)quickStartClicked:(id)sender {
    (void)sender;
    [self guard:^{
      if (self->_loading || self->_transition || self->_activeSession || self.desktopChooser ||
          self.iconPickerSheet)
          return;
      activateDPPApp();
      [self.panel makeKeyAndOrderFront:nil];
      if (self.preview.active && [self displayMatches]) {
          self->_quickAutoStart = YES;
          self.confirmed.state = NSControlStateValueOn;
          [self refreshButtons];
          [self startSession:nil];
          return;
      }
      self->_quickAutoStart = YES;
      NSURL* desktop = [NSFileManager.defaultManager URLsForDirectory:NSDesktopDirectory
                                                            inDomains:NSUserDomainMask]
                           .firstObject;
      NSData* savedBm = [NSUserDefaults.standardUserDefaults dataForKey:@"dpp.desktopBookmarkV1"];
      if (savedBm) {
          BOOL stale = NO;
          NSURL* resolved = [NSURL URLByResolvingBookmarkData:savedBm
                                                      options:NSURLBookmarkResolutionWithSecurityScope
                                                relativeToURL:nil
                                          bookmarkDataIsStale:&stale
                                                        error:nil];
          if (resolved && [fullDesktopURLIdentity(resolved) isEqual:fullDesktopURLIdentity(desktop)]) {
              [self beginSnapshotWithDesktopURL:resolved saveBookmark:stale];
              return;
          }
      }
      // Non-sandboxed native app: trigger macOS's standard Desktop folder access directly without
      // forcing an extra folder-picker sheet unless direct access is unavailable.
      if (desktop &&
          [NSFileManager.defaultManager contentsOfDirectoryAtURL:desktop
                                      includingPropertiesForKeys:nil
                                                         options:NSDirectoryEnumerationSkipsHiddenFiles
                                                           error:nil] != nil) {
          [self beginSnapshotWithDesktopURL:desktop saveBookmark:NO];
          return;
      }
      [self readAll:nil];
    }];
}
- (void)completeDesktopChoice:(NSOpenPanel*)chooser response:(NSModalResponse)response {
    if (!chooser || self.desktopChooser != chooser) return;  // stale/closed panel callback
    NSURL* selectedURL = chooser.URL;
    self.desktopChooser = nil;
    std::printf("desktop_choice_response=%ld has_url=%s\n", static_cast<long>(response),
                selectedURL ? "true" : "false");
    std::fflush(stdout);
    [self refreshButtons];
    if (response != NSModalResponseOK) {
        _quickAutoStart = NO;
        self.status.stringValue = @"已取消选择，尚未开始读取。请重新选择桌面，并点击“选择此桌面”。";
        return;
    }
    [self guard:^{
      const BOOL keepQuick = self->_quickAutoStart;
      [self beginSnapshotWithDesktopURL:selectedURL saveBookmark:YES];
      self->_quickAutoStart = keepQuick;
    }];
}
- (void)readAll:(id)sender {
    (void)sender;
    [self guard:^{
      if (self->_loading || self->_transition || self->_activeSession || self.desktopChooser) return;
      NSOpenPanel* chooser = NSOpenPanel.openPanel;
      chooser.canChooseDirectories = YES;
      chooser.canChooseFiles = NO;
      chooser.allowsMultipleSelection = NO;
      chooser.canCreateDirectories = NO;
      chooser.prompt = @"选择此桌面";
      chooser.message = @"请选择系统桌面目录，然后点击“选择此桌面”。此步骤不隐藏原图标。";
      chooser.directoryURL = [NSFileManager.defaultManager URLsForDirectory:NSDesktopDirectory
                                                                  inDomains:NSUserDomainMask]
                                 .firstObject;
      self.desktopChooser = chooser;
      self.status.stringValue = @"等待确认桌面目录；请在选择窗口点击“选择此桌面”。";
      [self refreshButtons];
      std::puts("desktop_choice_presented=true style=attached_sheet");
      std::fflush(stdout);
      __weak DPPProtectedMirrorPanel* weak = self;
      __weak NSOpenPanel* weakChooser = chooser;
      [chooser beginSheetModalForWindow:self.panel
                      completionHandler:^(NSModalResponse response) {
                        DPPProtectedMirrorPanel* owner = weak;
                        NSOpenPanel* completed = weakChooser;
                        if (!owner || !completed) return;
                        [owner guard:^{
                          [owner completeDesktopChoice:completed response:response];
                        }];
                      }];
    }];
}
- (void)acquire:(BOOL)revalidate {
    _loading = YES;
    if (_thumbnailCancelled) _thumbnailCancelled->store(true);
    _thumbnailCancelled = std::make_shared<std::atomic_bool>(false);
    const auto cancelled = _thumbnailCancelled;
    const auto epoch = ++_epoch;
    [self refreshButtons];
    self.status.stringValue =
        revalidate ? @"正在重新核对全部项目、位置与文件身份…" : @"正在读取全部桌面项目并准备图像…";
    NSURL* directory = self.desktopURL;
    NSDictionary* finder = self.finderIdentity;
    FinderSnapshot previous = _snapshot;
    NSArray* previousIdentities = self.identities;
    NSSet* previousVolumes = self.volumePaths;
    NSSet* previousMounts = self.mountedRoots;
    const CGFloat scale = _scale;
    __weak DPPProtectedMirrorPanel* weak = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      @autoreleasepool {
          FinderSnapshot snapshot;
          NSMutableArray* thumbnails = [NSMutableArray array];
          NSMutableArray* identities = [NSMutableArray array];
          NSString* failure = nil;
          NSMutableSet* volumes = [NSMutableSet set];
          NSSet* mounts = nil;
          @try {
              try {
                  OSStatus perm = finderPermission(false);
                  if (perm != noErr && !weak.syntheticCheckMode) {
                      perm = finderPermission(true);
                  }
                  requireMirror(
                      perm == noErr,
                      "尚未授予访达(Finder)自动化权限：请在弹出的系统授权框点击“好”，或前往「系统设置 → 隐私与安全性 → 自动化」勾选允许控制 Finder");
                  auto target = [NSAppleEventDescriptor descriptorWithBundleIdentifier:@"com.apple.finder"];
                  mounts = mountedRootPaths();
                  snapshot = fullDesktopSnapshot(
                      [target](NSAppleEventDescriptor* spec) { return readFinder(target, spec); });
                  requireMirror([finder isEqual:mirrorFinderIdentity()],
                                "Finder changed during full snapshot");
                  const auto thumbnailDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
                  for (NSURL* url in snapshot.urls) {
                      requireMirror(!cancelled->load(), "snapshot cancelled");
                      const auto kind = desktopItemScope(url, directory, mounts, isVolumeRoot(url));
                      requireMirror(kind != DesktopItemScope::unsupported,
                                    "桌面项目既非桌面文件，也非可验证的已挂载磁盘；未隐藏图标");
                      if (kind == DesktopItemScope::mountedVolume)
                          [volumes addObject:fullDesktopURLIdentity(url)];
                      [identities addObject:fileIdentity(url)];
                      if (!revalidate) {
                          const auto pixels = static_cast<NSUInteger>(
                              std::clamp([snapshot.report[@"desktop_icon_size"][@"raw"] doubleValue] * scale,
                                         64.0, 256.0));
                          NSDictionary* thumbnail =
                              kind == DesktopItemScope::mountedVolume
                                  ? thumbnailUnavailable(@"volume_system_icon")
                                  : documentThumbnail(url, pixels, scale, thumbnailDeadline, cancelled);
                          if (thumbnail[@"image"] == NSNull.null && kind != DesktopItemScope::mountedVolume &&
                              !cancelled->load()) {
                              NSDictionary* photo = previewThumbnail(url, pixels);
                              if (photo[@"image"] != NSNull.null) thumbnail = photo;
                          }
                          [thumbnails addObject:thumbnail];
                      }
                  }
                  requireMirror([mounts isEqualToSet:mountedRootPaths()],
                                "mounted volumes changed during snapshot");
                  if (revalidate) {
                      requireMirror(
                          [volumes isEqualToSet:previousVolumes] && [mounts isEqualToSet:previousMounts],
                          "desktop volume membership changed; read again");
                      requireMirror(fullDesktopSnapshotsMatch(previous, snapshot),
                                    "desktop items or positions changed; read again");
                      for (NSUInteger i = 0; i < previous.urls.count; ++i)
                          requireMirror([fileIdentity(previous.urls[i]) isEqual:previousIdentities[i]],
                                        "desktop file identity changed");
                  }
              } catch (const std::exception& error) {
                  failure = [NSString stringWithUTF8String:error.what()];
              }
          } @catch (NSException*) {
              failure = @"native snapshot exception";
          }
          dispatch_async(dispatch_get_main_queue(), ^{
            DPPProtectedMirrorPanel* owner = weak;
            if (!owner || owner->_epoch != epoch) return;
            [owner guard:^{
              owner->_loading = NO;
              requireMirror(failure == nil, failure ? failure.UTF8String : "snapshot failed");
              requireMirror([owner displayMatches] && [finder isEqual:mirrorFinderIdentity()],
                            "display or Finder changed");
              if (revalidate)
                  [owner activatePrepared];
              else {
                  owner->_snapshot = snapshot;
                  owner.identities = identities;
                  owner.volumePaths = volumes;
                  owner.mountedRoots = mounts;
                  [owner decode:thumbnails];
                  [owner showCandidate];
              }
              [owner refreshButtons];
            }];
          });
      }
    });
}
- (void)decode:(NSArray*)thumbnails {
    NSMutableArray* images = [NSMutableArray array];
    NSMutableArray* labels = [NSMutableArray array];
    NSUInteger content = 0, photos = 0, icons = 0, unavailable = 0;
    for (NSUInteger i = 0; i < _snapshot.urls.count; ++i) {
        NSURL* url = _snapshot.urls[i];
        id thumbnail = thumbnails[i][@"image"];
        NSString* imageStatus = thumbnails[i][@"status"];
        if ([imageStatus isEqual:@"quicklook_content"])
            ++content;
        else if ([imageStatus isEqual:@"decoded"])
            ++photos;
        else {
            ++icons;
            if (![imageStatus isEqual:@"volume_system_icon"] && ![imageStatus isEqual:@"system_icon_route"])
                ++unavailable;
        }
        NSRect rect = NSMakeRect(0, 0, 256, 256);
        NSImage* icon = thumbnail == NSNull.null ? [NSWorkspace.sharedWorkspace iconForFile:url.path] : nil;
        CGImageRef image = thumbnail != NSNull.null
                               ? (__bridge CGImageRef)thumbnail
                               : [icon CGImageForProposedRect:&rect context:nil hints:nil];
        requireMirror(image && CGImageGetWidth(image) && CGImageGetHeight(image),
                      "a desktop image could not be prepared");
        [images addObject:(__bridge id)image];
        NSString* label = nil;
        if ([self.volumePaths containsObject:fullDesktopURLIdentity(url)])
            [url getResourceValue:&label forKey:NSURLVolumeLocalizedNameKey error:nil];
        else
            label = [NSFileManager.defaultManager displayNameAtPath:url.path];
        requireMirror(label.length > 0, "a desktop label is unavailable");
        [labels addObject:label];
        requireMirror([fileIdentity(url) isEqual:self.identities[i]],
                      "desktop identity changed during decoding");
    }
    self.images = images;
    self.labels = labels;
    self.imageSummary =
        [NSString stringWithFormat:
                      @"内容缩略图 %lu 项，照片备用预览 %lu 项，系统图标 %lu 项（含缩略图未取得 %lu 项）。",
                      content, photos, icons, unavailable];
    std::printf("full_desktop_images quicklook_content=%lu photo_fallback=%lu system_icons=%lu "
                "thumbnail_unavailable=%lu\n",
                content, photos, icons, unavailable);
    [self watchDesktop];
}
- (void)watchURL:(NSURL*)url directory:(BOOL)directory volume:(BOOL)volume {
    const int fd = open(url.fileSystemRepresentation, O_EVTONLY | O_NOFOLLOW | O_CLOEXEC);
    requireMirror(fd >= 0, "desktop change monitor unavailable; takeover refused");
    // A disk's ordinary content writes do not change its desktop representation.
    const unsigned long mask = volume
                                   ? (DISPATCH_VNODE_DELETE | DISPATCH_VNODE_RENAME | DISPATCH_VNODE_REVOKE)
                                   : (DISPATCH_VNODE_WRITE | DISPATCH_VNODE_DELETE | DISPATCH_VNODE_RENAME |
                                      DISPATCH_VNODE_ATTRIB | DISPATCH_VNODE_EXTEND | DISPATCH_VNODE_REVOKE);
    dispatch_source_t source = dispatch_source_create(DISPATCH_SOURCE_TYPE_VNODE, static_cast<uintptr_t>(fd),
                                                      mask, dispatch_get_main_queue());
    if (!source) {
        close(fd);
        throw std::runtime_error("desktop change monitor creation failed");
    }
    NSArray* version = (!directory && !volume) ? watchedItemVersion(url) : nil;
    __weak DPPProtectedMirrorPanel* weak = self;
    const auto epoch = _epoch;
    __weak dispatch_source_t weakSource = source;
    dispatch_source_set_event_handler(source, ^{
      dispatch_source_t liveSource = weakSource;
      if (!liveSource) return;
      DPPProtectedMirrorPanel* owner = weak;
      if (!owner || owner->_epoch != epoch) return;
      [owner guard:^{
        const auto flags = dispatch_source_get_data(liveSource);
        if (directory) {
            NSArray<NSURL*>* entries =
                [NSFileManager.defaultManager contentsOfDirectoryAtURL:owner.desktopURL
                                            includingPropertiesForKeys:nil
                                                               options:NSDirectoryEnumerationSkipsHiddenFiles
                                                                 error:nil];
            // Hidden .DS_Store writes alone must not stop a scene; volume icons
            // belong to Finder's desktop but not this directory listing.
            if (desktopDirectoryMatches(owner->_snapshot.urls, owner.volumePaths, entries)) return;
        }
        if (!directory && !volume && harmlessItemMetadata(flags, version, watchedItemVersion(url))) {
            std::printf("desktop_item_event flags=%lu content_and_identity_unchanged=true action=keep\n",
                        flags);
            std::fflush(stdout);
            return;
        }
        if (owner->_activeSession && owner.preview.continuousSession && !owner->_loading &&
            !owner->_transition) {
            if (volume && (flags & (DISPATCH_VNODE_DELETE | DISPATCH_VNODE_RENAME | DISPATCH_VNODE_REVOKE))) {
                dispatch_source_cancel(liveSource);
                [owner.volumeWatchers removeObject:liveSource];
            }
            std::printf(
                "desktop_item_event flags=%lu directory=%s volume=%s action=keep_continuous_session\n", flags,
                directory ? "true" : "false", volume ? "true" : "false");
            std::fflush(stdout);
            return;
        }
        std::printf("desktop_item_event flags=%lu directory=%s volume=%s action=restore\n", flags,
                    directory ? "true" : "false", volume ? "true" : "false");
        std::fflush(stdout);
        [owner finish:@"desktop_file_or_membership_changed"];
      }];
    });
    dispatch_source_set_cancel_handler(source, ^{
      close(fd);
    });
    if (volume)
        [self.volumeWatchers addObject:source];
    else
        [self.fileWatchers addObject:source];
    dispatch_resume(source);
}
- (void)releaseVolumeWatchers {
    for (dispatch_source_t source in self.volumeWatchers) dispatch_source_cancel(source);
    self.volumeWatchers = nil;
}
- (void)watchDesktop {
    for (dispatch_source_t source in self.fileWatchers) dispatch_source_cancel(source);
    [self releaseVolumeWatchers];
    self.fileWatchers = [NSMutableArray array];
    self.volumeWatchers = [NSMutableArray array];
    [self watchURL:self.desktopURL directory:YES volume:NO];
    for (NSURL* url in _snapshot.urls)
        [self watchURL:url directory:NO volume:[self.volumePaths containsObject:fullDesktopURLIdentity(url)]];
}
- (BOOL)directoryMembershipMatches {
    NSArray<NSURL*>* entries =
        [NSFileManager.defaultManager contentsOfDirectoryAtURL:self.desktopURL
                                    includingPropertiesForKeys:nil
                                                       options:NSDirectoryEnumerationSkipsHiddenFiles
                                                         error:nil];
    return desktopDirectoryMatches(_snapshot.urls, self.volumePaths, entries);
}
- (void)volumeChanged:(NSNotification*)notification {
    (void)notification;
    // Notifications are delivered by NSWorkspace's own center. Serialize all
    // scene and recovery work onto the UI thread, including during snapshot IO.
    __weak DPPProtectedMirrorPanel* weak = self;
    const auto action = ^{
      DPPProtectedMirrorPanel* owner = weak;
      if (!owner) return;
      [owner guard:^{
        if (owner->_loading || owner->_transition) {
            [owner finish:@"mounted_volume_changed"];
            return;
        }
        if (owner->_activeSession || owner.preview.active) {
            // Release any open volume fds immediately so Finder can eject DMGs/USBs cleanly,
            // and keep the active playground session undisturbed.
            [owner releaseVolumeWatchers];
            owner.mountedRoots = mountedRootPaths();
            std::printf("mounted_volume_event active_session=%s action=keep_session\n",
                        owner->_activeSession ? "true" : "false");
            std::fflush(stdout);
        }
      }];
    };
    if (NSThread.isMainThread)
        action();
    else
        dispatch_async(dispatch_get_main_queue(), action);
}
- (BOOL)checkSyntheticVolumeChanges {
    // Only called by --mirror-panel-check, before any snapshot/session exists.
    // Do not post fake notifications to the system workspace notification center.
    _loading = YES;
    const auto epoch = _epoch;
    [self volumeChanged:nil];
    const bool discarded = !_loading && _epoch != epoch && !_protectedSession;
    _transition = YES;
    _cancelRequested = NO;
    [self volumeChanged:nil];
    const bool cancelled = _cancelRequested && _transition && !_protectedSession;
    _transition = NO;
    [self finish:@"synthetic_volume_check_complete"];
    return discarded && cancelled;
}

- (void)closePassiveScreenWindows {
    for (NSWindow* win in self.passiveScreenWindows) {
        [win orderOut:nil];
    }
    self.passiveScreenWindows = nil;
    self.passiveLabelLayers = nil;
}

- (void)presentPassiveScreenWindowsForSelectedScreen:(NSUInteger)selectedIdx {
    [self closePassiveScreenWindows];
    NSArray<NSScreen*>* screens = NSScreen.screens;
    if (screens.count <= 1 || _displayMode < 0) return;
    NSArray* rows = [_snapshot.report[@"items"] isKindOfClass:NSArray.class] ? _snapshot.report[@"items"] : nil;
    if (!rows || !self.images.count || !self.labels.count) return;
    const float iconSize = [_snapshot.report[@"desktop_icon_size"][@"raw"] floatValue];
    const float drawSz = iconSize > 0 ? iconSize : 64.0f;
    const dpp::Rect primaryRect = mirrorRect(screens.firstObject.frame);
    const BOOL showNames = (self.filenameToggle.state == NSControlStateValueOn);

    self.passiveScreenWindows = [NSMutableArray array];
    self.passiveLabelLayers = [NSMutableArray array];

    for (NSUInteger sIdx = 0; sIdx < screens.count; ++sIdx) {
        if (sIdx == selectedIdx) continue;
        NSScreen* scr = screens[sIdx];
        const NSRect vis = scr.visibleFrame;
        const dpp::Rect visRect = mirrorRect(vis);
        const CGFloat backingScale = std::max<CGFloat>(1.0, scr.backingScaleFactor);

        NSMutableArray<NSNumber*>* screenItems = [NSMutableArray array];
        for (NSUInteger i = 0; i < rows.count && i < self.images.count; ++i) {
            const Vec2 raw{[rows[i][@"position_raw"][0] floatValue], [rows[i][@"position_raw"][1] floatValue]};
            const Vec2 global = macTopLeftPointsToGlobal(raw, primaryRect);
            if (owningScreenIndexForGlobalPoint(NSMakePoint(global.x, global.y), screens) == sIdx) {
                [screenItems addObject:@(i)];
            }
        }
        if (screenItems.count == 0) continue;

        NSWindow* win = [[NSWindow alloc] initWithContentRect:vis
                                                    styleMask:NSWindowStyleMaskBorderless
                                                      backing:NSBackingStoreBuffered
                                                        defer:NO];
        win.releasedWhenClosed = NO;
        win.opaque = NO;
        win.backgroundColor = NSColor.clearColor;
        win.hasShadow = NO;
        win.ignoresMouseEvents = YES;
        win.level = CGWindowLevelForKey(kCGDesktopIconWindowLevelKey) + 1;
        win.collectionBehavior = NSWindowCollectionBehaviorStationary |
                                 NSWindowCollectionBehaviorCanJoinAllSpaces |
                                 NSWindowCollectionBehaviorFullScreenNone;

        DPPFlippedPassiveView* view =
            [[DPPFlippedPassiveView alloc] initWithFrame:NSMakeRect(0, 0, vis.size.width, vis.size.height)];
        view.wantsLayer = YES;
        view.layer.contentsScale = backingScale;
        win.contentView = view;

        for (NSNumber* num in screenItems) {
            const NSUInteger idx = num.unsignedIntegerValue;
            const Vec2 raw{[rows[idx][@"position_raw"][0] floatValue],
                           [rows[idx][@"position_raw"][1] floatValue]};
            const Vec2 global = macTopLeftPointsToGlobal(raw, primaryRect);
            const Vec2 anchor = macGlobalPointsToScene(global, visRect);
            const CGFloat half = drawSz * 0.5;
            const CGFloat cx = std::clamp<CGFloat>(anchor.x, half, std::max<CGFloat>(half, vis.size.width - half));
            const CGFloat cy = std::clamp<CGFloat>(anchor.y, half, std::max<CGFloat>(half, vis.size.height - half));

            CALayer* imgLayer = [CALayer layer];
            imgLayer.contentsScale = backingScale;
            imgLayer.contentsGravity = kCAGravityResizeAspect;
            imgLayer.contents = self.images[idx];
            imgLayer.frame = NSMakeRect(cx - half, cy - half, drawSz, drawSz);
            [view.layer addSublayer:imgLayer];

            if (idx < self.labels.count) {
                CATextLayer* txt = [CATextLayer layer];
                txt.contentsScale = backingScale;
                txt.string = self.labels[idx];
                txt.font = (__bridge CFTypeRef)[NSFont systemFontOfSize:11.5 weight:NSFontWeightMedium];
                txt.fontSize = 11.5;
                txt.foregroundColor = NSColor.whiteColor.CGColor;
                txt.alignmentMode = kCAAlignmentCenter;
                txt.truncationMode = kCATruncationMiddle;
                txt.wrapped = NO;
                txt.backgroundColor = [NSColor colorWithWhite:0.0 alpha:0.42].CGColor;
                txt.cornerRadius = 4.0;
                txt.hidden = !showNames;
                const CGFloat lw = std::clamp<CGFloat>(drawSz + 44.0, 84.0, 132.0);
                txt.frame = NSMakeRect(cx - lw * 0.5, cy + half + 2.0, lw, 17.0);
                [view.layer addSublayer:txt];
                [self.passiveLabelLayers addObject:txt];
            }
        }
        [win orderFront:nil];
        [self.passiveScreenWindows addObject:win];
    }
}

- (MirrorSnapshot)scene:(BOOL)candidate {
    NSArray<NSScreen*>* screens = NSScreen.screens;
    const BOOL spanAllScreens = (_displayMode < 0 && screens.count > 1);
    const NSUInteger selectedIdx =
        spanAllScreens ? 0u
                       : std::min<NSUInteger>(static_cast<NSUInteger>(std::max(0, _displayMode)),
                                              std::max<NSUInteger>(1u, screens.count) - 1u);

    MirrorSnapshot scene;
    scene.screen = mirrorRect(_screenFrame);
    scene.workArea = mirrorRect(_workArea);
    scene.singleScreen = (screens.count == 1);
    scene.multiScreenAllowed = YES;
    scene.fullDesktop = YES;
    scene.candidatePreview = candidate;
    scene.capturedAt = NSProcessInfo.processInfo.systemUptime;

    for (NSScreen* scr in screens) {
        scene.screenFrames.push_back(mirrorRect(scr.frame));
    }
    if (spanAllScreens) {
        auto toUnionSceneRect = [&](NSRect vf) -> dpp::Rect {
            const float sx = static_cast<float>(vf.origin.x - _workArea.origin.x);
            const float sy = static_cast<float>((_workArea.origin.y + _workArea.size.height) -
                                                (vf.origin.y + vf.size.height));
            return {sx, sy, static_cast<float>(vf.size.width), static_cast<float>(vf.size.height)};
        };
        for (NSScreen* scr in screens) {
            scene.screenWorkAreasScene.push_back(toUnionSceneRect(scr.visibleFrame));
        }
        scene.primaryArenaScene = dpp::computeMultiScreenSpanArenaRect(
            {scene.workArea.width, scene.workArea.height}, scene.screenWorkAreasScene);
    }

    const float size = [_snapshot.report[@"desktop_icon_size"][@"raw"] floatValue];
    NSArray* rows = _snapshot.report[@"items"];
    NSMutableArray* filteredImages = [NSMutableArray array];
    NSMutableArray<NSString*>* filteredLabels = [NSMutableArray array];
    _activeOrigIndices.clear();

    for (NSUInteger i = 0; i < rows.count; ++i) {
        const Vec2 raw{[rows[i][@"position_raw"][0] floatValue], [rows[i][@"position_raw"][1] floatValue]};
        const Vec2 global = macTopLeftPointsToGlobal(raw, scene.screen);
        if (!spanAllScreens && screens.count > 1) {
            const NSUInteger ownerIdx =
                owningScreenIndexForGlobalPoint(NSMakePoint(global.x, global.y), screens);
            if (ownerIdx != selectedIdx) continue;
        }
        const uint32_t localTexIdx = static_cast<uint32_t>(filteredImages.count + 1);
        MirrorItem item;
        item.id = IconId{static_cast<uint32_t>(i + 1)};
        item.texture = TextureId{localTexIdx};
        item.rawDpos = raw;
        item.sceneAnchor = macGlobalPointsToScene(global, scene.workArea);
        item.coordinateValidated = !candidate;
        item.finderIconSize = size;
        item.dppDrawSize = {size, size};
        item.dppCollisionBox = {size * .9f, size * .9f};
        scene.items.push_back(item);
        _activeOrigIndices.push_back(static_cast<std::size_t>(i));
        if (i < self.images.count) [filteredImages addObject:self.images[i]];
        if (i < self.labels.count) [filteredLabels addObject:self.labels[i]];
    }
    scene.finderItemCount = scene.items.size();
    self.activeImages = filteredImages;
    self.activeLabels = filteredLabels;
    return scene;
}
- (void)showCandidate {
    _scene = [self scene:YES];
    self.preview.snapshotID = NSUUID.UUID.UUIDString;
    self.preview.labels = self.activeLabels;
    self.preview.deferPresentation = NO;
    if (![self.preview start:_scene images:self.activeImages]) {
        NSDictionary* failure = self.preview.startupFailure;
        NSString* detail = failure ? [NSString stringWithFormat:@"static candidate failed: %@ / %@",
                                                                failure[@"stage"], failure[@"error"]]
                                   : @"full static candidate failed to initialize";
        throw std::runtime_error(detail.UTF8String);
    }
    const NSUInteger insetCount =
        [[self.preview diagnostics][@"startup_edge_inset_count"] unsignedIntegerValue];
    self.confirmed.title = insetCount ? @"已核对副本(含内移)" : @"已核对副本对应";
    self.confirmed.toolTip = insetCount ? @"已核对全部副本及边缘内移位置（原图标不移动）"
                                        : @"已核对所有项目都有副本，图像位置与原件对应";
    self.confirmed.state = NSControlStateValueOff;
    self.status.stringValue = [NSString
        stringWithFormat:@"已就绪：共载入 %lu 个桌面图标（本场参演 %lu 项），正在进入桌面游乐场…",
                         static_cast<unsigned long>(self.images.count),
                         static_cast<unsigned long>(self.activeImages.count)];
    std::printf("full_desktop_volume_icons=%lu directory_items=%lu active_items=%lu\n",
                self.volumePaths.count, self.images.count - self.volumePaths.count, self.activeImages.count);
    std::printf("full_desktop_resources_prepared=%lu system_visibility_changed=false\n", self.images.count);
    std::fflush(stdout);
    if (_quickAutoStart) {
        self.confirmed.state = NSControlStateValueOn;
        [self refreshButtons];
        [self startSession:nil];
    }
}
- (void)startSession:(id)sender {
    (void)sender;
    [self guard:^{
      if (!self.startButton.enabled || self->_loading || self->_activeSession) return;
      requireMirror(self.confirmed.state == NSControlStateValueOn && self.preview.active,
                    "full visual confirmation required");
      std::printf("full_snapshot_user_confirmed=true snapshot_id=%s items=%lu active_items=%lu "
                  "screen_width=%.0f screen_height=%.0f backing_scale=%.2f\n",
                  self.preview.snapshotID.UTF8String, self.images.count, self.activeImages.count,
                  self->_screenFrame.size.width, self->_screenFrame.size.height, self->_scale);
      std::fflush(stdout);
      [self acquire:YES];
    }];
}
- (void)activatePrepared {
    _transition = YES;
    _cancelRequested = NO;
    // The revalidation worker advanced the epoch. Rebind monitors to this exact
    // immutable snapshot before starting any system mutation.
    [self watchDesktop];
    requireMirror([self.mountedRoots isEqualToSet:mountedRootPaths()],
                  "mounted volumes changed before takeover");
    requireMirror([self directoryMembershipMatches] && [self.finderIdentity isEqual:mirrorFinderIdentity()],
                  "desktop membership or Finder changed before takeover");
    for (NSUInteger i = 0; i < _snapshot.urls.count; ++i)
        requireMirror([fileIdentity(_snapshot.urls[i]) isEqual:self.identities[i]],
                      "desktop file changed before takeover");
    [self refreshButtons];
    // Keep callbacks from interpreting the planned candidate replacement as an exit.
    auto callback = self.preview.didClose;
    self.preview.didClose = nil;
    [self.preview close:@"full_candidate_confirmed"];
    self.preview.didClose = callback;
    _scene = [self scene:NO];
    _openTargets.clear();
    _openConsumed = false;
    _cancelOpen = false;
    for (std::size_t k = 0; k < _scene.items.size() && k < _activeOrigIndices.size(); ++k) {
        const std::size_t origIdx = _activeOrigIndices[k];
        if (origIdx >= _snapshot.urls.count) continue;
        NSURL* url = [_snapshot.urls[origIdx] copy];
        const auto target = bindOpenTarget(url);
        NSArray* sourceIdentity = target.aliasURL ? target.aliasIdentity : target.identity;
        if (target.url && [sourceIdentity isEqual:self.identities[origIdx]])
            _openTargets.emplace(static_cast<uint32_t>(_scene.items[k].id), target);
    }
    self.preview.labels = self.activeLabels;
    self.preview.deferPresentation = YES;
    self.preview.continuousSession = YES;
    BOOL resourcesReady = [self.preview start:_scene images:self.activeImages];
    if (!resourcesReady || ![self displayMatches] || !NSApp.active || _cancelRequested) {
        _transition = NO;
        [self finish:@"resource_preparation_invalidated"];
        return;
    }
    _protectedSession = std::make_unique<DPPProtectedMirrorSession>();
    const NSUInteger journalItemCount = std::max<NSUInteger>(1u, self.images.count);
    BOOL started =
        _protectedSession->start(self.finderIdentity, journalItemCount, self.preview.snapshotID, true);
    _activeSession = started;
    self.preview.allowsBackgroundSession = started;  // Display Settings may own foreground during migration.
    __weak DPPProtectedMirrorPanel* migrationOwner = self;
    self.preview.migrationProtectionValid = ^BOOL {
      auto owner = migrationOwner;
      return owner && owner->_activeSession && !owner->_transition && !owner->_cancelRequested &&
             owner->_protectedSession && owner->_protectedSession->tick();
    };
    self.preview.displayCommitted = ^{
      auto owner = migrationOwner;
      if (owner && owner->_activeSession && !owner->_transition) {
          if (NSScreen.screens.count == 1) {
              owner->_displaySnapshot = mirrorDisplay();
              owner->_workArea = NSScreen.screens.firstObject.visibleFrame;
              owner->_hudWorkArea = owner->_workArea;
          }
          const NSRect hudVis = (owner->_hudWorkArea.size.width > 0) ? owner->_hudWorkArea : owner->_workArea;
          if (owner.hudBar.isVisible) [owner.hudBar showInWorkArea:hudVis];
          if (owner.modeSettingsPanel.visible) [owner repositionModeSettingsCard];
          [owner refreshButtons];
      }
    };
    self.preview.protectionIdentity = _protectedSession->diagnosticIdentity();
    [self.preview recordEvent:@"protection_start_result" details:@{@"started" : @(started)}];
    _transition = NO;
    if (!started || _cancelRequested || !self.preview.active || ![self displayMatches] || !NSApp.active) {
        [self finish:@"takeover_start_failed_or_cancelled"];
        return;
    }
    requireMirror([self.preview presentPrepared], "prepared mirror presentation failed");
    if (_displayMode >= 0 && NSScreen.screens.count > 1) {
        const NSUInteger selIdx = std::min<NSUInteger>(static_cast<NSUInteger>(_displayMode),
                                                       NSScreen.screens.count - 1u);
        [self presentPassiveScreenWindowsForSelectedScreen:selIdx];
    }
    // Release external volume fds now that takeover succeeded so unmounting DMGs/USBs never blocks.
    [self releaseVolumeWatchers];
    // Collapse startup panel first, then activate physics + desktop interaction and show the Floating Capsule
    // Bar!
    _quickAutoStart = NO;
    [self.panel orderOut:nil];
    activateDPPApp();
    [self.preview playPause];
    if (!_syntheticCheckMode && _savedPlayMode != dpp::PlayMode::Gravity) {
        [self switchToPlayMode:_savedPlayMode];
    }
    [self.preview ensureInteractiveIfAllowed];
    if (!_syntheticCheckMode && !self.appNapActivity) {
        // Prevent macOS App Nap from throttling the 5s watchdog heartbeat timer when screen is locked
        // or occluded by fullscreen apps, while still allowing normal idle/lid system sleep.
        self.appNapActivity = [NSProcessInfo.processInfo
            beginActivityWithOptions:NSActivityUserInitiatedAllowingIdleSystemSleep
                              reason:@"DPP continuous desktop session watchdog heartbeat"];
    }
    if (!_syntheticCheckMode) {
        const NSRect hudVis = (_hudWorkArea.size.width > 0) ? _hudWorkArea : _workArea;
        [self.hudBar showInWorkArea:hudVis];
        const auto startedEpoch = _epoch;
        __weak DPPProtectedMirrorPanel* weakFocusOwner = self;
        for (int delayMs : {380, 900}) {
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, delayMs * NSEC_PER_MSEC), dispatch_get_main_queue(),
                           ^{
                             DPPProtectedMirrorPanel* o = weakFocusOwner;
                             if (!o || o->_epoch != startedEpoch || !o->_activeSession || o->_transition ||
                                 o.modeSettingsPanel.visible)
                                 return;
                             [o.preview ensureInteractiveIfAllowed];
                           });
        }
    }
    self.status.stringValue =
        @"游乐场运行中：已收起为顶部悬浮胶囊条与菜单栏图标，按 Esc 或点“还原”立即恢复原桌面。";
    [self refreshButtons];
}
- (void)openOriginal:(uint32_t)itemID {
    if (!_activeSession || _transition || self.preview.migrating || _openConsumed || !_protectedSession ||
        !_protectedSession->remainingMs() || !_openTargets.contains(itemID))
        return;
    const OpenTarget target = _openTargets.at(itemID);  // survives all preview/snapshot cleanup
    _openConsumed = true;
    NSDictionary* audit = _protectedSession->diagnosticIdentity();
    NSString* requestID = NSUUID.UUID.UUIDString;
    NSString* previewID = [self.preview diagnostics][@"session_id"];
    [self.preview recordEvent:@"open_request"
                      details:@{@"request_id" : requestID, @"target_item_id" : @(itemID)}];
    std::printf("open_request session_id=%s original_deadline_ms=%llu target_item_id=%u input_released=%s "
                "uptime=%.6f\n",
                [audit[@"protected_session_id"] UTF8String],
                [audit[@"original_deadline_ms"] unsignedLongLongValue], itemID,
                [[self.preview diagnostics][@"grabbing"] boolValue] ? "false" : "true",
                NSProcessInfo.processInfo.systemUptime);
    if (!validOpenTarget(target)) {
        [self.preview recordEvent:@"open_rejected"
                          details:@{@"request_id" : requestID, @"target_item_id" : @(itemID)}];
        self.status.stringValue = @"目标失效或身份改变，未打开；请停止并重新读取桌面。";
        _openConsumed = false;
        [self.preview completeOpenRequest];
        return;
    }
    // Explicit opening authorizes this session to remain passive behind other apps.
    // Deadline, host validation, file watchers and independent recovery remain active.
    self.preview.allowsBackgroundSession = YES;
    __weak DPPProtectedMirrorPanel* weak = self;
    const auto epoch = _epoch;
    [NSWorkspace.sharedWorkspace
                  openURL:target.url
            configuration:[NSWorkspaceOpenConfiguration configuration]
        completionHandler:^(NSRunningApplication* app, NSError* error) {
          const BOOL accepted = app != nil && error == nil;
          dispatch_async(dispatch_get_main_queue(), ^{
            std::printf(
                "open_original_keep_session system_accepted=%s user_confirmed=unverified error_code=%ld\n",
                accepted ? "true" : "false", static_cast<long>(error.code));
            std::fflush(stdout);
            DPPProtectedMirrorPanel* owner = weak;
            const BOOL current = owner && owner->_epoch == epoch && owner->_activeSession;
            writeMirrorTrace(
                @"mirror_open_completion=", @{
                    @"request_id" : requestID,
                    @"preview_session_id" : previewID,
                    @"protected_session_id" : audit[@"protected_session_id"],
                    @"target_item_id" : @(itemID),
                    @"system_accepted" : @(accepted),
                    @"error_code" : @(error.code),
                    @"same_active_session" : @(current),
                    @"user_confirmed" : @"unverified"
                });
            if (!current) return;
            [owner guard:^{
              [owner.preview recordEvent:@"open_result"
                                 details:@{
                                     @"request_id" : requestID,
                                     @"target_item_id" : @(itemID),
                                     @"system_accepted" : @(accepted),
                                     @"error_code" : @(error.code),
                                     @"user_confirmed" : @"unverified"
                                 }];
              owner->_openConsumed = false;
              [owner.preview completeOpenRequest];
              owner.status.stringValue =
                  accepted
                      ? @"系统已接受打开请求；可直接回到桌面右键下一个镜像，无需重新开启交互。保护时限不变。"
                      : [NSString stringWithFormat:@"打开失败：%@。镜像会话保留，可停止/恢复。",
                                                   error ? error.localizedDescription : @"系统未接受请求"];
            }];
          });
        }];
}
- (BOOL)finish:(NSString*)reason {
    if (_activeSession && !_syntheticCheckMode) {
        _savedPlayMode = [self.preview playMode];
        [self saveUserPreferences];
    }
    if (self.iconPickerSheet) {
        NSPanel* sheet = self.iconPickerSheet;
        self.iconPickerSheet = nil;
        self.iconCheckboxes = nil;
        if (sheet.sheetParent) [sheet.sheetParent endSheet:sheet returnCode:NSModalResponseCancel];
        [sheet orderOut:nil];
    }
    [self.modeSettingsPanel orderOut:nil];
    if (self.desktopChooser) {
        NSOpenPanel* chooser = self.desktopChooser;
        self.desktopChooser = nil;
        [self.panel endSheet:chooser returnCode:NSModalResponseCancel];
    }
    if (![reason isEqual:@"open_original"]) _cancelOpen = true;
    if (_transition) {
        _cancelRequested = YES;
        return NO;
    }
    _openTargets.clear();
    const bool attempted = _protectedSession != nullptr;
    if (_thumbnailCancelled) _thumbnailCancelled->store(true);
    _transition = YES;
    ++_epoch;
    _loading = NO;
    self.status.stringValue = _protectedSession ? @"正在释放输入并恢复原图标，请稍候…" : @"正在关闭副本…";
    [self refreshButtons];
    for (dispatch_source_t source in self.fileWatchers) dispatch_source_cancel(source);
    self.fileWatchers = nil;
    [self releaseVolumeWatchers];
    const BOOL hadScene = self.preview.active;
    [self closePassiveScreenWindows];
    [self.preview close:reason];  // releases captured input before any Finder operation
    if (hadScene) {
        NSDictionary* report = self.preview.sessionSummaries.lastObject;
        NSMutableDictionary* compact = [NSMutableDictionary dictionary];
        for (NSString* key in @[
                 @"session_id",
                 @"snapshot_id",
                 @"experiment_id",
                 @"reason",
                 @"bodies",
                 @"image_count",
                 @"grabs",
                 @"releases",
                 @"pauses",
                 @"resets",
                 @"billiards_toggles",
                 @"billiards_shots",
                 @"billiards_pocketed",
                 @"billiards_white_placements",
                 @"billiards_tube_opens",
                 @"billiards_best_shots",
                 @"input_after_close",
                 @"owned_windows_after_close",
                 @"owned_bodies_after_close",
                 @"owned_images_after_close",
                 @"protection",
                 @"events_dropped"
             ])
            if (report[key]) compact[key] = report[key];
        NSData* json = [NSJSONSerialization dataWithJSONObject:compact options:0 error:nil];
        if (json) {
            std::fputs("mirror_session_summary=", stdout);
            std::fwrite(json.bytes, 1, json.length, stdout);
            std::puts("");
        }
    }
    BOOL restored = !_protectedSession || _protectedSession->stop();
    if (attempted)
        [self.preview recordEvent:@"recovery_result"
                          details:@{
                              @"reason" : reason,
                              @"recovery_completed" : @(restored),
                              @"visual_restore" : @"requires_user_confirmation"
                          }];
    if (restored) {
        if (self.appNapActivity) {
            [NSProcessInfo.processInfo endActivity:self.appNapActivity];
            self.appNapActivity = nil;
        }
        _systemSleeping = NO;
        _screenLocked = NO;
        _protectedSession.reset();
        _activeSession = NO;
        self.preview.protectionIdentity = nil;
        self.images = nil;
        self.labels = nil;
        self.activeImages = nil;
        self.activeLabels = nil;
        self.identities = nil;
        self.volumePaths = nil;
        self.mountedRoots = nil;
        self.preview.labels = nil;
        _activeOrigIndices.clear();
        _customIconIndices.clear();
        _snapshot = {};
        _scene = {};
        if (self.scoped) [self.desktopURL stopAccessingSecurityScopedResource];
        self.scoped = NO;
        self.desktopURL = nil;
        self.confirmed.state = NSControlStateValueOff;
        _quickAutoStart = NO;
        [self.hudBar hide];
        [self refreshDisplayScopePopup];
        [self syncLauncherModePills];
        self.panel.title = @"桌面游乐场 · DeskFling";
        if (attempted && !_syntheticCheckMode && ![reason isEqual:@"application_quit"] &&
            ![reason isEqual:@"panel_close"]) {
            [self.panel makeKeyAndOrderFront:nil];
        }
        self.status.stringValue =
            attempted ? @"桌面已安全还原 · 可随时再次点击「▶ 开始游玩」进入游乐场"
                      : @"已停止准备 · 尚未隐藏原图标，无需恢复。";
    } else
        self.status.stringValue = @"恢复未完成，日志已保留。请重试停止/恢复，或运行同目录独立恢复入口。";
    _transition = NO;
    [self refreshButtons];
    std::printf("protected_mirror_stop reason=%s takeover_attempted=%s recovery_completed=%s\n",
                reason.UTF8String, attempted ? "true" : "false", restored ? "true" : "false");
    std::fflush(stdout);
    return restored;
}
- (void)heartbeat {
    if (_transition || _loading) return;
    if (_activeSession && _protectedSession) {
        if (!_protectedSession->tick() || ![self displayMatches] ||
            (!NSApp.active && !self.preview.allowsBackgroundSession)) {
            [self finish:@"configuration_focus_host_or_helper_changed"];
            return;
        }
        if (_protectedSession->isContinuous()) {
            self.panel.title = @"DPP · 详细设置（独立守护运行中 · 按 Esc 随时还原原桌面）";
        } else {
            self.panel.title = [NSString stringWithFormat:@"DPP · 真实桌面实验 · 剩余 %llu 秒",
                                                          _protectedSession->remainingMs() / 1000];
        }
    } else if (self.preview.active && ![self displayMatches])
        [self finish:@"candidate_display_changed"];
}
- (void)play:(id)sender {
    (void)sender;
    [self guard:^{
      if (self->_activeSession && !self->_transition) [self.preview playPause];
    }];
}
- (void)resetScene:(id)sender {
    (void)sender;
    [self guard:^{
      if (self->_activeSession && !self->_transition) [self.preview reset];
    }];
}
- (void)disableInteraction:(id)sender {
    (void)sender;
    [self.preview disableInteraction];
}
- (void)interact:(id)sender {
    (void)sender;
    [self guard:^{
      if (self->_activeSession && !self->_transition) [self.preview toggleInteraction];
    }];
}
- (void)stopSession:(id)sender {
    (void)sender;
    [self guard:^{
      [self finish:@"user_stop"];
    }];
}
- (BOOL)windowShouldClose:(NSWindow*)window {
    (void)window;
    if (_activeSession && !_syntheticCheckMode) {
        // Closing the settings drawer while a session is active simply tucks it back under the capsule/menu
        // bar!
        [self.panel orderOut:nil];
        [self refreshButtons];
        return NO;
    }
    __block BOOL closed = NO;
    [self guard:^{
      closed = [self finish:@"panel_close"];
    }];
    if (closed) {
        [self.hudBar hide];
        if (self.statusItem) {
            [NSStatusBar.systemStatusBar removeStatusItem:self.statusItem];
            self.statusItem = nil;
        }
        [NSNotificationCenter.defaultCenter removeObserver:self];
        [NSWorkspace.sharedWorkspace.notificationCenter removeObserver:self];
        [NSDistributedNotificationCenter.defaultCenter removeObserver:self];
        [self.timer invalidate];
        if (self.escapeMonitor) [NSEvent removeMonitor:self.escapeMonitor];
        self.escapeMonitor = nil;
        if (self.settingsLocalMouseMonitor) [NSEvent removeMonitor:self.settingsLocalMouseMonitor];
        self.settingsLocalMouseMonitor = nil;
        if (self.settingsGlobalMouseMonitor) [NSEvent removeMonitor:self.settingsGlobalMouseMonitor];
        self.settingsGlobalMouseMonitor = nil;
        [NSApp stop:nil];
        [NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                            location:NSZeroPoint
                                       modifierFlags:0
                                           timestamp:0
                                        windowNumber:0
                                             context:nil
                                             subtype:0
                                               data1:0
                                               data2:0]
                 atStart:NO];
    }
    return closed;
}
- (void)applicationDidResignActive:(NSNotification*)notification {
    (void)notification;
    if ([self canAutoDismissModeSettingsCard]) {
        [self guard:^{
          [self closeModeSettingsCard:nil];
        }];
    }
}
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)application {
    (void)application;
    __block BOOL restored = NO;
    [self guard:^{
      restored = [self finish:@"application_quit"];
    }];
    if (restored) {
        [self.hudBar hide];
        if (self.statusItem) {
            [NSStatusBar.systemStatusBar removeStatusItem:self.statusItem];
            self.statusItem = nil;
        }
        [NSNotificationCenter.defaultCenter removeObserver:self];
        if (self.settingsLocalMouseMonitor) [NSEvent removeMonitor:self.settingsLocalMouseMonitor];
        self.settingsLocalMouseMonitor = nil;
        if (self.settingsGlobalMouseMonitor) [NSEvent removeMonitor:self.settingsGlobalMouseMonitor];
        self.settingsGlobalMouseMonitor = nil;
    }
    return restored ? NSTerminateNow : NSTerminateCancel;
}
@end
int auditProtectedMirrorScope() {
    const OSStatus permission = finderPermission(false);
    if (permission != noErr) {
        std::printf("scope_audit_permission_status=%d prompts_requested=false\n", (int)permission);
        return 1;
    }
    NSURL* directory = [NSFileManager.defaultManager URLsForDirectory:NSDesktopDirectory
                                                            inDomains:NSUserDomainMask]
                           .firstObject;
    auto target = [NSAppleEventDescriptor descriptorWithBundleIdentifier:@"com.apple.finder"];
    auto snapshot =
        fullDesktopSnapshot([target](NSAppleEventDescriptor* spec) { return readFinder(target, spec); });
    NSSet* mounts = mountedRootPaths();
    NSUInteger oldMatches = 0, normalizedMatches = 0, localhostURLs = 0, outsideVolumes = 0,
               supportedVolumes = 0;
    for (NSURL* url in snapshot.urls) {
        if ([urlIdentity(url.URLByDeletingLastPathComponent) isEqual:urlIdentity(directory)]) ++oldMatches;
        const BOOL normalized = [fullDesktopURLIdentity(url.URLByDeletingLastPathComponent)
            isEqual:fullDesktopURLIdentity(directory)];
        if (normalized) ++normalizedMatches;
        if ([url.host caseInsensitiveCompare:@"localhost"] == NSOrderedSame && url.host.length)
            ++localhostURLs;
        if (!normalized) {
            NSNumber* volume = nil;
            if ([url getResourceValue:&volume forKey:NSURLIsVolumeKey error:nil] && volume.boolValue)
                ++outsideVolumes;
            if (desktopItemScope(url, directory, mounts, isVolumeRoot(url)) ==
                DesktopItemScope::mountedVolume)
                ++supportedVolumes;
        }
    }
    NSDictionary* report = @{
        @"finder_items" : @(snapshot.urls.count),
        @"legacy_parent_matches" : @(oldMatches),
        @"normalized_parent_matches" : @(normalizedMatches),
        @"localhost_url_items" : @(localhostURLs),
        @"outside_directory_volume_roots" : @(outsideVolumes),
        @"supported_volume_icons" : @(supportedVolumes),
        @"unsupported_items" : @(snapshot.urls.count - normalizedMatches - supportedVolumes),
        @"preferences_written" : @NO,
        @"finder_quit_requested" : @NO,
        @"private_paths_logged" : @NO
    };
    NSData* json = [NSJSONSerialization dataWithJSONObject:report
                                                   options:NSJSONWritingPrettyPrinted
                                                     error:nil];
    requireMirror(json != nil, "scope audit encoding failed");
    std::fwrite(json.bytes, 1, json.length, stdout);
    std::puts("");
    std::printf("桌面项目 %lu 项；原比较匹配 %lu 项，本地路径归一化后匹配 %lu 项；目录外磁盘项目 %lu 项。\n",
                snapshot.urls.count, oldMatches, normalizedMatches, outsideVolumes);
    std::printf("可纳入磁盘副本 %lu 项；无法映射 %lu 项。\n", supportedVolumes,
                snapshot.urls.count - normalizedMatches - supportedVolumes);
    return [mounts isEqualToSet:mountedRootPaths()] &&
                   normalizedMatches + supportedVolumes == snapshot.urls.count
               ? 0
               : 2;
}
int runProtectedMirrorPanel(bool checkOnly) {
    DPPProtectedMirrorPanel* panel = [DPPProtectedMirrorPanel new];
    panel.syntheticCheckMode = checkOnly;
    NSApp.delegate = panel;
    [panel show];
    __block int result = 0;
    if (checkOnly) {
        // Synthetic controls only: no Finder reads, file chooser, session or helper.
        [NSTimer
            scheduledTimerWithTimeInterval:.3
                                   repeats:NO
                                     block:^(NSTimer*) {
                                       result =
                                           panel.panel.visible && !panel.confirmed.hidden &&
                                                   !panel.startButton.hidden && !panel.startButton.enabled &&
                                                   !panel.confirmed.enabled && panel.readButton.enabled &&
                                                   panel.quickStartButton.enabled &&
                                                   !panel.playModePopup.enabled &&
                                                   panel.playModePopup.numberOfItems == 5 &&
                                                   panel.playModePopup.indexOfSelectedItem == 0 &&
                                                   panel.rulePopup.numberOfItems == 3 &&
                                                   panel.matchPopup.numberOfItems == 4 &&
                                                   panel.tableThemePopup.numberOfItems == 3 &&
                                                   panel.tableThemePopup.indexOfSelectedItem == 0 &&
                                                   panel.aiDifficultyPopup.numberOfItems == 4 &&
                                                   panel.aiDifficultyPopup.indexOfSelectedItem == 1 &&
                                                   !panel.aiDifficultyPopup.enabled &&
                                                   !panel.aiIntentToggle.enabled &&
                                                   panel.slingPresetPopup.numberOfItems == 6 &&
                                                   panel.slingEditButton != nil &&
                                                   panel.lanScanButton != nil &&
                                                   panel.saveLayoutButton != nil &&
                                                   !panel.saveLayoutButton.enabled &&
                                                   panel.hockeyMatchPopup.numberOfItems == 3 &&
                                                   panel.hudBar != nil && !panel.hudBar.isVisible &&
                                                   panel.iconSelPopup.numberOfItems == 3 &&
                                                   panel.extraIconsPopup.numberOfItems == 2 &&
                                                   !panel.pickIconsButton.enabled
                                               ? 0
                                               : 1;
                                       [panel.matchPopup selectItemAtIndex:1];
                                       [panel billiardsOptionsChanged:nil];
                                       bool aiPopupOK = panel.aiDifficultyPopup.enabled &&
                                                        panel.aiIntentToggle.enabled &&
                                                        [panel currentBilliardsConfig].matchMode ==
                                                            dpp::BilliardsMatchMode::VsAi &&
                                                        [panel currentBilliardsConfig].aiDifficulty ==
                                                            dpp::BilliardsAiDifficulty::Medium &&
                                                        [panel currentBilliardsConfig].showAiIntent;
                                       panel.aiIntentToggle.state = NSControlStateValueOff;
                                       [panel billiardsOptionsChanged:nil];
                                       aiPopupOK = aiPopupOK && ![panel currentBilliardsConfig].showAiIntent;
                                       panel.aiIntentToggle.state = NSControlStateValueOn;
                                       [panel.aiDifficultyPopup selectItemAtIndex:3];
                                       [panel billiardsOptionsChanged:nil];
                                       aiPopupOK = aiPopupOK && [panel currentBilliardsConfig].aiDifficulty ==
                                                                    dpp::BilliardsAiDifficulty::Goat;
                                       [panel.aiDifficultyPopup selectItemAtIndex:1];
                                       [panel.matchPopup selectItemAtIndex:0];
                                       [panel.tableThemePopup selectItemAtIndex:1];
                                       [panel billiardsOptionsChanged:nil];
                                       aiPopupOK = aiPopupOK && [panel currentBilliardsConfig].tableTheme ==
                                                                    dpp::BilliardsTableTheme::Minimal &&
                                                   ![panel currentBilliardsConfig].showTableChrome;
                                       [panel.tableThemePopup selectItemAtIndex:2];
                                       [panel billiardsOptionsChanged:nil];
                                       aiPopupOK = aiPopupOK && [panel currentBilliardsConfig].tableTheme ==
                                                                    dpp::BilliardsTableTheme::Borderless;
                                       [panel.tableThemePopup selectItemAtIndex:0];
                                       [panel billiardsOptionsChanged:nil];
                                       aiPopupOK = aiPopupOK && [panel currentBilliardsConfig].tableTheme ==
                                                                    dpp::BilliardsTableTheme::Full &&
                                                   [panel currentBilliardsConfig].showTableChrome;
                                       if (!aiPopupOK || panel.aiDifficultyPopup.enabled ||
                                           panel.aiIntentToggle.enabled)
                                           result = 1;
                                       std::printf("mirror_panel_initial_gates_passed=%s "
                                                   "desktop_session_started=false\n",
                                                   result ? "false" : "true");
                                       BOOL labelsOK = panel.filenameToggle.enabled &&
                                                       panel.filenameToggle.state == NSControlStateValueOn &&
                                                       panel.preview.showsFilenames;
                                       panel.filenameToggle.state = NSControlStateValueOff;
                                       [panel filenameVisibilityChanged:nil];
                                       labelsOK = labelsOK && !panel.preview.showsFilenames;
                                       panel.filenameToggle.state = NSControlStateValueOn;
                                       [panel filenameVisibilityChanged:nil];
                                       labelsOK = labelsOK && panel.preview.showsFilenames &&
                                                  !panel.preview.active && !panel.startButton.enabled;
                                       if (!labelsOK) result = 1;
                                       std::printf("filename_toggle_panel_passed=%s\n",
                                                   labelsOK ? "true" : "false");
                                       NSOpenPanel* cancelled = NSOpenPanel.openPanel;
                                       panel.desktopChooser = cancelled;
                                       [panel refreshButtons];
                                       const bool blocked = !panel.readButton.enabled;
                                       [panel completeDesktopChoice:cancelled response:NSModalResponseCancel];
                                       const bool cancelledOK =
                                           blocked && panel.readButton.enabled && !panel.desktopChooser &&
                                           !panel.preview.active &&
                                           [panel.status.stringValue containsString:@"已取消选择"];
                                       [panel completeDesktopChoice:cancelled response:NSModalResponseOK];
                                       const bool staleOK =
                                           !panel.preview.active &&
                                           [panel.status.stringValue containsString:@"已取消选择"];
                                       if (!cancelledOK || !staleOK) result = 1;
                                       std::printf("desktop_choice_cancel_and_stale_passed=%s\n",
                                                   cancelledOK && staleOK ? "true" : "false");
                                       [panel guard:^{
                                         throw std::runtime_error("中文范围检查合成失败");
                                       }];
                                       const bool accurate = [panel.status.stringValue
                                                                 containsString:@"中文范围检查合成失败"] &&
                                                             [panel.status.stringValue
                                                                 containsString:@"尚未隐藏原图标，无需恢复"];
                                       if (!accurate) result = 1;
                                       std::printf("pre_takeover_failure_message_passed=%s\n",
                                                   accurate ? "true" : "false");
                                       const bool invalidated = [panel checkSyntheticVolumeChanges];
                                       if (!invalidated) result = 1;
                                       std::printf("synthetic_volume_change_invalidation_passed=%s\n",
                                                   invalidated ? "true" : "false");
                                       [panel windowShouldClose:panel.panel];
                                       [panel.panel orderOut:nil];
                                     }];
    }
    [NSApp run];
    NSApp.delegate = nil;
    return result;
}
