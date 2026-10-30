#pragma once
#include <cstdint>
#include <limits>
namespace vkr {
// Only negotiated A/V tracks use running PTS. Legacy inputs keep their existing
// latest-frame policy; their PTS may be an unrelated absolute monotonic epoch.
struct AvSchedule {
    enum class Decision { Hold, Present, Drop, Invalid };
    uint64_t presented=0, held=0, late=0, invalid=0, resets=0, skipped=0;
    uint64_t lastSequence=0;unsigned queued=0,maxQueued=0;
    void enqueue(uint64_t sequence,unsigned depth){
        if(lastSequence && sequence>lastSequence+1)skipped+=sequence-lastSequence-1;
        lastSequence=sequence;queued=depth;if(depth>maxQueued)maxQueued=depth;
    }
    int64_t lastErrorNs=0;
    uint64_t generation=0;
    void reset(uint64_t gen) {generation=gen;lastErrorNs=0;lastSequence=0;queued=0;++resets;}
    Decision decide(int64_t pts, uint64_t now, uint64_t base, uint64_t latency,
                    uint64_t lead=0, uint64_t lateLimit=50000000) {
        const auto max=(std::numeric_limits<uint64_t>::max)();
        if(pts<0 || base>max-latency || uint64_t(pts)>max-base-latency || now>max-lead) {
            ++invalid;return Decision::Invalid;
        }
        const uint64_t target=base+latency+uint64_t(pts), effective=now+lead;
        if(target>effective) {++held;lastErrorNs=-(int64_t)((target-effective)>INT64_MAX?INT64_MAX:target-effective);return Decision::Hold;}
        const auto error=effective-target;
        lastErrorNs=(int64_t)(error>INT64_MAX?INT64_MAX:error);
        if(error>lateLimit){++late;return Decision::Drop;}
        ++presented;return Decision::Present;
    }
};
}
