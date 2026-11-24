#pragma once
#include <optional>

#include "core/scene/types.h"

namespace dpp::winmirror {
// Per-session button state. Focus release is temporary; explicit disable persists.
// Native callers validate protection, display generation and timestamps before claiming a hit.
class DirectInput {
   public:
    bool enabled() const { return !disabled_; }
    void enable() { disabled_ = false; }
    void disable() {
        disabled_ = true;
        cancel();
    }
    void cancel() { left_ = right_ = false; }
    void up(bool right) { (right ? right_ : left_) = false; }
    bool claim(TextureId hit, bool right, bool allowed, uint32_t eventTime) {
        bool& down = right ? right_ : left_;
        auto& last = right ? lastRight_ : lastLeft_;
        if (disabled_ || !allowed || hit == kNoTexture || down || (last && *last == eventTime)) return false;
        last = eventTime;
        down = true;
        return true;
    }

   private:
    bool disabled_ = false, left_ = false, right_ = false;
    std::optional<uint32_t> lastLeft_, lastRight_;
};
}  // namespace dpp::winmirror
