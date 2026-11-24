#pragma once
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace dpp::winprotected {
struct SessionTiming {
    std::string id;
    uint64_t started = 0, ownerDeadline = 0, helperDeadline = 0;
    int state = 0;
};
// In-memory, bounded low-frequency operations; flushed with the existing report.
class SessionTrace {
    std::optional<SessionTiming> initial_;
    std::vector<std::string> events_;
    uint64_t sequence_ = 0, dropped_ = 0;
public:
    void record(const std::string& preview, const char* operation, uint64_t now,
                const std::optional<SessionTiming>& current, uintptr_t request = 0, int result = -1) {
        ++sequence_;
        if (current && !initial_) initial_ = current;
        if (events_.size() >= 256) { ++dropped_; return; }
        std::ostringstream o;
        o << "{\"sequence\":" << sequence_ << ",\"monotonic_ms\":" << now
          << ",\"preview_session_id\":\"" << preview << "\",\"operation\":\"" << operation
          << "\",\"request\":" << request << ",\"result\":" << result << ",\"protection\":";
        if (!current) o << "null";
        else o << "{\"session_id\":\"" << current->id << "\",\"started_ms\":" << current->started
               << ",\"initial_owner_deadline_ms\":" << initial_->ownerDeadline
               << ",\"initial_helper_deadline_ms\":" << initial_->helperDeadline
               << ",\"current_owner_deadline_ms\":" << current->ownerDeadline
               << ",\"current_helper_deadline_ms\":" << current->helperDeadline
               << ",\"state\":" << current->state << "}";
        o << '}'; events_.push_back(o.str());
    }
    std::string json() const {
        std::ostringstream o;
        o << "{\"preview_identity_basis\":\"existing snapshot UUID; one MarkerWindow per snapshot\",\"clock\":\"GetTickCount64 milliseconds since system boot including sleep; not UTC\",\"dropped\":"
          << dropped_ << ",\"events\":[";
        for (size_t i=0; i<events_.size(); ++i) { if (i) o << ','; o << events_[i]; }
        o << "]}"; return o.str();
    }
};
}
