#pragma once
#include <cstddef>
namespace dpp::mac {
// This candidate remains unavailable on other devices/OS versions until new validation.
// Scope: local manual observation, session A2791BEC-3FEF-4874-A425-134FA4FF619A.
inline constexpr bool assistedCandidateAvailable() { return false; }
inline constexpr const char* assistedUnavailableReason =
    "本机验证未通过，已停用。本次人工观察中，调整系统设置后原图标仍然存在。";
// Historical state machine retained for diagnostic tests; production entry points gate above it.
// User reports never establish machine-observed visibility or system recovery.
struct AssistedExperiment {
    enum class Phase { Idle, Waiting, Verified, Running, Finished };
    enum class Original { Unknown, Visible, Hidden };
    Phase phase = Phase::Idle;
    Original original = Original::Unknown;
    bool hiddenReported = false, changedReported = false, restoredReported = false;
    std::size_t preparedImages = 0;
    bool prepare(bool explicitConsent, bool valid, Original value) {
        if (!explicitConsent || !valid || phase == Phase::Waiting || phase == Phase::Running) return false;
        original = value;
        hiddenReported = changedReported = restoredReported = false;
        preparedImages = 3;
        phase = Phase::Waiting;
        return true;
    }
    bool verify(bool machinePrerequisites, bool hidden, bool unchangedLayout, bool changed) {
        if (phase != Phase::Waiting) return false;
        changedReported = changed;
        hiddenReported = hidden;
        if (!machinePrerequisites || !hidden || !unchangedLayout) {
            finish();
            return false;
        }
        phase = Phase::Verified;
        return true;
    }
    bool start() {
        if (phase != Phase::Verified || !hiddenReported) return false;
        preparedImages = 0;
        phase = Phase::Running;
        return true;
    }
    void finish() {
        preparedImages = 0;
        phase = Phase::Finished;
    }
    bool restoreRequired() const { return changedReported; }
    bool confirmRestored() {
        if (original == Original::Unknown || phase != Phase::Finished || !changedReported) return false;
        return restoredReported = true;
    }
    bool capturesInput() const { return false; }  // preparation never owns an input path
};
}  // namespace dpp::mac
