#include "platforms/macos/mirror_preview.h"
#import <QuartzCore/QuartzCore.h>
#include "platforms/display_migration.h"
#include "platforms/filename_geometry.h"
#include "platforms/macos/assisted_experiment.h"
#include "platforms/macos/billiards_audio.h"
#include "platforms/macos/billiards_lan.h"
#include "platforms/macos/cached_draw_renderer.h"
#include "platforms/macos/display_guard.h"
#include "platforms/macos/native_measurement.h"
#include "platforms/macos/session_trace.h"
#include "platforms/mirror_hit.h"
#ifndef DPP_BUILD_ID
#define DPP_BUILD_ID "synthetic-fixture-build"
#endif
using namespace dpp;
using namespace dpp::mac;
static NSString* mirrorJournalKey() {
#ifdef DPP_MIRROR_FAULT_TESTING
    return @"DPPMirrorFixtureSessionsJSONV1";
#else
    return @"DPPMirrorSessionsJSONV1";
#endif
}
static double mirrorTime() { return NSProcessInfo.processInfo.systemUptime; }
static void activatePreviewApp() {
    if (@available(macOS 14.0, *)) {
        [NSApp activate];
    } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        [NSApp activateIgnoringOtherApps:YES];
#pragma clang diagnostic pop
    }
}
@interface DPPMirrorWindow : NSWindow
@property BOOL interactive;
@end
@implementation DPPMirrorWindow
- (BOOL)canBecomeKeyWindow {
    return self.interactive;
}
- (BOOL)canBecomeMainWindow {
    return NO;
}
@end
@class DPPMirrorView;
@interface DPPMirrorPreview () {
    MirrorSession _session;
    DisplayMigration _migration;
    MirrorDisplay _migrationDisplay;
    NSTimer* _migrationTimer;
    NSArray* _cachedImages;
    double _committedAt;
    uint64_t _nativeMismatchRevision, _preparedRevision;
    NSString* _migrationStage;
    std::unique_ptr<CachedDrawRenderer> _migrationRenderer;
    CALayer* _migrationLayer;
#ifdef DPP_MIRROR_FAULT_TESTING
    BOOL _fixtureDisplayEnabled;
    MirrorDisplay _fixtureDisplay;
#endif

