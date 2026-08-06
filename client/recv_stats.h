// M23: shared receive-pipeline instrumentation for the cast receivers.
//
// One measurement model on every platform (it is all the same GStreamer):
//
//   behind-live(probe) = pipeline_clock_now - (base_time + buffer_PTS)
//
// For a live WebRTC/RTSP source the buffer PTS is the stream's running time,
// so behind-live at a probe point is exactly how far that frame is behind
// real time at that stage: at the webrtcbin src pad it is jitterbuffer
// residence (the `latency` property, default 200 ms, dominates here); at the
// sink/appsink it adds depay+decode+convert; the delta between two probes is
// the stage's residence. No sender clock sync required.
//
// Drops are counted BY CAUSE, because "dropped frames" conflates three very
// different things:
//   - qdrop: appsink queue overflow (drop=true) - the consumer is slower
//     than the producer; unhealthy under steady state.
//   - ringdrop: producer handoff starvation (vkrDequeueBuffer timeout /
//     codec input stall) - the renderer side is not consuming; unhealthy.
//   - gap: upstream PTS discontinuity > 1.5 frame periods - jitterbuffer
//     loss or a sender pause; network-side.
// The renderer's latest-wins coalescing (adoptions vs presents in the
// existing renderer logs) is deliberate freshness, not loss - compare these
// counters against those logs when attributing "missing" frames.
//
// Header-only; GLib + GStreamer only; used by the Windows/Linux/Android
// receivers. Emit cadence: report() every kReportNs per stream, one line,
// grep/parse-friendly, prefixed `recv-stats:`.
#pragma once

#include <gst/gst.h>

#include <atomic>
#include <cstdint>
#include <cstdio>

namespace vkr {

// Where a stage probe sits in the receive pipeline.
enum RecvStage {
    kStageIngress = 0, // source element src pad (post-jitterbuffer for WebRTC)
    kStageSink = 1,    // terminal sink's sink pad (decoded + converted)
    kStageCount
};

struct RecvStageAcc {
    std::atomic<uint64_t> count{0};
    std::atomic<int64_t> behindSumNs{0};
    std::atomic<int64_t> behindMaxNs{0};
};

class RecvStats {
  public:
    // `pipeline` is borrowed (not reffed): it must outlive this object.
    // `logFn` lets Android route lines to logcat; default is stdout.
    void init(GstElement* pipeline, unsigned sessionId,
              void (*logFn)(const char*) = nullptr) {
        pipeline_ = pipeline;
        sessionId_ = sessionId;
        logFn_ = logFn;
        windowStartNs_ = monotonicNs();
    }

    // Attach a behind-live probe to `pad` for `stage`. Call once per stage.
    void attachProbe(GstPad* pad, RecvStage stage) {
        auto* ctx = new ProbeCtx{this, stage};
        gst_pad_add_probe(
            pad, GST_PAD_PROBE_TYPE_BUFFER,
            [](GstPad*, GstPadProbeInfo* info, gpointer data) -> GstPadProbeReturn {
                auto* c = static_cast<ProbeCtx*>(data);
                GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info);
                if (buf) c->stats->onBuffer(c->stage, buf);
                return GST_PAD_PROBE_OK;
            },
            ctx, [](gpointer data) { delete static_cast<ProbeCtx*>(data); });
    }

    // Producer-handoff accounting (call from the appsink pull path).
    void onPulled() { pulled_.fetch_add(1, std::memory_order_relaxed); }
    void onRingDrop() { ringDrops_.fetch_add(1, std::memory_order_relaxed); }

    // Emit a report line if the window elapsed. Cheap; call from any
    // per-frame path (or a poll loop).
    void maybeReport() {
        int64_t now = monotonicNs();
        int64_t start = windowStartNs_.load(std::memory_order_relaxed);
        if (now - start < kReportNs) return;
        if (!windowStartNs_.compare_exchange_strong(start, now)) return; // racer reports
        report(now - start);
    }

  private:
    struct ProbeCtx {
        RecvStats* stats;
        RecvStage stage;
    };

    static int64_t monotonicNs() { return (int64_t)g_get_monotonic_time() * 1000; }

