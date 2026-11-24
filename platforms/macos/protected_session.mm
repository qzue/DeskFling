#include "platforms/macos/session_trace.h"
// Opt-in native visibility experiments. Finder restart is a separate, explicitly consented mode.
#import <AppKit/AppKit.h>
#import <CoreServices/CoreServices.h>
#include <bsm/audit.h>
#include <fcntl.h>
#include <libproc.h>
#include <mach-o/dyld.h>
#include <mach/mach_time.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include "core/session/session.h"
#ifdef DPP_PROTECTED_MIRROR
#include "platforms/macos/protected_mirror_session.h"
#endif
extern char** environ;
namespace {
constexpr int helperSocket = 198;
constexpr int helperTimingSocket = 199;
constexpr unsigned long long limitMs = 20000;
constexpr unsigned long long finderLimitMs = 45000;
constexpr unsigned long long finderMirrorLimitMs = 180000;
constexpr unsigned long long finderContinuousMirrorLimitMs = 43200000ULL;
bool mirrorUIRuntime = false;
bool isFinderRecord(NSDictionary* r) {
    return [r[@"version"] isEqual:@4] || [r[@"version"] isEqual:@5] || [r[@"version"] isEqual:@6];
}
CFStringRef domain = CFSTR("com.apple.WindowManager");
CFStringRef visibilityKey = CFSTR("StandardHideDesktopIcons");
CFStringRef clickKey = CFSTR("EnableStandardClickToShowDesktop");
void need(bool ok, const char* stage) {
    if (!ok) throw std::runtime_error(stage);
}
// These command-line probes use AppKit too. NSScreen's session state can be stale
// until NSApplication exists; querying screens alone did not initialize it here.
void initializeAppKit() {
    need(NSThread.isMainThread, "AppKit requires main thread");
    NSApplication* application = NSApplication.sharedApplication;
    if (mirrorUIRuntime) {
        need(application != nil && (application.activationPolicy == NSApplicationActivationPolicyAccessory ||
                                    [application setActivationPolicy:NSApplicationActivationPolicyAccessory]),
             "initialize mirror application");
        return;
    }
    need(application != nil && (application.activationPolicy == NSApplicationActivationPolicyProhibited ||
                                [application setActivationPolicy:NSApplicationActivationPolicyProhibited]),
         "initialize non-activating AppKit application");
}
void serviceEvents(double seconds) {
    // No window, focus change, synthetic input, Shell enumeration or busy loop.
    const auto result = CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, false);
    if (result == kCFRunLoopRunFinished) usleep(static_cast<useconds_t>(seconds * 1000000));
}
void verifyAppKitRuntime() {
    initializeAppKit();
    __block bool serviced = false;
    CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopDefaultMode, ^{
      serviced = true;
    });
    serviceEvents(0.05);
    need(serviced, "main run loop did not service readiness callback");
}
unsigned long long nowMs() {
    mach_timebase_info_data_t base{};
    mach_timebase_info(&base);
    return static_cast<unsigned long long>(static_cast<long double>(mach_continuous_time()) * base.numer /
                                           base.denom / 1000000.0L);
}
unsigned long long awakeNowMs() {
    mach_timebase_info_data_t base{};
    mach_timebase_info(&base);
    return static_cast<unsigned long long>(static_cast<long double>(mach_absolute_time()) * base.numer /
                                           base.denom / 1000000.0L);
}
unsigned int loginSession() {
    auditinfo_addr_t info{};
    need(getaudit_addr(&info, sizeof(info)) == 0, "read login audit session");
    return info.ai_asid;
}
struct FD {
    int value = -1;
    explicit FD(int f = -1) : value(f) {}
    ~FD() {
        if (value >= 0) close(value);
    }
    FD(const FD&) = delete;
    FD& operator=(const FD&) = delete;
};
NSDictionary* process(pid_t pid) {
    proc_bsdinfo info{};
    if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) != sizeof(info)) return nil;
    return @{
        @"pid" : @(pid),
        @"seconds" : @(info.pbi_start_tvsec),
        @"microseconds" : @(info.pbi_start_tvusec),
        @"uid" : @(info.pbi_uid)
    };
}
bool sameProcess(NSDictionary* identity) {
    return identity && [process([identity[@"pid"] intValue]) isEqualToDictionary:identity];
}
NSDictionary* host(NSString* bundle) {
    NSArray<NSRunningApplication*>* apps =
        [NSRunningApplication runningApplicationsWithBundleIdentifier:bundle];
    need(apps.count == 1, "desktop host unavailable or ambiguous");
    NSDictionary* value = process(apps.firstObject.processIdentifier);
    need(value != nil, "desktop host identity unavailable");
    return value;
}
NSString* readSetting(CFStringRef key) {
    need(CFPreferencesAppSynchronize(domain), "preferences synchronize/read failed");
    CFPropertyListRef value =
        CFPreferencesCopyValue(key, domain, kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
    if (!value) return @"absent";
    const bool valid = CFGetTypeID(value) == CFBooleanGetTypeID();
    const bool hidden = valid && CFBooleanGetValue(static_cast<CFBooleanRef>(value));
    CFRelease(value);
    need(valid, "visibility preference is not Boolean; refusing coercion");
    return hidden ? @"true" : @"false";
}
NSString* readVisibility() { return readSetting(visibilityKey); }
void writeSetting(CFStringRef key, NSString* value, bool appAPI) {
    need(CFEqual(key, visibilityKey) || CFEqual(key, clickKey), "unapproved preference key");
    need([@[ @"absent", @"false", @"true" ] containsObject:value], "invalid visibility value");
    CFPropertyListRef setting =
        [value isEqual:@"absent"] ? nullptr : ([value isEqual:@"true"] ? kCFBooleanTrue : kCFBooleanFalse);
    if (appAPI) {
        CFPreferencesSetAppValue(key, setting, domain);
        need(CFPreferencesAppSynchronize(domain), "app visibility write/synchronize failed");
    } else {
        CFPreferencesSetValue(key, setting, domain, kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
        need(CFPreferencesSynchronize(domain, kCFPreferencesCurrentUser, kCFPreferencesAnyHost),
             "visibility write/synchronize failed");
    }
    need([readSetting(key) isEqual:value], "preference readback failed");
}
// Fixed settings only; injected IO is used solely by offline recovery tests.
using SettingRead = std::function<NSString*(bool)>;
using SettingWrite = std::function<void(bool, NSString*)>;
void validateFinderRecord(NSDictionary* record);
bool restoreChanges(NSMutableDictionary* r, const SettingRead& read, const SettingWrite& write) {
    const bool pair = [r[@"version"] isEqual:@2];
    bool success = true;
    // Restore icons first, then the temporary wallpaper-click behavior.
    for (bool click : {false, true}) {
        if (click && !pair) continue;
        if ((!click && [r[@"icons_recovered"] isEqual:@YES]) ||
            (click && [r[@"click_phase"] isEqual:@"restored"]))
            continue;
        NSString* original = r[click ? @"click_original" : @"original"];
        NSString* phase = r[click ? @"click_phase" : @"phase"];
        @try {
            try {
                NSString* current = read(click);
                if (![current isEqual:original]) {
                    need(![phase isEqual:@"prepared"] && [current isEqual:@"true"],
                         "external setting conflict; value retained");
                    write(click, original);
                }
                need([read(click) isEqual:original], "setting restoration readback failed");
                if (click)
                    r[@"click_phase"] = @"restored";
                else if (pair)
                    r[@"icons_recovered"] = @YES;
            } catch (...) {
                success = false;
            }
        } @catch (NSException*) {
            success = false;
        }
    }
    return success;
}
struct Store {
    NSString* directory;
    NSString* journal;
    FD directoryFD, operation, owner;
    explicit Store(NSString* testDirectory = nil) {
        NSString* support =
            NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES)
                .firstObject;
        directory = testDirectory
                        ? testDirectory
                        : [support stringByAppendingPathComponent:@"DesktopPhysicsPlayground-ProtectedMac"];
        need(mkdir(directory.fileSystemRepresentation, 0700) == 0 || errno == EEXIST,
             "create recovery directory");
        directoryFD.value =
            open(directory.fileSystemRepresentation, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        struct stat st{};
        need(directoryFD.value >= 0 && fstat(directoryFD.value, &st) == 0 && st.st_uid == getuid() &&
                 (st.st_mode & 0077) == 0,
             "unsafe recovery directory");
        journal = [directory stringByAppendingPathComponent:@"session.json"];
        operation.value =
            openat(directoryFD.value, "operation.lock", O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
        owner.value =
            openat(directoryFD.value, "owner.lock", O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
        for (int fd : {operation.value, owner.value}) {
            need(fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == getuid() &&
                     (st.st_mode & 0077) == 0 && st.st_nlink == 1,
                 "unsafe recovery lock");
        }
    }
    static void validate(NSDictionary* r) {
        need([r isKindOfClass:NSDictionary.class], "recovery record is not an object");
        if (isFinderRecord(r)) {
            validateFinderRecord(r);
            return;
        }
        need([r isKindOfClass:NSDictionary.class] &&
                 ([r[@"version"] isEqual:@1] || [r[@"version"] isEqual:@2] || [r[@"version"] isEqual:@3]) &&
                 [r[@"uid"] isEqual:@(getuid())] && [r[@"session_id"] isKindOfClass:NSString.class] &&
                 [[NSUUID alloc] initWithUUIDString:r[@"session_id"]] &&
                 [@[ @"absent", @"false" ] containsObject:r[@"original"]] &&
                 [r[@"expected"] isEqual:@"true"] &&
                 [@[ @"prepared", @"armed", @"active", @"restored" ] containsObject:r[@"phase"]] &&
                 [r[@"limit_ms"] isEqual:@(limitMs)] && [r[@"owner"] isKindOfClass:NSDictionary.class] &&
                 [r[@"started_ms"] isKindOfClass:NSNumber.class] &&
                 (!r[@"write_api"] || [@[ @"primitive", @"app_value" ] containsObject:r[@"write_api"]]),
             "invalid recovery record; retained");
        if ([r[@"version"] isEqual:@3]) {
            need([r[@"runtime"] isEqual:@"appkit_run_loop_v1"] && [r[@"write_api"] isEqual:@"app_value"],
                 "invalid AppKit recovery record; retained");
        } else {
            need(!r[@"runtime"], "AppKit recovery record requires version 3");
        }
        if ([r[@"version"] isEqual:@2]) {
            need([r[@"click_original"] isEqual:@"false"] && [r[@"click_expected"] isEqual:@"true"] &&
                     [r[@"write_api"] isEqual:@"app_value"] &&
                     [@[ @"prepared", @"armed", @"changed", @"restored" ] containsObject:r[@"click_phase"]] &&
                     (!r[@"icons_recovered"] || [@[ @YES, @NO ] containsObject:r[@"icons_recovered"]]),
                 "invalid two-setting recovery record; retained");
        } else {
            need(!r[@"click_original"] && !r[@"click_phase"], "two-setting record requires version 2");
        }
    }
    NSMutableDictionary* read() {
        FD file(openat(directoryFD.value, "session.json", O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
        if (file.value < 0) {
            need(errno == ENOENT, "open recovery record");
            return nil;
        }
        struct stat st{};
        need(fstat(file.value, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == getuid() && st.st_size > 0 &&
                 st.st_size <= 16384 && (st.st_mode & 0077) == 0,
             "unsafe recovery record");
        NSMutableData* data = [NSMutableData dataWithLength:static_cast<NSUInteger>(st.st_size)];
        size_t done = 0;
        while (done < data.length) {
            const ssize_t count =
                ::read(file.value, static_cast<char*>(data.mutableBytes) + done, data.length - done);
            need(count > 0, "read recovery record");
            done += static_cast<size_t>(count);
        }
        NSError* error = nil;
        id value = [NSJSONSerialization JSONObjectWithData:data
                                                   options:NSJSONReadingMutableContainers
                                                     error:&error];
        need(error == nil, "corrupt recovery JSON; retained");
        validate(value);
        return value;
    }
    void write(NSDictionary* record) {
        validate(record);
        NSError* error = nil;
        NSData* data = [NSJSONSerialization dataWithJSONObject:record
                                                       options:NSJSONWritingPrettyPrinted
                                                         error:&error];
        need(data && !error, "encode recovery record");
        NSString* name = [NSUUID.UUID.UUIDString stringByAppendingString:@".tmp"];
        FD file(openat(directoryFD.value, name.UTF8String,
                       O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW | O_CLOEXEC, 0600));
        need(file.value >= 0, "create temporary record");
        size_t done = 0;
        while (done < data.length) {
            const ssize_t count =
                ::write(file.value, static_cast<const char*>(data.bytes) + done, data.length - done);
            need(count > 0, "write recovery record");
            done += static_cast<size_t>(count);
        }
        need(fsync(file.value) == 0 && fcntl(file.value, F_FULLFSYNC) == 0, "flush recovery record");
        need(renameat(directoryFD.value, name.UTF8String, directoryFD.value, "session.json") == 0 &&
                 fsync(directoryFD.value) == 0,
             "commit recovery record");
    }
    void archive(NSDictionary* record) {
        NSString* name = [record[@"session_id"] stringByAppendingString:@".recovered.json"];
        need(renameatx_np(directoryFD.value, "session.json", directoryFD.value, name.UTF8String,
                          RENAME_EXCL) == 0 &&
                 fsync(directoryFD.value) == 0,
             "archive recovered record");
    }
};
struct Guard {
    int fd;
    explicit Guard(int value) : fd(value) {
        const auto end = nowMs() + 2000;
        while (flock(fd, LOCK_EX | LOCK_NB) != 0) {
            need((errno == EWOULDBLOCK || errno == EINTR) && nowMs() < end,
                 "recovery operation busy; record retained");
            usleep(25000);
        }
    }
    ~Guard() { flock(fd, LOCK_UN); }
};
bool recoverFinder(Store& store, NSString* nonce, NSString* trigger);
bool recoverFinderLocked(Store& store, NSMutableDictionary* record, NSString* trigger);
int finderHelper(NSString* nonce);
struct Backend : dpp::SessionBackend {
    Store store;
    NSMutableDictionary* record = nil;
    FD channel;
    FD timingChannel;
    pid_t child = -1;
    bool locked = false, persisted = false, prepareOnly = false, appAPI = false, clickDiagnostic = false,
         clickUnchanged = false, appkitRuntime = false;
    bool lock() override { return locked = flock(store.owner.value, LOCK_EX | LOCK_NB) == 0; }
    bool recover(NSString* expectedSession = nil, NSString* trigger = @"independent_command") {
        Guard guard(store.operation.value);
        NSMutableDictionary* r = store.read();
        if (!r || (expectedSession && ![r[@"session_id"] isEqual:expectedSession]) ||
            [r[@"phase"] isEqual:@"restored"])
            return true;
        if (isFinderRecord(r)) {
            // recoverFinderLocked shares this operation lock with all native mutations.
            return recoverFinderLocked(store, r, trigger);
        }
        NSString* initialPhase = r[@"phase"];
        @try {
            try {
                const bool restored = restoreChanges(
                    r, [](bool click) { return readSetting(click ? clickKey : visibilityKey); },
                    [&](bool click, NSString* value) {
                        writeSetting(click ? clickKey : visibilityKey, value,
                                     [r[@"write_api"] isEqual:@"app_value"]);
                    });
                need(restored, "one or more settings could not be restored; journal retained");
                if ([r[@"version"] isEqual:@3]) {
                    // Restore the preference first even if the GUI environment changed.
                    // Failure to initialize/flush retains the journal for a later retry.
                    initializeAppKit();
                    serviceEvents(0.25);
                    r[@"recovery_runtime_serviced"] = @YES;
                }
                r[@"phase"] = @"restored";
                r[@"result"] = @"preference_restored_visual_unverified";
                r[@"recovery_trigger"] = trigger;
                r[@"restored_ms"] = @(nowMs());
                store.write(r);
                std::puts("preference_restored=true actual_icons_require_observation=true");
                return true;
            } catch (...) {
                r[@"phase"] = initialPhase;
                r[@"result"] = @"recovery_failed_record_retained";
                store.write(r);
                throw;
            }
        } @catch (NSException*) {
            r[@"phase"] = initialPhase;
            r[@"result"] = @"recovery_exception_record_retained";
            store.write(r);
            throw std::runtime_error("recovery Objective-C exception");
        }
    }
    bool recoverResidual() override {
        if (!recover()) return false;
        Guard guard(store.operation.value);
        auto r = store.read();
        if (r) store.archive(r);
        return true;
    }
    bool probe() override {
        try {
            if (appkitRuntime) verifyAppKitRuntime();
            const auto os = NSProcessInfo.processInfo.operatingSystemVersion;
            need(os.majorVersion == 15 && os.minorVersion == 7 && os.patchVersion == 5 &&
                     NSScreen.screens.count == 1,
                 "unvalidated Mac OS/display configuration");
            (void)host(@"com.apple.finder");
            (void)host(@"com.apple.WindowManager");
            need(![readVisibility() isEqual:@"true"], "icons already configured hidden; experiment refused");
            if (clickDiagnostic) {
                NSString* click = readSetting(clickKey);
                need([@[ @"false", @"true" ] containsObject:click], "click setting is absent or invalid");
                clickUnchanged = [click isEqual:@"true"];
                need([readSetting(CFSTR("GloballyEnabled")) isEqual:@"false"],
                     "Stage Manager must remain off");
                need(!CFPreferencesAppValueIsForced(clickKey, domain) &&
                         !CFPreferencesAppValueIsForced(visibilityKey, domain),
                     "managed setting cannot be tested");
            }
            return true;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "probe_rejected_reason=%s\n", error.what());
            throw;
        }
    }
    bool snapshot() override {
        NSString* original = readVisibility();
        need(![original isEqual:@"true"], "visibility changed before snapshot");
        record = [@{
            @"version" : @1,
            @"session_id" : NSUUID.UUID.UUIDString,
            @"uid" : @(getuid()),
            @"login_session" : @(loginSession()),
            @"owner" : process(getpid()),
            @"finder" : host(@"com.apple.finder"),
            @"window_manager" : host(@"com.apple.WindowManager"),
            @"original" : original,
            @"expected" : @"true",
            @"phase" : @"prepared",
            @"result" : @"pending",
            @"started_ms" : @(nowMs()),
            @"limit_ms" : @(limitMs),
            @"write_api" : appAPI ? @"app_value" : @"primitive",
            @"backend" : @"WindowManager_StandardHideDesktopIcons_undocumented"
        } mutableCopy];
        if (appkitRuntime) {
            record[@"version"] = @3;
            record[@"runtime"] = @"appkit_run_loop_v1";
            record[@"screens_have_separate_spaces"] = @(NSScreen.screensHaveSeparateSpaces);
        }
        if (clickDiagnostic && clickUnchanged) {
            need([readSetting(clickKey) isEqual:@"true"], "click setting changed before snapshot");
            record[@"condition_click_original"] = @"true";
            record[@"condition_click_modified"] = @NO;
            std::puts("click_condition=already_always click_setting_will_be_modified=false");
        }
        if (clickDiagnostic && !clickUnchanged) {
            need([readSetting(clickKey) isEqual:@"false"], "click setting changed before snapshot");
            record[@"version"] = @2;
            record[@"click_original"] = @"false";
            record[@"click_expected"] = @"true";
            record[@"click_phase"] = @"prepared";
        }
        return true;
    }
    bool persistIntent() override {
        Guard guard(store.operation.value);
        store.write(record);
        persisted = true;
        return true;
    }
    bool prepareScene() override { return true; }  // first experiment has no images or physics
    bool recoveryReady() override {
        int sockets[2]{};
        need(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0, "helper socketpair");
        channel.value = sockets[0];
        FD childChannel(sockets[1]);
        for (int fd : sockets) {
            const int yes = 1;
            need(setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes)) == 0 &&
                     fcntl(fd, F_SETFD, FD_CLOEXEC) == 0,
                 "configure helper IPC");
        }
        FD childTiming;
        if ([record[@"version"] isEqual:@5]) {
            int timing[2]{};
            if (socketpair(AF_UNIX, SOCK_DGRAM, 0, timing) == 0) {
                timingChannel.value = timing[0];
                childTiming.value = timing[1];
                for (int fd : timing) {
                    const int yes = 1;
                    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
                    need(fcntl(fd, F_SETFD, FD_CLOEXEC) == 0, "configure timing descriptor inheritance");
                }
            }
        }
        char exe[4096]{};
        uint32_t length = sizeof(exe);
        need(_NSGetExecutablePath(exe, &length) == 0, "helper executable path");
        std::string nonce([record[@"session_id"] UTF8String]);
        char helperArg[] = "--helper";
        char* args[]{exe, helperArg, nonce.data(), nullptr};
        posix_spawn_file_actions_t actions;
        need(posix_spawn_file_actions_init(&actions) == 0, "helper spawn actions");
        int action = posix_spawn_file_actions_adddup2(&actions, childChannel.value, helperSocket);
        if (action == 0 && childTiming.value >= 0)
            action = posix_spawn_file_actions_adddup2(&actions, childTiming.value, helperTimingSocket);
        const int spawned = action == 0 ? posix_spawn(&child, exe, &actions, nullptr, args, environ) : action;
        posix_spawn_file_actions_destroy(&actions);
        need(spawned == 0, "launch recovery helper");
        pollfd ready{channel.value, POLLIN, 0};
        char response = 0;
        need(poll(&ready, 1, 5000) > 0 && recv(channel.value, &response, 1, 0) == 1 &&
                 response == (isFinderRecord(record) ? 'F' : (appkitRuntime ? 'A' : 'R')),
             "recovery helper not ready");
        record[@"helper_ready_ms"] = @(nowMs());
        std::puts("independent_helper_ready=true");
        return true;
    }
    bool activate() override {
        if (clickDiagnostic && !clickUnchanged && !prepareOnly) {
            {
                Guard guard(store.operation.value);
                auto r = store.read();
                need([r[@"session_id"] isEqual:record[@"session_id"]] && [r[@"phase"] isEqual:@"prepared"] &&
                         [r[@"click_phase"] isEqual:@"prepared"] && sameProcess(record[@"finder"]) &&
                         sameProcess(record[@"window_manager"]) &&
                         [readSetting(clickKey) isEqual:record[@"click_original"]] &&
                         [readVisibility() isEqual:record[@"original"]] &&
                         nowMs() - [record[@"started_ms"] unsignedLongLongValue] < limitMs,
                     "stale click activation");
                record[@"click_phase"] = @"armed";
                record[@"click_armed_ms"] = @(nowMs());
                store.write(record);
                writeSetting(clickKey, @"true", true);
                record[@"click_phase"] = @"changed";
                record[@"click_readback_ms"] = @(nowMs());
                store.write(record);
            }
            // Give this single changed condition time to apply, without holding the recovery lock.
            for (int i = 0; i < 8; ++i) {
                need(send(channel.value, "H", 1, 0) == 1, "helper unavailable during settling");
                usleep(250000);
            }
        }
        Guard guard(store.operation.value);
        auto current = store.read();
        need([current[@"session_id"] isEqual:record[@"session_id"]] &&
                 [current[@"phase"] isEqual:@"prepared"] &&
                 nowMs() - [record[@"started_ms"] unsignedLongLongValue] < limitMs,
             "stale activation");
        if (prepareOnly) return true;
        if (clickDiagnostic) {
            need([readSetting(clickKey) isEqual:@"true"], "click condition changed before hide");
            if (!clickUnchanged)
                need([current[@"click_phase"] isEqual:@"changed"] && ![current[@"icons_recovered"] boolValue],
                     "stale condition before hide");
        }
        need(sameProcess(record[@"finder"]) && sameProcess(record[@"window_manager"]),
             "desktop hosts changed before mutation");
        need([readVisibility() isEqual:record[@"original"]],
             "visibility changed externally before activation");
        record[@"phase"] = @"armed";
        record[@"armed_ms"] = @(nowMs());
        store.write(record);
        writeSetting(visibilityKey, @"true", appAPI);
        record[@"phase"] = @"active";
        record[@"write_readback_ms"] = @(nowMs());
        store.write(record);
        std::puts("hide_preference_readback=true actual_icons_require_observation=true");
        std::fflush(stdout);
        return true;
    }
    bool restore() override { return !persisted || recover(record[@"session_id"], @"normal_stop"); }
    void unlock() noexcept override {
        if (locked) {
            flock(store.owner.value, LOCK_UN);
            locked = false;
        }
    }
};
#include "finder_restart_backend.inc"
#ifdef DPP_WINDOWMANAGER_AUTHORIZED_RECHECK
#include "windowmanager_authorized_recheck.inc"
#endif
int helper(NSString* nonce) {
    // A terminal interrupt/close must not terminate the recovery executor with its owner.
    struct sigaction ignored{};
    ignored.sa_handler = SIG_IGN;
    sigemptyset(&ignored.sa_mask);
    need(sigaction(SIGHUP, &ignored, nullptr) == 0 && sigaction(SIGINT, &ignored, nullptr) == 0 &&
             sigaction(SIGPIPE, &ignored, nullptr) == 0,
         "helper terminal signal protection");
    bool finderMode = false;
    {
        Store store;
        Guard guard(store.operation.value);
        auto r = store.read();
        finderMode = isFinderRecord(r);
    }
    if (finderMode) return finderHelper(nonce);
    Backend backend;
    {
        Guard guard(backend.store.operation.value);
        backend.record = backend.store.read();
        need([backend.record[@"session_id"] isEqual:nonce] &&
                 [backend.record[@"phase"] isEqual:@"prepared"] && sameProcess(backend.record[@"owner"]),
             "helper record/owner mismatch");
    }
    const bool appkitRuntime = [backend.record[@"version"] isEqual:@3];
    if (appkitRuntime) {
        verifyAppKitRuntime();
    }
    int yes = 1;
    need(setsockopt(helperSocket, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes)) == 0,
         "helper channel unavailable");
    need(send(helperSocket, appkitRuntime ? "A" : "R", 1, 0) == 1, "helper ready acknowledgement");
    auto heartbeat = nowMs();
    for (;;) {
        pollfd input{helperSocket, POLLIN, 0};
        const int wait = poll(&input, 1, appkitRuntime ? 0 : 500);
        bool ownerClosed = false;
        if (wait > 0) {
            char bytes[64];
            const auto n = recv(helperSocket, bytes, sizeof(bytes), MSG_DONTWAIT);
            if (n > 0)
                heartbeat = nowMs();
            else if (n == 0)
                ownerClosed = true;
        }
        {
            Guard guard(backend.store.operation.value);
            auto r = backend.store.read();
            need(r != nil, "helper record disappeared");
            if (![r[@"session_id"] isEqual:nonce] || [r[@"phase"] isEqual:@"restored"]) return 0;
        }
        NSString* cause = nil;
        if (ownerClosed || !sameProcess(backend.record[@"owner"]))
            cause = @"owner_exit";
        else if (nowMs() - heartbeat >= 5000)
            cause = @"heartbeat_timeout";
        else if (nowMs() - [backend.record[@"started_ms"] unsignedLongLongValue] >= limitMs)
            cause = @"maximum_session_time";
        if (cause) return backend.recover(nonce, cause) ? 0 : 1;
        if (appkitRuntime) serviceEvents(0.25);
    }
}
NSDictionary* preferenceAudit(CFStringRef key) {
    auto describe = [](CFPropertyListRef raw) -> NSString* {
        if (!raw) return @"absent";
        NSString* result = CFGetTypeID(raw) == CFBooleanGetTypeID()
                               ? (CFBooleanGetValue(static_cast<CFBooleanRef>(raw)) ? @"true" : @"false")
                               : @"non_boolean";
        CFRelease(raw);
        return result;
    };
    return @{
        @"current_user_any_host" :
            describe(CFPreferencesCopyValue(key, domain, kCFPreferencesCurrentUser, kCFPreferencesAnyHost)),
        @"current_user_current_host" : describe(
            CFPreferencesCopyValue(key, domain, kCFPreferencesCurrentUser, kCFPreferencesCurrentHost)),
        @"effective" : describe(CFPreferencesCopyAppValue(key, domain)),
        @"managed" : @(CFPreferencesAppValueIsForced(key, domain))
    };
}
// Read-only telemetry in a separate process. Never invoked by the recovery helper.
// No window names, images, file paths or arbitrary application metadata are emitted.
NSDictionary* observation() {
    NSString* exact = readVisibility();
    NSMutableDictionary* identities = [NSMutableDictionary dictionary];
    for (NSString* bundle in @[ @"com.apple.finder", @"com.apple.WindowManager", @"com.apple.dock" ])
        identities[bundle] = host(bundle);
    CFArrayRef raw = CGWindowListCopyWindowInfo(kCGWindowListOptionAll, kCGNullWindowID);
    need(raw != nullptr, "window metadata unavailable; no screenshot/permission request attempted");
    NSArray* list = CFBridgingRelease(raw);
    NSMutableArray* windows = [NSMutableArray array];
    for (NSDictionary* w in list) {
        NSNumber* pid = w[(__bridge NSString*)kCGWindowOwnerPID];
        for (NSString* bundle in identities) {
            if (![pid isEqual:identities[bundle][@"pid"]]) continue;
            NSMutableDictionary* row = [@{@"host" : bundle, @"pid" : pid} mutableCopy];
            for (NSString* key in @[
                     (__bridge NSString*)kCGWindowNumber, (__bridge NSString*)kCGWindowLayer,
                     (__bridge NSString*)kCGWindowBounds, (__bridge NSString*)kCGWindowIsOnscreen,
                     (__bridge NSString*)kCGWindowAlpha
                 ])
                if (w[key]) row[key] = w[key];
            [windows addObject:row];
        }
    }
    return @{
        @"monotonic_ms" : @(nowMs()),
        @"observer" : process(getpid()),
        @"appkit_initialized" : @(NSApp != nil),
        @"screens_have_separate_spaces" : @(NSScreen.screensHaveSeparateSpaces),
        @"exact" : exact,
        @"preference" : preferenceAudit(visibilityKey),
        @"click_preference" : preferenceAudit(clickKey),
        @"hosts" : identities,
        @"windows" : windows,
        @"writes_performed" : @NO,
        @"visual_visibility" : @"unverified_metadata_only"
    };
}
void observe(bool once) {
    const auto start = nowMs();
    for (unsigned int i = 0; i < (once ? 1U : 23U); ++i) {
        @autoreleasepool {
            NSDictionary* sample = observation();
            NSError* error = nil;
            NSData* data = [NSJSONSerialization dataWithJSONObject:sample options:0 error:&error];
            need(data && !error, "encode observation");
            need(std::fwrite(data.bytes, 1, data.length, stdout) == data.length &&
                     std::fputc('\n', stdout) != EOF && std::fflush(stdout) == 0,
                 "write observation");
        }
        if (once || i == 22) break;
        const auto next = start + (i + 1ULL) * 1000;
        const auto now = nowMs();
        if (next > now) usleep(static_cast<useconds_t>((next - now) * 1000));
    }
}
void audit() {
    std::printf("backend=WindowManager_StandardHideDesktopIcons_undocumented original=%s\n",
                readVisibility().UTF8String);
    NSDictionary* value = @{
        @"os" : NSProcessInfo.processInfo.operatingSystemVersionString,
        @"screens" : @(NSScreen.screens.count),
        @"login_session" : @(loginSession()),
        @"finder" : host(@"com.apple.finder"),
        @"window_manager" : host(@"com.apple.WindowManager"),
        @"writes_performed" : @NO,
        @"candidate_enabled" : @NO,
        @"appkit_initialized" : @(NSApp != nil),
        @"screens_have_separate_spaces" : @(NSScreen.screensHaveSeparateSpaces),
        @"visibility_verdict" : @"failed_human_observation_20261113",
        @"preferences" : @{
            @"StandardHideDesktopIcons" : preferenceAudit(visibilityKey),
            @"HideDesktop" : preferenceAudit(CFSTR("HideDesktop")),
            @"GloballyEnabled" : preferenceAudit(CFSTR("GloballyEnabled")),
            @"EnableStandardClickToShowDesktop" : preferenceAudit(CFSTR("EnableStandardClickToShowDesktop"))
        }
    };
    NSData* data = [NSJSONSerialization dataWithJSONObject:value
                                                   options:NSJSONWritingPrettyPrinted
                                                     error:nil];
    std::fwrite(data.bytes, 1, data.length, stdout);
    std::puts("");
}
void selfTest() {
    finderSelfTest();
    char path[] = "/tmp/dpp-mac-journal-XXXXXX";
    need(mkdtemp(path), "test directory");
    Store store([NSString stringWithUTF8String:path]);
    NSMutableDictionary* r = [@{
        @"version" : @1,
        @"uid" : @(getuid()),
        @"session_id" : NSUUID.UUID.UUIDString,
        @"original" : @"absent",
        @"expected" : @"true",
        @"phase" : @"prepared",
        @"limit_ms" : @(limitMs),
        @"owner" : process(getpid()),
        @"started_ms" : @(nowMs())
    } mutableCopy];
    store.write(r);
    need([store.read() isEqualToDictionary:r], "journal roundtrip");
    r[@"original"] = @"true";
    bool rejected = false;
    try {
        store.write(r);
    } catch (...) {
        rejected = true;
    }
    need(rejected, "already-hidden original accepted");
    r[@"original"] = @"absent";
    r[@"write_api"] = @"app_value";
    store.write(r);
    need([store.read()[@"write_api"] isEqual:@"app_value"], "app API record roundtrip");
    r[@"version"] = @3;
    r[@"runtime"] = @"appkit_run_loop_v1";
    store.write(r);
    need([store.read()[@"version"] isEqual:@3], "AppKit recovery record roundtrip");
    r[@"runtime"] = @"arbitrary_runtime";
    rejected = false;
    try {
        store.write(r);
    } catch (...) {
        rejected = true;
    }
    need(rejected, "unknown AppKit runtime accepted");
    r[@"runtime"] = @"appkit_run_loop_v1";
    r[@"version"] = @1;
    rejected = false;
    try {
        store.write(r);
    } catch (...) {
        rejected = true;
    }
    need(rejected, "AppKit recovery schema downgrade accepted");
    [r removeObjectForKey:@"runtime"];
    r[@"write_api"] = @"arbitrary_command";
    rejected = false;
    try {
        store.write(r);
    } catch (...) {
        rejected = true;
    }
    need(rejected, "unknown write API accepted");
    r[@"write_api"] = @"app_value";
    // Recovery fault checks use only in-memory values, never system preferences.
    r[@"version"] = @2;
    r[@"original"] = @"false";
    r[@"click_original"] = @"false";
    r[@"click_expected"] = @"true";
    r[@"click_phase"] = @"prepared";
    store.write(r);
    need([store.read() isEqualToDictionary:r], "two-setting journal roundtrip");
    NSMutableDictionary* bad = [r mutableCopy];
    bad[@"version"] = @1;
    rejected = false;
    try {
        Store::validate(bad);
    } catch (...) {
        rejected = true;
    }
    need(rejected, "two-setting downgrade accepted");
    [bad removeObjectForKey:@"click_original"];
    bad[@"version"] = @2;
    rejected = false;
    try {
        Store::validate(bad);
    } catch (...) {
        rejected = true;
    }
    need(rejected, "missing second original accepted");
    for (int scenario = 0; scenario < 4; ++scenario) {
        NSMutableDictionary* fixture = [r mutableCopy];
        fixture[@"phase"] = scenario < 2 ? @"prepared" : @"active";
        fixture[@"click_phase"] = scenario == 0 ? @"prepared" : @"armed";
        NSMutableArray* values =
            [@[ scenario < 2 ? @"false" : @"true", scenario == 0 ? @"false" : @"true" ] mutableCopy];
        NSMutableArray* writes = [NSMutableArray array];
        bool failIcon = scenario == 3;
        SettingRead reader = [&](bool click) -> NSString* { return values[click ? 1 : 0]; };
        SettingWrite writer = [&](bool click, NSString* value) {
            if (!click && failIcon) throw std::runtime_error("injected icon restore failure");
            [writes addObject:click ? @"click" : @"icons"];
            values[click ? 1 : 0] = value;
        };
        bool ok = restoreChanges(fixture, reader, writer);
        need(ok == (scenario != 3), "recovery failure result");
        need([values[1] isEqual:@"false"], "second recovery skipped after first failure");
        NSArray* expected = scenario == 0 ? @[] : (scenario == 2 ? @[ @"icons", @"click" ] : @[ @"click" ]);
        need([writes isEqual:expected], "wrong restoration order or untouched setting overwritten");
        if (scenario == 3) {
            failIcon = false;
            values[1] = @"true";  // external edit AFTER our completed click recovery must be left alone.
            need(restoreChanges(fixture, reader, writer) && [values[1] isEqual:@"true"],
                 "partial retry overwrote completed setting");
        }
        NSUInteger count = writes.count;
        need(restoreChanges(fixture, reader, writer) && writes.count == count, "non-idempotent recovery");
    }
    NSMutableDictionary* unchanged = [r mutableCopy];
    unchanged[@"version"] = @1;
    unchanged[@"phase"] = @"active";
    [unchanged removeObjectForKey:@"click_original"];
    [unchanged removeObjectForKey:@"click_expected"];
    [unchanged removeObjectForKey:@"click_phase"];
    unchanged[@"condition_click_original"] = @"true";
    unchanged[@"condition_click_modified"] = @NO;
    Store::validate(unchanged);
    NSString* iconValue = @"true";
    need(restoreChanges(
             unchanged,
             [&](bool click) -> NSString* {
                 need(!click, "untouched click condition read during recovery");
                 return iconValue;
             },
             [&](bool click, NSString* value) {
                 need(!click, "untouched click condition overwritten");
                 iconValue = value;
             }),
         "already-always recovery failed");
    need([iconValue isEqual:@"false"], "icons not recovered for already-always case");
    std::puts("two_setting_recovery_order_partial_failure_idempotence_schema_guards_passed=true");
    FD corrupt(openat(store.directoryFD.value, "session.json", O_WRONLY | O_TRUNC | O_NOFOLLOW));
    need(corrupt.value >= 0 && ::write(corrupt.value, "broken", 6) == 6, "corruption fixture");
    rejected = false;
    try {
        (void)store.read();
    } catch (...) {
        rejected = true;
    }
    need(rejected, "corrupt journal accepted");
    need(sameProcess(process(getpid())), "process identity");
    std::puts("journal_roundtrip_hidden_original_and_corruption_guards_passed=true "
              "write_api_allowlist_passed=true system_visibility_written=false");
    unlinkat(store.directoryFD.value, "session.json", 0);
    unlinkat(store.directoryFD.value, "operation.lock", 0);
    unlinkat(store.directoryFD.value, "owner.lock", 0);
    rmdir(path);
}
}  // namespace
#ifdef DPP_PROTECTED_MIRROR
struct DPPProtectedMirrorSession::Impl {
    FinderBackend backend{false, true};
    dpp::SessionCoordinator coordinator{backend};
    NSDictionary* hidden = nil;
    bool active = false;
    bool continuous = false;
    NSDictionary* originalTiming = nil;
    unsigned long long helperSampleRequests = 0;
    std::string error;
    unsigned long long activeLimitMs() const {
        return continuous ? finderContinuousMirrorLimitMs : finderMirrorLimitMs;
    }
};
DPPProtectedMirrorSession::DPPProtectedMirrorSession() : impl_(std::make_unique<Impl>()) {}
DPPProtectedMirrorSession::~DPPProtectedMirrorSession() { (void)stop(); }
bool DPPProtectedMirrorSession::start(NSDictionary* finder, NSUInteger itemCount, NSString* snapshotID,
                                      bool continuous) {
    try {
        impl_->continuous = continuous;
        impl_->backend.continuousMirror = continuous;
        impl_->backend.sceneFinder = finder;
        impl_->backend.sceneItemCount = itemCount;
        impl_->backend.sceneSnapshotID = snapshotID;
        const bool started = impl_->coordinator.start();
        impl_->originalTiming = [impl_->backend.record copy];
        if (!started) {
            impl_->error = impl_->coordinator.failure();
            return false;
        }
        Guard guard(impl_->backend.store.operation.value);
        auto r = impl_->backend.store.read();
        NSNumber* expectedVersion = continuous ? @6 : @5;
        need([r[@"session_id"] isEqual:impl_->backend.record[@"session_id"]] &&
                 [r[@"phase"] isEqual:@"active"] && [r[@"version"] isEqual:expectedVersion],
             "mirror recovery session not active");
        impl_->hidden = r[@"hidden_finder"];
        impl_->active = true;
        std::printf("protected_mirror_session_id=%s version=%d\n", [r[@"session_id"] UTF8String],
                    expectedVersion.intValue);
        std::fflush(stdout);
        return true;
    } catch (const std::exception& error) {
        impl_->error = error.what();
        return false;
    }
}
bool DPPProtectedMirrorSession::tick() {
    drainTimingSamples();
    if (!impl_->active || !sameProcess(impl_->hidden) || remainingMs() == 0) return false;
    if (send(impl_->backend.channel.value, "P", 1, 0) != 1) return false;
    pollfd event{impl_->backend.channel.value, POLLIN, 0};
    return poll(&event, 1, 0) == 0;
}
bool DPPProtectedMirrorSession::notifySystemSleep(bool sleeping) {
    if (!impl_ || !impl_->active || !impl_->continuous || impl_->backend.channel.value < 0) return false;
    const char cmd = sleeping ? 'Z' : 'W';
    if (send(impl_->backend.channel.value, &cmd, 1, 0) != 1) return false;
    if (!sleeping) {
        (void)send(impl_->backend.channel.value, "P", 1, 0);
    }
    return true;
}
bool DPPProtectedMirrorSession::stop() {
    if (!impl_) return true;
    const bool ok = impl_->coordinator.stop();
    if (ok)
        impl_->active = false;
    else
        impl_->error = "recovery incomplete; journal retained; use independent recovery";
    if (ok && impl_->backend.child > 0) {
        int status = 0;
        const auto end = nowMs() + 3000;
        pid_t waited = 0;
        do {
            waited = waitpid(impl_->backend.child, &status, WNOHANG);
            if (waited == 0) serviceEvents(.05);
        } while (waited == 0 && nowMs() < end);
        if (waited == impl_->backend.child) {
            impl_->backend.child = -1;
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                impl_->error = "recovery recorded but helper exit was not clean";
                return false;
            }
        } else if (waited < 0 && errno == ECHILD)
            impl_->backend.child = -1;
        else {
            impl_->error = "helper exit not yet confirmed; retry recovery";
            return false;
        }
    }
    drainTimingSamples();
    return ok;
}
unsigned long long DPPProtectedMirrorSession::remainingMs() const {
    if (!impl_->active) return 0;
    const auto limit = impl_->activeLimitMs();
    const auto elapsed = nowMs() - [impl_->backend.record[@"started_ms"] unsignedLongLongValue];
    return elapsed < limit ? limit - elapsed : 0;
}
bool DPPProtectedMirrorSession::isContinuous() const { return impl_ && impl_->continuous; }
NSDictionary* DPPProtectedMirrorSession::diagnosticIdentity() const {
    NSMutableDictionary* sample =
        [dpp::mac::ownerTimingSample(impl_->originalTiming, impl_->backend.record, impl_->active, nowMs(),
                                     impl_->activeLimitMs()) mutableCopy];
    if (sample.count) sample[@"host_identity_matches"] = @(impl_->hidden && sameProcess(impl_->hidden));
    return sample;
}
NSDictionary* DPPProtectedMirrorSession::sampleTiming() const {
    NSMutableDictionary* sample = [diagnosticIdentity() mutableCopy];
    drainTimingSamples();
    bool requested = false;
    const uint64_t sequence = impl_->helperSampleRequests + 1;
    if (impl_->active && impl_->backend.timingChannel.value >= 0 && impl_->helperSampleRequests < 260 &&
        send(impl_->backend.timingChannel.value, &sequence, sizeof(sequence), MSG_DONTWAIT) ==
            sizeof(sequence)) {
        ++impl_->helperSampleRequests;
        requested = true;
    }
    sample[@"helper_sample_requested"] = @(requested);
    sample[@"helper_sample_request"] = requested ? @(impl_->helperSampleRequests) : (id)NSNull.null;
    return sample;
}
void DPPProtectedMirrorSession::drainTimingSamples() const {
    if (impl_->backend.timingChannel.value < 0) return;
    @try {
        for (unsigned n = 0; n < 16; ++n) {
            char bytes[4096];
            const auto count = recv(impl_->backend.timingChannel.value, bytes, sizeof(bytes), MSG_DONTWAIT);
            if (count <= 0) break;
            NSData* data = [NSData dataWithBytes:bytes length:static_cast<NSUInteger>(count)];
            id sample = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
            if ([sample isKindOfClass:NSDictionary.class] &&
                [sample[@"protected_session_id"] isEqual:impl_->backend.record[@"session_id"]])
                dpp::mac::writeMirrorTrace(@"mirror_helper_timing=", sample);
        }
    } @catch (NSException*) {
    }
}
std::string DPPProtectedMirrorSession::failure() const { return impl_->error; }
int requestFinderAuthorizationFromUI() { return requestFinderAuthorization(); }
bool recoverProtectedMirrorJournalFromUI() {
    Backend backend;
    return backend.recover();
}
NSString* dppAppLogDirectoryPath() {
    NSArray<NSURL*>* appSupp = [NSFileManager.defaultManager URLsForDirectory:NSApplicationSupportDirectory
                                                                    inDomains:NSUserDomainMask];
    NSString* base = appSupp.firstObject ? appSupp.firstObject.path
                                         : [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support"];
    return [[base stringByAppendingPathComponent:@"DesktopPhysicsPlayground"]
        stringByAppendingPathComponent:@"logs"];
}
static void redirectAppBundleLogsIfDetached() {
    if (isatty(STDOUT_FILENO) && isatty(STDERR_FILENO)) return;
    NSString* logDir = dppAppLogDirectoryPath();
    NSFileManager* fm = NSFileManager.defaultManager;
    [fm createDirectoryAtPath:logDir
        withIntermediateDirectories:YES
                         attributes:@{NSFilePosixPermissions : @0700}
                              error:nil];
    // Prune older mirror-*.log files beyond the newest 20 so disk usage stays bounded.
    NSArray<NSURL*>* files =
        [fm contentsOfDirectoryAtURL:[NSURL fileURLWithPath:logDir isDirectory:YES]
            includingPropertiesForKeys:@[ NSURLContentModificationDateKey ]
                               options:NSDirectoryEnumerationSkipsHiddenFiles
                                 error:nil];
    NSMutableArray<NSURL*>* mirrorLogs = [NSMutableArray array];
    for (NSURL* u in files) {
        if ([u.lastPathComponent hasPrefix:@"mirror-"] && [u.pathExtension isEqualToString:@"log"]) {
            [mirrorLogs addObject:u];
        }
    }
    if (mirrorLogs.count >= 20) {
        [mirrorLogs sortUsingComparator:^NSComparisonResult(NSURL* a, NSURL* b) {
          NSDate *da = nil, *db = nil;
          [a getResourceValue:&da forKey:NSURLContentModificationDateKey error:nil];
          [b getResourceValue:&db forKey:NSURLContentModificationDateKey error:nil];
          if (!da && !db) return NSOrderedSame;
          if (!da) return NSOrderedAscending;
          if (!db) return NSOrderedDescending;
          return [da compare:db];
        }];
        while (mirrorLogs.count >= 20) {
            [fm removeItemAtURL:mirrorLogs.firstObject error:nil];
            [mirrorLogs removeObjectAtIndex:0];
        }
    }
    NSString* logFile = [logDir
        stringByAppendingPathComponent:[NSString stringWithFormat:@"mirror-%@.log", NSUUID.UUID.UUIDString]];
    const int fd = open(logFile.fileSystemRepresentation, O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC, 0600);
    if (fd >= 0) {
        (void)dup2(fd, STDOUT_FILENO);
        (void)dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO) close(fd);
        std::setvbuf(stdout, nullptr, _IOLBF, 0);
        std::setvbuf(stderr, nullptr, _IONBF, 0);
    }
}
static bool isAppBundleDefaultLaunchArgs(int argc, char** argv) {
    if (argc <= 1) return true;
    for (int i = 1; i < argc; ++i) {
        NSString* arg = [NSString stringWithUTF8String:argv[i]];
        if ([arg hasPrefix:@"-psn_"]) continue;
        if ([arg isEqual:@"-NSDocumentRevisionsDebugMode"] || [arg isEqual:@"YES"] || [arg isEqual:@"NO"])
            continue;
        return false;
    }
    return true;
}
#endif
int main(int argc, char** argv) {
    @autoreleasepool {
        @try {
            try {
                NSString* mode = argc > 1 ? [NSString stringWithUTF8String:argv[1]] : @"--help";
#ifdef DPP_PROTECTED_MIRROR
                if (isAppBundleDefaultLaunchArgs(argc, argv)) {
                    redirectAppBundleLogsIfDetached();
                    mirrorUIRuntime = true;
                    initializeAppKit();
                    return runProtectedMirrorPanel(false);
                }
#endif
#ifdef DPP_WINDOWMANAGER_AUTHORIZED_RECHECK
                if (argc == 2 && [mode isEqual:@"--audit-authorized-recheck"]) {
                    checkRecheckAuthorization();
                    audit();
                    return 0;
                }
                if (argc == 2 && [mode isEqual:@"--prepare-authorized-recheck"])
                    return runAuthorizedWindowManagerRecheck(true);
                if (argc == 3 && [mode isEqual:@"--recheck-windowmanager-after-authorization"] &&
                    std::string(argv[2]) == "--confirm-manual-15s") {
                    std::fputs("authorized_recheck_disabled: Finder permission passed but icons remained "
                               "visible in session AE03B0F0 on 2026-11-14; no system change\n",
                               stderr);
                    return 3;
                }
                if (argc == 2 && [mode isEqual:@"--recheck-self-test"]) {
                    recheckOfflineTest();
                    selfTest();
                    return 0;
                }
                if ([mode isEqual:@"--experiment-finder-restart"] ||
                    [mode isEqual:@"--experiment-finder-owner-exit"] || [mode isEqual:@"--prepare-finder"] ||
                    [mode isEqual:@"--authorize-finder"]) {
                    std::fputs("unrelated mutation/authorization mode unavailable in recheck binary\n",
                               stderr);
                    return 3;
                }
#endif
#ifdef DPP_PROTECTED_MIRROR
                if (argc == 2 && [mode isEqual:@"--mirror-scope-audit"]) {
                    initializeAppKit();
                    return auditProtectedMirrorScope();
                }
                if (argc == 2 &&
                    ([mode isEqual:@"--mirror-finder"] || [mode isEqual:@"--mirror-panel-check"])) {
                    mirrorUIRuntime = true;
                    initializeAppKit();
                    return runProtectedMirrorPanel([mode isEqual:@"--mirror-panel-check"]);
                }
#endif
                if (argc <= 2 && [mode isEqual:@"--help"]) {
                    std::puts("Read-only: --audit, --runtime-check, --observe-readonly, --observe-once. "
                              "Recovery: --recover, --status. Offline: --self-test. "
                              "Preparation only: --prepare, --prepare-click-diagnostic, --prepare-appkit. "
                              "No copies/physics. All four visibility mutation candidates are retired after "
                              "failed human observation; audit/recovery remain available.");
                    std::puts("Finder compatibility (separate backend): --audit-finder, --prepare-finder. "
                              "Manual experiment: --experiment-finder-restart --confirm-finder-restarts. "
                              "May close Finder windows; 15s active, independent 45s session limit.");
                    std::puts("Manual authorization only: --authorize-finder --confirm-permission-prompt. "
                              "Requests consent without sending quit or starting an experiment.");
                    return 0;
                }
                if (argc == 2 && [mode isEqual:@"--audit-finder"]) {
                    auditFinder();
                    return 0;
                }
                if (argc == 3 && [mode isEqual:@"--authorize-finder"] &&
                    std::string(argv[2]) == "--confirm-permission-prompt") {
                    return requestFinderAuthorization();
                }
                if (argc == 2 && [mode isEqual:@"--authorization-info"]) {
                    NSDictionary* info = NSBundle.mainBundle.infoDictionary;
                    need([info[@"CFBundleIdentifier"]
                             isEqual:@"org.desktopphysicsplayground.protected-probe"] &&
                             [info[@"NSAppleEventsUsageDescription"] isKindOfClass:NSString.class] &&
                             [info[@"NSAppleEventsUsageDescription"] length] > 0,
                         "authorization usage description missing");
                    std::puts(
                        "authorization_usage_description_present=true permission_request_performed=false");
                    return 0;
                }
                if ((argc == 2 && [mode isEqual:@"--prepare-finder"]) ||
                    (argc == 3 && [mode isEqual:@"--experiment-finder-restart"] &&
                     std::string(argv[2]) == "--confirm-finder-restarts")) {
                    return runFinderSession([mode isEqual:@"--prepare-finder"]);
                }
                if (argc == 4 && [mode isEqual:@"--experiment-finder-owner-exit"] &&
                    std::string(argv[2]) == "--confirm-finder-restarts" &&
                    std::string(argv[3]) == "--confirm-owner-exit") {
                    return runFinderSession(false, true);
                }
                if (argc == 2 && [mode isEqual:@"--runtime-check"]) {
                    verifyAppKitRuntime();
                    std::printf("appkit_initialized=true run_loop_callback_serviced=true "
                                "screens_have_separate_spaces=%s system_visibility_written=false\n",
                                NSScreen.screensHaveSeparateSpaces ? "true" : "false");
                    return 0;
                }
                if (argc == 2 && ([mode isEqual:@"--observe-readonly"] || [mode isEqual:@"--observe-once"])) {
                    initializeAppKit();
                    observe([mode isEqual:@"--observe-once"]);
                    return 0;
                }
                if (argc == 2 && [mode isEqual:@"--audit"]) {
                    initializeAppKit();
                    audit();
                    return 0;
                }
                if (argc == 2 && [mode isEqual:@"--self-test"]) {
                    selfTest();
                    return 0;
                }
                if (argc == 3 && [mode isEqual:@"--helper"])
                    return helper([NSString stringWithUTF8String:argv[2]]);
                if (argc == 2 && [mode isEqual:@"--recover"]) {
                    Backend backend;
                    return backend.recover() ? 0 : 1;
                }
                if (argc == 2 && [mode isEqual:@"--status"]) {
                    Store store;
                    Guard guard(store.operation.value);
                    auto r = store.read();
                    if (r)
                        std::printf("session=%s phase=%s result=%s\n", [r[@"session_id"] UTF8String],
                                    [r[@"phase"] UTF8String], [r[@"result"] UTF8String]);
                    else
                        std::puts("no journal");
                    return 0;
                }
                // The native preference round trip succeeded but the user's icons remained visible.
                // Keep independent recovery available; do not retry this candidate or start physics.
                if (argc == 2 &&
                    ([mode isEqual:@"--experiment"] || [mode isEqual:@"--diagnostic-app-value"] ||
                     [mode isEqual:@"--diagnostic-click-always"] || [mode isEqual:@"--diagnostic-appkit"])) {
                    std::fputs(
                        "experiment_disabled: primitive, AppValue, click-condition and AppKit candidates "
                        "failed native "
                        "visibility validation on 2026-11-13; no system change\n",
                        stderr);
                    return 3;
                }
                if (argc == 2 &&
                    ([mode isEqual:@"--prepare"] || [mode isEqual:@"--prepare-click-diagnostic"] ||
                     [mode isEqual:@"--prepare-appkit"] || [mode isEqual:@"--diagnostic-appkit"])) {
                    Backend backend;
                    backend.prepareOnly = ![mode isEqual:@"--diagnostic-appkit"];
                    backend.clickDiagnostic = [mode isEqual:@"--prepare-click-diagnostic"];
                    backend.appkitRuntime =
                        [mode isEqual:@"--prepare-appkit"] || [mode isEqual:@"--diagnostic-appkit"];
                    backend.appAPI = backend.clickDiagnostic || backend.appkitRuntime;
                    dpp::SessionCoordinator coordinator(backend);
                    if (!coordinator.start()) {
                        std::fprintf(stderr, "start_failed=%s\n", coordinator.failure());
                        return 1;
                    }
                    const auto activeStarted = nowMs();
                    const char* stopReason = "prepare_only";
                    if (!backend.prepareOnly) {
                        stopReason = "normal_15_second_limit";
                        const auto started = nowMs();
                        while (nowMs() - started < 15000) {
                            if (send(backend.channel.value, "H", 1, 0) != 1) {
                                stopReason = "helper_channel_closed";
                                break;
                            }
                            pollfd stop{STDIN_FILENO, POLLIN, 0};
                            if (isatty(STDIN_FILENO)) {
                                if (poll(&stop, 1, backend.appkitRuntime ? 0 : 250) > 0 &&
                                    (stop.revents & POLLIN)) {
                                    stopReason = "terminal_input";
                                    break;
                                }
                            } else if (!backend.appkitRuntime) {
                                usleep(250000);
                            }
                            if (backend.appkitRuntime) serviceEvents(0.25);
                            int status = 0;
                            if (waitpid(backend.child, &status, WNOHANG) == backend.child) {
                                backend.child = -1;
                                stopReason = "helper_exited";
                                break;
                            }
                        }
                    }
                    std::printf("active_elapsed_ms=%llu stop_reason=%s\n", nowMs() - activeStarted,
                                stopReason);
                    need(coordinator.stop(), "restore failed; recovery journal retained");
                    int status = 0;
                    const auto deadline = nowMs() + 3000;
                    if (backend.child > 0) {
                        pid_t waited;
                        do {
                            waited = waitpid(backend.child, &status, WNOHANG);
                            if (waited == 0) usleep(25000);
                        } while (waited == 0 && nowMs() < deadline);
                        need(waited == backend.child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                             "helper clean shutdown not verified; journal retained");
                    }
                    std::puts("session_closed=true visual_restoration_requires_observation=true");
                    return 0;
                }
                std::fputs("invalid arguments; no system change\n", stderr);
                return 2;
            } catch (const std::exception& e) {
                std::fprintf(stderr, "%s; no visual success claimed\n", e.what());
                return 1;
            }
        } @catch (NSException* e) {
            std::fprintf(stderr, "native exception: %s\n", e.name.UTF8String);
            return 1;
        }
    }
}
