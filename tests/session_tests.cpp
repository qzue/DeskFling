#include <gtest/gtest.h>

#include "core/session/session.h"
using namespace dpp;
struct MockBackend : SessionBackend {
    int fail = 0, call = 0, restores = 0, unlocks = 0;
    bool restoreOk = true, throwActivation = false;
    bool next() { return ++call != fail; }
    bool lock() override { return next(); }
    bool recoverResidual() override { return next(); }
    bool probe() override { return next(); }
    bool snapshot() override { return next(); }
    bool persistIntent() override { return next(); }
    bool prepareScene() override { return next(); }
    bool recoveryReady() override { return next(); }
    bool activate() override {
        if (throwActivation) throw 42;
        return next();
    }
    bool restore() override {
        ++restores;
        return restoreOk;
    }
    void unlock() noexcept override { ++unlocks; }
};
TEST(Session, EveryStartupStageCanFail) {
    for (int i = 1; i <= 8; ++i) {
        MockBackend b;
        b.fail = i;
        SessionCoordinator s(b);
        EXPECT_FALSE(s.start()) << i;
        if (i == 2)
            EXPECT_EQ(s.state(), SessionState::RecoveryFailed);
        else
            EXPECT_EQ(s.state(), SessionState::Idle);
        EXPECT_TRUE(s.stop());
        EXPECT_LE(b.unlocks, 1);
    }
}
TEST(Session, RestoreIsIdempotentAndNoSecondOwner) {
    MockBackend b;
    SessionCoordinator s(b);
    ASSERT_TRUE(s.start());
    EXPECT_FALSE(s.start());
    EXPECT_TRUE(s.stop());
    EXPECT_TRUE(s.stop());
    EXPECT_EQ(b.restores, 1);
    EXPECT_EQ(b.unlocks, 1);
}
TEST(Session, FailedRestoreIsVisibleAndRetryable) {
    MockBackend b;
    SessionCoordinator s(b);
    ASSERT_TRUE(s.start());
    b.restoreOk = false;
    EXPECT_FALSE(s.stop());
    EXPECT_EQ(s.state(), SessionState::RecoveryFailed);
    EXPECT_EQ(b.unlocks, 0);
    EXPECT_FALSE(s.start());
    b.restoreOk = true;
    EXPECT_TRUE(s.stop());
    EXPECT_EQ(s.state(), SessionState::Idle);
}
TEST(Session, ExceptionDoesNotEscapeCallbackBoundary) {
    MockBackend b;
    b.throwActivation = true;
    SessionCoordinator s(b);
    EXPECT_FALSE(s.start());
    EXPECT_EQ(s.state(), SessionState::Idle);
    EXPECT_EQ(b.restores, 1);
}

#include "platforms/windows/session_trace.h"
TEST(SessionTrace, RealAssociationSequenceAndAuthoritativeDeadline) {
    using namespace dpp::winprotected;
    SessionTrace trace;
    trace.record("preview-actual", "preview", 100, std::nullopt);
    SessionTiming state{"backend-nonce", 101, 178101, 180101, 3};
    trace.record("preview-actual", "start", 102, state);
    state.ownerDeadline = 178099;
    trace.record("preview-actual", "open", 103, state, 7, 1);
    const auto json = trace.json();
    EXPECT_NE(json.find("\"protection\":null"), std::string::npos);
    EXPECT_NE(json.find("backend-nonce"), std::string::npos);
    EXPECT_NE(json.find("\"sequence\":3"), std::string::npos);
    EXPECT_NE(json.find("\"initial_owner_deadline_ms\":178101"), std::string::npos);
    EXPECT_NE(json.find("\"current_owner_deadline_ms\":178099"), std::string::npos);
    for (int i=0;i<300;++i) trace.record("preview-actual", "pause", 104+i,state);
    EXPECT_NE(trace.json().find("\"dropped\":47"),std::string::npos);
}

TEST(ContinuousWatchdog, DistinguishesKernelSleepJumpAndExplicitSuspendFromRealUiHang) {
    ContinuousWatchdogClock wd;
    wd.reset(1000, 1000);

    // Normal sub-second heartbeats keep session alive.
    wd.observeLoop(2000, 2000, 500);
    wd.recordPulse(2000);
    wd.observeLoop(4500, 4500, 500);
    EXPECT_FALSE(wd.heartbeatTimedOut(4500, 4500));

    // 1. Kernel sleep/lid-close jump: continuous clock jumps +2 hours (7,200,000 ms),
    // while awake CPU clock advances only +120 ms! Must NOT false-trigger recovery,
    // and must grant a 15-second post-wake grace window for GPU/display wake.
    wd.observeLoop(4500 + 7200000ull, 4500 + 120ull, 500);
    EXPECT_TRUE(wd.inWakeGrace(4500 + 7200000ull));
    EXPECT_FALSE(wd.heartbeatTimedOut(4500 + 7200000ull, 4500 + 120ull));
    wd.observeLoop(4500 + 7210000ull, 4620 + 10000ull, 500);
    EXPECT_FALSE(wd.heartbeatTimedOut(4500 + 7210000ull, 4620 + 10000ull));

    // Heartbeat resumes after wake -> clears grace window and arms normal 5s awake watchdog again.
    wd.observeLoop(4500 + 7216000ull, 4620 + 16000ull, 500);
    wd.recordPulse(4620 + 16000ull);
    EXPECT_FALSE(wd.inWakeGrace(4500 + 7219000ull));
    wd.observeLoop(4500 + 7219000ull, 4620 + 19000ull, 500);
    EXPECT_FALSE(wd.heartbeatTimedOut(4500 + 7219000ull, 4620 + 19000ull));
    // Real UI hang while CPU is wide awake (>= 5000 ms on awake clock) MUST trigger recovery!
    wd.observeLoop(4500 + 7221500ull, 4620 + 21500ull, 500);
    EXPECT_TRUE(wd.heartbeatTimedOut(4500 + 7221500ull, 4620 + 21500ull));

    // 2. Explicit owner sleep ('Z') / wake ('W') notifications + aborted-sleep safety bound.
    wd.reset(10000, 10000);
    wd.recordSuspendState(/*enteringSleep=*/true, 12000, 12000);
    EXPECT_TRUE(wd.suspended());
    // System sleeps 30 minutes with minimal awake time -> does not expire.
    EXPECT_FALSE(wd.heartbeatTimedOut(1812000, 12400));
    wd.recordSuspendState(/*enteringSleep=*/false, 1812500, 12900);
    EXPECT_FALSE(wd.suspended());
    EXPECT_TRUE(wd.inWakeGrace(1820000));
    EXPECT_FALSE(wd.heartbeatTimedOut(1820000, 20400));

    // If owner sent sleep ('Z') but system aborted sleep and stayed awake >= 20s without pulse -> recover!
    wd.recordSuspendState(/*enteringSleep=*/true, 1821000, 21400);
    EXPECT_TRUE(wd.heartbeatTimedOut(1845000, 45400));
}
