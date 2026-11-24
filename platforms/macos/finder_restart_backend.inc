// Included inside protected_session.mm's private namespace. Shares its durable
// store, locks, process identity checks and SessionCoordinator; no core changes.
const CFStringRef finderDomain = CFSTR("com.apple.finder");
const CFStringRef finderKey = CFSTR("CreateDesktop");

bool validIdentity(id value) {
    if (![value isKindOfClass:NSDictionary.class]) return false;
    for (NSString* key in @[ @"pid", @"seconds", @"microseconds", @"uid" ])
        if (![value[key] isKindOfClass:NSNumber.class]) return false;
    return [value count] == 4 && [value[@"pid"] intValue] > 1 && [value[@"seconds"] longLongValue] > 0 &&
           [value[@"microseconds"] longLongValue] >= 0 && [value[@"microseconds"] longLongValue] < 1000000 &&
           [value[@"uid"] isEqual:@(getuid())];
}
void validateFinderRecord(NSDictionary* r) {
    NSArray* keys = @[
        @"version",
        @"backend",
        @"consent",
        @"session_id",
        @"uid",
        @"login_session",
        @"owner",
        @"finder",
        @"helper",
        @"hidden_finder",
        @"restore_finder",
        @"restored_finder",
        @"original",
        @"expected",
        @"phase",
        @"step",
        @"result",
        @"started_ms",
        @"limit_ms",
        @"helper_ready_ms",
        @"active_ms",
        @"restored_ms",
        @"hide_restart_started",
        @"restore_restart_started",
        @"preference_restored",
        @"recovery_trigger",
        @"session_kind",
        @"mirror_item_count",
        @"mirror_snapshot_id"
    ];
    for (id key in r) need([keys containsObject:key], "unknown Finder recovery field; retained");
    const bool v5 = [r[@"version"] isEqual:@5];
    const bool v6 = [r[@"version"] isEqual:@6];
    const bool mirror = v5 || v6;
    need(mirror ? ([r[@"mirror_item_count"] isKindOfClass:NSNumber.class] &&
                   [r[@"mirror_item_count"] unsignedLongLongValue] >= 1 &&
                   [r[@"mirror_item_count"] unsignedLongLongValue] <= 128 &&
                   [r[@"mirror_item_count"] doubleValue] ==
                       static_cast<double>([r[@"mirror_item_count"] unsignedLongLongValue]) &&
                   [r[@"mirror_snapshot_id"] isKindOfClass:NSString.class] &&
                   [[NSUUID alloc] initWithUUIDString:r[@"mirror_snapshot_id"]])
                : (!r[@"mirror_item_count"] && !r[@"mirror_snapshot_id"]),
         "invalid mirror recovery metadata");
    need((v5 ? [r[@"session_kind"] isEqual:@"protected_full_mirror"]
             : (v6 ? [r[@"session_kind"] isEqual:@"protected_continuous_mirror"] : !r[@"session_kind"])),
         "invalid Finder session kind");
    const unsigned long long expectedLimitMs =
        v6 ? finderContinuousMirrorLimitMs : (v5 ? finderMirrorLimitMs : finderLimitMs);
    need(isFinderRecord(r) && [r[@"backend"] isEqual:@"finder_create_desktop_restart_v1"] &&
             [r[@"consent"] isEqual:@"manual_finder_restart_experiment"] && [r[@"uid"] isEqual:@(getuid())] &&
             [r[@"session_id"] isKindOfClass:NSString.class] &&
             [[NSUUID alloc] initWithUUIDString:r[@"session_id"]] &&
             [r[@"login_session"] isKindOfClass:NSNumber.class] && validIdentity(r[@"owner"]) &&
             validIdentity(r[@"finder"]) && [@[ @"absent", @"true" ] containsObject:r[@"original"]] &&
             [r[@"expected"] isEqual:@"false"] &&
             [r[@"limit_ms"] isEqual:@(expectedLimitMs)] &&
             [r[@"started_ms"] isKindOfClass:NSNumber.class] &&
             [@[ @"prepared", @"armed", @"active", @"restored" ] containsObject:r[@"phase"]] && [@[
                 @"prepared", @"hide_pref_armed", @"hide_pref_written", @"hide_restart_armed", @"active",
                 @"restore_pref_armed", @"restore_pref_written", @"restore_restart_armed", @"restored"
             ] containsObject:r[@"step"]] &&
             [@[ @"pending", @"restored_visual_unverified", @"recovery_failed" ] containsObject:r[@"result"]],
         "invalid Finder recovery record; retained");
    for (NSString* key in @[ @"helper", @"hidden_finder", @"restore_finder", @"restored_finder" ])
        need(!r[key] || validIdentity(r[key]), "invalid Finder process identity");
    for (NSString* key in @[ @"hide_restart_started", @"restore_restart_started", @"preference_restored" ])
        need(!r[key] || CFGetTypeID((__bridge CFTypeRef)r[key]) == CFBooleanGetTypeID(),
             "invalid Finder transition flag");
    for (NSString* key in @[ @"helper_ready_ms", @"active_ms", @"restored_ms" ])
        need(!r[key] || [r[key] isKindOfClass:NSNumber.class], "invalid Finder timestamp");
    need(!r[@"recovery_trigger"] || [@[
        @"helper_error", @"owner_exit", @"heartbeat_timeout", @"maximum_session_time",
        @"owner_channel_closed", @"normal_stop", @"owner_fallback_after_helper_exit", @"independent_command"
    ] containsObject:r[@"recovery_trigger"]],
         "invalid Finder recovery trigger");
    need(![r[@"restore_restart_started"] boolValue] || validIdentity(r[@"restore_finder"]),
         "missing restoration target identity");
    need(![r[@"phase"] isEqual:@"active"] || validIdentity(r[@"hidden_finder"]),
         "missing active Finder identity");
    need(![r[@"phase"] isEqual:@"restored"] ||
             ([r[@"preference_restored"] isEqual:@YES] && [r[@"step"] isEqual:@"restored"]),
         "incomplete Finder restoration record");
}

