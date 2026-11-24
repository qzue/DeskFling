#pragma once
#include <cstddef>
#include <cstdint>
namespace dpp::mac {
enum class Permission { Unknown, Granted, NeedsConsent, NotPermitted, Timeout, TargetUnavailable, Failed };
inline Permission permissionFromStatus(int status) {
    switch (status) {
        case 0:
            return Permission::Granted;
        case -1744:
            return Permission::NeedsConsent;
        case -1743:
            return Permission::NotPermitted;
        case -1712:
            return Permission::Timeout;
        case -600:
        case -609:
            return Permission::TargetUnavailable;
        default:
            return Permission::Failed;
    }
}
enum class AccessOperation { Check, Request, Read };
// Main-thread state; no platform objects, IO, permission calls or windows in this state machine.
// Closing invalidates delivery but does NOT cancel the outstanding system operation or free its slot.
struct DesktopAccessState {
    Permission permission = Permission::Unknown;
    int rawStatus = 0;
    bool hasStatus = false, busy = false, open = false, exiting = false;
    bool coordinateValidated = false, previewActive = false, previewEverOpened = false;
    bool alignmentAccepted = false;
    std::size_t parsed = 0, failed = 0;
    std::uint64_t generation = 0, pending = 0;
    void show() {
        if (!exiting) open = true;
    }
    void stopPreview() { previewActive = false; }
    void invalidateLayout() {
        stopPreview();
        parsed = failed = 0;
        coordinateValidated = alignmentAccepted = false;
        previewEverOpened = false;
    }
    void close() {
        open = false;
        ++generation;
        invalidateLayout();
    }
    void shutdown() {
        close();
        exiting = true;
    }
    std::uint64_t begin(AccessOperation operation, bool explicitConsent = false) {
        if (!open || exiting || busy || (operation == AccessOperation::Request && !explicitConsent) ||
            (operation == AccessOperation::Read && permission != Permission::Granted))
            return 0;
        invalidateLayout();
        busy = true;
        pending = ++generation;
        return pending;
    }
    bool finish(std::uint64_t token, int status) {
        if (!busy || token != pending) return false;
        busy = false;
        if (token != generation || !open || exiting) return false;
        hasStatus = true;
        rawStatus = status;
        permission = permissionFromStatus(status);
        return true;
    }
    bool canPreview() const {
        return open && !exiting && !busy && permission == Permission::Granted && parsed >= 2 && failed == 0 &&
               coordinateValidated;
    }
    bool startPreview() {
        if (!canPreview()) return false;
        previewActive = previewEverOpened = true;
        alignmentAccepted = false;
        return true;
    }
    bool acceptAlignment() {
        if (!previewActive || !canPreview()) return false;
        alignmentAccepted = true;
        return true;
    }
};
}  // namespace dpp::mac