    std::unique_ptr<CachedDrawRenderer> _renderer;
    double _previous;
    NSUInteger _grabs, _releases, _pauses;
    NSUInteger _ticks, _inputs, _starts, _closes, _resets, _externalSwitches, _panelSwitches;
    NSUInteger _labelStringWrites, _labelScaleWrites, _labelGeometryWrites;
    BOOL _labelContentDirty;
    BOOL _menuActive, _menuSelected, _openSubmitted;
    BOOL _returnAfterOpen, _desktopReady;
    BOOL _directDisabled, _userPaused;
    BOOL _pointerGestureActive;
    double _wakeActivationDeadline;
    NSEvent* _lastDown;
    MirrorDisplay _capturedDisplay;
    NSRect _fixedWindowFrame, _fixedContentBounds, _fixedBacking;
    BOOL _geometryReady;
    NSUInteger _traceSequence, _traceDropped;
    double _initialPreviewDeadline;
    NSMenu* _openMenu;
    CGFloat _labelContentsScale;
    std::vector<uint32_t> _labelTextures;
    float _toyBlackHoleScale, _toyBlackHoleElevation, _toyBlackHoleTilt;
    BOOL _toyBlackHoleDevour;
    float _pendingGravityMagnitude, _pendingGravityRestitution;
    GravityDirection _pendingGravityDirection;
    NSUInteger _billiardsToggles, _billiardsTubeOpens;
    int _loggedBilliardsShots, _loggedBilliardsPocketed, _loggedBilliardsPlacements;
    BilliardsConfig _pendingBilliardsConfig;
    SlingshotConfig _pendingSlingshotConfig;
    SlingshotProgress _pendingSlingshotProgress;
    int _pendingSlingshotHighScore;
    AirHockeyConfig _pendingAirHockeyConfig;
    unsigned int _p2KeyMask;
    BOOL _systemSuspended;
    id _globalMoveMonitor;
    double _lastImmediateRenderAt;
    BilliardsAudio _billiardsAudio;
    BilliardsLan _billiardsLan;
    double _lastLanSnapSent;
    std::vector<SoundEvent> _soundBuf;
    std::vector<SoundEvent> _lanSoundBuf;
    std::vector<FilenamePeer> _filenamePeers;
    std::vector<std::unique_ptr<CachedDrawRenderer>> _extraRenderers;
    DrawList _viewportScratchDraw;
}
@property(strong) DPPMirrorWindow* window;
@property(strong) DPPMirrorView* view;
@property(strong) NSMutableArray<DPPMirrorWindow*>* extraWindows;
@property(strong) NSMutableArray<DPPMirrorView*>* extraViews;
@property(strong) NSMutableArray<NSMutableArray<CATextLayer*>*>* extraLabelLayers;
@property(strong) NSTimer* timer;
@property(strong) NSTimer* deadline;
@property(strong) NSMutableArray* traceEvents;
@property(copy) NSDictionary* lastReport;
@property(copy, readwrite) NSDictionary* startupFailure;
@property(copy) NSString* reason;
@property(strong) NSMutableArray* summaries;
@property(strong) NSMutableArray* interactionIntervals;
@property(copy) NSString* sessionID;
@property(copy) NSString* startedAt;
@property(copy) NSString* interactionStart;
@property(strong) NSMutableArray<CATextLayer*>* labelLayers;
- (NSDictionary*)currentProtection:(BOOL)requestHelper;
- (void)pointer:(NSEvent*)event type:(InputType)type;
- (void)hover:(NSEvent*)event;
- (void)recordBilliardsProgress;
- (void)flushSounds;
- (void)syncLanRole;
- (void)syncLanOutbound:(const std::vector<SoundEvent>&)sounds force:(BOOL)force;
- (BOOL)acceptsCurrentEvent:(NSEvent*)event;
- (void)handleResetShortcut;
- (void)handleP2Key:(NSEvent*)event pressed:(BOOL)pressed;
- (void)contextMenu:(NSEvent*)event;
- (void)guard:(void (^)(void))operation;
- (void)releaseInteraction;
- (void)wakeForDesktopEvent:(NSEvent*)event;
@end
@interface DPPMirrorView : NSView {
    NSTrackingArea* _trackingArea;
}
@property(weak) DPPMirrorPreview* owner;
@property NSPoint sceneOriginOffset;
@end
@implementation DPPMirrorView
- (BOOL)isFlipped {
    return YES;
}
- (BOOL)acceptsFirstResponder {
    return YES;
}
- (BOOL)acceptsFirstMouse:(NSEvent*)event {
    (void)event;
    return self.owner.desktopReady || self.owner.continuousSession;
}
- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (_trackingArea) [self removeTrackingArea:_trackingArea];
    _trackingArea = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:NSTrackingMouseMoved | NSTrackingActiveAlways | NSTrackingInVisibleRect
               owner:self
            userInfo:nil];
    [self addTrackingArea:_trackingArea];
}
- (void)mouseMoved:(NSEvent*)event {
    [self.owner hover:event];
}
- (void)mouseDown:(NSEvent*)event {
    [self.owner pointer:event type:InputType::PointerDown];
}
- (void)rightMouseDown:(NSEvent*)event {
    [self.owner contextMenu:event];
}
- (void)mouseDragged:(NSEvent*)event {
    [self.owner pointer:event type:InputType::PointerMove];
}
- (void)mouseUp:(NSEvent*)event {
    [self.owner pointer:event type:InputType::PointerUp];
}
- (void)keyDown:(NSEvent*)event {
    if (![self.owner acceptsCurrentEvent:event]) return;
    [self.owner guard:^{
      const NSEventModifierFlags mods =
          event.modifierFlags &
          (NSEventModifierFlagCommand | NSEventModifierFlagControl | NSEventModifierFlagOption);
      if (event.keyCode == 53 && mods == 0)
          [self.owner close:@"escape_while_key"];
      else if (event.keyCode == 48 && mods == 0 && !event.isARepeat && self.owner.onToggleHudShortcut)
          self.owner.onToggleHudShortcut();
      else if (mods == 0 && !event.isARepeat && [event.charactersIgnoringModifiers.lowercaseString isEqual:@"r"])
          [self.owner handleResetShortcut];
      else if (mods == 0 && !event.isARepeat && [event.charactersIgnoringModifiers isEqual:@" "])
          [self.owner playPause];
      else
          [self.owner handleP2Key:event pressed:YES];
    }];
}
- (void)keyUp:(NSEvent*)event {
    if (![self.owner acceptsCurrentEvent:event]) return;
    [self.owner guard:^{
      [self.owner handleP2Key:event pressed:NO];
    }];
}
@end
@implementation DPPMirrorPreview
- (instancetype)init {
    self = [super init];
    if (self) {
        _reason = @"not_started";
        _showsFilenames = YES;
        _toyBlackHoleScale = 1;
        _toyBlackHoleElevation = BlackHoleState::kDefaultElevation;
        _toyBlackHoleTilt = 0;
        _pendingGravityMagnitude = kEarthGravityMps2;
        _pendingGravityDirection = GravityDirection::Down;
        _pendingGravityRestitution = kDefaultGravityRestitution;
        _pendingBilliardsConfig = BilliardsConfig::desktop();
        _lastLanSnapSent = 0;
        __weak DPPMirrorPreview* weakSelf = self;
        _billiardsLan.onStatus = [weakSelf](BilliardsLan::State st, const std::string& text) {
            DPPMirrorPreview* strong = weakSelf;
            if (!strong) return;
            [strong guard:^{
              [strong syncLanRole];
              if (st == BilliardsLan::State::ConnectedHost && strong->_session.game() &&
                  strong->_session.game()->billiardsEnabled()) {
                  strong->_billiardsLan.sendRuleSync(strong->_session.game()->billiardsConfig().rule);
                  [strong syncLanOutbound:{} force:YES];
              }
              if (strong->_session.game() && strong->_session.game()->billiardsEnabled()) {
                  strong->_session.game()->forceRedraw();
                  [strong render];
              }
              if (strong.lanStatusChanged) {
                  NSString* ns = [NSString stringWithUTF8String:text.c_str()];
                  const BOOL activeOrWait =
                      (st != BilliardsLan::State::Disconnected || strong->_billiardsLan.reconnecting());
                  const BOOL hostRole =
                      (st == BilliardsLan::State::HostingWaiting || st == BilliardsLan::State::ConnectedHost);
                  strong.lanStatusChanged(ns ? ns : @"", activeOrWait, hostRole);
              }
              if (st == BilliardsLan::State::ConnectedHost && strong->_session.game() &&
                  strong->_session.game()->airHockeyEnabled()) {
                  [strong syncLanOutbound:{} force:YES];
              }
            }];
        };
        _billiardsLan.onRoomsUpdated = [weakSelf](const std::vector<lan_codec::DiscoveredLanRoom>& rooms) {
            DPPMirrorPreview* strong = weakSelf;
            if (!strong || !strong.lanRoomsUpdated) return;
            NSMutableArray<NSString*>* arr = [NSMutableArray arrayWithCapacity:rooms.size()];
            for (const auto& rm : rooms) {
                NSString* s = [NSString stringWithUTF8String:rm.displayLabel().c_str()];
                if (s) [arr addObject:s];
            }
            strong.lanRoomsUpdated(arr);
        };
        _billiardsLan.onRuleSync = [weakSelf](BilliardsRule rule) {
            DPPMirrorPreview* strong = weakSelf;
            if (!strong) return;
            [strong guard:^{
              if (!strong->_session.game()) return;
              if (strong->_session.game()->billiardsEnabled()) {
                  strong->_pendingBilliardsConfig = strong->_session.game()->billiardsConfig();
              }
              strong->_pendingBilliardsConfig.rule = rule;
              strong->_pendingBilliardsConfig.matchMode = BilliardsMatchMode::Lan;
              if (rule != BilliardsRule::Free) strong->_pendingBilliardsConfig.freeModeMaxRackBalls = 0u;
              strong->_session.game()->setBilliardsConfig(strong->_pendingBilliardsConfig);
              [strong syncLanRole];
              [strong render];
            }];
        };
        _billiardsLan.onAction = [weakSelf](const BilliardsRemoteAction& act) {
            DPPMirrorPreview* strong = weakSelf;
            if (!strong) return;
            [strong guard:^{
              if (!strong.active || strong.migrating || !strong->_session.game() ||
                  !strong->_session.game()->billiardsEnabled())
                  return;
              if (auto* bm = strong->_session.game()->mutableBilliardsMode()) {
                  bm->applyRemoteLanAction(act);
                  strong->_session.game()->forceRedraw();
                  [strong render];
                  [strong flushSounds];
                  [strong recordBilliardsProgress];
                  strong->_previous = mirrorTime();
                  strong.timer.fireDate = NSDate.date;
              }
            }];
        };
        _billiardsLan.onSnapshot = [weakSelf](const BilliardsLanSnapshot& snap,
                                              const std::vector<SoundEvent>& sounds) {
            DPPMirrorPreview* strong = weakSelf;
            if (!strong) return;
            [strong guard:^{
              if (!strong.active || strong.migrating || !strong->_session.game() ||
                  !strong->_session.game()->billiardsEnabled())
                  return;
              if (auto* bm = strong->_session.game()->mutableBilliardsMode()) {
                  bm->applyLanSnapshot(snap);
                  strong->_session.game()->forceRedraw();
                  [strong render];
                  if (!sounds.empty() && !strong->_soundMuted) {
                      strong->_billiardsAudio.play(sounds, false);
                  }
                  [strong recordBilliardsProgress];
              }
            }];
        };
        _billiardsLan.onHockeyInput = [weakSelf](const AirHockeyRemoteInput& in) {
            DPPMirrorPreview* strong = weakSelf;
            if (!strong) return;
            [strong guard:^{
              if (!strong.active || strong.migrating || !strong->_session.game() ||
                  !strong->_session.game()->airHockeyEnabled())
                  return;
              if (auto* ah = strong->_session.game()->mutableAirHockeyMode()) {
                  ah->applyRemoteInput(in);
                  strong->_previous = mirrorTime();
                  strong.timer.fireDate = NSDate.date;
              }
            }];
        };
        _billiardsLan.onHockeyState = [weakSelf](const AirHockeyLanState& st,
                                                 const std::vector<SoundEvent>& sounds) {
            DPPMirrorPreview* strong = weakSelf;
            if (!strong) return;
            [strong guard:^{
              if (!strong.active || strong.migrating || !strong->_session.game() ||
                  !strong->_session.game()->airHockeyEnabled())
                  return;
              if (auto* ah = strong->_session.game()->mutableAirHockeyMode()) {
                  ah->applyLanState(st);
                  strong->_session.game()->forceRedraw();
                  [strong render];
                  if (!sounds.empty() && !strong->_soundMuted) {
                      strong->_billiardsAudio.play(sounds, false);
                  }
              }
            }];
        };
        NSData* saved = [NSUserDefaults.standardUserDefaults dataForKey:mirrorJournalKey()];
        id decoded = saved ? [NSJSONSerialization JSONObjectWithData:saved options:0 error:nil] : nil;
        _summaries = [decoded isKindOfClass:NSArray.class] ? [decoded mutableCopy] : [NSMutableArray array];
        while (_summaries.count > 32) [_summaries removeObjectAtIndex:0];
        [NSNotificationCenter.defaultCenter addObserver:self
                                               selector:@selector(appResigned:)
                                                   name:NSApplicationDidResignActiveNotification
                                                 object:nil];
        [NSNotificationCenter.defaultCenter addObserver:self
                                               selector:@selector(appDidBecomeActive:)
                                                   name:NSApplicationDidBecomeActiveNotification
                                                 object:nil];
        [NSNotificationCenter.defaultCenter addObserver:self
                                               selector:@selector(configurationChanged:)
                                                   name:NSApplicationDidChangeScreenParametersNotification
                                                 object:nil];
        for (NSString* name in @[
                 NSWorkspaceActiveSpaceDidChangeNotification, NSWorkspaceWillSleepNotification,
                 NSWorkspaceDidWakeNotification
             ])
            [NSWorkspace.sharedWorkspace.notificationCenter addObserver:self
                                                               selector:@selector(configurationChanged:)
                                                                   name:name
                                                                 object:nil];
    }
    return self;
}
- (void)guard:(void (^)(void))operation {
    @try {
        try {
            operation();
        } catch (const std::exception& error) {
            if (_migration.active()) {
                // Diagnostic failure must never interrupt the existing close/recovery path.
                @try {
                    try {
                        [self recordMigrationFailure:error];
                    } catch (...) {
                    }
                } @catch (NSException*) {
                }
            }
            [self close:@"native_callback_failed"];
        } catch (...) {
            [self close:@"native_callback_failed"];
        }
    } @catch (NSException*) {
        [self close:@"native_exception"];
    }
}
- (void)recordMigrationFailure:(const std::exception&)error {
    const auto* frozen = _migration.frozenScene();
    if (!frozen) return;
    auto number = [](float value) -> id { return std::isfinite(value) ? @(value) : (id)NSNull.null; };
    auto vector = [&](Vec2 value) -> NSArray* { return @[ number(value.x), number(value.y) ]; };
    NSMutableArray* icons = [NSMutableArray array];
    for (size_t i = 0; i < frozen->setup.icons.size() && i < 128; ++i) {
        const auto& icon = frozen->setup.icons[i];
        const float rotation =
            i < frozen->bodies.size() ? SceneToPhysics::angleToScene(frozen->bodies[i].angle) : 0;
        [icons addObject:@{
            @"index" : @(i),
            @"center" : vector(icon.center),
            @"size" : vector(icon.iconSize),
            @"rotation" : number(rotation),
            @"reset_center" : i < frozen->resetSetup.icons.size() ? vector(frozen->resetSetup.icons[i].center)
                                                                  : (id)NSNull.null
        }];
    }
    NSString* stage = [_migrationStage isEqual:@"candidate_prepare"]
                          ? [NSString stringWithUTF8String:migrationStageName(_migration.preparationStage())]
                          : (_migrationStage ? _migrationStage : @"unknown");
    [self recordEvent:@"display_migration_failed"
              details:@{
                  @"stage" : stage,
                  @"reason" : [NSString stringWithUTF8String:migrationFailureCategory(error)],
                  @"revision" : @(_migration.revision()),
                  @"generation" : @(_migration.generation()),
                  @"old_work_size" : vector(frozen->setup.workAreaSize),
                  @"new_work_size" :
                      @[ @(_migrationDisplay.work.size.width), @(_migrationDisplay.work.size.height) ],
                  @"user_paused" : @(frozen->userPaused),
                  @"frozen_icons" : icons
              }];
}
- (MirrorDisplay)sampledDisplay {
#ifdef DPP_MIRROR_FAULT_TESTING
    if (_fixtureDisplayEnabled) return _fixtureDisplay;
#endif
    return _multiScreenMode ? mirrorDisplayAt(_targetScreenIndex) : mirrorDisplay();
}
#ifdef DPP_MIRROR_FAULT_TESTING
- (void)fixtureMigrationWork:(NSRect)work scale:(CGFloat)scale {
    _fixtureDisplay = _capturedDisplay;
    _fixtureDisplay.work = work;
    _fixtureDisplay.scale = scale;
    _fixtureDisplayEnabled = YES;
    [self configurationChanged:[NSNotification
                                   notificationWithName:NSApplicationDidChangeScreenParametersNotification
                                                 object:nil]];
}
- (void)fixtureAdvanceMigration {
    [self advanceMigration];
}
#endif
- (BOOL)migrating {
    return _migration.active();
}
- (BOOL)samePhysicalDisplayRespectingMulti:(const MirrorDisplay&)a with:(const MirrorDisplay&)b {
    if (_multiScreenMode) {
        return validMirrorDisplayAnyCount(a) && validMirrorDisplayAnyCount(b) && a.count == b.count &&
               [a.id isEqual:b.id] && NSEqualRects(a.frame, b.frame) && a.scale == b.scale;
    }
    return samePhysicalDisplay(a, b);
}
- (BOOL)sameDisplayIgnoringMinorWorkJitter:(const MirrorDisplay&)d {
    if (![self samePhysicalDisplayRespectingMulti:_capturedDisplay with:d] || ![self fixedSceneGeometryValid])
        return NO;
    if (NSEqualRects(_capturedDisplay.work, d.work)) return YES;
#ifdef DPP_MIRROR_FAULT_TESTING
    if (_fixtureDisplayEnabled) return NO;
#endif
    // Ignore minor Dock magnification / bounce / auto-hide or Menu Bar work-area jitter
    // on the exact same physical display resolution and Retina scale.
    const CGFloat dx = std::abs(d.work.origin.x - _capturedDisplay.work.origin.x);
    const CGFloat dy = std::abs(d.work.origin.y - _capturedDisplay.work.origin.y);
    const CGFloat dw = std::abs(d.work.size.width - _capturedDisplay.work.size.width);
    const CGFloat dh = std::abs(d.work.size.height - _capturedDisplay.work.size.height);
    return dx <= 96.0 && dy <= 96.0 && dw <= 96.0 && dh <= 96.0;
}
- (BOOL)committedDisplayMatches {
    const auto d = [self sampledDisplay];
    return [self sameDisplayIgnoringMinorWorkJitter:d];
}
- (BOOL)checkDisplayForMigration {
    if (!self.active) return NO;
    if (self.migrating) return YES;
    if (self.committedDisplayMatches) return YES;
    [self configurationChanged:[NSNotification
                                   notificationWithName:NSApplicationDidChangeScreenParametersNotification
                                                 object:nil]];
    return self.active && (self.migrating || self.committedDisplayMatches);
}
- (void)advanceMigration {
    [self guard:^{
      if (!self.active || !self->_migration.active()) return;
      const double now = mirrorTime();
      if (self->_migration.expired(now) || !self.deadline.valid ||
          self.deadline.fireDate.timeIntervalSinceNow <= 0 ||
          (self.migrationProtectionValid && !self.migrationProtectionValid())) {
          [self close:@"migration_deadline_or_recovery"];
          return;
      }
      const auto current = [self sampledDisplay];
      if (!validMirrorDisplay(current) || ![current.id isEqual:self->_capturedDisplay.id]) {
          [self close:@"migration_unsupported_display"];
          return;
      }
      if (!samePhysicalDisplay(current, self->_migrationDisplay) ||
          !NSEqualRects(current.work, self->_migrationDisplay.work)) {
          self->_migrationDisplay = current;
          self->_migration.changed(now);
          self->_migrationRenderer.reset();
          self->_migrationLayer = nil;
          return;
      }
      if (!self->_migration.settled(now)) return;
      const auto token = self->_migration.revision();
      const NSRect local = NSMakeRect(0, 0, current.work.size.width, current.work.size.height);
      if (self->_preparedRevision != token || !self->_migration.candidate()) {
          self->_migrationStage = @"candidate_prepare";
          self->_migration.prepare(
              {static_cast<float>(current.work.size.width), static_cast<float>(current.work.size.height)});
          self->_migrationStage = @"cached_render_prepare";
          // Detached layers use cached images; retain them while waiting for native backing.
          self->_migrationRenderer = std::make_unique<CachedDrawRenderer>(self->_cachedImages);
          self->_migrationLayer = [CALayer layer];
          self->_migrationLayer.frame = local;
          self->_migrationLayer.geometryFlipped = self.view.layer.geometryFlipped;
          self->_migrationLayer.contentsScale = current.scale;
          const bool migrationOutline = ![self->_experimentID isEqual:@"protected_finder"] &&
                                        !self.allowsBackgroundSession &&
                                        !self->_migration.candidate()->billiardsEnabled();
          self->_migrationRenderer->render(self->_migration.candidate()->drawList(), self->_migrationLayer,
                                           local, current.scale, migrationOutline);
          self->_preparedRevision = token;
      }
      self->_migrationStage = @"native_window_prepare";
      [self.window setFrame:current.work display:NO];
      [self.view setFrame:local];
      const auto verified = [self sampledDisplay];
      if (!samePhysicalDisplay(current, verified) || !NSEqualRects(current.work, verified.work)) {
          self->_migrationDisplay = verified;
          self->_migration.changed(mirrorTime());
          self->_migrationRenderer.reset();
          self->_migrationLayer = nil;
          return;
      }
      if (token != self->_migration.revision() || !self.active) return;
      self->_migrationStage = @"native_window_verification";
      const NSRect backing = [self.view convertRectToBacking:self.view.bounds];
      const bool nativeValid = NSEqualRects(self.window.frame, current.work) &&
                               NSEqualRects(self.view.bounds, local) &&
                               self.window.backingScaleFactor == current.scale &&
                               [self.window.screen.deviceDescription[@"NSScreenNumber"] isEqual:current.id] &&
                               backing.size.width == local.size.width * current.scale &&
                               backing.size.height == local.size.height * current.scale;
      if (!nativeValid) {
          if (self->_nativeMismatchRevision != token) {
              self->_nativeMismatchRevision = token;
              logDisplayDecision(
                  current, verified, self.window,
                  @{@"source" : @"migration_native_verification",
                    @"revision" : @(token)},
                  false);
          }
          return;
      }  // bounded retry while AppKit finishes its backing transition
      const bool protectedValid = self.deadline.valid && self.deadline.fireDate.timeIntervalSinceNow > 0 &&
                                  (!self.migrationProtectionValid || self.migrationProtectionValid());
      self->_migrationStage = @"commit";
      auto candidate = self->_migration.commit(token, mirrorTime(), protectedValid);
      if (!candidate) {
          [self close:@"migration_commit_invalidated"];
          return;
      }
      [CATransaction begin];
      [CATransaction setDisableActions:YES];
      self->_session.commitMigration(std::move(candidate));
      [self syncLanRole];
      self.view.layer = self->_migrationLayer;
      self->_migrationLayer = nil;
      self->_renderer = std::move(self->_migrationRenderer);
      self->_labelLayers = nil;
      self->_labelTextures.clear();
      self->_labelContentDirty = YES;
      self->_capturedDisplay = current;
      self->_fixedWindowFrame = current.work;
      self->_fixedContentBounds = local;
      self->_fixedBacking = backing;
      self->_geometryReady = YES;
      self->_committedAt = mirrorTime();
      self->_previous = self->_committedAt;
      [self render];
      [CATransaction commit];
      [self->_migrationTimer invalidate];
      self->_migrationTimer = nil;
      if (self.displayCommitted) self.displayCommitted();
      [self armDesktopReturn];
      [self recordEvent:@"display_migration_committed"
                details:@{
                    @"generation" : @(self->_migration.generation())
                }];
    }];
}
- (void)beginDisplayMigration:(const MirrorDisplay&)display {
    if (!validMirrorDisplay(display) || ![display.id isEqual:_capturedDisplay.id]) {
        [self close:@"migration_unsupported_display"];
        return;
    }
    if (!_migration.active()) {
        [_openMenu cancelTracking];
        _menuSelected = NO;
        _openSubmitted = NO;
        [self releaseInteraction];
        _returnAfterOpen = NO;
        _migrationStage = @"freeze";
        _billiardsAudio.stop();
        _migration.begin(*_session.game(), mirrorTime());
        _view.layer.hidden = YES;
        [self recordEvent:@"display_migration_started" details:@{@"revision" : @(_migration.revision())}];
        __weak DPPMirrorPreview* weak = self;
        _migrationTimer = [NSTimer timerWithTimeInterval:.05
                                                 repeats:YES
                                                   block:^(NSTimer*) {
                                                     [weak advanceMigration];
                                                   }];
        [NSRunLoop.mainRunLoop addTimer:_migrationTimer forMode:NSRunLoopCommonModes];
    }
    if (!samePhysicalDisplay(display, _migrationDisplay) ||
        !NSEqualRects(display.work, _migrationDisplay.work)) {
        _migrationDisplay = display;
        _migration.changed(mirrorTime());
        _migrationRenderer.reset();
        _migrationLayer = nil;
    }
}
- (BOOL)fixedSceneGeometryValid {
    return _geometryReady && self.active && _deadline.valid && _deadline.fireDate.timeIntervalSinceNow > 0 &&
           _session.game() && _renderer && _window && _window.contentView == _view &&
           NSEqualRects(_window.frame, _fixedWindowFrame) && NSEqualRects(_view.frame, _fixedContentBounds) &&
           NSEqualRects(_view.bounds, _fixedContentBounds) &&
           NSEqualRects([_view convertRectToBacking:_view.bounds], _fixedBacking) &&
           _window.backingScaleFactor == _capturedDisplay.scale &&
           [_window.screen.deviceDescription[@"NSScreenNumber"] isEqual:_capturedDisplay.id];
}
- (BOOL)active {
    return _session.phase() != MirrorPhase::Closed;
}
- (BOOL)start:(const MirrorSnapshot&)snapshot images:(NSArray*)images {
    self.startupFailure = nil;
    // Reject before allocating IDs, scene, cache, window, timers or journal entries.
    if (_experimentID.length && ![_experimentID isEqual:@"ordinary_mirror"] &&
        !(snapshot.fullDesktop && [_experimentID isEqual:@"protected_finder"]) &&
        !assistedCandidateAvailable()) {
        _reason = [NSString stringWithUTF8String:assistedUnavailableReason];
        return NO;
    }
    [self close:@"replaced"];
    // A new preview owns no protection until the panel explicitly binds its new session.
    self.protectionIdentity = nil;
    _traceEvents = [NSMutableArray array];
    _traceSequence = _traceDropped = 0;
    _initialPreviewDeadline = 0;
    _geometryReady = NO;
    _openSubmitted = NO;
    _allowsBackgroundSession = NO;
    _returnAfterOpen = _desktopReady = NO;
    _directDisabled = _userPaused = NO;
    _pointerGestureActive = NO;
    _wakeActivationDeadline = 0;
    _lastDown = nil;
    _committedAt = mirrorTime();
    _sessionID = NSUUID.UUID.UUIDString;
    _startedAt = [[[NSISO8601DateFormatter alloc] init] stringFromDate:NSDate.date];
    _interactionIntervals = [NSMutableArray array];
    _interactionStart = nil;
    _grabs = _releases = _pauses = 0;
    _ticks = _inputs = _resets = _externalSwitches = _panelSwitches = 0;
    _labelStringWrites = _labelScaleWrites = _labelGeometryWrites = 0;
    _billiardsToggles = _billiardsTubeOpens = 0;
    _loggedBilliardsShots = _loggedBilliardsPocketed = _loggedBilliardsPlacements = 0;
    NSString* stage = @"input_cache";
    NSString* failure = nil;
    @try {
        try {
            if (images.count != snapshot.items.size()) throw std::invalid_argument("missing image cache");
            if (snapshot.fullDesktop && _labels.count != images.count)
                throw std::invalid_argument("missing full desktop labels");
            stage = @"scene_validation_and_physics";
            _session.start(snapshot, mirrorTime());
            _session.game()->setMuted(_soundMuted);
            _session.game()->setGravitySettings(_pendingGravityMagnitude, _pendingGravityDirection,
                                                _pendingGravityRestitution);
            _session.game()->setBilliardsConfig(_pendingBilliardsConfig);
            _session.game()->setSlingshotConfig(_pendingSlingshotConfig);
            _session.game()->setSlingshotHighScore(_pendingSlingshotHighScore);
            _session.game()->setAirHockeyConfig(_pendingAirHockeyConfig);
            std::printf("mirror_startup_layout preview_session_id=%s edge_inset_count=%zu "
                        "original_icons_modified=false\n",
                        _sessionID.UTF8String, _session.startupInsetCount());
            std::fflush(stdout);
            _capturedDisplay = [self sampledDisplay];
#ifdef DPP_MIRROR_FAULT_TESTING
            if (_failAfterStage == 1) throw std::runtime_error("fixture after scene");
#endif
            stage = @"image_renderer";
            _cachedImages = [images copy];
            _renderer = std::make_unique<CachedDrawRenderer>(images);
#ifdef DPP_MIRROR_FAULT_TESTING
            if (_failAfterStage == 2) throw std::runtime_error("fixture after cache");
#endif
            stage = @"native_window";
            const BOOL spanMultiViewports = (_multiScreenViewports.count > 1);
            NSRect frame = spanMultiViewports ? _multiScreenViewports.firstObject.rectValue
                                              : NSMakeRect(snapshot.workArea.x, snapshot.workArea.y,
                                                           snapshot.workArea.width, snapshot.workArea.height);
            auto calcSceneOffset = [&](NSRect vpRect) -> NSPoint {
                const CGFloat ox = vpRect.origin.x - snapshot.workArea.x;
                const CGFloat oy =
                    (snapshot.workArea.y + snapshot.workArea.height) - (vpRect.origin.y + vpRect.size.height);
                return NSMakePoint(ox, oy);
            };
            _window = [[DPPMirrorWindow alloc] initWithContentRect:frame
                                                         styleMask:NSWindowStyleMaskBorderless
                                                           backing:NSBackingStoreBuffered
                                                             defer:NO];
            _window.releasedWhenClosed = NO;
            _window.delegate = self;
            _window.opaque = NO;
            _window.backgroundColor = NSColor.clearColor;
            _window.hasShadow = NO;
            _window.ignoresMouseEvents = YES;
            _window.acceptsMouseMovedEvents = YES;
            _window.level = CGWindowLevelForKey(kCGDesktopIconWindowLevelKey) + 1;
            _window.collectionBehavior =
                NSWindowCollectionBehaviorStationary | NSWindowCollectionBehaviorFullScreenNone;
            _view =
                [[DPPMirrorView alloc] initWithFrame:NSMakeRect(0, 0, frame.size.width, frame.size.height)];
            _view.owner = self;
            _view.sceneOriginOffset = spanMultiViewports ? calcSceneOffset(frame) : NSZeroPoint;
            _view.wantsLayer = YES;
            _window.contentView = _view;
            _extraWindows = [NSMutableArray array];
            _extraViews = [NSMutableArray array];
            _extraLabelLayers = [NSMutableArray array];
            _extraRenderers.clear();
            if (spanMultiViewports) {
                for (NSUInteger vi = 1; vi < _multiScreenViewports.count; ++vi) {
                    const NSRect vf = _multiScreenViewports[vi].rectValue;
                    DPPMirrorWindow* ew =
                        [[DPPMirrorWindow alloc] initWithContentRect:vf
                                                           styleMask:NSWindowStyleMaskBorderless
                                                             backing:NSBackingStoreBuffered
                                                               defer:NO];
                    ew.releasedWhenClosed = NO;
                    ew.delegate = self;
                    ew.opaque = NO;
                    ew.backgroundColor = NSColor.clearColor;
                    ew.hasShadow = NO;
                    ew.ignoresMouseEvents = YES;
                    ew.acceptsMouseMovedEvents = YES;
                    ew.level = CGWindowLevelForKey(kCGDesktopIconWindowLevelKey) + 1;
                    ew.collectionBehavior =
                        NSWindowCollectionBehaviorStationary | NSWindowCollectionBehaviorFullScreenNone;
                    DPPMirrorView* ev =
                        [[DPPMirrorView alloc] initWithFrame:NSMakeRect(0, 0, vf.size.width, vf.size.height)];
                    ev.owner = self;
                    ev.sceneOriginOffset = calcSceneOffset(vf);
                    ev.wantsLayer = YES;
                    ew.contentView = ev;
                    [_extraWindows addObject:ew];
                    [_extraViews addObject:ev];
                    [_extraLabelLayers addObject:[NSMutableArray array]];
                    _extraRenderers.push_back(std::make_unique<CachedDrawRenderer>(images));
                }
            }
            const NSRect backing = [_view convertRectToBacking:_view.bounds];
            _fixedWindowFrame = frame;
            _fixedContentBounds = _view.bounds;
            _fixedBacking = backing;
            _geometryReady = spanMultiViewports || NSEqualRects(frame, _capturedDisplay.work);
            std::printf("mirror_startup_units scene=AppKit_points y=down backing=%.1fx%.1f scale=%.3f "
                        "work=%.1f,%.1f,%.1f,%.1f\n",
                        backing.size.width, backing.size.height, _window.backingScaleFactor, frame.origin.x,
                        frame.origin.y, frame.size.width, frame.size.height);
#ifdef DPP_MIRROR_FAULT_TESTING
            if (_failAfterStage == 3) throw std::runtime_error("fixture after window");
#endif
            _ticks = _inputs = _resets = _externalSwitches = _panelSwitches = 0;
            ++_starts;
            _previous = mirrorTime();
            stage = @"initial_render";
            [self render];
            if (!_deferPresentation) {
                [_window orderFront:nil];
                for (DPPMirrorWindow* ew in _extraWindows) [ew orderFront:nil];
            }
            stage = @"timers";
            __weak DPPMirrorPreview* weakSelf = self;
            _timer = [NSTimer timerWithTimeInterval:1.0 / 120
                                            repeats:YES
                                              block:^(NSTimer*) {
                                                [weakSelf guard:^{
                                                  [weakSelf tick];
                                                }];
                                              }];
            _timer.tolerance = .001;
            [NSRunLoop.mainRunLoop addTimer:_timer forMode:NSRunLoopCommonModes];
            if (_continuousSession && !_globalMoveMonitor) {
                _globalMoveMonitor = [NSEvent
                    addGlobalMonitorForEventsMatchingMask:(NSEventMaskMouseMoved |
                                                           NSEventMaskLeftMouseDragged)
                                                  handler:^(NSEvent*) {
                                                    DPPMirrorPreview* strong = weakSelf;
                                                    if (!strong || !strong.active || strong.migrating ||
                                                        strong->_systemSuspended ||
                                                        strong->_pointerGestureActive || !strong->_window ||
                                                        !strong->_view || !strong->_session.game())
                                                        return;
                                                    if (!strong->_session.game()->airHockeyEnabled() &&
                                                        !strong->_session.game()->billiardsEnabled())
                                                        return;
                                                    const NSPoint mouseGlobal = NSEvent.mouseLocation;
                                                    const NSInteger topWin =
                                                        [NSWindow windowNumberAtPoint:mouseGlobal
                                                              belowWindowWithWindowNumber:0];
                                                    DPPMirrorWindow* hitWin = nil;
                                                    DPPMirrorView* hitView = nil;
                                                    if (topWin == strong->_window.windowNumber) {
                                                        hitWin = strong->_window;
                                                        hitView = strong->_view;
                                                    } else {
                                                        for (NSUInteger wi = 0; wi < strong->_extraWindows.count;
                                                             ++wi) {
                                                            if (topWin ==
                                                                strong->_extraWindows[wi].windowNumber) {
                                                                hitWin = strong->_extraWindows[wi];
                                                                hitView = strong->_extraViews[wi];
                                                                break;
                                                            }
                                                        }
                                                    }
                                                    if (!hitWin || !hitView) return;
                                                    [strong guard:^{
                                                      if (!strong.active || strong.migrating ||
                                                          strong->_directDisabled || strong->_userPaused ||
                                                          strong->_menuActive || strong->_openSubmitted ||
                                                          strong->_session.game()->paused())
                                                          return;
                                                      const NSPoint winPt =
                                                          [hitWin convertPointFromScreen:mouseGlobal];
                                                      const NSPoint viewPt =
                                                          [hitView convertPoint:winPt fromView:nil];
                                                      const Vec2 sp{
                                                          static_cast<float>(viewPt.x +
                                                                             hitView.sceneOriginOffset.x),
                                                          static_cast<float>(viewPt.y +
                                                                             hitView.sceneOriginOffset.y)};
                                                      strong->_session.game()->input(
                                                          {InputType::PointerMove, sp, mirrorTime()});
                                                      [strong syncLanOutbound:{} force:NO];
                                                      if (strong->_session.game()->airHockeyEnabled()) {
                                                          if (strong->_timer.valid &&
                                                              strong->_timer.fireDate.timeIntervalSinceNow >
                                                                  (1.0 / 95.0)) {
                                                              strong->_timer.fireDate = NSDate.date;
                                                          }
                                                      } else if (strong->_session.game()->billiardsEnabled()) {
                                                          const double nowT = mirrorTime();
                                                          if (nowT - strong->_lastImmediateRenderAt >=
                                                              (1.0 / 120.0)) {
                                                              if (strong->_session.game()->tick(0)) {
                                                                  strong->_lastImmediateRenderAt = nowT;
                                                                  [strong render];
                                                              }
                                                          }
                                                      }
                                                    }];
                                                  }];
            }
            // Always available independent of focus: panel/menu Close plus hard timeout.
            const NSTimeInterval safetyTimeout =
                (snapshot.fullDesktop && !snapshot.candidatePreview && _continuousSession)
                    ? 43200.0
                    : (snapshot.fullDesktop ? 180.0 : 120.0);
            _deadline = [NSTimer scheduledTimerWithTimeInterval:safetyTimeout
                                                        repeats:NO
                                                          block:^(NSTimer*) {
                                                            [weakSelf close:@"preview_safety_timeout"];
                                                          }];
            [NSRunLoop.mainRunLoop addTimer:_deadline forMode:NSRunLoopCommonModes];
            _initialPreviewDeadline = _deadline.fireDate.timeIntervalSince1970;
#ifdef DPP_MIRROR_FAULT_TESTING
            if (_failAfterStage == 4) throw std::runtime_error("fixture after timers");
#endif
            _reason = @"running";
            [self recordEvent:@"preview_started" details:@{@"candidate" : @(snapshot.candidatePreview)}];
            return YES;
        } catch (const std::exception& error) {
            failure = [NSString stringWithUTF8String:error.what()];
            [self close:@"startup_failed"];
        } catch (...) {
            failure = @"unknown_cpp_exception";
            [self close:@"startup_failed"];
        }
    } @catch (NSException* error) {
        // Exception reasons may contain native paths; only retain the exception class.
        failure = error.name;
        [self close:@"startup_native_exception"];
    }
    NSMutableArray* items = [NSMutableArray array];
    for (const auto& item : snapshot.items) {
        [items addObject:@{
            @"id" : @(static_cast<uint32_t>(item.id)),
            @"raw" : @[ @(item.rawDpos.x), @(item.rawDpos.y) ],
            @"anchor" : @[ @(item.sceneAnchor.x), @(item.sceneAnchor.y) ],
            @"icon_size" : @(item.finderIconSize),
            @"draw" : @[ @(item.dppDrawSize.x), @(item.dppDrawSize.y) ],
            @"collision" : @[ @(item.dppCollisionBox.x), @(item.dppCollisionBox.y) ]
        }];
    }
    auto rect = [](const dpp::Rect& r) { return @[ @(r.x), @(r.y), @(r.width), @(r.height) ]; };
    self.startupFailure = @{
        @"stage" : stage,
        @"error" : failure ? failure : @"unknown",
        @"build_id" : @DPP_BUILD_ID,
        @"preview_session_id" : _sessionID,
        @"snapshot_id" : _snapshotID ? _snapshotID : @"none",
        @"screen" : rect(snapshot.screen),
        @"work" : rect(snapshot.workArea),
        @"candidate" : @(snapshot.candidatePreview),
        @"items" : items,
        @"protection" : [self currentProtection:NO]
    };
    NSData* data = [NSJSONSerialization dataWithJSONObject:self.startupFailure options:0 error:nil];
    if (data) {
        std::fputs("mirror_startup_failure=", stderr);
        std::fwrite(data.bytes, 1, data.length, stderr);
        std::fputc('\n', stderr);
        std::fflush(stderr);
    }
    return NO;
}
- (NSDictionary*)currentProtection:(BOOL)requestHelper {
    if (!self.protectionIdentity.count) return @{};
    NSDictionary* sample =
        self.protectionReader ? self.protectionReader(requestHelper) : self.protectionIdentity;
    if (![sample[@"protected_session_id"] isEqual:self.protectionIdentity[@"protected_session_id"]])
        return @{@"binding_error" : @"live_session_does_not_match_preview_binding"};
    return sample ? sample : @{};
}
- (void)recordEvent:(NSString*)name details:(NSDictionary*)details {
    @try {
        try {
            if (!_sessionID || !_traceEvents) return;
            const BOOL terminal = [name isEqual:@"close_input_released"] || [name isEqual:@"closed"] ||
                                  [name isEqual:@"recovery_result"] ||
                                  [name isEqual:@"display_migration_failed"];
            if (!self.active && !terminal) return;
            ++_traceSequence;
            if (_traceEvents.count >= (terminal ? 260u : 256u)) {
                ++_traceDropped;
                return;
            }
            NSDictionary* timing = [self currentProtection:YES];
            NSDictionary* event = @{
                @"sequence" : @(_traceSequence),
                @"event" : name,
                @"details" : details ? details : @{},
                @"preview_session_id" : _sessionID,
                @"snapshot_id" : _snapshotID ? _snapshotID : @"none",
                @"build_id" : @DPP_BUILD_ID,
                @"protection" : timing,
                @"event_uptime_seconds" : @(mirrorTime()),
                @"preview_clock" : @"unix_epoch_seconds_NSTimer",
                @"initial_preview_deadline_epoch" : _initialPreviewDeadline > 0 ? @(_initialPreviewDeadline)
                                                                                : (id)NSNull.null,
                @"current_preview_deadline_epoch" : _deadline.valid
                    ? @(_deadline.fireDate.timeIntervalSince1970)
                    : (id)NSNull.null,
                @"preview_timer_valid" : @(_deadline.valid),
                @"input_grabbed" : @(_session.game() && _session.game()->mode().stats().grabbing),
                @"window_accepts_mouse" : @(_window && !_window.ignoresMouseEvents)
            };
            [_traceEvents addObject:event];
            writeMirrorTrace(@"mirror_event=", event);
            if (!self.active && [_lastReport[@"session_id"] isEqual:_sessionID]) {
                NSMutableDictionary* updated = [_lastReport mutableCopy];
                updated[@"events"] = [_traceEvents copy];
                updated[@"events_dropped"] = @(_traceDropped);
                _lastReport = updated;
                if ([_summaries.lastObject[@"session_id"] isEqual:_sessionID]) {
                    _summaries[_summaries.count - 1] = updated;
                    [self persistSummaries];
                }
            }
        } catch (...) {
        }
    } @catch (NSException*) {
    }
}
- (BOOL)presentPrepared {
    if (![self active] || !_window) return NO;
    _deferPresentation = NO;
    [_window orderFront:nil];
    for (DPPMirrorWindow* ew in _extraWindows) [ew orderFront:nil];
    [self armDesktopReturn];
    return YES;
}
- (void)setLabels:(NSArray<NSString*>*)labels {
    _labels = [labels copy];
    _labelContentDirty = YES;
}
- (void)setToyBlackHole:(BOOL)enabled {
    [self guard:^{
      if (!self.active || self->_migration.active() || !self->_session.game()) return;
      const bool wasInBilliards = self->_session.game()->billiardsEnabled();
      const bool wasInteractive = (self->_session.phase() == MirrorPhase::Interactive);
      if (enabled && (wasInBilliards || self->_session.game()->slingshotEnabled() ||
                      self->_session.game()->airHockeyEnabled())) {
          self->_billiardsAudio.stop();
          self->_billiardsLan.stop();
          if (wasInBilliards) {
              self->_session.game()->setBilliards(false);
              ++self->_billiardsToggles;
              [self recordEvent:@"billiards_mode"
                        details:@{
                            @"enabled" : @NO,
                            @"reason" : @"black_hole_enabled"
                        }];
          } else {
              self->_session.game()->setPlayMode(PlayMode::Gravity);
          }
      }
      self->_userPaused = NO;
      self->_session.ensurePlaying();
      self->_session.game()->setMuted(self->_soundMuted);
      // Mac draws the cinematic accretion-disk visual; Windows keeps the classic ring.
      self->_session.game()->setBlackHoleStyle(BlackHoleStyle::Cinematic);
      self->_session.game()->setBlackHoleScale(self->_toyBlackHoleScale);
      self->_session.game()->setBlackHoleAngles(self->_toyBlackHoleElevation, self->_toyBlackHoleTilt);
      self->_session.game()->setBlackHoleDevour(self->_toyBlackHoleDevour);
      self->_session.game()->setBlackHole(enabled);
      if (!self->_directDisabled) {
          if (wasInteractive && [self isAnyMirrorWindowKey] && NSApp.active) {
              self->_session.interact();
              self.window.interactive = YES;
              self.window.ignoresMouseEvents = NO;
              for (DPPMirrorWindow* ew in self->_extraWindows) {
                  ew.interactive = YES;
                  ew.ignoresMouseEvents = NO;
              }
          } else {
              [self armDesktopReturn];
          }
      }
      [self render];
      [self flushSounds];
      self->_previous = mirrorTime();
      self.timer.fireDate = NSDate.date;
    }];
}
- (dpp::PlayMode)playMode {
    if (!self.active || !_session.game()) return PlayMode::Gravity;
    return _session.game()->playMode();
}
- (void)applyGravityMagnitude:(float)mps2
                    direction:(dpp::GravityDirection)direction
                  restitution:(float)restitution {
    [self guard:^{
      if (std::isfinite(mps2)) {
          self->_pendingGravityMagnitude = std::clamp(mps2, kMinGravityMps2, kMaxGravityMps2);
      }
      if (static_cast<int>(direction) >= 0 && static_cast<int>(direction) <= 3) {
          self->_pendingGravityDirection = direction;
      }
      if (std::isfinite(restitution)) {
          self->_pendingGravityRestitution =
              std::clamp(restitution, kMinGravityRestitution, kMaxGravityRestitution);
      }
      if (!self.active || self->_migration.active() || !self->_session.game()) return;
      self->_session.game()->setGravitySettings(
          self->_pendingGravityMagnitude, self->_pendingGravityDirection, self->_pendingGravityRestitution);
      [self render];
      self->_previous = mirrorTime();
      self.timer.fireDate = NSDate.date;
    }];
}
- (float)gravityMagnitude {
    return _session.game() ? _session.game()->gravityMagnitude() : _pendingGravityMagnitude;
}
- (dpp::GravityDirection)gravityDirection {
    return _session.game() ? _session.game()->gravityDirection() : _pendingGravityDirection;
}
- (float)gravityRestitution {
    return _session.game() ? _session.game()->gravityRestitution() : _pendingGravityRestitution;
}
- (void)setPlayMode:(dpp::PlayMode)mode {
    [self guard:^{
      if (!self.active || self->_migration.active() || !self->_session.game()) return;
      if (mode == PlayMode::Gravity) {
          self->_p2KeyMask = 0;
          if (self.billiardsEnabled) {
              [self setBilliards:NO];
              return;
          }
          [self releaseInteraction];
          self->_billiardsAudio.stop();
          self->_billiardsLan.stop();
          self->_session.game()->setPlayMode(PlayMode::Gravity);
          [self armDesktopReturn];
          [self render];
          self.timer.fireDate = NSDate.date;
          return;
      }
      if (mode == PlayMode::BlackHole) {
          self->_billiardsAudio.stop();
          self->_billiardsLan.stop();
          self->_p2KeyMask = 0;
          [self setToyBlackHole:YES];
          return;
      }
      if (mode == PlayMode::Billiards) {
          self->_p2KeyMask = 0;
          [self setBilliards:YES];
          return;
      }
      [self releaseInteraction];
      if (mode != PlayMode::AirHockey) {
          self->_billiardsLan.stop();
      }
      self->_p2KeyMask = 0;
      self->_userPaused = NO;
      self->_session.ensurePlaying();
      self->_session.game()->setMuted(self->_soundMuted);
      if (mode == PlayMode::Slingshot) {
          self->_session.game()->setSlingshotProgress(self->_pendingSlingshotProgress);
          self->_session.game()->setSlingshotConfig(self->_pendingSlingshotConfig);
      } else if (mode == PlayMode::AirHockey) {
          self->_session.game()->setAirHockeyConfig(self->_pendingAirHockeyConfig);
      }
      self->_session.game()->setPlayMode(mode);
      [self syncLanRole];
      [self armDesktopReturn];
      [self render];
      self->_previous = mirrorTime();
      self.timer.fireDate = NSDate.date;
    }];
}
- (BOOL)slingshotEnabled {
    return self.active && _session.game() && _session.game()->slingshotEnabled();
}
- (dpp::SlingshotConfig)slingshotConfig {
    return _session.game() ? _session.game()->slingshotConfig() : _pendingSlingshotConfig;
}
- (void)applySlingshotConfig:(const dpp::SlingshotConfig&)config {
    [self guard:^{
      self->_pendingSlingshotConfig = config;
      if (!self.active || self->_migration.active() || !self->_session.game()) return;
      self->_session.game()->setSlingshotProgress(self->_pendingSlingshotProgress);
      self->_session.game()->setSlingshotConfig(self->_pendingSlingshotConfig);
      [self render];
      self->_previous = mirrorTime();
      self.timer.fireDate = NSDate.date;
    }];
}
- (int)slingshotHighScore {
    if (_session.game()) {
        _pendingSlingshotHighScore =
            std::max(_pendingSlingshotHighScore, _session.game()->slingshotHighScore());
    }
    return _pendingSlingshotHighScore;
}
- (void)setSlingshotHighScore:(int)score {
    _pendingSlingshotHighScore = std::max(0, score);
    _pendingSlingshotProgress.highScore = std::max(_pendingSlingshotProgress.highScore, _pendingSlingshotHighScore);
    if (_session.game()) {
        _session.game()->setSlingshotHighScore(_pendingSlingshotHighScore);
    }
}
- (dpp::SlingshotProgress)slingshotProgress {
    if (_session.game()) {
        _pendingSlingshotProgress = _session.game()->slingshotProgress();
    }
    return _pendingSlingshotProgress;
}
- (void)applySlingshotProgress:(const dpp::SlingshotProgress&)progress {
    _pendingSlingshotProgress = progress;
    _pendingSlingshotHighScore = std::max(_pendingSlingshotHighScore, progress.highScore);
    if (_session.game()) {
        _session.game()->setSlingshotProgress(progress);
    }
}
- (NSWindow*)overlayWindow {
    return self.window;
}
- (void)enterSlingshotEditor {
    [self guard:^{
      if (!self.active || self->_migration.active() || !self->_session.game()) return;
      if (!self->_session.game()->slingshotEnabled()) {
          self->_pendingSlingshotConfig.preset = SlingshotPreset::CustomBuilder;
          self->_pendingSlingshotConfig.startInEditor = true;
          [self setPlayMode:PlayMode::Slingshot];
          self->_pendingSlingshotConfig.startInEditor = false;
          return;
      }
      if (auto* sl = self->_session.game()->mutableSlingshotMode()) {
          sl->setEditing(!sl->editing());
          self->_session.game()->forceRedraw();
          self->_pendingSlingshotProgress = self->_session.game()->slingshotProgress();
          self->_pendingSlingshotConfig = self->_session.game()->slingshotConfig();
          if (self->_session.game()->takeSlingshotPersistenceDirty() && self.slingshotPersistenceDidChange) {
              self.slingshotPersistenceDidChange(self->_pendingSlingshotProgress,
                                                 self->_pendingSlingshotConfig.customPieces);
          }
          [self render];
      }
    }];
}
- (BOOL)airHockeyEnabled {
    return self.active && _session.game() && _session.game()->airHockeyEnabled();
}
- (dpp::AirHockeyConfig)airHockeyConfig {
    return _session.game() ? _session.game()->airHockeyConfig() : _pendingAirHockeyConfig;
}
- (void)applyAirHockeyConfig:(const dpp::AirHockeyConfig&)config {
    [self guard:^{
      self->_pendingAirHockeyConfig = config;
      if (config.matchMode != AirHockeyMatchMode::Lan && !self.billiardsEnabled &&
          self->_billiardsLan.state() != BilliardsLan::State::Disconnected) {
          self->_billiardsLan.stop();
      }
      if (!self.active || self->_migration.active() || !self->_session.game()) return;
      self->_session.game()->setAirHockeyConfig(self->_pendingAirHockeyConfig);
      [self syncLanRole];
      [self render];
      self->_previous = mirrorTime();
      self.timer.fireDate = NSDate.date;
    }];
}
- (BOOL)canEnableBilliards {
    return self.active && !self.migrating && _session.game() && _session.game()->canEnableBilliards();
}
- (BOOL)canEnableBilliardsWithConfig:(const dpp::BilliardsConfig&)config {
    return self.active && !self.migrating && _session.game() &&
           _session.game()->canEnableBilliardsWith(config);
}
- (BOOL)canEnableAnyBilliards {
    return self.active && !self.migrating && _session.game() && _session.game()->canEnableAnyBilliards();
}
- (void)ensureInteractiveIfAllowed {
    [self guard:^{
      if (!self.active || self.migrating || self->_directDisabled || self->_userPaused ||
          self->_menuActive || self->_openSubmitted)
          return;
      activatePreviewApp();
      if (self->_session.phase() != MirrorPhase::Interactive) {
          [self toggleInteraction];
      } else {
          self.window.interactive = YES;
          self.window.ignoresMouseEvents = NO;
          for (DPPMirrorWindow* ew in self->_extraWindows) {
              ew.interactive = YES;
              ew.ignoresMouseEvents = NO;
              [ew orderFront:nil];
          }
          if (![self isAnyMirrorWindowKey]) {
              [self.window makeKeyAndOrderFront:nil];
              [self.window makeFirstResponder:self.view];
          }
      }
    }];
}
- (BOOL)billiardsEnabled {
    return self.active && _session.game() && _session.game()->billiardsEnabled();
}
- (dpp::BilliardsConfig)billiardsConfig {
    return _session.game() ? _session.game()->billiardsConfig() : _pendingBilliardsConfig;
}
- (void)applyBilliardsConfig:(const dpp::BilliardsConfig&)config {
    [self guard:^{
      self->_pendingBilliardsConfig = config;
      self->_pendingBilliardsConfig.style = BilliardsStyle::Desktop;
      const bool hockeyLanActive =
          self->_session.game() && self->_session.game()->airHockeyEnabled() &&
          self->_pendingAirHockeyConfig.matchMode == AirHockeyMatchMode::Lan;
      if (config.matchMode != BilliardsMatchMode::Lan && !hockeyLanActive &&
          self->_billiardsLan.state() != BilliardsLan::State::Disconnected) {
          self->_billiardsLan.stop();
      }
      if (!self.active || self->_migration.active() || !self->_session.game()) return;
      self->_session.game()->setBilliardsConfig(self->_pendingBilliardsConfig);
      [self syncLanRole];
      if (self->_session.game()->billiardsEnabled() &&
          self->_billiardsLan.state() == BilliardsLan::State::ConnectedHost) {
          self->_billiardsLan.sendRuleSync(self->_pendingBilliardsConfig.rule);
          [self syncLanOutbound:{} force:YES];
      }
      [self render];
      self->_previous = mirrorTime();
      self.timer.fireDate = NSDate.date;
    }];
}
- (void)syncLanRole {
    if (!_session.game()) return;
    const int role = _billiardsLan.isHost() ? 0 : (_billiardsLan.isGuest() ? 1 : -1);
    if (auto* bm = _session.game()->mutableBilliardsMode()) {
        bm->setLanLocalPlayer(role);
    }
    if (auto* ah = _session.game()->mutableAirHockeyMode()) {
        ah->setLanLocalPlayer(role);
    }
}
- (void)syncLanOutbound:(const std::vector<SoundEvent>&)sounds force:(BOOL)force {
    if (!_session.game()) return;
    if (auto* bm = _session.game()->mutableBilliardsMode()) {
        if (_billiardsLan.state() == BilliardsLan::State::ConnectedGuest) {
            if (auto act = bm->takeLocalLanAction()) {
                _billiardsLan.sendAction(*act);
            }
            return;
        }
        if (_billiardsLan.state() == BilliardsLan::State::ConnectedHost) {
            const double now = mirrorTime();
            const bool activeMotion = !_session.game()->idle() || bm->state() == BilliardsState::Charging;
            if (!force && sounds.empty() && !activeMotion && (now - _lastLanSnapSent) < 0.065) {
                return;
            }
            _lastLanSnapSent = now;
            _billiardsLan.sendSnapshot(bm->exportLanSnapshot(), sounds);
        }
        return;
    }
    if (auto* ah = _session.game()->mutableAirHockeyMode()) {
        if (_billiardsLan.state() == BilliardsLan::State::ConnectedGuest) {
            AirHockeyRemoteInput in{};
            if (ah->consumePendingRemoteInput(in)) {
                _billiardsLan.sendHockeyInput(in);
            }
            return;
        }
        if (_billiardsLan.state() == BilliardsLan::State::ConnectedHost) {
            const double now = mirrorTime();
            if (!force && sounds.empty() && (now - _lastLanSnapSent) < 0.022) {
                return;
            }
            _lastLanSnapSent = now;
            _billiardsLan.sendHockeyState(ah->exportLanState(), sounds);
        }
    }
}
- (BOOL)startLanHost:(uint16_t)port {
    __block BOOL ok = NO;
    [self guard:^{
      if (self.airHockeyEnabled) {
          self->_pendingAirHockeyConfig.matchMode = AirHockeyMatchMode::Lan;
          if (self.active && !self.migrating && self->_session.game()) {
              self->_session.game()->setAirHockeyConfig(self->_pendingAirHockeyConfig);
          }
          self->_billiardsLan.setAdvertisedGame(lan_codec::LanGameKind::AirHockey,
                                                self->_pendingAirHockeyConfig.targetGoals);
      } else {
          self->_pendingBilliardsConfig.matchMode = BilliardsMatchMode::Lan;
          if (self.active && !self.migrating && self->_session.game()) {
              self->_session.game()->setBilliardsConfig(self->_pendingBilliardsConfig);
          }
          self->_billiardsLan.setAdvertisedGame(lan_codec::LanGameKind::Billiards,
                                                static_cast<int>(self->_pendingBilliardsConfig.rule));
      }
      ok = self->_billiardsLan.host(port ? port : BilliardsLan::kDefaultPort, false);
      [self syncLanRole];
      if (self.active && self->_session.game()) {
          self->_session.game()->forceRedraw();
          [self render];
      }
    }];
    return ok;
}
- (BOOL)startLanDiscovery {
    __block BOOL ok = NO;
    [self guard:^{
      const auto gk =
          self.airHockeyEnabled ? lan_codec::LanGameKind::AirHockey : lan_codec::LanGameKind::Billiards;
      self->_billiardsLan.setAdvertisedGame(gk, 0);
      ok = self->_billiardsLan.startDiscovery(BilliardsLan::kDefaultDiscoveryPort, false);
    }];
    return ok;
}
- (void)stopLanDiscovery {
    [self guard:^{
      self->_billiardsLan.stopDiscovery();
    }];
}
- (BOOL)quickScanAndJoinLan {
    __block BOOL ok = NO;
    [self guard:^{
      if (self.airHockeyEnabled) {
          self->_pendingAirHockeyConfig.matchMode = AirHockeyMatchMode::Lan;
          if (self.active && !self.migrating && self->_session.game()) {
              self->_session.game()->setAirHockeyConfig(self->_pendingAirHockeyConfig);
          }
      } else {
          self->_pendingBilliardsConfig.matchMode = BilliardsMatchMode::Lan;
          if (self.active && !self.migrating && self->_session.game()) {
              self->_session.game()->setBilliardsConfig(self->_pendingBilliardsConfig);
          }
      }
      const auto gk =
          self.airHockeyEnabled ? lan_codec::LanGameKind::AirHockey : lan_codec::LanGameKind::Billiards;
      ok = self->_billiardsLan.quickScanAndJoin(gk, BilliardsLan::kDefaultDiscoveryPort, false);
      [self syncLanRole];
    }];
    return ok;
}
- (NSArray<NSString*>*)discoveredLanRoomLabels {
    const auto& rooms = _billiardsLan.discoveredRooms();
    NSMutableArray<NSString*>* out = [NSMutableArray arrayWithCapacity:rooms.size()];
    for (const auto& rm : rooms) {
        NSString* s = [NSString stringWithUTF8String:rm.displayLabel().c_str()];
        if (s) [out addObject:s];
    }
    return out;
}
- (NSString*)discoveredLanRoomEndpointAtIndex:(NSUInteger)index {
    const auto& rooms = _billiardsLan.discoveredRooms();
    if (index >= rooms.size()) return nil;
    return [NSString stringWithUTF8String:rooms[index].endpoint().c_str()];
}
- (BOOL)joinLanEndpoint:(NSString*)endpoint {
    __block BOOL ok = NO;
    [self guard:^{
      if (self.airHockeyEnabled) {
          self->_pendingAirHockeyConfig.matchMode = AirHockeyMatchMode::Lan;
          if (self.active && !self.migrating && self->_session.game()) {
              self->_session.game()->setAirHockeyConfig(self->_pendingAirHockeyConfig);
          }
      } else {
          self->_pendingBilliardsConfig.matchMode = BilliardsMatchMode::Lan;
          if (self.active && !self.migrating && self->_session.game()) {
              self->_session.game()->setBilliardsConfig(self->_pendingBilliardsConfig);
          }
      }
      const std::string ep = endpoint.length ? std::string(endpoint.UTF8String) : std::string("127.0.0.1");
      ok = self->_billiardsLan.join(ep, BilliardsLan::kDefaultPort);
      [self syncLanRole];
      if (self.active && self->_session.game()) {
          self->_session.game()->forceRedraw();
          [self render];
      }
    }];
    return ok;
}
- (void)stopLan {
    [self guard:^{
      self->_billiardsLan.stop();
      [self syncLanRole];
      if (self.active && self->_session.game()) {
          self->_session.game()->forceRedraw();
          [self render];
      }
    }];
}
- (BOOL)lanActive {
    return _billiardsLan.state() != BilliardsLan::State::Disconnected || _billiardsLan.reconnecting();
}
- (uint16_t)lanBoundPort {
    return _billiardsLan.boundPort();
}
- (NSString*)lanStatusString {
    NSString* s = [NSString stringWithUTF8String:_billiardsLan.statusText().c_str()];
    return s ? s : @"";
}
- (void)setBilliards:(BOOL)enabled {
    [self guard:^{
      if (!self.active || self->_migration.active() || !self->_session.game()) return;
      if (enabled) self->_session.game()->setBilliardsConfig(self->_pendingBilliardsConfig);
      if (enabled && !self->_session.game()->canEnableAnyBilliards()) return;
      if (self->_session.game()->billiardsEnabled() == static_cast<bool>(enabled)) return;
      [self releaseInteraction];
      if (!enabled) {
          self->_billiardsAudio.stop();
          self->_billiardsLan.stop();
      }
      self->_userPaused = NO;
      self->_session.ensurePlaying();
      self->_session.game()->setMuted(self->_soundMuted);
      if (enabled) {
          self->_session.game()->enableBilliardsAutoFit();
      } else {
          self->_session.game()->setBilliards(false);
      }
      [self syncLanRole];
      ++self->_billiardsToggles;
      const auto bs = self->_session.game()->billiardsSessionStats();
      [self recordEvent:@"billiards_mode"
                details:@{
                    @"enabled" : @(enabled),
                    @"toggles" : @(self->_billiardsToggles),
                    @"shots_total" : @(bs.totalShots),
                    @"pocketed_total" : @(bs.totalPocketed),
                    @"white_placements_total" : @(bs.totalWhitePlacements),
                    @"tube_opens" : @(self->_billiardsTubeOpens)
                }];
      [self armDesktopReturn];
      [self render];
      self->_previous = mirrorTime();
      self.timer.fireDate = NSDate.date;
    }];
}
- (void)recordBilliardsProgress {
    if (!_session.game()) return;
    const auto bs = _session.game()->billiardsSessionStats();
    const auto ms = _session.game()->mode().stats();
    if (bs.totalShots > _loggedBilliardsShots) {
        _loggedBilliardsShots = bs.totalShots;
        [self recordEvent:@"billiards_shot"
                  details:@{@"shots_total" : @(bs.totalShots), @"rack_shots" : @(ms.shots)}];
    }
    if (bs.totalPocketed > _loggedBilliardsPocketed) {
        _loggedBilliardsPocketed = bs.totalPocketed;
        [self recordEvent:@"billiards_pocketed"
                  details:@{
                      @"pocketed_total" : @(bs.totalPocketed),
                      @"rack_pocketed" : @(ms.pocketed),
                      @"targets" : @(ms.targets),
                      @"finished" : @(ms.finished),
                      @"best_shots" : @(bs.bestShots)
                  }];
    }
    if (bs.totalWhitePlacements > _loggedBilliardsPlacements) {
        _loggedBilliardsPlacements = bs.totalWhitePlacements;
        [self recordEvent:@"billiards_white_placed"
                  details:@{@"placements_total" : @(bs.totalWhitePlacements)}];
    }
}
- (void)setSoundMuted:(BOOL)muted {
    [self guard:^{
      if (self->_soundMuted == muted) return;
      self->_soundMuted = muted;
      if (muted) self->_billiardsAudio.stop();
      if (self->_session.game()) self->_session.game()->setMuted(muted);
      [self recordEvent:@"sound_muted" details:@{@"muted" : @(muted)}];
    }];
}
- (void)flushSounds {
    if (!_session.game()) return;
    _session.game()->drainSounds(_soundBuf);
    _lanSoundBuf = _soundBuf;
    if (!_soundMuted && self.active && !self.migrating && !_soundBuf.empty()) {
        _billiardsAudio.play(_soundBuf, false);
    }
    _soundBuf.clear();
    [self syncLanOutbound:_lanSoundBuf force:NO];
    _lanSoundBuf.clear();
}
- (void)setToyBlackHoleDevour:(BOOL)devour {
    [self guard:^{
      self->_toyBlackHoleDevour = devour;
      if (!self.active || self->_migration.active() || !self->_session.game()) return;
      self->_session.game()->setBlackHoleDevour(devour);
      [self recordEvent:@"black_hole_devour" details:@{@"enabled" : @(devour)}];
      self.timer.fireDate = NSDate.date;
    }];
}
- (void)setToyBlackHoleScale:(double)scale {
    [self guard:^{
      if (!std::isfinite(scale)) return;
      self->_toyBlackHoleScale =
          std::clamp(static_cast<float>(scale), BlackHoleState::kMinScale, BlackHoleState::kMaxScale);
      if (!self.active || self->_migration.active() || !self->_session.game()) return;
      self->_session.game()->setBlackHoleScale(self->_toyBlackHoleScale);
      [self render];
    }];
}
- (void)setToyBlackHoleElevation:(double)elevation tilt:(double)tilt {
    [self guard:^{
      if (std::isfinite(elevation))
          self->_toyBlackHoleElevation = std::clamp(
              static_cast<float>(elevation), BlackHoleState::kMinElevation, BlackHoleState::kMaxElevation);
      if (std::isfinite(tilt))
          self->_toyBlackHoleTilt =
              std::clamp(static_cast<float>(tilt), BlackHoleState::kMinTilt, BlackHoleState::kMaxTilt);
      if (!self.active || self->_migration.active() || !self->_session.game()) return;
      self->_session.game()->setBlackHoleAngles(self->_toyBlackHoleElevation, self->_toyBlackHoleTilt);
      [self render];
    }];
}
- (void)setShowsFilenames:(BOOL)visible {
    [self guard:^{
      if (self->_showsFilenames == visible) return;
      self->_showsFilenames = visible;
      [self recordEvent:@"filenames" details:@{@"visible" : @(visible)}];
      if (!visible) {
          [CATransaction begin];
          [CATransaction setDisableActions:YES];
          for (CATextLayer* label in self.labelLayers) label.hidden = YES;
          for (NSMutableArray<CATextLayer*>* extraArr in self.extraLabelLayers) {
              for (CATextLayer* label in extraArr) label.hidden = YES;
          }
          [CATransaction commit];
      } else if ([self active]) {
          [self render];
      }
    }];
}
- (void)render {
    const auto start = SteadyClock::now();
    const bool showOutline = ![_experimentID isEqual:@"protected_finder"] && !self.allowsBackgroundSession &&
                             !_session.game()->billiardsEnabled();
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    const DrawList* primaryDraw = &_session.game()->drawList();
    if (_view.sceneOriginOffset.x != 0.0 || _view.sceneOriginOffset.y != 0.0) {
        _viewportScratchDraw = *primaryDraw;
        _viewportScratchDraw.translate({-static_cast<float>(_view.sceneOriginOffset.x),
                                        -static_cast<float>(_view.sceneOriginOffset.y)});
        primaryDraw = &_viewportScratchDraw;
    }
    _renderer->render(*primaryDraw, _view.layer, _view.bounds, _window.backingScaleFactor,
                      showOutline, /*manageTransaction=*/false);
    if (_showsFilenames && _labels.count) {
        if (!_labelLayers) _labelLayers = [NSMutableArray array];
        const Vec2 workSz{static_cast<float>(_view.bounds.size.width),
                          static_cast<float>(_view.bounds.size.height)};
        buildFilenamePeers(*primaryDraw, workSz, _filenamePeers);
        std::size_t peerCursor = 0;
        NSUInteger index = 0;
        const CGFloat scale = _window.backingScaleFactor;
        const BOOL scaleChanged = _labelContentsScale != scale;
        for (const auto& command : primaryDraw->commands()) {
            const auto* sprite = std::get_if<SpriteItem>(&command);
            if (!sprite) continue;
            const bool isPeer = (sprite->opacity > 0.0f && sprite->interactive);
            float preY = 0.0f;
            if (isPeer && peerCursor < _filenamePeers.size()) {
                preY = _filenamePeers[peerCursor++].labelY;
            }
            const auto texture = static_cast<uint32_t>(sprite->texture);
            if (texture == 0 || texture > _labels.count) continue;
            const BOOL created = index == _labelLayers.count;
            if (created) {
                CATextLayer* label = [CATextLayer layer];
                label.fontSize = 12;
                label.alignmentMode = kCAAlignmentCenter;
                label.wrapped = YES;
                label.truncationMode = kCATruncationEnd;
                label.foregroundColor = NSColor.whiteColor.CGColor;
                label.shadowColor = NSColor.blackColor.CGColor;
                label.shadowOpacity = 1;
                label.shadowRadius = 2;
                label.zPosition = 9000;
                label.anchorPoint = CGPointZero;
                label.bounds = CGRectMake(0, 0, 140, 32);
                _labelTextures.push_back(0);
                [_labelLayers addObject:label];
                [_view.layer addSublayer:label];
            }
            const BOOL textureChanged = _labelTextures[index] != texture;
            _labelTextures[index] = texture;
            CATextLayer* label = _labelLayers[index++];
            if (!sprite->interactive || sprite->opacity <= 0.0f) {
                if (!label.hidden) label.hidden = YES;
                continue;
            }
            if (created || scaleChanged) {
                label.contentsScale = scale;
                ++_labelScaleWrites;
            }
            if (created || textureChanged || _labelContentDirty) {
                NSString* text = _labels[texture - 1];
                if (![label.string isEqual:text]) {
                    label.string = text;
                    ++_labelStringWrites;
                }
            }
            const auto name = filenameLabelBoundsFromPeers(*sprite, preY, workSz, _filenamePeers, 140);
            const BOOL shouldHide = name.width <= 0 || !sprite->interactive;
            if (label.hidden != shouldHide) label.hidden = shouldHide;
            if (!shouldHide) {
                const CGRect bounds = CGRectMake(0, 0, name.width, name.height);
                if (!CGRectEqualToRect(label.bounds, bounds)) {
                    label.bounds = bounds;
                    ++_labelGeometryWrites;
                }
                const CGPoint position = CGPointMake(name.x, name.y);
                if (!CGPointEqualToPoint(label.position, position)) {
                    label.position = position;
                    ++_labelGeometryWrites;
                }
            }
        }
        for (NSUInteger i = index; i < _labelLayers.count; ++i)
            if (!_labelLayers[i].hidden) _labelLayers[i].hidden = YES;
        _labelContentDirty = NO;
        _labelContentsScale = scale;
    }
    for (NSUInteger vi = 0; vi < _extraViews.count && vi < _extraRenderers.size(); ++vi) {
        DPPMirrorWindow* ew = _extraWindows[vi];
        DPPMirrorView* ev = _extraViews[vi];
        _viewportScratchDraw = _session.game()->drawList();
        _viewportScratchDraw.translate(
            {-static_cast<float>(ev.sceneOriginOffset.x), -static_cast<float>(ev.sceneOriginOffset.y)});
        _extraRenderers[vi]->render(_viewportScratchDraw, ev.layer, ev.bounds, ew.backingScaleFactor,
                                    showOutline, /*manageTransaction=*/false);
        if (_showsFilenames && _labels.count && vi < _extraLabelLayers.count) {
            NSMutableArray<CATextLayer*>* labelsArr = _extraLabelLayers[vi];
            const Vec2 workSz{static_cast<float>(ev.bounds.size.width),
                              static_cast<float>(ev.bounds.size.height)};
            buildFilenamePeers(_viewportScratchDraw, workSz, _filenamePeers);
            std::size_t peerCursor = 0;
            NSUInteger idx = 0;
            const CGFloat sc = ew.backingScaleFactor;
            for (const auto& command : _viewportScratchDraw.commands()) {
                const auto* sprite = std::get_if<SpriteItem>(&command);
                if (!sprite) continue;
                const bool isPeer = (sprite->opacity > 0.0f && sprite->interactive);
                float preY = 0.0f;
                if (isPeer && peerCursor < _filenamePeers.size()) preY = _filenamePeers[peerCursor++].labelY;
                const auto texture = static_cast<uint32_t>(sprite->texture);
                if (texture == 0 || texture > _labels.count) continue;
                if (idx == labelsArr.count) {
                    CATextLayer* label = [CATextLayer layer];
                    label.fontSize = 12;
                    label.alignmentMode = kCAAlignmentCenter;
                    label.wrapped = YES;
                    label.truncationMode = kCATruncationEnd;
                    label.foregroundColor = NSColor.whiteColor.CGColor;
                    label.shadowColor = NSColor.blackColor.CGColor;
                    label.shadowOpacity = 1;
                    label.shadowRadius = 2;
                    label.zPosition = 9000;
                    label.anchorPoint = CGPointZero;
                    label.bounds = CGRectMake(0, 0, 140, 32);
                    [labelsArr addObject:label];
                    [ev.layer addSublayer:label];
                }
                CATextLayer* label = labelsArr[idx++];
                if (!sprite->interactive || sprite->opacity <= 0.0f) {
                    label.hidden = YES;
                    continue;
                }
                if (label.contentsScale != sc) label.contentsScale = sc;
                NSString* text = _labels[texture - 1];
                if (![label.string isEqual:text]) label.string = text;
                const auto name = filenameLabelBoundsFromPeers(*sprite, preY, workSz, _filenamePeers, 140);
                const BOOL shouldHide = name.width <= 0;
                label.hidden = shouldHide;
                if (!shouldHide) {
                    label.bounds = CGRectMake(0, 0, name.width, name.height);
                    label.position = CGPointMake(name.x, name.y);
                }
            }
            for (NSUInteger k = idx; k < labelsArr.count; ++k) labelsArr[k].hidden = YES;
        }
    }
    [CATransaction commit];
    _session.game()->perf.submit.add(elapsedMs(start));
}
#ifdef DPP_MIRROR_FAULT_TESTING
- (void)fixturePointer:(NSEventType)type {
    if (!self.active || !_session.game()) return;
    for (const auto& command : _session.game()->drawList().commands()) {
        const auto* sprite = std::get_if<SpriteItem>(&command);
        if (!sprite) continue;
        NSPoint point = [_view convertPoint:NSMakePoint(sprite->center.x, sprite->center.y) toView:nil];
        NSEvent* event = [NSEvent mouseEventWithType:type
                                            location:point
                                       modifierFlags:0
                                           timestamp:mirrorTime()
                                        windowNumber:_window.windowNumber
                                             context:nil
                                         eventNumber:0
                                          clickCount:1
                                            pressure:1];
        if (type == NSEventTypeLeftMouseDown)
            [_view mouseDown:event];
        else if (type == NSEventTypeLeftMouseUp)
            [_view mouseUp:event];
        else if (type == NSEventTypeLeftMouseDragged)
            [_view mouseDragged:event];
        return;
    }
}
- (void)fixtureWorkAreaChange {
    if (!self.active) return;
    auto expanded = _capturedDisplay;
    expanded.work.origin.y -= 2;
    expanded.work.size.height += 2;
    if (!mirrorDisplayCompatible(_capturedDisplay, expanded, [self fixedSceneGeometryValid]))
        [self close:@"fixture_expansion_incompatible"];
    [self configurationChanged:[NSNotification
                                   notificationWithName:NSApplicationDidChangeScreenParametersNotification
                                                 object:nil]];
}
- (void)fixtureConfiguration:(NSString*)name changedGeometry:(BOOL)changed {
    if (changed) _capturedDisplay.scale += 1;
    [self configurationChanged:[NSNotification notificationWithName:name object:nil]];
}
- (void)fixtureContextMenu:(BOOL)select {
    if (!self.active || !_session.game()) return;
    const auto& draw = _session.game()->drawList().commands();
    const SpriteItem* sprite = nullptr;
    for (const auto& command : draw) {
        sprite = std::get_if<SpriteItem>(&command);
        if (sprite) break;
    }
    if (!sprite) return;
    NSPoint point = [_view convertPoint:NSMakePoint(sprite->center.x, sprite->center.y) toView:nil];
    self.fixtureMenuTimedOut = NO;
    id began = [NSNotificationCenter.defaultCenter
        addObserverForName:NSMenuDidBeginTrackingNotification
                    object:nil
                     queue:nil
                usingBlock:^(NSNotification* note) {
                  if (note.object != self->_openMenu) return;
                  CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{
                    if (!self->_menuActive || !self->_openMenu) return;
                    if (select) {
                        [self->_openMenu performActionForItemAtIndex:0];
                        [self->_openMenu performActionForItemAtIndex:0];
                    } else {
                        [self->_openMenu cancelTracking];
                    }
                  });
                  CFRunLoopWakeUp(CFRunLoopGetMain());
                }];
    NSTimer* cancel = [NSTimer timerWithTimeInterval:3
                                             repeats:NO
                                               block:^(NSTimer*) {
                                                 self.fixtureMenuTimedOut = YES;
                                                 [self->_openMenu cancelTracking];
                                               }];
    [NSRunLoop.mainRunLoop addTimer:cancel forMode:NSRunLoopCommonModes];
    [self contextMenu:[NSEvent mouseEventWithType:NSEventTypeRightMouseDown
                                         location:point
                                    modifierFlags:0
                                        timestamp:mirrorTime()
                                     windowNumber:_window.windowNumber
                                          context:nil
                                      eventNumber:0
                                       clickCount:1
                                         pressure:1]];
    [cancel invalidate];
    [NSNotificationCenter.defaultCenter removeObserver:began];
}
- (double)fixtureSubmitFrame:(BOOL)advance reset:(BOOL)reset {
    // CPU submission microbenchmark only, not display FPS. Never starts a
    // protected session, reads Finder or sends native input events.
    [_timer invalidate];
    if (reset) _session.reset();
    if (advance) {
        if (_session.game()->paused()) _session.playPause();
        _session.game()->tick(1.0 / 60);
    }
    const auto start = SteadyClock::now();
    [self render];
    return elapsedMs(start);
}
#endif
- (BOOL)desktopReady {
    return _desktopReady;
}
- (DPPMirrorView*)viewForEvent:(NSEvent*)event {
    if (!event || !_window) return _view;
    if (event.windowNumber == _window.windowNumber) return _view;
    for (NSUInteger i = 0; i < _extraWindows.count && i < _extraViews.count; ++i) {
        if (event.windowNumber == _extraWindows[i].windowNumber) return _extraViews[i];
    }
    return _view;
}
- (Vec2)scenePointForEvent:(NSEvent*)event {
    DPPMirrorView* v = [self viewForEvent:event];
    const NSPoint p = [v convertPoint:event.locationInWindow fromView:nil];
    return {static_cast<float>(p.x + v.sceneOriginOffset.x),
            static_cast<float>(p.y + v.sceneOriginOffset.y)};
}
- (BOOL)isAnyMirrorWindowKey {
    if (_window.keyWindow) return YES;
    for (DPPMirrorWindow* ew in _extraWindows) {
        if (ew.keyWindow) return YES;
    }
    return NO;
}
- (void)armDesktopReturn {
    if (self.migrating || !self.active ||
        (_directDisabled || (!_returnAfterOpen && !self.migrationProtectionValid)) ||
        !self.allowsBackgroundSession || _openSubmitted)
        return;
    // Stay below normal application windows. Only a deliberate hit wakes physics input.
    const BOOL wasReady = _desktopReady;
    _desktopReady = YES;
    const NSWindowLevel lvl = CGWindowLevelForKey(kCGDesktopIconWindowLevelKey) + 1;
    _window.level = lvl;
    _window.interactive = YES;
    _window.ignoresMouseEvents = NO;
    for (DPPMirrorWindow* ew in _extraWindows) {
        ew.level = lvl;
        ew.interactive = YES;
        ew.ignoresMouseEvents = NO;
    }
    if (!wasReady) [self recordEvent:@"desktop_return_ready" details:@{}];
}
- (void)wakeForDesktopEvent:(NSEvent*)event {
    if (!_desktopReady || _openSubmitted || !self.active) return;
    const Vec2 sp = [self scenePointForEvent:event];
    const bool alt = (event.modifierFlags & NSEventModifierFlagOption) != 0;
    const bool inArenaMode = _session.game()->billiardsEnabled() || _session.game()->slingshotEnabled() ||
                             _session.game()->airHockeyEnabled();
    const bool hitScene = inArenaMode ? _session.game()->interactiveArenaContains(sp)
                                      : (mirrorHit(_session.game()->drawList(), sp) != kNoTexture ||
                                         _session.game()->blackHoleEdge(sp, alt));
    if (!hitScene) return;
    _desktopReady = NO;
    if (!NSApp.active) {
        _wakeActivationDeadline = mirrorTime() + 0.8;
    }
    activatePreviewApp();
    if (_session.phase() != MirrorPhase::Interactive) {
        [self toggleInteraction];
    }
    [self recordEvent:@"desktop_return_hit" details:@{}];
}
- (void)completeOpenRequest {
    _openSubmitted = NO;
    _previous = mirrorTime();
    [self armDesktopReturn];
}
- (BOOL)menuActive {
    return _menuActive;
}
- (void)selectOpen:(id)sender {
    (void)sender;
    if (!_menuActive || _openSubmitted || _menuSelected) return;
    _menuSelected = YES;
    // Accessibility can invoke the action without ending AppKit's tracking loop.
    // Commit only after contextMenu: unwinds, just like a physical menu click.
    [_openMenu cancelTracking];
}
- (void)contextMenu:(NSEvent*)event {
    if (![self acceptsCurrentEvent:event]) return;
    [self guard:^{
      if (self.migrating || !self.active || self->_directDisabled || self->_menuActive ||
          self->_openSubmitted || !self.canOpenItem || !self.openItem ||
          (self->_session.phase() != MirrorPhase::Interactive && !self->_desktopReady))
          return;
      DPPMirrorView* menuView = [self viewForEvent:event];
      const Vec2 sp = [self scenePointForEvent:event];
      if (self->_session.game() && self->_session.game()->billiardsEnabled()) {
          self->_session.game()->input({InputType::PointerCancel, {}, mirrorTime()});
          if (self->_session.game()->tick(0)) [self render];
      }
      uint32_t hit = 0;
      const auto texture = mirrorHit(self->_session.game()->drawList(), sp);
      for (const auto& item : self->_session.items())
          if (item.texture == texture && !item.syntheticFixture) hit = static_cast<uint32_t>(item.id);
      if (!hit) return;
      const BOOL inBilliards = self->_session.game()->billiardsEnabled();
      const BOOL inTube = inBilliards && self->_session.game()->billiardsTubeHit(sp);
      const BOOL available = self.canOpenItem(hit);
      if (self->_session.game()->mode().stats().grabbing) ++self->_releases;
      self->_session.input({InputType::PointerCancel, {}, mirrorTime()});
      self->_menuActive = YES;
      self->_menuSelected = NO;
      NSString* session = self.sessionID;
      const auto revision = self->_migration.revision();
      self->_openMenu = [[NSMenu alloc] initWithTitle:@"DPP"];
      NSMenuItem* action =
          [self->_openMenu addItemWithTitle:(self.opensWithoutEndingSession ? @"打开（保留桌面会话）"
                                                                            : @"打开并结束本次桌面会话")
                                     action:@selector(selectOpen:)
                              keyEquivalent:@""];
      self->_openMenu.autoenablesItems = NO;
      action.enabled = available;
      if (!available) action.title = @"无法打开：目标未解析或不受支持";
      action.target = self;
      [NSMenu popUpContextMenu:self->_openMenu withEvent:event forView:menuView];
      self->_openMenu = nil;
      self->_menuActive = NO;
      self->_previous = mirrorTime();
      if (!self.active || self.migrating || revision != self->_migration.revision() ||
          ![session isEqual:self.sessionID])
          return;
      if (self->_menuSelected && !self->_openSubmitted && self.canOpenItem(hit)) {
          self->_openSubmitted = YES;
          self->_returnAfterOpen = self.opensWithoutEndingSession;
          if (inTube) ++self->_billiardsTubeOpens;
          [self releaseInteraction];
          [self recordEvent:@"open_selected"
                    details:@{
                        @"target_item_id" : @(hit),
                        @"billiards" : @(inBilliards),
                        @"in_tube" : @(inTube),
                        @"tube_opens" : @(self->_billiardsTubeOpens)
                    }];
          // Leave mouse/menu tracking before recovery. Capture only session identity and ID.
          dispatch_async(dispatch_get_main_queue(), ^{
            [self guard:^{
              if (self.active && !self.migrating && revision == self->_migration.revision() &&
                  [session isEqual:self.sessionID] && self.openItem)
                  self.openItem(hit);
            }];
          });
      }
    }];
}
- (void)tick {
    if (_systemSuspended || self.migrating || ![self active] || _menuActive || _openSubmitted) {
        _previous = mirrorTime();
        return;
    }
    const double now = mirrorTime(), dt = now - _previous;
    _previous = now;
    ++_ticks;
    if (_session.game()->tick(dt)) [self render];
    if (_session.game()->takeSlingshotPersistenceDirty()) {
        _pendingSlingshotProgress = _session.game()->slingshotProgress();
        _pendingSlingshotHighScore = std::max(_pendingSlingshotHighScore, _pendingSlingshotProgress.highScore);
        _pendingSlingshotConfig = _session.game()->slingshotConfig();
        if (self.slingshotPersistenceDidChange) {
            self.slingshotPersistenceDidChange(_pendingSlingshotProgress, _pendingSlingshotConfig.customPieces);
        }
    }
    [self flushSounds];
    [self recordBilliardsProgress];
    const double activeRate = (_session.game()->airHockeyEnabled()) ? (1.0 / 120.0) : (1.0 / 60.0);
    const double nextInterval = (_session.game()->idle() && !_billiardsLan.connected()) ? .2 : activeRate;
    _timer.fireDate = [NSDate dateWithTimeIntervalSinceNow:nextInterval];
}
- (void)handleP2Key:(NSEvent*)event pressed:(BOOL)pressed {
    if (!_session.game() || !_session.game()->airHockeyEnabled()) return;
    unsigned int bit = 0;
    switch (event.keyCode) {
        case 13:   // W
        case 126:  // Up
            bit = 1u;
            break;
        case 1:    // S
        case 125:  // Down
            bit = 2u;
            break;
        case 0:    // A
        case 123:  // Left
            bit = 4u;
            break;
        case 2:    // D
        case 124:  // Right
            bit = 8u;
            break;
        default:
            return;
    }
    if (pressed)
        _p2KeyMask |= bit;
    else
        _p2KeyMask &= ~bit;
    if (auto* ah = _session.game()->mutableAirHockeyMode()) {
        const float dx = ((_p2KeyMask & 8u) ? 1.0f : 0.0f) - ((_p2KeyMask & 4u) ? 1.0f : 0.0f);
        const float dy = ((_p2KeyMask & 2u) ? 1.0f : 0.0f) - ((_p2KeyMask & 1u) ? 1.0f : 0.0f);
        ah->setPlayer2KeyDirection({dx, dy});
        self.timer.fireDate = NSDate.date;
    }
}
- (void)hover:(NSEvent*)event {
    [self guard:^{
      if (![self acceptsCurrentEvent:event]) return;
      if (!self->_session.game() ||
          (!self->_session.game()->billiardsEnabled() && !self->_session.game()->airHockeyEnabled()))
          return;
      if (self->_directDisabled || self->_userPaused || self->_menuActive || self->_openSubmitted) return;
      if (self->_session.game()->paused()) return;
      const Vec2 sp = [self scenePointForEvent:event];
      if ((self->_session.game()->airHockeyEnabled() || self->_session.game()->billiardsEnabled()) &&
          self->_desktopReady && self->_session.game()->interactiveArenaContains(sp)) {
          [self wakeForDesktopEvent:event];
      }
      self->_session.game()->input({InputType::PointerMove, sp, mirrorTime()});
      [self syncLanOutbound:{} force:NO];
      if (self->_session.game()->airHockeyEnabled()) {
          // Do NOT run synchronous render or restart an already-imminent 120Hz timer on every mouseMoved!
          if (self.timer.valid && self.timer.fireDate.timeIntervalSinceNow > (1.0 / 95.0)) {
              self.timer.fireDate = NSDate.date;
          }
      } else if (self->_session.game()->billiardsEnabled()) {
          const double nowT = mirrorTime();
          if (nowT - self->_lastImmediateRenderAt >= (1.0 / 120.0)) {
              if (self->_session.game()->tick(0)) {
                  self->_lastImmediateRenderAt = nowT;
                  [self render];
              }
          }
      }
    }];
}
- (BOOL)acceptsCurrentEvent:(NSEvent*)event {
    __block BOOL accepted = NO;
    [self guard:^{
      BOOL winMatch = (event.windowNumber == self.window.windowNumber);
      if (!winMatch) {
          for (DPPMirrorWindow* ew in self->_extraWindows) {
              if (event.windowNumber == ew.windowNumber) {
                  winMatch = YES;
                  break;
              }
          }
      }
      accepted = self.active && !self.migrating && winMatch &&
                 event.timestamp >= self->_committedAt && self.deadline.valid &&
                 self.deadline.fireDate.timeIntervalSinceNow > 0 &&
                 (!self.migrationProtectionValid || self.migrationProtectionValid());
    }];
    return accepted;
}
- (void)pointer:(NSEvent*)event type:(InputType)type {
    [self guard:^{
      if (![self acceptsCurrentEvent:event]) return;
      const bool alt = (event.modifierFlags & NSEventModifierFlagOption) != 0;
      TextureId target = kNoTexture;
      if (type == InputType::PointerDown) {
          if (self->_lastDown == event || self->_directDisabled || self->_userPaused) return;
          self->_lastDown = event;
          const Vec2 sp = [self scenePointForEvent:event];
          const bool inArenaMode = self->_session.game()->billiardsEnabled() ||
                                   self->_session.game()->slingshotEnabled() ||
                                   self->_session.game()->airHockeyEnabled();
          if (inArenaMode) {
              if (!self->_session.game()->interactiveArenaContains(sp)) {
                  if (self->_session.phase() == MirrorPhase::Interactive) {
                      [self releaseInteraction];
                      [self armDesktopReturn];
                  }
                  return;
              }
          } else {
              target = mirrorHit(self->_session.game()->drawList(), sp);
              if (target == kNoTexture && !self->_session.game()->blackHoleEdge(sp, alt)) return;
          }
          [self wakeForDesktopEvent:event];
          if (![self acceptsCurrentEvent:event]) return;
          if (!self->_desktopReady) {
              if (self->_session.phase() != MirrorPhase::Interactive) {
                  [self toggleInteraction];
              }
              if (!NSApp.active || ![self isAnyMirrorWindowKey]) {
                  self->_wakeActivationDeadline = mirrorTime() + 0.8;
                  activatePreviewApp();
                  DPPMirrorView* evView = [self viewForEvent:event];
                  DPPMirrorWindow* evWin = (DPPMirrorWindow*)evView.window;
                  if (!evWin) evWin = self.window;
                  self.window.interactive = YES;
                  self.window.ignoresMouseEvents = NO;
                  for (DPPMirrorWindow* ew in self->_extraWindows) {
                      ew.interactive = YES;
                      ew.ignoresMouseEvents = NO;
                  }
                  [evWin makeKeyAndOrderFront:nil];
                  [evWin makeFirstResponder:evView];
              }
              if (self->_session.phase() == MirrorPhase::Interactive) {
                  self->_pointerGestureActive = YES;
              }
          }
      }
      if (self->_desktopReady) return;
      if (self->_menuActive || self->_openSubmitted) return;
      const bool pendingWake = self->_pointerGestureActive || (mirrorTime() < self->_wakeActivationDeadline) ||
                               self.continuousSession;
      if (self->_session.phase() != MirrorPhase::Interactive ||
          ((![self isAnyMirrorWindowKey] || !NSApp.active) && !pendingWake)) {
          [self releaseInteraction];
          [self armDesktopReturn];
          return;
      }
      if (type == InputType::PointerUp || type == InputType::PointerCancel) {
          self->_pointerGestureActive = NO;
      }
      const Vec2 spMove = [self scenePointForEvent:event];
      bool before = self->_session.game()->mode().stats().grabbing;
      self->_session.input(
          {type, spMove, mirrorTime(), target, alt});
      if (self->_session.game()->billiardsEnabled() || self->_session.game()->slingshotEnabled() ||
          self->_session.game()->airHockeyEnabled()) {
          if (self->_session.game()->billiardsEnabled()) {
              const bool guideNow = self->_session.game()->billiardsConfig().showAimGuide;
              if (guideNow != self->_pendingBilliardsConfig.showAimGuide) {
                  self->_pendingBilliardsConfig.showAimGuide = guideNow;
                  if (self.billiardsAimGuideDidChange) self.billiardsAimGuideDidChange(guideNow);
              }
              const bool intentNow = self->_session.game()->billiardsConfig().showAiIntent;
              if (intentNow != self->_pendingBilliardsConfig.showAiIntent) {
                  self->_pendingBilliardsConfig.showAiIntent = intentNow;
                  if (self.billiardsAiIntentDidChange) self.billiardsAiIntentDidChange(intentNow);
              }
              [self recordBilliardsProgress];
          } else if (self->_session.game()->slingshotEnabled()) {
              const bool trajNow = self->_session.game()->slingshotConfig().showTrajectory;
              if (trajNow != self->_pendingSlingshotConfig.showTrajectory) {
                  self->_pendingSlingshotConfig.showTrajectory = trajNow;
                  if (self.slingshotTrajectoryDidChange) self.slingshotTrajectoryDidChange(trajNow);
              }
              self->_pendingSlingshotHighScore =
                  std::max(self->_pendingSlingshotHighScore, self->_session.game()->slingshotHighScore());
              if (self->_session.game()->takeSlingshotPersistenceDirty()) {
                  self->_pendingSlingshotProgress = self->_session.game()->slingshotProgress();
                  self->_pendingSlingshotConfig = self->_session.game()->slingshotConfig();
                  if (self.slingshotPersistenceDidChange) {
                      self.slingshotPersistenceDidChange(self->_pendingSlingshotProgress,
                                                         self->_pendingSlingshotConfig.customPieces);
                  }
              }
          } else if (self->_session.game()->airHockeyEnabled()) {
              const auto& hc = self->_session.game()->airHockeyConfig();
              if (hc.difficulty != self->_pendingAirHockeyConfig.difficulty ||
                  hc.showRinkChrome != self->_pendingAirHockeyConfig.showRinkChrome) {
                  self->_pendingAirHockeyConfig.difficulty = hc.difficulty;
                  self->_pendingAirHockeyConfig.showRinkChrome = hc.showRinkChrome;
                  if (self.airHockeyOptionsDidChange)
                      self.airHockeyOptionsDidChange(hc.difficulty, hc.showRinkChrome);
              }
          }
          if (type != InputType::PointerMove ||
              (mirrorTime() - self->_lastImmediateRenderAt >= (1.0 / 120.0))) {
              if (self->_session.game()->tick(0)) {
                  self->_lastImmediateRenderAt = mirrorTime();
                  [self render];
              }
          }
          [self flushSounds];
      } else if (const auto* g = dynamic_cast<const GravityMode*>(&self->_session.game()->mode())) {
          const auto bh = g->blackHole();
          if (bh.elevation != self->_toyBlackHoleElevation || bh.tilt != self->_toyBlackHoleTilt) {
              self->_toyBlackHoleElevation = bh.elevation;
              self->_toyBlackHoleTilt = bh.tilt;
              if (mirrorTime() - self->_lastImmediateRenderAt >= (1.0 / 120.0)) {
                  if (self->_session.game()->tick(0)) {
                      self->_lastImmediateRenderAt = mirrorTime();
                      [self render];
                  }
              }
              if (self.blackHoleAnglesDidChange) self.blackHoleAnglesDidChange(bh.elevation, bh.tilt);
          }
          [self flushSounds];
      }
      bool after = self->_session.game()->mode().stats().grabbing;
      if (!before && after) {
          ++self->_grabs;
          [self recordEvent:@"grab" details:@{}];
      }
      if (before && !after) {
          ++self->_releases;
          [self recordEvent:@"release" details:@{}];
      }
      ++self->_inputs;
      const double wakeThresh =
          (self->_session.game() && self->_session.game()->airHockeyEnabled()) ? (1.0 / 95.0) : (1.0 / 50.0);
      if (self.timer.valid && self.timer.fireDate.timeIntervalSinceNow > wakeThresh) {
          self.timer.fireDate = NSDate.date;
      }
    }];
}
- (void)releaseInteraction {
    _desktopReady = NO;
    _pointerGestureActive = NO;
    if (_session.game() && _session.game()->mode().stats().grabbing) ++_releases;
    if (_interactionStart) {
        if (_interactionIntervals.count < 128)
            [_interactionIntervals addObject:@{
                @"start" : _interactionStart,
                @"end" : [[[NSISO8601DateFormatter alloc] init] stringFromDate:NSDate.date]
            }];
        _interactionStart = nil;
    }
    const NSWindowLevel passiveLvl = CGWindowLevelForKey(kCGDesktopIconWindowLevelKey) + 1;
    _window.ignoresMouseEvents = YES;
    _window.interactive = NO;
    _window.level = passiveLvl;
    for (DPPMirrorWindow* ew in _extraWindows) {
        ew.ignoresMouseEvents = YES;
        ew.interactive = NO;
        ew.level = passiveLvl;
    }
    _session.releaseInput();
    if (_window.keyWindow) [_window resignKeyWindow];
    for (DPPMirrorWindow* ew in _extraWindows) {
        if (ew.keyWindow) [ew resignKeyWindow];
    }
}
- (void)playPause {
    if (self.migrating) return;
    [self guard:^{
      self->_returnAfterOpen = self->_desktopReady = NO;
      [self releaseInteraction];
      self->_session.playPause();
      self->_userPaused = self->_session.game()->paused();
      if (self->_userPaused) self->_billiardsAudio.stop();
      [self armDesktopReturn];
      if (self->_session.game() && self->_session.game()->paused()) ++self->_pauses;
      if (self->_session.game())
          [self recordEvent:self->_session.game()->paused() ? @"pause" : @"resume" details:@{}];
      self->_previous = mirrorTime();
      self.timer.fireDate = NSDate.date;
    }];
}
- (void)handleResetShortcut {
    // Avoid accidental reset while Player 2 is holding WASD right next to 'R' in 2P Air Hockey.
    if (_session.game() && _session.game()->airHockeyEnabled() && _p2KeyMask != 0) return;
    [self reset];
}
- (void)reset {
    if (self.migrating) return;
    [self guard:^{
      self->_returnAfterOpen = self->_desktopReady = NO;
      if (![self active]) return;
      [self releaseInteraction];
      if (self->_session.game() &&
          (self->_session.game()->billiardsEnabled() || self->_session.game()->slingshotEnabled() ||
           self->_session.game()->airHockeyEnabled() ||
           self->_session.game()->playMode() == PlayMode::BlackHole)) {
          const BOOL inBil = self->_session.game()->billiardsEnabled();
          self->_session.resetBilliards();
          if (inBil) {
              [self syncLanRole];
              [self syncLanOutbound:{} force:YES];
          }
          self->_userPaused = NO;
          [self armDesktopReturn];
          ++self->_resets;
          [self recordEvent:@"reset" details:inBil ? @{@"billiards" : @YES} : @{}];
          [self render];
          self->_previous = mirrorTime();
          self.timer.fireDate = NSDate.date;
          return;
      }
      self->_session.reset();
      self->_userPaused = YES;
      [self armDesktopReturn];
      ++self->_resets;
      [self recordEvent:@"reset" details:@{}];
      [self render];
      self->_previous = mirrorTime();
    }];
}
- (void)disableInteraction {
    [self guard:^{
      self->_directDisabled = YES;
      self->_wakeActivationDeadline = 0;
      self->_returnAfterOpen = NO;
      [self releaseInteraction];
      [self recordEvent:@"interaction_disabled" details:@{}];
    }];
}
- (void)toggleInteraction {
    if (self.migrating) return;
    [self guard:^{
      if (![self active]) return;
      if (self->_session.phase() == MirrorPhase::Interactive) {
          self->_returnAfterOpen = self->_desktopReady = NO;
          self->_wakeActivationDeadline = 0;
          self->_directDisabled = YES;
          [self releaseInteraction];
          [self recordEvent:@"interaction_disabled" details:@{}];
          return;
      }
      self->_desktopReady = NO;
      self->_directDisabled = NO;
      // Re-enabling input is not a resume command. Keep explicit pause/reset
      // intact; the initial static preview (not user-paused) still wakes on hit.
      if (self->_userPaused) {
          [self armDesktopReturn];
          [self recordEvent:@"interaction_rearmed_while_paused" details:@{}];
          return;
      }
      if (!NSApp.active) {
          self->_wakeActivationDeadline = mirrorTime() + 0.8;
          activatePreviewApp();
      }
      self->_session.interact();
      self.interactionStart = [[[NSISO8601DateFormatter alloc] init] stringFromDate:NSDate.date];
      const NSWindowLevel actLvl = self.allowsBackgroundSession
                                       ? CGWindowLevelForKey(kCGDesktopIconWindowLevelKey) + 1
                                       : NSNormalWindowLevel;
      self.window.interactive = YES;
      self.window.ignoresMouseEvents = NO;
      self.window.level = actLvl;
      for (DPPMirrorWindow* ew in self->_extraWindows) {
          ew.interactive = YES;
          ew.ignoresMouseEvents = NO;
          ew.level = actLvl;
          [ew orderFront:nil];
      }
      [self.window makeKeyAndOrderFront:nil];
      [self.window makeFirstResponder:self.view];
      const bool pendingWake = (mirrorTime() < self->_wakeActivationDeadline) || self.continuousSession;
      if (![self isAnyMirrorWindowKey] && !pendingWake) {
          [self releaseInteraction];
          [self armDesktopReturn];
      }
      [self recordEvent:@"interaction_enabled" details:@{@"key_window" : @([self isAnyMirrorWindowKey])}];
      self->_previous = mirrorTime();
      self.timer.fireDate = NSDate.date;
    }];
}
- (void)appDidBecomeActive:(NSNotification*)note {
    (void)note;
    [self guard:^{
      self->_wakeActivationDeadline = 0;
      if ([self active] && !self.migrating && !self->_directDisabled && !self->_userPaused &&
          self->_session.phase() == MirrorPhase::Interactive) {
          self.window.interactive = YES;
          self.window.ignoresMouseEvents = NO;
          for (DPPMirrorWindow* ew in self->_extraWindows) {
              ew.interactive = YES;
              ew.ignoresMouseEvents = NO;
              [ew orderFront:nil];
          }
          if (![self isAnyMirrorWindowKey]) {
              [self.window makeKeyAndOrderFront:nil];
              [self.window makeFirstResponder:self.view];
          }
          self->_previous = mirrorTime();
          self.timer.fireDate = NSDate.date;
      }
    }];
}
- (void)windowDidResignKey:(NSNotification*)note {
    (void)note;
    [self guard:^{
      if (!self->_menuActive && NSApp.active && self->_session.phase() == MirrorPhase::Interactive &&
          ![self isAnyMirrorWindowKey] && !self->_pointerGestureActive) {
          ++self->_panelSwitches;
          [self releaseInteraction];
          [self armDesktopReturn];
      }
    }];
}
- (void)appResigned:(NSNotification*)note {
    (void)note;
    [self guard:^{
      if ([self active]) {
          ++self->_externalSwitches;
          if (self.allowsBackgroundSession) {
              [self releaseInteraction];
              [self armDesktopReturn];
              self->_previous = mirrorTime();
              return;
          }
          [self close:@"external_application_switch_reacquire_snapshot"];
      }
    }];
}
- (void)setSystemSuspended:(BOOL)suspended {
    [self guard:^{
      if (!self.active) return;
      self->_systemSuspended = suspended;
      if (suspended) {
          self->_p2KeyMask = 0;
          [self->_openMenu cancelTracking];
          self->_menuSelected = NO;
          [self releaseInteraction];
          self->_billiardsAudio.stop();
          self->_previous = mirrorTime();
          [self recordEvent:@"system_suspended" details:@{@"continuous" : @(self.continuousSession)}];
      } else {
          self->_previous = mirrorTime();
          if (self->_session.game()) {
              self->_labelContentDirty = YES;
              self->_session.game()->forceRedraw();
              [self render];
          }
          [self armDesktopReturn];
          if (self.timer.valid) self.timer.fireDate = NSDate.date;
          (void)[self checkDisplayForMigration];
          [self recordEvent:@"system_resumed" details:@{@"continuous" : @(self.continuousSession)}];
      }
    }];
}
- (std::vector<dpp::Vec2>)currentIconSceneCenters {
    if (!self.active || !_session.game()) return {};
    return _session.game()->currentIconSceneCenters();
}
- (void)windowDidChangeBackingProperties:(NSNotification*)note {
    [self configurationChanged:note];
}
- (void)configurationChanged:(NSNotification*)note {
    [self guard:^{
      if (!self.active) return;
      if (self.continuousSession) {
          if ([note.name isEqual:NSWorkspaceWillSleepNotification]) {
              [self setSystemSuspended:YES];
              return;
          }
          if ([note.name isEqual:NSWorkspaceDidWakeNotification]) {
              [self setSystemSuspended:NO];
              return;
          }
          if ([note.name isEqual:NSWorkspaceActiveSpaceDidChangeNotification]) {
              [self releaseInteraction];
              [self armDesktopReturn];
              self->_previous = mirrorTime();
              return;
          }
      } else if ([note.name isEqual:NSWorkspaceDidWakeNotification]) {
          return;
      }
#ifdef DPP_MIRROR_FAULT_TESTING
      std::printf("fixture_configuration menu_active=%d selected=%d submitted=%d return_after_open=%d "
                  "allows_background=%d app_active=%d key=%d phase=%d\n",
                  self->_menuActive, self->_menuSelected, self->_openSubmitted, self->_returnAfterOpen,
                  self.allowsBackgroundSession, NSApp.active, self->_window.keyWindow,
                  static_cast<int>(self->_session.phase()));
      std::fflush(stdout);
#endif
      NSString* reason = @"unknown_configuration_notification";
      if ([note.name isEqual:NSApplicationDidChangeScreenParametersNotification] ||
          [note.name isEqual:NSWindowDidChangeBackingPropertiesNotification]) {
          const auto current = [self sampledDisplay];
          const bool compatible =
              mirrorDisplayCompatible(self->_capturedDisplay, current, [self fixedSceneGeometryValid]) &&
              (!self->_geometryReady || [self fixedSceneGeometryValid]) &&
              self->_window.backingScaleFactor == self->_capturedDisplay.scale;
          logMirrorDisplay(self->_capturedDisplay, current, compatible);
          logDisplayDecision(
              self->_capturedDisplay, current, self->_window, @{
                  @"protection" : [self currentProtection:NO],
                  @"source" : note.name,
                  @"session_id" : self.sessionID ? self.sessionID : @"none",
                  @"phase" : @(static_cast<int>(self->_session.phase())),
                  @"preview_deadline_epoch" : self.deadline
                      ? @([self.deadline.fireDate timeIntervalSince1970])
                      : (id)NSNull.null,
                  @"menu_active" : @(self->_menuActive),
                  @"open_submitted" : @(self->_openSubmitted),
                  @"scene_draw_input_contract" : @"immutable_startup_work_AppKit_points_y_down",
                  @"baseline_sample" : @"startup_not_current"
              },
              compatible);
          if ([self sameDisplayIgnoringMinorWorkJitter:current] && !self.migrating) return;
          if (self.migrationProtectionValid) {
              [self beginDisplayMigration:current];
              return;
          }
          if (compatible) return;
          reason = @"screen_geometry_or_scale_changed";
      } else if ([note.name isEqual:NSWorkspaceActiveSpaceDidChangeNotification]) {
          reason = @"active_space_changed";
      } else if ([note.name isEqual:NSWorkspaceWillSleepNotification]) {
          reason = @"system_will_sleep";
      }
      std::printf("mirror_configuration_exit reason=%s\n", reason.UTF8String);
      std::fflush(stdout);
      [self close:reason];
    }];
}
- (NSDictionary*)diagnostics {
    if (![self active]) {
        NSMutableDictionary* report =
            [(_lastReport ? _lastReport : @{@"phase" : @"closed", @"reason" : _reason}) mutableCopy];
        if ([_reason isEqual:[NSString stringWithUTF8String:assistedUnavailableReason]])
            report[@"start_rejection"] = _reason;
        return report;
    }
    auto* g = _session.game();
    NSMutableArray* items = [NSMutableArray array];
    for (const auto& item : _session.items())
        [items addObject:@{
            @"internal_id" : @(static_cast<uint32_t>(item.id)),
            @"texture_id" : @(static_cast<uint32_t>(item.texture)),
            @"raw_dpos" : @[ @(item.rawDpos.x), @(item.rawDpos.y) ],
            @"scene_anchor" : @[ @(item.sceneAnchor.x), @(item.sceneAnchor.y) ],
            @"coordinate_validation" : item.syntheticFixture ? @"synthetic_fixture_not_manual"
                                                             : @"this_snapshot_manual_samples",
            @"position_source" : item.syntheticFixture ? @"synthetic_fixture" : @"Finder_dpos",
            @"dpp_draw_size" : @[ @(item.dppDrawSize.x), @(item.dppDrawSize.y) ],
            @"finder_icon_size_raw" : item.finderIconSize > 0 ? @(item.finderIconSize)
                                                              : static_cast<id>(NSNull.null),
            @"draw_size_source" : item.finderIconSize > 0 ? @"Finder_view_configuration_as_points"
                                                          : @"DPP_default_48pt",
            @"dpp_collision" : @"rounded box; GravityMode 0.90 scale",
            @"dpp_collision_size" : @[ @(item.dppCollisionBox.x), @(item.dppCollisionBox.y) ],
            @"system_image_bounds" : NSNull.null,
            @"system_label_bounds" : NSNull.null
        }];
    return @{
        @"phase" : _session.phase() == MirrorPhase::Static
            ? @"static"
            : (_session.phase() == MirrorPhase::Interactive ? @"interactive" : @"playing"),
        @"protection" : [self currentProtection:NO],
        @"session_id" : _sessionID,
        @"events" : [_traceEvents copy],
        @"events_dropped" : @(_traceDropped),
        @"events_limit" : @260,
        @"build_id" : @DPP_BUILD_ID,
        @"snapshot_id" : _snapshotID ? _snapshotID : @"synthetic_fixture",
        @"startup_edge_inset_count" : @(_session.startupInsetCount()),
        @"experiment_id" : _experimentID ? _experimentID : @"ordinary_mirror",
        @"started_at" : _startedAt,
        @"interaction_intervals" : [_interactionIntervals copy],
        @"interaction_intervals_limit" : @128,
        @"grabs" : @(_grabs),
        @"releases" : @(_releases),
        @"pauses" : @(_pauses),
        @"paused" : @(g->paused()),
        @"items" : items,
        @"ticks" : @(_ticks),
        @"pointer_events" : @(_inputs),
        @"starts" : @(_starts),
        @"closes_before_this_session" : @(_closes),
        @"resets" : @(_resets),
        @"external_switches" : @(_externalSwitches),
        @"panel_switches" : @(_panelSwitches),
        @"grabbing" : @(g->mode().stats().grabbing),
        @"desktop_ready" : @(_desktopReady),
        @"bodies" : @(g->mode().stats().bodies),
        @"billiards_enabled" : @(g->billiardsEnabled()),
        @"billiards_toggles" : @(_billiardsToggles),
        @"billiards_shots" : @(g->billiardsSessionStats().totalShots),
        @"billiards_pocketed" : @(g->billiardsSessionStats().totalPocketed),
        @"billiards_white_placements" : @(g->billiardsSessionStats().totalWhitePlacements),
        @"billiards_tube_opens" : @(_billiardsTubeOpens),
        @"billiards_best_shots" : @(g->billiardsSessionStats().bestShots),
        @"billiards_sound_events" : @(g->billiardsSessionStats().soundEvents),
        @"sound_muted" : @(_soundMuted),
        @"layer_count" : @(_renderer ? _renderer->layerCount() : 0),
        @"image_count" : @(_session.items().size()),
        @"filename_string_writes" : @(_labelStringWrites),
        @"filename_scale_writes" : @(_labelScaleWrites),
        @"filename_geometry_writes" : @(_labelGeometryWrites),
        @"filename_layer_count" : @(_labelLayers.count),
        @"filenames_visible" : @(_showsFilenames),
        @"filename_visible_layers" :
            @([[_labelLayers filteredArrayUsingPredicate:[NSPredicate predicateWithBlock:^BOOL(
                                                                          CATextLayer* layer, NSDictionary*) {
                               return !layer.hidden;
                             }]] count]),
        @"physics" : statistics(g->perf.physics),
        @"submit" : statistics(g->perf.submit),
        @"interval" : statistics(g->perf.interval),
        @"performance_classification" : @"diagnostic_only_not_validated_workload",
        @"gpu_ms" : NSNull.null,
        @"presented_fps" : NSNull.null,
        @"visual_acceptance" : @"requires_user_report",
        @"real_desktop_mode" : @"disabled",
        @"original_icons" : _experimentID ? @"user_reported_hidden_not_machine_verified"
                                          : @"retained_possible_duplicates"
    };
}
- (void)close:(NSString*)reason {
    _pointerGestureActive = NO;
    _wakeActivationDeadline = 0;
    _systemSuspended = NO;
    if (_globalMoveMonitor) {
        [NSEvent removeMonitor:_globalMoveMonitor];
        _globalMoveMonitor = nil;
    }
    _billiardsLan.stopDiscovery();
    _billiardsLan.stop();
    _billiardsAudio.stop();
    _soundBuf.clear();
    _lanSoundBuf.clear();
    _migration.invalidate();
    _migrationRenderer.reset();
    _migrationLayer = nil;
#ifdef DPP_MIRROR_FAULT_TESTING
    _fixtureDisplayEnabled = NO;
#endif
    [_migrationTimer invalidate];
    _migrationTimer = nil;
    _cachedImages = nil;
    [_openMenu cancelTracking];
    _menuSelected = NO;
    const BOOL existed = [self active] || _window != nil;
    if ([self active]) {
        [self releaseInteraction];
        [self recordEvent:@"close_input_released" details:@{@"reason" : reason}];
        @try {
            try {
                _lastReport = [self diagnostics];
            } catch (...) {
            }
        } @catch (NSException*) {
        }
    }
    [_timer invalidate];
    _timer = nil;
    [_deadline invalidate];
    _deadline = nil;
    // Release input before teardown; destroying the physics world destroys any mouse joint.
    for (DPPMirrorWindow* ew in _extraWindows) {
        ew.ignoresMouseEvents = YES;
        ew.interactive = NO;
        ew.delegate = nil;
        [ew orderOut:nil];
        [ew close];
    }
    for (DPPMirrorView* ev in _extraViews) ev.owner = nil;
    _extraWindows = nil;
    _extraViews = nil;
    _extraLabelLayers = nil;
    _extraRenderers.clear();
    _window.ignoresMouseEvents = YES;
    _window.interactive = NO;
    _window.delegate = nil;
    [_window orderOut:nil];
    [_window close];
    _view.owner = nil;
    _view = nil;
    _window = nil;
    _session.close();
    _renderer.reset();
    _labelLayers = nil;
    _labelTextures.clear();
    _reason = reason;
    if (existed) {
        ++_closes;
        NSMutableDictionary* report = [(_lastReport ? _lastReport : @{}) mutableCopy];
        report[@"phase"] = @"closed";
        report[@"reason"] = reason;
        report[@"closes"] = @(_closes);
        report[@"owned_windows_after_close"] = @0;
        report[@"owned_bodies_after_close"] = @0;
        report[@"owned_images_after_close"] = @0;
        report[@"input_after_close"] = @"released";
        report[@"ended_at"] = [[[NSISO8601DateFormatter alloc] init] stringFromDate:NSDate.date];
        report[@"owned_layers_after_close"] = @0;
        _lastReport = report;
        [_summaries addObject:report];
        while (_summaries.count > 32) [_summaries removeObjectAtIndex:0];
        [self persistSummaries];
        [self recordEvent:@"closed" details:@{@"reason" : reason}];
        if (_didClose) _didClose(reason);
    }
}
- (void)persistSummaries {
    // JSON data supports null values; bounded writes occur only on close / human report.
    NSData* data = [NSJSONSerialization dataWithJSONObject:_summaries options:0 error:nil];
    if (data) [NSUserDefaults.standardUserDefaults setObject:data forKey:mirrorJournalKey()];
}
- (NSArray*)sessionSummaries {
    return [_summaries copy];
}
- (void)recordPreparedCleanup:(NSUInteger)images references:(NSUInteger)references {
    if (!_summaries.count || ![_summaries.lastObject[@"session_id"] isEqual:_sessionID]) return;
    NSMutableDictionary* row = [_summaries.lastObject mutableCopy];
    row[@"prepared_images_after_close"] = @(images);
    row[@"prepared_references_after_close"] = @(references);
    _summaries[_summaries.count - 1] = row;
    [self persistSummaries];
}
- (void)recordHuman:(NSDictionary*)observation {
    if (!_summaries.count) return;
    NSMutableDictionary* row = [_summaries.lastObject mutableCopy];
    NSMutableArray* reports =
        row[@"human_reports"] ? [row[@"human_reports"] mutableCopy] : [NSMutableArray array];
    if (reports.count == 32) [reports removeObjectAtIndex:0];
    [reports addObject:@{
        @"session_id" : row[@"session_id"],
        @"source" : @"user_report",
        @"machine_verified" : @NO,
        @"observation" : observation
    }];
    row[@"human_reports"] = reports;
    _summaries[_summaries.count - 1] = row;
    [self persistSummaries];
}
- (void)dealloc {
    [_timer invalidate];
    [_deadline invalidate];
    [NSNotificationCenter.defaultCenter removeObserver:self];
    [NSWorkspace.sharedWorkspace.notificationCenter removeObserver:self];
}
@end
