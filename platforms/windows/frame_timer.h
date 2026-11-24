#pragma once

#include <windows.h>

#include <stdexcept>

namespace dpp {
// One UI-thread timer per sandbox window. Replacing a Win32 timer restarts its
// deadline, so repeated input and ticks must leave an unchanged interval alone.
class FrameTimer {
   public:
    static constexpr UINT_PTR id = 1;
    static constexpr UINT fastArenaInterval = 8;
    static constexpr UINT activeInterval = 15;
    static constexpr UINT idleInterval = 200;

    void set(HWND window, UINT interval) {
        if (interval_ == interval) return;
        if (!SetTimer(window, id, interval, nullptr)) throw std::runtime_error("Frame timer creation failed");
        interval_ = interval;
    }
    void stop(HWND window) noexcept {
        KillTimer(window, id);
        interval_ = 0;
    }
    bool accepts(WPARAM timerId) const noexcept { return interval_ != 0 && timerId == id; }

   private:
    UINT interval_ = 0;
};
}  // namespace dpp