NSString* readFinderPreference() {
    need(CFPreferencesAppSynchronize(finderDomain), "read Finder preference failed");
    CFPropertyListRef raw =
        CFPreferencesCopyValue(finderKey, finderDomain, kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
    if (!raw) return @"absent";
    const bool valid = CFGetTypeID(raw) == CFBooleanGetTypeID();
    const bool shown = valid && CFBooleanGetValue(static_cast<CFBooleanRef>(raw));
    CFRelease(raw);
    need(valid, "CreateDesktop is not Boolean; refusing coercion");
    return shown ? @"true" : @"false";
}
void writeFinderPreference(NSString* value) {
    need([@[ @"absent", @"true", @"false" ] containsObject:value], "invalid Finder preference value");
    CFPreferencesSetAppValue(
        finderKey,
        [value isEqual:@"absent"] ? nullptr : ([value isEqual:@"true"] ? kCFBooleanTrue : kCFBooleanFalse),
        finderDomain);
    need(CFPreferencesAppSynchronize(finderDomain) && [readFinderPreference() isEqual:value],
         "Finder preference persistence/readback failed");
}
enum class FinderIdentityState { Pending, Valid, Rejected };
FinderIdentityState finderIdentityState(NSDictionary* identity, NSString* bundle, NSString* executable) {
    // Missing launch metadata is not evidence of a foreign process. Explicitly
    // conflicting metadata, however, must never be retried into acceptance.
    if ((identity && !validIdentity(identity)) ||
        (bundle && ![bundle isEqual:@"/System/Library/CoreServices/Finder.app"]) ||
        (executable &&
         ![executable isEqual:@"/System/Library/CoreServices/Finder.app/Contents/MacOS/Finder"]))
        return FinderIdentityState::Rejected;
    if (!identity || !bundle || !executable) return FinderIdentityState::Pending;
    return FinderIdentityState::Valid;
}
NSRunningApplication* currentFinderApplication() {
    const auto deadline = nowMs() + 2000;
    for (;;) {
        auto apps = [NSRunningApplication runningApplicationsWithBundleIdentifier:@"com.apple.finder"];
        need(apps.count <= 1, "ambiguous Finder process; refusing termination");
        if (apps.count == 0) return nil;
        NSRunningApplication* app = apps.firstObject;
        if (app.terminated) return nil;
        NSDictionary* identity = process(app.processIdentifier);
        NSString* bundle = app.bundleURL.path;
        NSString* executable = app.executableURL.path;
        const auto state = finderIdentityState(identity, bundle, executable);
        if (state == FinderIdentityState::Valid) return app;
        if (state == FinderIdentityState::Rejected || nowMs() >= deadline) {
            std::fprintf(stderr,
                         "finder_identity_check=%s pid=%d process_metadata_present=%s "
                         "bundle_metadata_present=%s executable_metadata_present=%s\n",
                         state == FinderIdentityState::Rejected ? "rejected" : "metadata_timeout",
                         app.processIdentifier, identity ? "true" : "false", bundle ? "true" : "false",
                         executable ? "true" : "false");
            throw std::runtime_error(state == FinderIdentityState::Rejected
                                         ? "Finder executable/owner identity mismatch"
                                         : "Finder identity metadata not ready; no unverified process used");
        }
        serviceEvents(0.05);
    }
}
struct FinderEvent {
    AEDesc descriptor{typeNull, nullptr};
    ~FinderEvent() { AEDisposeDesc(&descriptor); }
};
// A separate manual command may request consent. This function never sends an
// event, writes preferences, opens Store, or starts a desktop session.
int requestFinderAuthorization() {
    verifyAppKitRuntime();
    NSRunningApplication* app = currentFinderApplication();
    need(app != nil, "Finder unavailable; authorization not requested");
    NSDictionary* expected = process(app.processIdentifier);
    const pid_t pid = app.processIdentifier;
    struct Result {
        std::atomic<bool> done{false};
        std::atomic<OSStatus> status{paramErr};
    };
    auto result = std::make_shared<Result>();
    std::puts("finder_authorization_request_only=true quit_sent=false preferences_written=false");
    std::fflush(stdout);
    // Apple's SDK prohibits blocking the main thread on a consent prompt.
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      @autoreleasepool {
          @try {
              try {
                  need(sameProcess(expected), "Finder changed before authorization request");
                  FinderEvent target;
                  need(AECreateDesc(typeKernelProcessID, &pid, sizeof(pid), &target.descriptor) == noErr,
                       "authorization target creation failed");
                  result->status.store(AEDeterminePermissionToAutomateTarget(
                      &target.descriptor, kCoreEventClass, kAEQuitApplication, true));
              } catch (const std::exception& error) {
                  std::fprintf(stderr, "finder_authorization_error=%s\n", error.what());
              }
          } @catch (NSException*) {
              std::fputs("finder_authorization_error=Objective-C exception\n", stderr);
          }
          result->done.store(true);
      }
    });
    const auto deadline = nowMs() + 120000;
    while (!result->done.load() && nowMs() < deadline) serviceEvents(0.1);
    need(result->done.load(), "authorization wait timed out; no desktop session started");
    const OSStatus status = result->status.load();
    std::printf("finder_authorization_status=%d quit_sent=false preferences_written=false\n", (int)status);
    if (status != noErr) return 1;
    need(sameProcess(expected), "Finder changed during authorization; rerun preparation");
    std::puts("authorization_granted_for_this_process=true helper_readiness_still_requires_preparation=true");
    return 0;
}
void finderQuitPreflight(NSDictionary* expected, bool sendQuit) {
    NSRunningApplication* target = currentFinderApplication();
    need(target && [process(target.processIdentifier) isEqualToDictionary:expected] && sameProcess(expected),
         "Finder changed before quit preflight");
    pid_t pid = target.processIdentifier;
    FinderEvent address, event;
    need(AECreateDesc(typeKernelProcessID, &pid, sizeof(pid), &address.descriptor) == noErr,
         "Finder quit address creation failed");
    const OSStatus permission = AEDeterminePermissionToAutomateTarget(&address.descriptor, kCoreEventClass,
                                                                      kAEQuitApplication, false);
    if (permission != noErr)
        std::fprintf(stderr, "finder_quit_preflight_status=%d prompts_requested=false\n", (int)permission);
    need(permission == noErr, "normal Finder quit unavailable without prompting; no permission requested");
    if (!sendQuit) return;
    need(AECreateAppleEvent(kCoreEventClass, kAEQuitApplication, &address.descriptor, kAutoGenerateReturnID,
                            kAnyTransactionID, &event.descriptor) == noErr &&
             sameProcess(expected),
         "Finder quit event creation/identity failed");
    const OSStatus status = AESendMessage(&event.descriptor, nullptr,
                                          kAENoReply | kAENeverInteract | kAEDoNotPromptForUserConsent, 120);
    if (status != noErr) std::fprintf(stderr, "finder_quit_send_status=%d\n", (int)status);
    need(status == noErr, "Finder normal quit request failed; no forced quit attempted");
}
void checkFinderPreferenceScope() {
    need(!CFPreferencesAppValueIsForced(finderKey, finderDomain), "managed Finder setting refused");
    CFPropertyListRef override =
        CFPreferencesCopyValue(finderKey, finderDomain, kCFPreferencesCurrentUser, kCFPreferencesCurrentHost);
    const bool absent = override == nullptr;
    if (override) CFRelease(override);
    need(absent, "Finder current-host override refused");
    CFPropertyListRef effective = CFPreferencesCopyAppValue(finderKey, finderDomain);
    const bool supported = !effective || (CFGetTypeID(effective) == CFBooleanGetTypeID() &&
                                          CFBooleanGetValue(static_cast<CFBooleanRef>(effective)));
    if (effective) CFRelease(effective);
    need(supported, "effective Finder desktop disabled or invalid; refusing experiment");
}
// Dependency injection covers the recovery state machine without touching Finder.
struct FinderIO {
    virtual ~FinderIO() = default;
    virtual NSString* read() = 0;
    virtual void write(NSString*) = 0;
    virtual NSDictionary* current() = 0;
    virtual NSDictionary* ensureRunning() = 0;
    virtual NSDictionary* restart(NSDictionary*) = 0;
};
struct NativeFinderIO final : FinderIO {
    NSString* read() override { return readFinderPreference(); }
    void write(NSString* value) override { writeFinderPreference(value); }
    NSDictionary* current() override {
        NSRunningApplication* app = currentFinderApplication();
        return app ? process(app.processIdentifier) : nil;
    }
    NSDictionary* ensureRunning() override {
        if (NSDictionary* present = current()) return present;
        initializeAppKit();
        NSWorkspaceOpenConfiguration* config = NSWorkspaceOpenConfiguration.configuration;
        config.activates = NO;
        config.addsToRecentItems = NO;
        // NSWorkspace calls completion on a concurrent queue. Its retained state
        // must also remain alive if the bounded wait times out first.
        auto completion = std::make_shared<std::atomic<int>>(0);
        [NSWorkspace.sharedWorkspace
            openApplicationAtURL:[NSURL fileURLWithPath:@"/System/Library/CoreServices/Finder.app"]
                   configuration:config
               completionHandler:^(NSRunningApplication* app, NSError* error) {
                 @try {
                     completion->store(error != nil || app == nil ? -1 : 1);
                 } @catch (NSException*) {
                     completion->store(-1);
                 }
               }];
        const auto deadline = nowMs() + 5000;
        while (completion->load() == 0 && nowMs() < deadline) serviceEvents(0.05);
        need(completion->load() == 1, "Finder relaunch not acknowledged; recovery record retained");
        NSDictionary* result = current();
        need(result != nil, "Finder absent after launch");
        return result;
    }
    NSDictionary* restart(NSDictionary* expected) override {
        initializeAppKit();
        NSRunningApplication* target = currentFinderApplication();
        need(target && [process(target.processIdentifier) isEqualToDictionary:expected] &&
                 sameProcess(expected),
             "Finder changed before normal quit; no new process terminated");
        // Never use forceTerminate, killall, signals or PID-only targeting.
        finderQuitPreflight(expected, true);
        const auto deadline = nowMs() + 5000;
        while (sameProcess(expected) && nowMs() < deadline) serviceEvents(0.05);
        need(!sameProcess(expected), "Finder normal quit timed out; no force termination attempted");
        NSDictionary* replacement = ensureRunning();
        need(![replacement isEqualToDictionary:expected], "Finder identity did not change");
        return replacement;
    }
};

