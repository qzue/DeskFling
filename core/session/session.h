#pragma once
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
namespace dpp {
enum class SessionState { Idle, Probing, Preparing, Active, Restoring, RecoveryFailed };

// Pure state machine for continuous-session (12h) watchdog sleep/wake resilience.
// Distinguishes genuine UI-thread hangs (awake CPU time >= 5000 ms without pulse)
// from OS lid-close / standby / hibernate jumps (where continuous clock advances
// faster than awake clock or owner signals WillSleep/DidWake).
class ContinuousWatchdogClock {
   public:
    static constexpr std::uint64_t kHeartbeatTimeoutMs = 5000ULL;
    static constexpr std::uint64_t kWakeGraceWindowMs = 15000ULL;
    static constexpr std::uint64_t kSleepDivergenceMs = 1000ULL;

    void reset(std::uint64_t continuousMs, std::uint64_t awakeMs) noexcept {
        lastContinuousMs_ = continuousMs;
        lastAwakeMs_ = awakeMs;
        lastPulseAwakeMs_ = awakeMs;
        wakeGraceUntilContinuousMs_ = 0;
        suspended_ = false;
    }

    void observeLoop(std::uint64_t continuousMs, std::uint64_t awakeMs,
                     std::uint64_t expectedPollMs = 500ULL) noexcept {
        const std::uint64_t contDelta =
            continuousMs >= lastContinuousMs_ ? (continuousMs - lastContinuousMs_) : 0ULL;
        const std::uint64_t awakeDelta = awakeMs >= lastAwakeMs_ ? (awakeMs - lastAwakeMs_) : 0ULL;
        lastContinuousMs_ = continuousMs;
        lastAwakeMs_ = awakeMs;

        // Kernel sleep jump: continuous time advanced significantly more than awake time,
        // or the helper process itself was frozen across OS suspend for > 4x its poll interval.
        const bool kernelSlept =
            (contDelta > awakeDelta + kSleepDivergenceMs) ||
            (expectedPollMs > 0ULL && contDelta >= std::max<std::uint64_t>(2500ULL, expectedPollMs * 5ULL) &&
             awakeDelta < contDelta / 2ULL);
        if (kernelSlept) {
            suspended_ = false;
            lastPulseAwakeMs_ = awakeMs;
            wakeGraceUntilContinuousMs_ = continuousMs + kWakeGraceWindowMs;
        }
    }

    void recordPulse(std::uint64_t awakeMs) noexcept {
        lastPulseAwakeMs_ = awakeMs;
        wakeGraceUntilContinuousMs_ = 0;
        suspended_ = false;
    }

    void recordSuspendState(bool enteringSleep, std::uint64_t continuousMs,
                            std::uint64_t awakeMs) noexcept {
        lastContinuousMs_ = continuousMs;
        lastAwakeMs_ = awakeMs;
        lastPulseAwakeMs_ = awakeMs;
        if (enteringSleep) {
            suspended_ = true;
        } else {
            suspended_ = false;
            wakeGraceUntilContinuousMs_ = continuousMs + kWakeGraceWindowMs;
        }
    }

    [[nodiscard]] bool inWakeGrace(std::uint64_t continuousMs) const noexcept {
        return continuousMs < wakeGraceUntilContinuousMs_;
    }

    [[nodiscard]] bool suspended() const noexcept { return suspended_; }

    [[nodiscard]] bool heartbeatTimedOut(std::uint64_t continuousMs,
                                         std::uint64_t awakeMs) const noexcept {
        if (suspended_) {
            // Safety fallback if WillSleep was aborted without DidWake and owner stopped pulsing
            // while the OS remained awake for 20+ seconds.
            return awakeMs >= lastPulseAwakeMs_ && (awakeMs - lastPulseAwakeMs_ >= 20000ULL);
        }
        if (continuousMs < wakeGraceUntilContinuousMs_) {
            return false;
        }
        return awakeMs >= lastPulseAwakeMs_ && (awakeMs - lastPulseAwakeMs_ >= kHeartbeatTimeoutMs);
    }

   private:
    std::uint64_t lastContinuousMs_ = 0;
    std::uint64_t lastAwakeMs_ = 0;
    std::uint64_t lastPulseAwakeMs_ = 0;
    std::uint64_t wakeGraceUntilContinuousMs_ = 0;
    bool suspended_ = false;
};
// Contract for future real desktop adapters. No production adapter is enabled yet.
// Methods returning false may have partially completed: restore must be idempotent.
struct SessionBackend {
    virtual ~SessionBackend() = default;
    virtual bool lock() = 0;
    virtual bool recoverResidual() = 0;
    virtual bool probe() = 0;
    virtual bool snapshot() = 0;
    virtual bool persistIntent() = 0;
    virtual bool prepareScene() = 0;
    virtual bool recoveryReady() = 0;
    virtual bool activate() = 0;
    virtual bool restore() = 0;
    virtual void unlock() noexcept = 0;
};
class SessionCoordinator {
   public:
    explicit SessionCoordinator(SessionBackend& backend) : backend_(backend) {}
    ~SessionCoordinator();
    SessionCoordinator(const SessionCoordinator&) = delete;
    SessionCoordinator& operator=(const SessionCoordinator&) = delete;
    bool start() noexcept;
    bool stop() noexcept;
    SessionState state() const { return state_; }
    const char* failure() const { return failure_; }

   private:
    SessionBackend& backend_;
    SessionState state_ = SessionState::Idle;
    bool locked_ = false;
    const char* failure_ = "";
};
}  // namespace dpp
