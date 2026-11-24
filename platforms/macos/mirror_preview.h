#pragma once
#import <AppKit/AppKit.h>
#include "platforms/macos/mirror_session.h"
// Own-resource preview only. No desktop writes or recovery backend.
@interface DPPMirrorPreview : NSObject <NSWindowDelegate>
@property(copy) NSString* snapshotID;
@property(copy) NSDictionary* protectionIdentity;
@property(copy) NSDictionary* (^protectionReader)(BOOL requestHelperSample);
- (void)recordEvent:(NSString*)name details:(NSDictionary*)details;
@property(copy) NSString* experimentID;
@property(nonatomic, copy)
    NSArray<NSString*>* labels;  // optional full desktop labels, retained only in memory
@property BOOL deferPresentation;
@property BOOL continuousSession;
@property BOOL multiScreenMode;
@property NSUInteger targetScreenIndex;
@property(nonatomic, copy) NSArray<NSValue*>* multiScreenViewports;
- (void)setToyBlackHole:(BOOL)enabled;
// Size factor in [0.5, 2]; remembered for later enables, scales radius and range together.
- (void)setToyBlackHoleScale:(double)scale;
// 3D pitch (elevation, [-85 deg, +85 deg]) and 2D screen roll (tilt, [-pi, +pi]) in radians.
- (void)setToyBlackHoleElevation:(double)elevation tilt:(double)tilt;
@property(copy) void (^blackHoleAnglesDidChange)(double elevation, double tilt);
// Devour-all toggle: attraction grows until every copy is swallowed (copies only).
- (void)setToyBlackHoleDevour:(BOOL)devour;
- (void)applyGravityMagnitude:(float)mps2
                    direction:(dpp::GravityDirection)direction
                  restitution:(float)restitution;
- (float)gravityMagnitude;
- (dpp::GravityDirection)gravityDirection;
- (float)gravityRestitution;
- (dpp::PlayMode)playMode;
- (void)setPlayMode:(dpp::PlayMode)mode;
- (BOOL)canEnableBilliards;
- (BOOL)canEnableBilliardsWithConfig:(const dpp::BilliardsConfig&)config;
- (BOOL)canEnableAnyBilliards;
- (void)ensureInteractiveIfAllowed;
@property(readonly) NSWindow* overlayWindow;
@property(copy) void (^onToggleHudShortcut)(void);
@property(readonly) BOOL billiardsEnabled;
- (void)setBilliards:(BOOL)enabled;
- (dpp::BilliardsConfig)billiardsConfig;
- (void)applyBilliardsConfig:(const dpp::BilliardsConfig&)config;
@property(readonly) BOOL slingshotEnabled;
- (dpp::SlingshotConfig)slingshotConfig;
- (void)applySlingshotConfig:(const dpp::SlingshotConfig&)config;
@property(nonatomic) int slingshotHighScore;
- (dpp::SlingshotProgress)slingshotProgress;
- (void)applySlingshotProgress:(const dpp::SlingshotProgress&)progress;
- (void)enterSlingshotEditor;
@property(copy) void (^slingshotTrajectoryDidChange)(BOOL enabled);
@property(copy) void (^slingshotPersistenceDidChange)(const dpp::SlingshotProgress& progress,
                                                      const std::vector<dpp::SlingshotCustomPiece>& pieces);
@property(readonly) BOOL airHockeyEnabled;
- (dpp::AirHockeyConfig)airHockeyConfig;
- (void)applyAirHockeyConfig:(const dpp::AirHockeyConfig&)config;
@property(copy) void (^airHockeyOptionsDidChange)(dpp::AirHockeyDifficulty difficulty, BOOL showChrome);
@property(copy) void (^billiardsAimGuideDidChange)(BOOL enabled);
@property(copy) void (^billiardsAiIntentDidChange)(BOOL enabled);
@property(copy) void (^lanStatusChanged)(NSString* status, BOOL activeOrWaiting, BOOL isHost);
@property(copy) void (^lanRoomsUpdated)(NSArray<NSString*>* labels);
- (BOOL)startLanHost:(uint16_t)port;
- (BOOL)joinLanEndpoint:(NSString*)endpoint;
- (BOOL)startLanDiscovery;
- (void)stopLanDiscovery;
- (BOOL)quickScanAndJoinLan;
- (NSArray<NSString*>*)discoveredLanRoomLabels;
- (NSString*)discoveredLanRoomEndpointAtIndex:(NSUInteger)index;
- (void)stopLan;
@property(readonly) BOOL lanActive;
@property(readonly) uint16_t lanBoundPort;
- (NSString*)lanStatusString;
- (void)setSystemSuspended:(BOOL)suspended;
- (std::vector<dpp::Vec2>)currentIconSceneCenters;
@property(nonatomic) BOOL soundMuted;      // defaults to NO; mutes procedural billiards sound effects
@property(nonatomic) BOOL showsFilenames;  // defaults to YES; affects only cached replica labels
- (BOOL)presentPrepared;
- (NSArray*)sessionSummaries;
- (void)recordPreparedCleanup:(NSUInteger)images references:(NSUInteger)references;
- (void)recordHuman:(NSDictionary*)observation;
// Called immediately before migration commit; checks the existing helper/deadline.
@property(copy) BOOL (^migrationProtectionValid)(void);
@property(copy) void (^displayCommitted)(void);
- (BOOL)checkDisplayForMigration;
@property(readonly) BOOL migrating;
@property(readonly) BOOL committedDisplayMatches;
@property(copy) BOOL (^canOpenItem)(uint32_t itemID);
@property(copy) void (^openItem)(uint32_t itemID);
@property(readonly) BOOL menuActive;
@property(copy, readonly) NSDictionary* startupFailure;
@property(readonly) BOOL desktopReady;
@property BOOL opensWithoutEndingSession;
@property BOOL allowsBackgroundSession;
- (void)completeOpenRequest;
@property(copy) void (^didClose)(NSString* reason);
#ifdef DPP_MIRROR_FAULT_TESTING
@property NSUInteger failAfterStage;
@property BOOL fixtureMenuTimedOut;
- (void)fixtureWorkAreaChange;
- (void)fixtureMigrationWork:(NSRect)work scale:(CGFloat)scale;
- (void)fixtureAdvanceMigration;

- (void)fixtureConfiguration:(NSString*)name changedGeometry:(BOOL)changed;
- (void)fixtureContextMenu:(BOOL)select;
- (void)fixturePointer:(NSEventType)type;
- (double)fixtureSubmitFrame:(BOOL)advance reset:(BOOL)reset;
#endif
- (BOOL)start:(const dpp::mac::MirrorSnapshot&)snapshot images:(NSArray*)images;
- (void)playPause;
- (void)reset;
- (void)toggleInteraction;
- (void)disableInteraction;
- (void)close:(NSString*)reason;
- (NSDictionary*)diagnostics;
- (BOOL)active;
- (BOOL)fixedSceneGeometryValid;
@end
