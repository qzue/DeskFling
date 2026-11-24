#include <gtest/gtest.h>
#include <windows.h>

#include <chrono>

#include "platforms/windows/frame_timer.h"

namespace {
class WindowsFrameTimer : public ::testing::Test {
   protected:
    HWND window = nullptr;
    dpp::FrameTimer timer;
    unsigned ticks = 0;

    static LRESULT CALLBACK procedure(HWND w, UINT msg, WPARAM wp, LPARAM lp) noexcept {
        auto* self = reinterpret_cast<WindowsFrameTimer*>(GetWindowLongPtrW(w, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            self = static_cast<WindowsFrameTimer*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self && msg == WM_TIMER) {
            if (self->timer.accepts(wp)) ++self->ticks;
            return 0;
        }
        return DefWindowProcW(w, msg, wp, lp);
    }
    void SetUp() override {
        WNDCLASSW wc{};
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = procedure;
        wc.lpszClassName = L"DPPFrameTimerTest";
        ASSERT_NE(RegisterClassW(&wc), 0);
        window = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance,
                                 this);
        ASSERT_NE(window, nullptr);
    }
    void TearDown() override {
        if (window) {
            timer.stop(window);
            DestroyWindow(window);
        }
        UnregisterClassW(L"DPPFrameTimerTest", GetModuleHandleW(nullptr));
    }
    void pump(unsigned milliseconds, UINT repeatInterval = 0) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
        while (std::chrono::steady_clock::now() < end) {
            if (repeatInterval) timer.set(window, repeatInterval);
            MSG msg{};
            while (PeekMessageW(&msg, window, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
            // Bounded test-only wait, no real input or desktop access.
            MsgWaitForMultipleObjectsEx(0, nullptr, 5, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
    }
};

TEST_F(WindowsFrameTimer, RepeatedRequestsDoNotPostponeTicks) {
    timer.set(window, dpp::FrameTimer::idleInterval);
    pump(1200, dpp::FrameTimer::idleInterval);
    // A resetting implementation starves the timer while requests continue.
    EXPECT_GE(ticks, 2u);
}

TEST_F(WindowsFrameTimer, WakeStopAndRestart) {
    timer.set(window, dpp::FrameTimer::idleInterval);
    timer.set(window, dpp::FrameTimer::activeInterval);
    pump(300);
    EXPECT_GT(ticks, 0u);
    timer.stop(window);
    const auto stopped = ticks;
    // Simulate an already queued message after focus loss. It must be ignored.
    ASSERT_TRUE(PostMessageW(window, WM_TIMER, dpp::FrameTimer::id, 0));
    pump(250);
    EXPECT_EQ(ticks, stopped);
    EXPECT_FALSE(timer.accepts(dpp::FrameTimer::id));
    timer.set(window, dpp::FrameTimer::activeInterval);
    EXPECT_FALSE(timer.accepts(dpp::FrameTimer::id + 1));
    pump(300);
    EXPECT_GT(ticks, stopped);
}
}  // namespace
