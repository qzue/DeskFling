// Separate manually launched diagnostic, requested on 2026-11-14. The four
// failed legacy entries stay disabled, including in this binary. No mirror.
void checkRecheckAuthorization() {
    verifyAppKitRuntime();
    auto finder = host(@"com.apple.finder");
    const pid_t pid = [finder[@"pid"] intValue];
    FinderEvent target;
    need(AECreateDesc(typeKernelProcessID, &pid, sizeof(pid), &target.descriptor) == noErr,
         "recheck permission target unavailable");
    const auto read =
        AEDeterminePermissionToAutomateTarget(&target.descriptor, kAECoreSuite, kAEGetData, false);
    const auto quit =
        AEDeterminePermissionToAutomateTarget(&target.descriptor, kCoreEventClass, kAEQuitApplication, false);
    std::printf(
        "recheck_finder_read_permission=%d recheck_finder_quit_permission=%d "
        "prompts_requested=false finder_events_sent=false\n",
        (int)read, (int)quit);
    need(read == noErr && quit == noErr, "existing Finder authorization not proved in this launch context");
    need(sameProcess(finder), "Finder changed during authorization check");
    need(![readFinderPreference() isEqual:@"false"], "Finder CreateDesktop already false; no recheck");
    CFPropertyListRef effective = CFPreferencesCopyAppValue(finderKey, finderDomain);
    const bool effectiveVisible = !effective || (CFGetTypeID(effective) == CFBooleanGetTypeID() &&
                                                 CFBooleanGetValue(static_cast<CFBooleanRef>(effective)));
    if (effective) CFRelease(effective);
    need(effectiveVisible, "Finder desktop is already suppressed; no recheck");
    need(!CFPreferencesAppValueIsForced(visibilityKey, domain), "managed visibility setting; no recheck");
    CFPropertyListRef hostValue =
        CFPreferencesCopyValue(visibilityKey, domain, kCFPreferencesCurrentUser, kCFPreferencesCurrentHost);
    const bool hostOverride = hostValue != nullptr;
    if (hostValue) CFRelease(hostValue);
    need(!hostOverride, "host-specific visibility override; no recheck");
}

bool recheckResidualAllowed(NSDictionary* record) {
    return !record || [record[@"phase"] isEqual:@"restored"];
}
struct AuthorizedWindowManagerRecheck : Backend {
    NSString* finderPreference = nil;
    NSString* clickPreference = nil;
    explicit AuthorizedWindowManagerRecheck(bool preparation) {
        prepareOnly = preparation;
        appAPI = true;
        appkitRuntime = true;
    }
    bool recoverResidual() override {
        Guard guard(store.operation.value);
        auto previous = store.read();
        // This diagnostic must never recover an unrelated Finder session and
        // accidentally restart Finder before testing the no-restart route.
        need(recheckResidualAllowed(previous), "unfinished session; use its recovery entry before recheck");
        if (previous) store.archive(previous);
        return true;
    }
    bool probe() override {
        try {
            checkRecheckAuthorization();
            return Backend::probe();
        } catch (const std::exception& error) {
            std::fprintf(stderr, "authorized_recheck_probe_error=%s\n", error.what());
            throw;
        }
    }
    bool snapshot() override {
        finderPreference = readFinderPreference();
        clickPreference = readSetting(clickKey);
        return Backend::snapshot();
    }
    bool activate() override {
        need([readFinderPreference() isEqual:finderPreference] &&
                 [readSetting(clickKey) isEqual:clickPreference],
             "conditions changed before recheck");
        checkRecheckAuthorization();
        std::printf(
            "authorized_recheck_session_id=%s preparation_only=%s "
            "backend=original_windowmanager_appkit_v3 finder_restart_requested=false\n",
            [record[@"session_id"] UTF8String], prepareOnly ? "true" : "false");
        return Backend::activate();
    }
};

int runAuthorizedWindowManagerRecheck(bool preparation) {
    need(preparation, "post-authorization visibility recheck failed; mutation retired");
    AuthorizedWindowManagerRecheck backend(preparation);
    dpp::SessionCoordinator coordinator(backend);
    if (!coordinator.start()) {
        std::fprintf(stderr, "authorized_recheck_start_failed=%s\n", coordinator.failure());
        return 1;
    }
    const auto start = nowMs();
    const char* reason = "preparation_only";
    if (!preparation) {
        reason = "normal_15_second_limit";
        while (nowMs() - start < 15000) {
            if (!sameProcess(backend.record[@"finder"]) || !sameProcess(backend.record[@"window_manager"])) {
                reason = "desktop_host_changed";
                break;
            }
            if (send(backend.channel.value, "H", 1, 0) != 1) {
                reason = "helper_channel_closed";
                break;
            }
            pollfd input{STDIN_FILENO, POLLIN, 0};
            if (isatty(STDIN_FILENO) && poll(&input, 1, 0) > 0 && (input.revents & POLLIN)) {
                reason = "terminal_input";
                break;
            }
            serviceEvents(.25);
            int status = 0;
            if (waitpid(backend.child, &status, WNOHANG) == backend.child) {
                backend.child = -1;
                reason = "helper_exited";
                break;
            }
        }
    }
    std::printf("active_elapsed_ms=%llu stop_reason=%s\n", nowMs() - start, reason);
    need(coordinator.stop(), "recheck preference restore failed; record retained");
    if (backend.child > 0) {
        int status = 0;
        pid_t waited = 0;
        const auto deadline = nowMs() + 3000;
        while ((waited = waitpid(backend.child, &status, WNOHANG)) == 0 && nowMs() < deadline)
            serviceEvents(.025);
        need(waited == backend.child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
             "recheck helper clean shutdown unverified");
    }
    std::printf("finder_identity_unchanged=%s finder_setting_unchanged=%s click_setting_unchanged=%s\n",
                sameProcess(backend.record[@"finder"]) ? "true" : "false",
                [readFinderPreference() isEqual:backend.finderPreference] ? "true" : "false",
                [readSetting(clickKey) isEqual:backend.clickPreference] ? "true" : "false");
    std::puts("session_closed=true actual_hiding_and_restoration_require_human_observation=true");
    return 0;
}

void recheckOfflineTest() {
    need(recheckResidualAllowed(nil), "empty recheck store should be allowed");
    need(recheckResidualAllowed(@{@"phase" : @"restored"}), "restored record should be allowed");
    for (NSString* phase in @[ @"prepared", @"armed", @"active", @"unknown" ])
        need(!recheckResidualAllowed(@{@"phase" : phase}), "recheck must not recover unfinished sessions");
    std::puts("recheck_residual_refusal_passed=true finder_events_sent=false preferences_written=false");
}
