#pragma once
#include <algorithm>
#include <chrono>

namespace vkr {
// Fixed wall-clock cadence: processing consumes the period instead of adding
// to it. A missed deadline discards schedule debt rather than catch-up bursts.
class HeadlessPacer {
public:
    using Clock = std::chrono::steady_clock;
    explicit HeadlessPacer(Clock::time_point now) : next_(now) {}
    Clock::time_point deadline() const { return next_; }
    void advance(Clock::time_point now) {
        next_ = (std::max)(next_ + std::chrono::milliseconds(16), now);
    }
    void reset(Clock::time_point now) { next_ = now; }
private:
    Clock::time_point next_;
};
}
