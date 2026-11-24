#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <vector>
namespace dpp {
using SteadyClock = std::chrono::steady_clock;
inline double elapsedMs(SteadyClock::time_point start) {
    return std::chrono::duration<double, std::milli>(SteadyClock::now() - start).count();
}
// Bounded ring. Percentiles are computed only when explicitly requested.
class Samples {
   public:
    void add(double ms) {
        data_[next_] = ms;
        next_ = (next_ + 1) % data_.size();
        count_ = std::min(count_ + 1, data_.size());
    }
    double percentile(double p) const {
        if (!count_) return 0;
        std::vector<double> v(data_.begin(), data_.begin() + static_cast<std::ptrdiff_t>(count_));
        std::sort(v.begin(), v.end());
        return v[static_cast<std::size_t>(p * static_cast<double>(v.size() - 1))];
    }
    std::size_t count() const { return count_; }

   private:
    std::vector<double> data_ = std::vector<double>(36000);  // allocate once; avoid Windows stack overflow
    std::size_t next_ = 0, count_ = 0;
};
struct PerformanceMonitor {
    Samples input, physics, rules, draw, submit, interval;
    std::size_t stalls = 0;
    void frame(double ms) {
        interval.add(ms);
        if (ms > 33.34) ++stalls;
    }
};
}  // namespace dpp
