#pragma once

#include <cstdint>
#include <string>

struct AVFormatContext;
struct AVBSFContext;
struct AMediaCodec;
struct ANativeWindow;

namespace vkr {

// M8 Android decoder: RTSP demux (FFmpeg avformat) -> AMediaCodec decoding
// directly INTO a renderer-provided Surface. BufferQueue carries the frames
// cross-process (zero-copy, fences and timestamps included) - this class has
// no frame IPC, no buffer ring, and no fence handling of its own.
//
// releaseOutputBuffer(render=true) queues each frame with its MediaCodec
// presentation timestamp; backpressure is BufferQueue's: when the consumer
// holds all buffers, dequeueOutputBuffer starves and decode naturally stalls.
class SurfaceDecoder {
public:
    // Blocks for one session: RTSP failures reconnect internally (capped
    // backoff, wall-clock watchdog); returns only when the surface becomes
    // unusable (renderer died) or stop() is observed.
    int run(const std::string& url, ANativeWindow* window);

private:
    bool openStream(const std::string& url, ANativeWindow* window);
    void closeStream();
    // Returns false when the surface/codec is dead (ends the session).
    bool drainDecoder();

    AVFormatContext* fmt_ = nullptr;
    AVBSFContext* bsf_ = nullptr;
    int videoStream_ = -1;
    AMediaCodec* codec_ = nullptr;
    int64_t readDeadlineSec_ = 0;
    bool surfaceDead_ = false;

    uint64_t queuedPackets_ = 0, decodedFrames_ = 0;
};

} // namespace vkr