void restoreFinderState(NSMutableDictionary* r, FinderIO& io, const std::function<void()>& save) {
    if ([r[@"phase"] isEqual:@"restored"]) return;
    NSString* original = r[@"original"];
    NSString* current = io.read();
    if (![current isEqual:original]) {
        need(![r[@"phase"] isEqual:@"prepared"] && [current isEqual:@"false"],
             "external Finder preference conflict; record retained");
        r[@"step"] = @"restore_pref_armed";
        save();
        io.write(original);
    }
    need([io.read() isEqual:original], "Finder preference restoration readback failed");
    r[@"preference_restored"] = @YES;
    r[@"step"] = @"restore_pref_written";
    save();
    if ([r[@"hide_restart_started"] boolValue]) {
        // Preference restoration above is independent of display/host configuration.
        need([r[@"login_session"] isEqual:@(loginSession())],
             "login session changed; preference restored, Finder restart requires review");
        NSDictionary* currentHost = io.current();
        if (!currentHost) {
            r[@"restored_finder"] = io.ensureRunning();
        } else if ([r[@"restore_restart_started"] boolValue] &&
                   ![currentHost isEqualToDictionary:r[@"restore_finder"]]) {
            // A previous restoration quit already completed. Do not restart its replacement again.
            r[@"restored_finder"] = currentHost;
        } else if ([currentHost isEqualToDictionary:r[@"hidden_finder"]]) {
            r[@"restore_finder"] = currentHost;
            r[@"restore_restart_started"] = @YES;
            r[@"step"] = @"restore_restart_armed";
            save();
            r[@"restored_finder"] = io.restart(currentHost);
        } else if (!r[@"hidden_finder"] && [currentHost isEqualToDictionary:r[@"finder"]]) {
            // Hide never reached a replacement Finder. The original is still running.
            r[@"restored_finder"] = currentHost;
        } else {
            throw std::runtime_error(
                "Finder host changed outside recorded transition; preference restored, "
                "restart not attempted and journal retained");
        }
    }
    r[@"phase"] = @"restored";
    r[@"step"] = @"restored";
    r[@"result"] = @"restored_visual_unverified";
    r[@"restored_ms"] = @(nowMs());
    save();
}
bool recoverFinderLocked(Store& store, NSMutableDictionary* r, NSString* trigger) {
    if ([r[@"phase"] isEqual:@"restored"]) return true;
    NSString* initialPhase = r[@"phase"];
    r[@"recovery_trigger"] = trigger;
    bool success = false;
    @try {
        try {
            NativeFinderIO io;
            restoreFinderState(r, io, [&] { store.write(r); });
            success = true;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "finder_recovery_error=%s\n", error.what());
        }
    } @catch (NSException*) {
        std::fputs("finder_recovery_error=Objective-C exception\n", stderr);
    }
    if (!success) {
        r[@"phase"] = initialPhase;
        r[@"result"] = @"recovery_failed";
        store.write(r);
    }
    return success;
}
bool recoverFinder(Store& store, NSString* nonce, NSString* trigger) {
    Guard guard(store.operation.value);
    auto r = store.read();
    if (!r || ![r[@"session_id"] isEqual:nonce]) return true;  // never affect a newer session
    need(isFinderRecord(r), "recovery backend mismatch");
    return recoverFinderLocked(store, r, trigger);
}
void hideFinder(Store& store, NSString* nonce) {
    Guard guard(store.operation.value);
    auto r = store.read();
    need([r[@"session_id"] isEqual:nonce] && [r[@"phase"] isEqual:@"prepared"] && isFinderRecord(r) &&
             sameProcess(r[@"owner"]) && [r[@"login_session"] isEqual:@(loginSession())] &&
             nowMs() - [r[@"started_ms"] unsignedLongLongValue] < [r[@"limit_ms"] unsignedLongLongValue],
         "stale Finder activation");
    NativeFinderIO io;
    checkFinderPreferenceScope();
    finderQuitPreflight(r[@"finder"], false);
    need([io.current() isEqualToDictionary:r[@"finder"]] && [io.read() isEqual:r[@"original"]] &&
             ![readVisibility() isEqual:@"true"],
         "Finder/configuration changed before activation");
    r[@"phase"] = @"armed";
    r[@"step"] = @"hide_pref_armed";
    store.write(r);
    io.write(@"false");
    r[@"step"] = @"hide_pref_written";
    store.write(r);
    r[@"hide_restart_started"] = @YES;
    r[@"step"] = @"hide_restart_armed";
    store.write(r);
    r[@"hidden_finder"] = io.restart(r[@"finder"]);
    need([io.read() isEqual:@"false"], "Finder preference changed while relaunching");
    r[@"phase"] = @"active";
    r[@"step"] = @"active";
    r[@"active_ms"] = @(nowMs());
    store.write(r);
}
int finderHelper(NSString* nonce) {
    verifyAppKitRuntime();
    Store store;
    NSDictionary* initial = nil;
    {
        Guard guard(store.operation.value);
        auto r = store.read();
        need([r[@"session_id"] isEqual:nonce] && isFinderRecord(r) && [r[@"phase"] isEqual:@"prepared"] &&
                 sameProcess(r[@"owner"]) && [r[@"login_session"] isEqual:@(loginSession())],
             "Finder helper identity mismatch");
        r[@"helper"] = process(getpid());
        r[@"helper_ready_ms"] = @(nowMs());
        store.write(r);
        initial = r;
    }
    finderQuitPreflight(initial[@"finder"], false);
    int yes = 1;
    need(setsockopt(helperSocket, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes)) == 0,
         "Finder helper channel configuration");
    need(send(helperSocket, "F", 1, 0) == 1, "Finder helper ready acknowledgement");
    auto heartbeat = nowMs();
    const bool isContinuous = [initial[@"version"] isEqual:@6];
    dpp::ContinuousWatchdogClock continuousClock{};
    continuousClock.reset(heartbeat, awakeNowMs());
    unsigned timingRequests = 0;
    const bool hasTiming = [initial[@"version"] isEqual:@5] || isContinuous;
    const int timingFD = hasTiming ? helperTimingSocket : -1;
    if (hasTiming)
        dpp::mac::sendHelperTimingSample(timingFD,
            dpp::mac::helperTimingSample(initial, heartbeat, false, 0, nowMs()));
    NSString* reason = @"helper_error";
    bool failed = false;
    @try {
        try {
            bool active = false, stop = false;
            while (!stop) {
                const auto curContMs = nowMs();
                const auto curAwakeMs = awakeNowMs();
                if (isContinuous) {
                    continuousClock.observeLoop(curContMs, curAwakeMs, 100ULL);
                }
                if (!sameProcess(initial[@"owner"])) {
                    reason = @"owner_exit";
                    break;
                }
                if (isContinuous ? continuousClock.heartbeatTimedOut(curContMs, curAwakeMs)
                                 : (curContMs - heartbeat >= 5000)) {
                    reason = @"heartbeat_timeout";
                    break;
                }
                if (curContMs - [initial[@"started_ms"] unsignedLongLongValue] >=
                    [initial[@"limit_ms"] unsignedLongLongValue]) {
                    reason = @"maximum_session_time";
                    break;
                }
                {
                    Guard guard(store.operation.value);
                    auto r = store.read();
                    if (![r[@"session_id"] isEqual:nonce] || [r[@"phase"] isEqual:@"restored"]) return 0;
                }
                pollfd input{helperSocket, POLLIN, 0};
                if (poll(&input, 1, 0) > 0) {
                    char bytes[64];
                    const auto count = recv(helperSocket, bytes, sizeof(bytes), MSG_DONTWAIT);
                    if (count == 0) {
                        reason = @"owner_channel_closed";
                        break;
                    }
                    if (count < 0) need(errno == EAGAIN || errno == EINTR, "Finder helper read failed");
                    for (ssize_t i = 0; i < count && !stop; ++i) {
                        if (bytes[i] == 'P') {
                            heartbeat = nowMs();
                            if (isContinuous) continuousClock.recordPulse(awakeNowMs());
                        } else if (isContinuous && (bytes[i] == 'Z' || bytes[i] == 'W')) {
                            heartbeat = nowMs();
                            continuousClock.recordSuspendState(bytes[i] == 'Z', heartbeat, awakeNowMs());
                        } else if (bytes[i] == 'B') {
                            need(!active, "duplicate Finder activation refused");
                            hideFinder(store, nonce);
                            active = true;
                            // Drain queued heartbeats next iteration; a native restart is bounded.
                            heartbeat = nowMs();
                            if (isContinuous) continuousClock.reset(heartbeat, awakeNowMs());
                            need(send(helperSocket, "H", 1, 0) == 1,
                                 "Finder activation acknowledgement failed");
                        } else if (bytes[i] == 'S') {
                            reason = @"normal_stop";
                            stop = true;
                        } else
                            throw std::runtime_error("unknown Finder helper command");
                    }
                }
                dpp::mac::serviceHelperTimingRequests(timingFD, initial, heartbeat, active, nowMs(), timingRequests);
                if (!stop) serviceEvents(0.1);
            }
        } catch (const std::exception& error) {
            std::fprintf(stderr, "finder_helper_error=%s\n", error.what());
            failed = true;
        }
    } @catch (NSException*) {
        std::fputs("finder_helper_error=Objective-C exception\n", stderr);
        failed = true;
    }
    bool restored = recoverFinder(store, nonce, reason);
    (void)send(helperSocket, restored && !failed ? "R" : "E", 1, 0);
    return restored ? 0 : 1;
}

