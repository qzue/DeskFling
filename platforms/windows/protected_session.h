#pragma once
#include <cstdint>
#include <memory>

#include "core/session/session.h"
#include "session_trace.h"
namespace dpp::winprotected {
// Separate, opt-in visibility experiment / recovery process entry point.
int run(int argc, wchar_t** argv);
// Auto-recovers and archives any residual journal left by a dead owner before GUI startup.
void tryAutoRecoverStaleSession() noexcept;
// UI-thread owner. Resources and an exit control must exist before begin().
class ProtectedSession {
   public:
    ProtectedSession();
    ~ProtectedSession();
    bool begin(std::uintptr_t expectedList, bool startupExperiment = false, bool continuousSession = false);
    bool heartbeat() noexcept;
    void notifyPowerSuspend(bool suspending) noexcept;
    unsigned remainingSeconds() const noexcept;
    bool isContinuous() const noexcept;
    bool stop() noexcept;
    bool active() const noexcept;
    std::optional<SessionTiming> timing() const;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace dpp::winprotected
