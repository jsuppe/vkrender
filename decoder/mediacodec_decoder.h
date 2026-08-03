#pragma once

#include <cstdint>
#include <deque>
#include <set>
#include <string>

#include "ipc.h"
#include "protocol.h"

struct AVFormatContext;
struct AVBSFContext;
struct AMediaCodec;
struct AImageReader;
struct AImage;
struct AHardwareBuffer;

namespace vkr {

// Android backend: RTSP demux (FFmpeg avformat, no avcodec decoding) ->
// AMediaCodec hardware decode -> AImageReader surface -> AHardwareBuffer per
// frame, shared zero-copy with the renderer process over the Unix socket via
// AHardwareBuffer_sendHandleToUnixSocket.
//
// Buffer identity: AImageReader cycles a fixed set of gralloc buffers; the
// AHardwareBuffer* pointer is stable per underlying buffer, so its value is
// the surface key. Each distinct buffer is announced once (SurfaceDesc with
// numObjects == 0, followed by the AHB handle out-of-band on the socket).
//
// Lifetime: acquired AImages are held until the renderer's FrameConsumed
// releases them (AImage_delete returns the buffer to the codec). The held
// ring is capped like the other backends; frames are dropped when full.
class MediaCodecDecoder {
public:
    MediaCodecDecoder(const proto::HelloMsg& hello, uint32_t maxHeldFrames);
    ~MediaCodecDecoder();

    int run(const std::string& url, IpcChannel& ch);

private:
    bool openStream(const std::string& url);
    void closeStream();
    bool drainDecoder(IpcChannel& ch);
    bool submitImage(AImage* image, int64_t ptsUs, int acquireFence, IpcChannel& ch);
    bool pollRenderer(IpcChannel& ch);
    bool backoffWait(IpcChannel& ch, uint32_t ms);
    void releaseConsumed(uint64_t consumedValue);

    uint32_t maxHeld_;

    AVFormatContext* fmt_ = nullptr;
    AVBSFContext* bsf_ = nullptr; // annex-b conversion when stream is avcC
    int videoStream_ = -1;
    // Wall-clock deadline for the current blocking demux call. Wall time (not
    // monotonic) on purpose: after a process freeze/thaw the clock jumps
    // forward past the deadline and the stale read aborts immediately,
    // dropping into the normal reconnect path.
    int64_t readDeadlineSec_ = 0;

    AMediaCodec* codec_ = nullptr;
    AImageReader* reader_ = nullptr;
    bool inputDone_ = false;

    bool headerSent_ = false;
    uint32_t poolWidth_ = 0, poolHeight_ = 0;
    std::set<uint64_t> announced_; // AHardwareBuffer* values

    struct HeldImage {
        uint64_t readyValue;
        AImage* image;
    };
    std::deque<HeldImage> held_;
    uint64_t readyCounter_ = 0;
    uint64_t droppedFrames_ = 0, sentFrames_ = 0;
    uint64_t decodedFrames_ = 0, acquireFailures_ = 0;
    uint64_t queuedPackets_ = 0, inputStalls_ = 0;
};

} // namespace vkr
