#pragma once

#include <cstdint>
#include <deque>
#include <set>
#include <string>

#include "ipc.h"
#include "protocol.h"

extern "C" {
#include <libavutil/frame.h>
}

#include <va/va.h>

struct AVFormatContext;
struct AVCodecContext;
struct AVBufferRef;

namespace vkr {

// RTSP -> VAAPI hardware decode -> dma-buf export of the decode surfaces
// themselves (no blit at all: the renderer samples the exact memory NVDEC/
// the media engine wrote).
//
// Surface lifetime: FFmpeg recycles decode surfaces when their AVFrame refs
// drop. We hold a ref for every frame announced to the renderer and release
// it when the renderer's FrameConsumed says it will never read it again -
// that hold is what makes zero-copy safe. Producer-side sync is
// vaSyncSurface before FrameReady (decode certainly finished); consumer-side
// is the FrameConsumed message (sent after the renderer's fences prove its
// GPU reads retired).
class VAAPIDecoder {
public:
    VAAPIDecoder(const proto::HelloMsg& hello, uint32_t maxHeldFrames,
                 const std::string& drmDevice);
    ~VAAPIDecoder();

    // Blocks until the renderer goes away; RTSP failures auto-reconnect.
    int run(const std::string& url, IpcChannel& ch);

private:
    bool openStream(const std::string& url);
    void closeStream();
    bool announceSurface(VASurfaceID sid, IpcChannel& ch);
    bool submitFrame(AVFrame* frame, IpcChannel& ch);
    bool pollRenderer(IpcChannel& ch);       // also processes FrameConsumed
    bool backoffWait(IpcChannel& ch, uint32_t ms);
    void releaseConsumed(uint64_t consumedValue);

    uint32_t maxHeld_;
    std::string drmDevice_;

    // FFmpeg
    AVFormatContext* fmt_ = nullptr;
    AVCodecContext* codec_ = nullptr;
    AVBufferRef* hwDevice_ = nullptr;
    int videoStream_ = -1;
    VADisplay display_ = nullptr; // owned by hwDevice_

    // Stream header state
    bool headerSent_ = false;
    uint32_t poolWidth_ = 0, poolHeight_ = 0;
    std::set<VASurfaceID> announced_;

    // Frames announced to the renderer and not yet consumed
    struct HeldFrame {
        uint64_t readyValue;
        AVFrame* frame;
    };
    std::deque<HeldFrame> held_;
    uint64_t readyCounter_ = 0;
    uint64_t droppedFrames_ = 0, sentFrames_ = 0;
};

} // namespace vkr