    void onBuffer(RecvStage stage, GstBuffer* buf) {
        RecvStageAcc& acc = stages_[stage];
        acc.count.fetch_add(1, std::memory_order_relaxed);

        GstClockTime pts = GST_BUFFER_PTS(buf);
        if (pipeline_ && GST_CLOCK_TIME_IS_VALID(pts)) {
            GstClock* clock = gst_element_get_clock(pipeline_);
            if (clock) {
                GstClockTime now = gst_clock_get_time(clock);
                GstClockTime base = gst_element_get_base_time(pipeline_);
                gst_object_unref(clock);
                if (GST_CLOCK_TIME_IS_VALID(base) && now > base) {
                    int64_t behind = (int64_t)(now - base) - (int64_t)pts;
                    int64_t prevMax = acc.behindMaxNs.load(std::memory_order_relaxed);
                    while (behind > prevMax &&
                           !acc.behindMaxNs.compare_exchange_weak(prevMax, behind)) {
                    }
                    acc.behindSumNs.fetch_add(behind, std::memory_order_relaxed);
                }
            }
        }

        // Upstream gap detection at the ingress probe: a PTS jump of more
        // than 1.5 frame periods (vs the observed median cadence) means the
        // jitterbuffer gave up on packets or the sender paused.
        if (stage == kStageIngress && GST_CLOCK_TIME_IS_VALID(pts)) {
            int64_t last = lastIngressPtsNs_.exchange((int64_t)pts,
                                                      std::memory_order_relaxed);
            if (last >= 0) {
                int64_t delta = (int64_t)pts - last;
                int64_t period = framePeriodNs_.load(std::memory_order_relaxed);
                if (period > 0 && delta > period + period / 2) {
                    gaps_.fetch_add(1, std::memory_order_relaxed);
                } else if (delta > 0) {
                    // EWMA of the cadence; initializes on the first delta.
                    framePeriodNs_.store(period > 0 ? (period * 7 + delta) / 8 : delta,
                                         std::memory_order_relaxed);
                }
            }
        }

        if (stage == kStageSink) maybeReport();
    }

    void report(int64_t elapsedNs) {
        char line[512];
        double secs = (double)elapsedNs / 1e9;
        uint64_t in = stages_[kStageIngress].count.exchange(0);
        int64_t inSum = stages_[kStageIngress].behindSumNs.exchange(0);
        int64_t inMax = stages_[kStageIngress].behindMaxNs.exchange(0);
        uint64_t sk = stages_[kStageSink].count.exchange(0);
        int64_t skSum = stages_[kStageSink].behindSumNs.exchange(0);
        int64_t skMax = stages_[kStageSink].behindMaxNs.exchange(0);
        uint64_t pulled = pulled_.exchange(0);
        uint64_t ringDrops = ringDrops_.exchange(0);
        uint64_t gaps = gaps_.exchange(0);

        // qdrop: frames that reached the sink pad but were never pulled
        // (appsink drop=true overflow). Slight undercount while frames sit
        // in the queue; exact over a steady window.
        uint64_t qdrop = (pulled && sk > pulled) ? sk - pulled : 0;

        // NOTE: the ingress probe sits on the source's RTP-level pad, so its
        // count is PACKETS (content-dependent: a noisy 640x360 frame is 20+
        // packets, a flat one 1-2); the sink probe counts decoded FRAMES.
        snprintf(line, sizeof(line),
                 "recv-stats: [session %u] %.1fs pkts=%llu frames=%llu (%.1f fps) "
                 "behind_ms ingress=%.1f/%.1f sink=%.1f/%.1f (avg/max) "
                 "stage_ms=%.1f pulled=%llu qdrop=%llu ringdrop=%llu gap=%llu",
                 sessionId_, secs, (unsigned long long)in, (unsigned long long)sk,
                 secs > 0 ? sk / secs : 0.0, in ? inSum / 1e6 / in : 0.0, inMax / 1e6,
                 sk ? skSum / 1e6 / sk : 0.0, skMax / 1e6,
                 (in && sk) ? (skSum / (double)sk - inSum / (double)in) / 1e6 : 0.0,
                 (unsigned long long)pulled, (unsigned long long)qdrop,
                 (unsigned long long)ringDrops, (unsigned long long)gaps);
        if (logFn_) {
            logFn_(line);
        } else {
            printf("%s\n", line);
        }
    }

    static constexpr int64_t kReportNs = 5'000'000'000; // 5s windows

    GstElement* pipeline_ = nullptr;
    unsigned sessionId_ = 0;
    void (*logFn_)(const char*) = nullptr;

    RecvStageAcc stages_[kStageCount];
    std::atomic<uint64_t> pulled_{0};
    std::atomic<uint64_t> ringDrops_{0};
    std::atomic<uint64_t> gaps_{0};
    std::atomic<int64_t> lastIngressPtsNs_{-1};
    std::atomic<int64_t> framePeriodNs_{0};
    std::atomic<int64_t> windowStartNs_{0};
};

} // namespace vkr
