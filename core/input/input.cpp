#include "core/input/input.h"

#include <cmath>

namespace dpp {

void PointerVelocityTracker::addSample(Vec2 pos, double time) {
    if (!pos.isFinite() || !std::isfinite(time)) return;
    // Out-of-order timestamps (clock jumps) invalidate history.
    if (count_ > 0) {
        const Sample& last = samples_[(head_ + kCapacity - 1) % kCapacity];
        if (time < last.time) reset();
    }
    samples_[head_] = Sample{pos, time};
    head_ = (head_ + 1) % kCapacity;
    if (count_ < kCapacity) ++count_;
}

Vec2 PointerVelocityTracker::velocity(double now) const {
    if (count_ < 2) return {};
    const Sample& newest = samples_[(head_ + kCapacity - 1) % kCapacity];
    if (now - newest.time > kStaleSeconds) return {};

    // Least-squares slope of position over time for samples within the window.
    double n = 0, st = 0, stt = 0, sx = 0, sy = 0, stx = 0, sty = 0;
    for (std::size_t k = 0; k < count_; ++k) {
        const Sample& s = samples_[(head_ + kCapacity - 1 - k) % kCapacity];
        const double t = s.time - newest.time;  // <= 0
        if (t < -kWindowSeconds) break;
        n += 1;
        st += t;
        stt += t * t;
        sx += s.pos.x;
        sy += s.pos.y;
        stx += t * s.pos.x;
        sty += t * s.pos.y;
    }
    if (n < 2) return {};
    const double denom = n * stt - st * st;
    if (std::abs(denom) < 1e-12) return {};
    const double vx = (n * stx - st * sx) / denom;
    const double vy = (n * sty - st * sy) / denom;
    if (!std::isfinite(vx) || !std::isfinite(vy)) return {};
    return {static_cast<float>(vx), static_cast<float>(vy)};
}

}  // namespace dpp
