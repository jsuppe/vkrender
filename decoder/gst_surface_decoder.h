#pragma once

#include <cstdint>
#include <string>

#include <gst/gst.h>

struct AMediaCodec;
struct ANativeWindow;

namespace vkr {

// M9 Android decoder: GStreamer handles transport/demux/parsing
// (rtspsrc -> parsebin -> appsink emitting byte-stream access units), and
// platform AMediaCodec decodes directly INTO the renderer-provided Surface
// (M8 architecture - BufferQueue carries the frames; GStreamer's own
// amcvideodec cannot target a foreign AImageReader surface without an extra
// GL copy, so the decode element stays MediaCodec).
class GstSurfaceDecoder {
public:
    // One session into the given surface; RTSP failures reconnect internally,
    // returns when the surface dies.
    int run(const std::string& url, ANativeWindow* window);

private:
    bool buildPipeline(const std::string& url);
    void teardownPipeline();
    bool ensureCodec(GstCaps* caps, ANativeWindow* window);
    void closeCodec();
    bool feedAccessUnit(GstBuffer* buffer);
    bool drainDecoder(); // false when the surface/codec died

    GstElement* pipeline_ = nullptr;
    GstElement* appsink_ = nullptr;
    GstBus* bus_ = nullptr;

    AMediaCodec* codec_ = nullptr;
    bool surfaceDead_ = false;
    std::string codecMime_;
    int codecW_ = 0, codecH_ = 0;

    uint64_t queuedPackets_ = 0, decodedFrames_ = 0;
};

} // namespace vkr
