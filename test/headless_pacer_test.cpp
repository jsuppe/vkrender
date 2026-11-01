#include "headless_pacer.h"
#include <cstdio>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"failed line %d: %s\n",__LINE__,#x); return 1; } } while(0)
int main() {
    using namespace std::chrono;
    using P = vkr::HeadlessPacer;
    auto zero=P::Clock::time_point{};
    P p(zero);
    CHECK(p.deadline()==zero);
    p.advance(zero);
    CHECK(p.deadline()==zero+nanoseconds(16000000));
    // 11ms of CPU work leaves only 5ms to wait, not another16ms.
    CHECK(p.deadline()-(zero+milliseconds(11))==nanoseconds(5000000));
    p.advance(zero+nanoseconds(16000000));
    CHECK(p.deadline()==zero+nanoseconds(32000000));
    // Long stalls discard missed ticks. No many-frame catch-up loop.
    p.advance(zero+milliseconds(200));
    CHECK(p.deadline()==zero+milliseconds(200));
    p.advance(zero+milliseconds(200));
    CHECK(p.deadline()==zero+milliseconds(200)+nanoseconds(16000000));
    // Leaving display-paced mode starts a fresh offscreen epoch.
    p.reset(zero+seconds(4));
    CHECK(p.deadline()==zero+seconds(4));
    return 0;
}