struct FinderBackend final : Backend {
    bool fullMirror = false;
    bool continuousMirror = false;
    NSDictionary* sceneFinder = nil;
    NSUInteger sceneItemCount = 0;
    NSString* sceneSnapshotID = nil;
    explicit FinderBackend(bool preparation, bool mirror = false) : fullMirror(mirror) {
        prepareOnly = preparation;
    }
    bool recoverResidual() override {
        if (!prepareOnly) return Backend::recoverResidual();
        // A preparation command must never repair a mutated residual session by
        // changing Finder. Leave it for the explicitly invoked recovery entry.
        Guard guard(store.operation.value);
        auto r = store.read();
        need(!r || [r[@"phase"] isEqual:@"restored"],
             "unrecovered session exists; preparation refused, use independent recovery");
        if (r) store.archive(r);
        return true;
    }
    bool probe() override {
        verifyAppKitRuntime();
        const auto os = NSProcessInfo.processInfo.operatingSystemVersion;
        if (fullMirror) {
            need(os.majorVersion >= 13 && NSScreen.screens.count >= 1,
                 "macOS 13+ with at least one active display required");
        } else {
            need(os.majorVersion == 15 && os.minorVersion == 7 && os.patchVersion == 5 &&
                     NSScreen.screens.count == 1,
                 "Finder candidate not validated for this OS/display profile");
        }
        need(currentFinderApplication() != nil, "Finder unavailable before experiment");
        if (fullMirror)
            need(validIdentity(sceneFinder) && [sceneFinder isEqual:host(@"com.apple.finder")],
                 "Finder changed since complete mirror snapshot");
        finderQuitPreflight(host(@"com.apple.finder"), false);
        need(![readFinderPreference() isEqual:@"false"], "desktop already disabled; refusing experiment");
        need(![readVisibility() isEqual:@"true"],
             "WindowManager hide setting enabled; refusing mixed backends");
        checkFinderPreferenceScope();
        return true;
    }
    bool snapshot() override {
        record = [@{
            @"version" : @4,
            @"backend" : @"finder_create_desktop_restart_v1",
            @"consent" : @"manual_finder_restart_experiment",
            @"session_id" : NSUUID.UUID.UUIDString,
            @"uid" : @(getuid()),
            @"login_session" : @(loginSession()),
            @"owner" : process(getpid()),
            @"finder" : fullMirror ? sceneFinder : host(@"com.apple.finder"),
            @"original" : readFinderPreference(),
            @"expected" : @"false",
            @"phase" : @"prepared",
            @"step" : @"prepared",
            @"result" : @"pending",
            @"started_ms" : @(nowMs()),
            @"limit_ms" : @(fullMirror ? finderMirrorLimitMs : finderLimitMs)
        } mutableCopy];
        if (fullMirror) {
            record[@"version"] = continuousMirror ? @6 : @5;
            record[@"session_kind"] =
                continuousMirror ? @"protected_continuous_mirror" : @"protected_full_mirror";
            record[@"limit_ms"] =
                @(continuousMirror ? finderContinuousMirrorLimitMs : finderMirrorLimitMs);
            record[@"mirror_item_count"] = @(sceneItemCount);
            record[@"mirror_snapshot_id"] = sceneSnapshotID;
        }
        return true;
    }
    char waitReply(unsigned long long duration) {
        const auto end = nowMs() + duration;
        auto nextHeartbeat = nowMs();
        while (nowMs() < end) {
            if (nowMs() >= nextHeartbeat) {
                need(send(channel.value, "P", 1, 0) == 1, "Finder helper unavailable");
                nextHeartbeat = nowMs() + 250;
            }
            pollfd input{channel.value, POLLIN, 0};
            if (poll(&input, 1, 0) > 0) {
                char value = 0;
                need(recv(channel.value, &value, 1, 0) == 1, "Finder helper closed before reply");
                return value;
            }
            serviceEvents(0.05);
        }
        throw std::runtime_error("Finder helper reply timed out; recovery record retained");
    }
    bool activate() override {
        if (prepareOnly) return true;
        need(send(channel.value, "B", 1, 0) == 1 && waitReply(15000) == 'H',
             "Finder hide/relaunch failed; recovery requested");
        std::puts("finder_relaunched_hidden_preference=true visual_hiding_requires_observation=true");
        std::fflush(stdout);
        return true;
    }
    bool restore() override {
        if (!persisted) return true;
        bool acknowledged = false;
        try {
            if (channel.value >= 0 && send(channel.value, "S", 1, 0) == 1) {
                char reply;
                do {
                    reply = waitReply(15000);
                } while (reply == 'H');
                acknowledged = reply == 'R';
            }
        } catch (...) { /* Verify the journal or recover after the executor exits. */
        }
        {
            Guard guard(store.operation.value);
            auto r = store.read();
            need([r[@"session_id"] isEqual:record[@"session_id"]], "Finder restore session changed");
            if ([r[@"phase"] isEqual:@"restored"]) return true;
            if (acknowledged) return false;
            if (r[@"helper"] && sameProcess(r[@"helper"])) return false;
        }
        return recoverFinder(store, record[@"session_id"], @"owner_fallback_after_helper_exit");
    }
};
int runFinderSession(bool preparation, bool manualOwnerExit = false) {
    need(!preparation || !manualOwnerExit, "owner-exit test cannot run in preparation mode");
    FinderBackend backend(preparation);
    dpp::SessionCoordinator coordinator(backend);
    if (!coordinator.start()) {
        std::fprintf(stderr, "finder_start_failed=%s; inspect recovery log before retry\n",
                     coordinator.failure());
        return 1;
    }
    std::printf("finder_session_id=%s\n", [backend.record[@"session_id"] UTF8String]);
    std::fflush(stdout);
    const auto started = nowMs();
    if (!preparation) {
        while (nowMs() - started < (manualOwnerExit ? 2000 : 15000)) {
            if (send(backend.channel.value, "P", 1, 0) != 1) {
                manualOwnerExit = false;
                break;
            }
            pollfd stop{STDIN_FILENO, POLLIN, 0};
            if (isatty(STDIN_FILENO) && poll(&stop, 1, 0) > 0 && (stop.revents & POLLIN)) {
                manualOwnerExit = false;
                break;
            }
            pollfd helperEvent{backend.channel.value, POLLIN, 0};
            if (poll(&helperEvent, 1, 0) > 0) {
                manualOwnerExit = false;
                break;
            }
            serviceEvents(0.25);
        }
    }
    if (manualOwnerExit) {
        // Separate, explicitly confirmed manual fault entry only. Exit this
        // process without coordinator cleanup; do not signal any other process.
        std::puts("manual_owner_exit_test=true owner_exits_without_cleanup=true expected_exit_code=77");
        std::fflush(nullptr);
        _exit(77);
    }
    std::printf("finder_active_elapsed_ms=%llu preparation_only=%s\n", nowMs() - started,
                preparation ? "true" : "false");
    need(coordinator.stop(), "Finder restoration incomplete; journal retained, run independent recovery");
    int status = 0;
    const auto end = nowMs() + 3000;
    pid_t waited;
    do {
        waited = waitpid(backend.child, &status, WNOHANG);
        if (waited == 0) serviceEvents(0.05);
    } while (waited == 0 && nowMs() < end);
    need(waited == backend.child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
         "Finder helper clean exit not verified");
    backend.child = -1;
    std::puts("finder_session_closed=true visibility_and_desktop_operation_require_observation=true");
    return 0;
}
void auditFinder() {
    verifyAppKitRuntime();
    NativeFinderIO io;
    NSDictionary* finder = io.current();
    NSDictionary* value = @{
        @"os" : NSProcessInfo.processInfo.operatingSystemVersionString,
        @"screens" : @(NSScreen.screens.count),
        @"finder" : finder ? (id)finder : (id)NSNull.null,
        @"create_desktop_original" : io.read(),
        @"window_manager_hide" : readVisibility(),
        @"writes_performed" : @NO,
        @"finder_quit_requested" : @NO,
        @"visual_acceptance" : @"unverified"
    };
    NSData* data =
        [NSJSONSerialization dataWithJSONObject:value options:NSJSONWritingPrettyPrinted error:nil];
    need(data != nil, "encode Finder audit");
    std::fwrite(data.bytes, 1, data.length, stdout);
    std::puts("");
}

