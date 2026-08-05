#pragma once

// M18: feeds AirPlay-mirror H.264 access units (annex-b, SPS/PPS in-band)
// into AMediaCodec decoding straight into the renderer-provided Surface -
// the same decode tail as the M9 GStreamer client, minus the transport.

#include <cstdint>
#include <mutex>
#include <string>

struct AMediaCodec;
struct ANativeWindow;

namespace vkr {

class AirplayVideoSink {
public:
    // window is borrowed; the caller keeps it alive for the sink's lifetime.
    bool start(ANativeWindow* window);
    void feed(const uint8_t* data, int len, int64_t ptsUs);
    void flush();
    void stop();

    bool surfaceDead() const { return surfaceDead_; }
    uint64_t frames() const { return decodedFrames_; }

private:
    bool ensureCodec();
    bool drainDecoder();

    std::mutex mutex_; // raop delivers from its own thread
    ANativeWindow* window_ = nullptr;
    AMediaCodec* codec_ = nullptr;
    bool surfaceDead_ = false;
    uint64_t decodedFrames_ = 0;
    uint64_t queued_ = 0;
};

} // namespace vkr
