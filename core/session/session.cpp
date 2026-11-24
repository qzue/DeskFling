#include "core/session/session.h"
namespace dpp {
SessionCoordinator::~SessionCoordinator() { stop(); }
bool SessionCoordinator::start() noexcept {
    if (state_ != SessionState::Idle) return false;
    try {
        failure_ = "single instance";
        if (!(locked_ = backend_.lock())) return false;
        state_ = SessionState::Probing;
        failure_ = "residual recovery";
        if (!backend_.recoverResidual()) {
            state_ = SessionState::RecoveryFailed;
            return false;
        }
        failure_ = "capability/permission";
        if (!backend_.probe()) {
            stop();
            return false;
        }
        state_ = SessionState::Preparing;
        failure_ = "snapshot";
        if (!backend_.snapshot()) {
            stop();
            return false;
        }
        failure_ = "persistent intent";
        if (!backend_.persistIntent()) {
            stop();
            return false;
        }
        failure_ = "scene resources";
        if (!backend_.prepareScene()) {
            stop();
            return false;
        }
        failure_ = "recovery readiness";
        if (!backend_.recoveryReady()) {
            stop();
            return false;
        }
        failure_ = "activation";
        if (!backend_.activate()) {
            stop();
            return false;
        }
        state_ = SessionState::Active;
        failure_ = "";
        return true;
    } catch (...) {
        stop();
        return false;
    }
}
bool SessionCoordinator::stop() noexcept {
    if (!locked_) return state_ == SessionState::Idle;
    state_ = SessionState::Restoring;
    try {
        if (!backend_.restore()) {
            state_ = SessionState::RecoveryFailed;
            return false;
        }
    } catch (...) {
        state_ = SessionState::RecoveryFailed;
        return false;
    }
    backend_.unlock();
    locked_ = false;
    state_ = SessionState::Idle;
    return true;
}
}  // namespace dpp
