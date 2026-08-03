#pragma once

#include <cstdint>
#include <deque>
#include <set>
#include <string>

#include "ipc.h"
#include "protocol.h"

#include <gst/gst.h>
#include <va/va.h>

namespace vkr {

// M9 Linux decoder: GStreamer end-to-end (rtspsrc -> decodebin3 ->
// hardware VA decoder) with an appsink constrained to memory:VAMemory so
// decoded frames stay in VA surfaces. Each buffer's VASurfaceID (public
// gstreamer-va-1.0 API) feeds the same vaExportSurfaceHandle(SEPARATE_LAYERS)
// dma-buf export verified in M6 - the renderer side is unchanged.
//
// Surface lifetime: a gst_buffer_ref pins each announced frame's surface (the
// decoder pool recycles on unref); FrameConsumed drops the ref. Held ring is
// capped; excess frames are dropped to bound latency.
class GstVaDecoder {
public:
    GstVaDecoder(const proto::HelloMsg& hello, uint32_t maxHeldFrames);
    ~GstVaDecoder();

    int run(const std::string& url, IpcChannel& ch);

private:
    bool buildPipeline(const std::string& url);
    void teardownPipeline();
    bool resolveVaDisplay(); // from the pipeline's gst.va.display.handle context
    bool announceSurface(VASurfaceID sid, IpcChannel& ch);
    bool submitBuffer(GstBuffer* buffer, GstCaps* caps, IpcChannel& ch);
    bool pollRenderer(IpcChannel& ch);
    bool backoffWait(IpcChannel& ch, uint32_t ms);
    void releaseConsumed(uint64_t consumedValue);

    uint32_t maxHeld_;

    GstElement* pipeline_ = nullptr;
    GstElement* appsink_ = nullptr;
    GstBus* bus_ = nullptr;
    void* gstVaDisplay_ = nullptr; // GstVaDisplay*, keeps VADisplay alive
    VADisplay display_ = nullptr;

    bool headerSent_ = false;
    uint32_t poolWidth_ = 0, poolHeight_ = 0;
    std::set<VASurfaceID> announced_;

    struct HeldFrame {
        uint64_t readyValue;
        GstBuffer* buffer;
    };
    std::deque<HeldFrame> held_;
    uint64_t readyCounter_ = 0;
    uint64_t droppedFrames_ = 0, sentFrames_ = 0;
};

} // namespace vkr