void finderSelfTest() {
    NSDictionary* identityFixture =
        @{@"pid" : @101,
          @"seconds" : @1234,
          @"microseconds" : @5,
          @"uid" : @(getuid())};
    NSString* systemBundle = @"/System/Library/CoreServices/Finder.app";
    NSString* systemExecutable = @"/System/Library/CoreServices/Finder.app/Contents/MacOS/Finder";
    need(finderIdentityState(identityFixture, systemBundle, systemExecutable) == FinderIdentityState::Valid,
         "valid Finder identity rejected");
    need(finderIdentityState(nil, systemBundle, systemExecutable) == FinderIdentityState::Pending &&
             finderIdentityState(identityFixture, nil, systemExecutable) == FinderIdentityState::Pending &&
             finderIdentityState(identityFixture, systemBundle, nil) == FinderIdentityState::Pending,
         "missing Finder metadata mistaken for a valid or foreign host");
    NSMutableDictionary* foreignIdentity = [identityFixture mutableCopy];
    foreignIdentity[@"uid"] = @(getuid() + 1);
    need(finderIdentityState(foreignIdentity, nil, nil) == FinderIdentityState::Rejected &&
             finderIdentityState(nil, @"unexpected", systemExecutable) == FinderIdentityState::Rejected &&
             finderIdentityState(nil, systemBundle, @"unexpected") == FinderIdentityState::Rejected,
         "conflicting Finder metadata allowed to retry");
    std::puts("finder_missing_metadata_separated_from_identity_conflict_passed=true");
    struct Fake final : FinderIO {
        NSString* preference = @"false";
        NSDictionary* currentIdentity;
        NSDictionary* replacement;
        int writes = 0, restarts = 0, launches = 0;
        bool failWrite = false, failRestart = false;
        NSString* read() override { return preference; }
        void write(NSString* v) override {
            if (failWrite) throw std::runtime_error("injected write failure");
            preference = v;
            ++writes;
        }
        NSDictionary* current() override { return currentIdentity; }
        NSDictionary* ensureRunning() override {
            ++launches;
            return currentIdentity = replacement;
        }
        NSDictionary* restart(NSDictionary* expected) override {
            need([expected isEqualToDictionary:currentIdentity], "fake foreign process targeted");
            ++restarts;
            if (failRestart) throw std::runtime_error("injected restart refusal");
            return currentIdentity = replacement;
        }
    };
    auto identity = [](int pid) -> NSDictionary* {
        return @{@"pid" : @(pid), @"seconds" : @1234, @"microseconds" : @5, @"uid" : @(getuid())};
    };
    NSDictionary* original = identity(101);
    NSDictionary* hidden = identity(102);
    NSDictionary* replacement = identity(103);
    auto fixture = [&]() -> NSMutableDictionary* {
        return [@{
            @"version" : @4,
            @"backend" : @"finder_create_desktop_restart_v1",
            @"consent" : @"manual_finder_restart_experiment",
            @"session_id" : NSUUID.UUID.UUIDString,
            @"uid" : @(getuid()),
            @"login_session" : @(loginSession()),
            @"owner" : identity(104),
            @"finder" : original,
            @"hidden_finder" : hidden,
            @"original" : @"absent",
            @"expected" : @"false",
            @"phase" : @"active",
            @"step" : @"active",
            @"result" : @"pending",
            @"started_ms" : @(nowMs()),
            @"limit_ms" : @(finderLimitMs),
            @"hide_restart_started" : @YES
        } mutableCopy];
    };
    for (int version : {4, 5, 6}) {
        for (int scenario = 0; scenario < 11; ++scenario) {
            auto r = fixture();
            if (version == 5 || version == 6) {
                r[@"version"] = @(version);
                r[@"session_kind"] =
                    version == 6 ? @"protected_continuous_mirror" : @"protected_full_mirror";
                r[@"limit_ms"] =
                    @(version == 6 ? finderContinuousMirrorLimitMs : finderMirrorLimitMs);
                r[@"mirror_item_count"] = @6;
                r[@"mirror_snapshot_id"] = NSUUID.UUID.UUIDString;
            }
            Fake io;
            io.currentIdentity = scenario == 2 ? replacement : hidden;
            io.replacement = replacement;
            if (scenario == 1 || scenario == 8) {
                r[@"phase"] = @"prepared";
                r[@"step"] = @"prepared";
                [r removeObjectForKey:@"hide_restart_started"];
                [r removeObjectForKey:@"hidden_finder"];
                io.preference = scenario == 8 ? @"false" : @"absent";
            }
            io.failWrite = scenario == 3;
            io.failRestart = scenario == 4;
            if (scenario == 5) {
                r[@"restore_restart_started"] = @YES;
                r[@"restore_finder"] = hidden;
                io.currentIdentity = replacement;
                io.preference = @"absent";
            }
            if (scenario == 6) io.currentIdentity = nil;
            if (scenario == 7) r[@"original"] = @"true";
            int saves = 0;
            bool success = true;
            try {
                restoreFinderState(r, io, [&] {
                    Store::validate(r);
                    ++saves;
                    if ((scenario == 9 && saves == 1) || (scenario == 10 && saves == 3))
                        throw std::runtime_error("injected durable save failure");
                    if ([r[@"step"] isEqual:@"restore_restart_armed"])
                        need([io.preference isEqual:r[@"original"]],
                             "restart armed before preference restored");
                });
            } catch (...) {
                success = false;
            }
            need(success ==
                     (scenario == 0 || scenario == 1 || scenario == 5 || scenario == 6 || scenario == 7),
                 "Finder recovery fault outcome mismatch");
            need(io.restarts == (scenario == 0 || scenario == 4 || scenario == 7 ? 1 : 0),
                 "unnecessary/foreign Finder restart");
            need(io.writes ==
                     (scenario == 1 || scenario == 3 || scenario == 5 || scenario == 8 || scenario == 9 ? 0
                                                                                                        : 1),
                 "Finder original not restored exactly");
            if (success) {
                int before = io.restarts + io.writes + io.launches;
                restoreFinderState(r, io, [] {});
                need(io.restarts + io.writes + io.launches == before, "Finder recovery not idempotent");
            } else
                need(![r[@"phase"] isEqual:@"restored"], "failed Finder recovery marked complete");
        }
    }
    auto bad = fixture();
    bad[@"command"] = @"not executable";
    bool rejected = false;
    try {
        Store::validate(bad);
    } catch (...) {
        rejected = true;
    }
    need(rejected, "Finder arbitrary command field accepted");
    [bad removeObjectForKey:@"command"];
    bad[@"version"] = @3;
    rejected = false;
    try {
        Store::validate(bad);
    } catch (...) {
        rejected = true;
    }
    need(rejected, "Finder journal downgrade accepted");
    auto mirrorRecord = fixture();
    mirrorRecord[@"version"] = @5;
    mirrorRecord[@"session_kind"] = @"protected_full_mirror";
    mirrorRecord[@"limit_ms"] = @(finderMirrorLimitMs);
    mirrorRecord[@"mirror_item_count"] = @6;
    mirrorRecord[@"mirror_snapshot_id"] = NSUUID.UUID.UUIDString;
    char testDirectory[] = "/tmp/dpp-mirror-journal-XXXXXX";
    need(mkdtemp(testDirectory) != nullptr, "create mirror test journal directory");
    {
        Store testStore([NSString stringWithUTF8String:testDirectory]);
        testStore.write(mirrorRecord);
        need([testStore.read() isEqualToDictionary:mirrorRecord], "v5 durable JSON round trip failed");
        mirrorRecord[@"limit_ms"] = @45000;
        bool refused = false;
        try {
            testStore.write(mirrorRecord);
        } catch (...) {
            refused = true;
        }
        need(refused && [testStore.read()[@"limit_ms"] isEqual:@(finderMirrorLimitMs)],
             "invalid v5 limit modified durable record");
        auto continuousRecord = fixture();
        continuousRecord[@"version"] = @6;
        continuousRecord[@"session_kind"] = @"protected_continuous_mirror";
        continuousRecord[@"limit_ms"] = @(finderContinuousMirrorLimitMs);
        continuousRecord[@"mirror_item_count"] = @6;
        continuousRecord[@"mirror_snapshot_id"] = NSUUID.UUID.UUIDString;
        testStore.write(continuousRecord);
        need([testStore.read() isEqualToDictionary:continuousRecord], "v6 durable JSON round trip failed");
        continuousRecord[@"limit_ms"] = @180000;
        refused = false;
        try {
            testStore.write(continuousRecord);
        } catch (...) {
            refused = true;
        }
        need(refused && [testStore.read()[@"limit_ms"] isEqual:@(finderContinuousMirrorLimitMs)],
             "invalid v6 limit modified durable record");
        unlinkat(testStore.directoryFD.value, "session.json", 0);
        unlinkat(testStore.directoryFD.value, "operation.lock", 0);
        unlinkat(testStore.directoryFD.value, "owner.lock", 0);
    }
    rmdir(testDirectory);
    std::puts(
        "finder_v4_v5_offline_recovery_order_failure_identity_idempotence_schema_passed=true "
        "finder_preferences_written=false finder_restart_performed=false");
}
