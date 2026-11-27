#pragma once
#include <cstdint>
#include <limits>
#include <cstring>
#include <stdexcept>
namespace vkr {
// Only negotiated A/V tracks use running PTS. Legacy inputs keep their existing
// latest-frame policy; their PTS may be an unrelated absolute monotonic epoch.
// Each poll drains every frame already announced, without blocking, into the
// pending queue until it is full or the channel has none (6a, 2026-11-27). A
// vsync-paced render loop runs at the display rate, so a single attempt per
// iteration let FrameLink's one-frame mailbox replace a frame whenever two
// source frames arrived within one iteration. The pending queue's cap is
// unchanged, so neither the timed buffering nor ring ownership grows.
// Timed buffering and transport ownership are different bounds. Six future
// frames at 60fps represent 100ms; increasing storage must NOT increase that
// queue or change either A/V deadline. Extra slots cover retained current,
// older GPU reads, ordered retirement lag, the IPC mailbox and producer writer.
// Thirteen is a conservative normal-cadence operating envelope, not a guarantee
// against arbitrary GPU stalls or latest-wins replacement during catch-up bursts.
// The IPC mailbox holds two unread frames (flSetReceiveQueue): with one, a render
// loop paced at the display rate lost a frame whenever two source frames arrived
// within one iteration (6a, 2026-11-27: Linux FIFO at 60 Hz, 5 of 8 runs).
struct AvRingBudget {
    static constexpr unsigned pendingSlots=6;
    static constexpr unsigned currentSlots=1, priorGpuSlots=2,
                              retirementSlots=1, mailboxSlots=2, writerSlots=1;
    static constexpr unsigned ringSlots=pendingSlots+currentSlots+priorGpuSlots+
                                        retirementSlots+mailboxSlots+writerSlots;
    // Controlled same-binary capacity experiment; not a lossless-delivery mode.
    static unsigned testRingSlots(const char* value) {
        if(!value || !*value || std::strcmp(value,"13")==0)return ringSlots;
        if(std::strcmp(value,"12")==0)return 12; // the one-frame-mailbox budget, before 2026-11-27
        if(std::strcmp(value,"8")==0)return 8;
        throw std::invalid_argument("VKR_TEST_AV_POOL_SLOTS must be 8, 12 or 13");
    }
    static uint64_t maxLatencyNs(uint32_t fpsNum,uint32_t fpsDen) {
        if(!fpsNum || !fpsDen)return 0;
        const uint64_t scaled=1000000000ull*fpsDen;
        const uint64_t q=scaled/fpsNum,r=scaled%fpsNum;
        const auto max=(std::numeric_limits<uint64_t>::max)();
        const uint64_t tail=r*pendingSlots/fpsNum;
        return q>(max-tail)/pendingSlots ? max : q*pendingSlots+tail;
    }
};
struct AvAcquireBudget {
    // acquire() takes one frame into the queue and returns true, or returns
    // false when none is available (or the channel failed).
    template<class Acquire> unsigned poll(unsigned depth, unsigned capacity, Acquire acquire) {
        unsigned taken=0;
        while(depth+taken<capacity && acquire())++taken;
        return taken;
    }
};
struct AvSchedule {
    enum class Decision { Hold, Present, Drop, Invalid };
    uint64_t importFailures=0;
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
