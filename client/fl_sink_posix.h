// M39: the shared Linux sink - a decoded BGRA GStreamer frame into a framelink
// input slot. Both Linux receivers (WHIP, RTSP) use exactly this, which is the
// point of it being one header: under the waylandsink era they at least shared
// waylandsink, and this keeps them from growing two copies of the framelink
// path the way the Windows receivers once grew four copies of a connect.
//
// The slot prefix comes from WAYLAND_DISPLAY (default vkrender-0): the
// compositor that owns that socket also owns <socket>.in.0 .. .in.11, so the
// name that picks the scene picks the slots.
//
// CPU path by design: flImageMap into a FL_MAP_CPU ring, row-by-row copy
// (GStreamer's stride and the ring's need not agree), flSubmit stamped with
// CLOCK_MONOTONIC so the compositor can measure the submit->adopt hop -
// steady_clock shares that epoch across processes.
#pragma once

#ifndef _WIN32

#include <gst/app/gstappsink.h>
#include <gst/gst.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "framelink.h"
#include "framelink_dmabuf.h"
#include "framelink_slots.h"
#include "recv_stats.h"

namespace vkr {

inline std::string flSlotPrefix() {
    const char* d = getenv("WAYLAND_DISPLAY");
    return std::string(d && *d ? d : "vkrender-0") + ".in";
}

// One stream's frame path: its input slot + counters. The slot is taken on the
// FIRST decoded frame so flRequestGeometry can ask for the stream's actual
// resolution.
struct FlSink {
    flChannel* channel = nullptr;
    unsigned id = 0;
    const char* tag = "sink"; // log prefix: "webrtc" / "rtsp"
    bool connectFailed = false;
    std::atomic<uint64_t> frames{0};
    vkr::RecvStats stats;
};

// The appsink new_sample callback body. Returns GST_FLOW_OK always: a frame
// with nowhere to go is dropped and counted, never an error - a receiver must
// not stall its network session because the renderer is busy.
inline GstFlowReturn flSinkNewSample(GstAppSink* sink, gpointer user) {
    FlSink* fs = static_cast<FlSink*>(user);
    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_OK;
    GstCaps* caps = gst_sample_get_caps(sample);
    GstStructure* s = caps ? gst_caps_get_structure(caps, 0) : nullptr;
    int w = 0, h = 0;
    if (s) {
        gst_structure_get_int(s, "width", &w);
        gst_structure_get_int(s, "height", &h);
    }
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstMapInfo map;
    if (w > 0 && h > 0 && buffer && gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        if (!fs->channel && !fs->connectFailed) {
            // No wait: a decoded frame is in hand and this callback runs on a
            // streaming thread. All slots occupied means this stream has
            // nowhere to go.
            uint32_t slot = 0;
            const std::string prefix = flSlotPrefix();
            flOpenProducerSlot(prefix.c_str(), 12, (uint32_t)w, (uint32_t)h, FL_FORMAT_BGRA8,
                               0, &fs->channel, &slot);
            if (!fs->channel) {
                fs->connectFailed = true; // no free slot; drop frames quietly
                fprintf(stderr, "%s: [%u] no free input slot on '%s' - discarding stream\n",
                        fs->tag, fs->id, prefix.c_str());
            } else {
                printf("%s: [%u] producing %dx%d on '%s.%u'\n", fs->tag, fs->id, w, h,
                       prefix.c_str(), slot);
            }
        }
        fs->stats.onPulled();
        flBuffer buf{};
        if (!fs->channel) {
            fs->stats.onRingDrop(); // nowhere to send it; frame discarded
        } else if (flAcquireBuffer(fs->channel, &buf, 5) != FL_OK) {
            fs->stats.onRingDrop(); // ring starved: renderer not consuming
        } else {
            void* px = nullptr;
            uint64_t dstStride = 0;
            flChannelInfo info{};
            flQuery(fs->channel, &info);
            if (flImageMap(buf.image, &px, &dstStride) == FL_OK && px) {
                // Row copy, clamped both ways: the consumer may have declined
                // the geometry request, and GStreamer's stride is its own.
                const uint32_t cw = (uint32_t)w < info.width ? (uint32_t)w : info.width;
                const uint32_t chh = (uint32_t)h < info.height ? (uint32_t)h : info.height;
                const uint8_t* srcRow = (const uint8_t*)map.data;
                uint8_t* dstRow = (uint8_t*)px;
                const size_t srcStride = (size_t)w * 4;
                for (uint32_t y = 0; y < chh; ++y) {
                    memcpy(dstRow, srcRow, (size_t)cw * 4);
                    srcRow += srcStride;
                    dstRow += dstStride;
                }
            }
            // Stamp a CLOCK, not media time: the compositor subtracts its own
            // steady_clock to measure the submit -> adopt hop. CLOCK_MONOTONIC
            // is one epoch for every process, so the subtraction means
            // something.
            const int64_t submitNs =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
            flSubmit(fs->channel, &buf, submitNs);
            uint64_t n = ++fs->frames;
            if (n % 300 == 1)
                printf("%s: [%u] %llu frames into the scene\n", fs->tag, fs->id,
                       (unsigned long long)n);
        }
        gst_buffer_unmap(buffer, &map);
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

inline void flSinkClose(FlSink* fs) {
    if (!fs) return;
    // Detaching frees the slot for the next stream; the channel is the
    // renderer's and outlives us.
    if (fs->channel) flClose(fs->channel);
    delete fs;
}

} // namespace vkr

#endif // !_WIN32
