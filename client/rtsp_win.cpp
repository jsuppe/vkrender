// M27: RTSP multi-source receiver -> vkrender producer (Windows).
//
// The shared client/rtsp_room.cpp core owns the ingest (one private pipeline
// per RTSP URL, HTTP control API, backoff reconnect); this file supplies the
// Windows sink: each source's decoded video is uploaded into its OWN
// vkrender_client connection. The M14 service keys one scene stream per pipe
// connection, so every camera lands on its own presentation tile with zero
// renderer changes - exactly like a WHIP presenter.
//
//   vkr_rtsp [--pipe name] [--port 8890] [--max 8] [--url rtsp://... ]...
//
// Unlike the WHIP/AirPlay/Miracast receivers this one is a CLIENT: it dials
// out to cameras and encoders rather than waiting to be discovered, so there
// is no PIN and no mDNS advertisement. Credentials ride in the URL
// (rtsp://user:pass@host/path).
//
// Add and remove sources while it runs:
//   curl -d rtsp://cam1.local/stream http://localhost:8890/sources
//   curl http://localhost:8890/sources
//   curl -X DELETE http://localhost:8890/sources/1
//
// The SAME rtsp_room core drives the Linux and Android receivers (there the
// sink differs: waylandsink into the compositor / the renderer Surface).

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <gst/app/gstappsink.h>
#include <gst/gst.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "framelink.h"
#include "framelink_slots.h"
#include "framelink_d3d11.h"
#include "recv_stats.h"
#include "rtsp_room.h"

using Microsoft::WRL::ComPtr;

namespace {

const char* g_pipe = "vkrender";

// One source's frame path: its input slot + frame counter. The slot is taken
// on the FIRST decoded frame, so flRequestGeometry can ask for the camera's
// actual resolution (a fixed-size ring would letterbox a 4:3 camera into a
// corner of its tile).
struct Stream {
    flChannel* channel = nullptr;
    unsigned sourceId = 0;
    bool connectFailed = false;
    std::atomic<uint64_t> frames{0};
    vkr::RecvStats stats; // M23: behind-live latency + drop-by-cause counters
};

// A decoded BGRA frame arrived from one source's appsink.
GstFlowReturn onNewSample(GstAppSink* sink, gpointer user) {
    Stream* st = static_cast<Stream*>(user);
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
        if (!st->channel && !st->connectFailed) {
            // No wait: a camera frame is in hand and an appsink callback is not
            // the place to block. If every slot is occupied this source has
            // nowhere to go, which is the same answer the old 12-client limit
            // gave.
            uint32_t slot = 0;
            const std::string prefix = std::string(g_pipe) + ".in";
            flOpenProducerSlot(prefix.c_str(), 12, (uint32_t)w, (uint32_t)h,
                               FL_FORMAT_BGRA8, 0, &st->channel, &slot);
            if (!st->channel) {
                st->connectFailed = true; // no free slot; drop frames quietly
                fprintf(stderr,
                        "rtsp: [source %u] no free input slot on '%s' - discarding stream\n",
                        st->sourceId, g_pipe);
            } else {
                printf("rtsp: [source %u] producing %dx%d on '%s.in.%u'\n", st->sourceId, w, h,
                       g_pipe, slot);
            }
        }
        st->stats.onPulled();
        flBuffer buf{};
        if (!st->channel) {
            st->stats.onRingDrop(); // nowhere to send it; frame discarded
        } else if (flAcquireBuffer(st->channel, &buf, 5) != FL_OK) {
            st->stats.onRingDrop(); // ring starved: renderer not consuming
        } else {
            ID3D11Device* dev = flChannelD3D11Device(st->channel);
            ComPtr<ID3D11DeviceContext> ctx;
            dev->GetImmediateContext(&ctx);
            ID3D11Texture2D* tex = flImageD3D11(buf.image);
            D3D11_TEXTURE2D_DESC td{};
            tex->GetDesc(&td);
            UINT cw = (UINT)w < td.Width ? (UINT)w : td.Width;
            UINT chh = (UINT)h < td.Height ? (UINT)h : td.Height;
            D3D11_BOX box{0, 0, 0, cw, chh, 1};
            ctx->UpdateSubresource(tex, 0, &box, map.data, (UINT)w * 4, (UINT)w * 4 * chh);
            flSubmit(st->channel, &buf, -1);
            uint64_t n = ++st->frames;
            if (n % 300 == 1)
                printf("rtsp: [source %u] %llu frames into the scene\n", st->sourceId,
                       (unsigned long long)n);
        }
        gst_buffer_unmap(buffer, &map);
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

// rtsp_room callback: a source's video pad appeared (rtspsrc's encoded RTP
// pad - the same contract as whip_room's onTrack, so this is the WHIP sink
// with the leaky queue added). decodebin handles depay + decode for whatever
// the camera negotiated.
void* onStream(GstElement* pipeline, GstPad* pad, unsigned sourceId, GstElement**, int,
               void*) {
    Stream* st = new Stream();
    st->sourceId = sourceId; // producer connection made on first frame
    st->stats.init(pipeline, sourceId);
    st->stats.attachProbe(pad, vkr::kStageIngress); // post-jitterbuffer RTP

    GstElement* decode = gst_element_factory_make("decodebin", nullptr);
    // leaky queue: a camera that outruns the renderer must drop, never stall
    // the RTSP session (a stalled session eventually drops the connection).
    GstElement* queue = gst_element_factory_make("queue", nullptr);
    GstElement* convert = gst_element_factory_make("videoconvert", nullptr);
    GstElement* sink = gst_element_factory_make("appsink", nullptr);
    if (!decode || !queue || !convert || !sink) {
        fprintf(stderr,
                "rtsp: [source %u] missing elements (decodebin/queue/videoconvert/appsink)\n",
                sourceId);
        delete st;
        return nullptr;
    }
    g_object_set(queue, "leaky", 2 /* downstream */, "max-size-buffers", 3,
                 "max-size-bytes", 0, "max-size-time", (guint64)0, nullptr);
    GstCaps* bgra = gst_caps_from_string("video/x-raw,format=BGRA");
    g_object_set(sink, "caps", bgra, "emit-signals", TRUE, "max-buffers", 4, "drop", TRUE,
                 "sync", FALSE, nullptr);
    gst_caps_unref(bgra);
    GstAppSinkCallbacks cbs{};
    cbs.new_sample = onNewSample;
    gst_app_sink_set_callbacks(GST_APP_SINK(sink), &cbs, st, nullptr);

    gst_bin_add_many(GST_BIN(pipeline), decode, queue, convert, sink, nullptr);
    gst_element_sync_state_with_parent(decode);
    gst_element_sync_state_with_parent(queue);
    gst_element_sync_state_with_parent(convert);
    gst_element_sync_state_with_parent(sink);
    gst_element_link_many(queue, convert, sink, nullptr);
    // decodebin's src pad appears dynamically -> link to the queue then.
    g_signal_connect(decode, "pad-added", G_CALLBACK(+[](GstElement*, GstPad* p, gpointer c) {
                         GstPad* q = gst_element_get_static_pad(GST_ELEMENT(c), "sink");
                         gst_pad_link(p, q);
                         gst_object_unref(q);
                     }),
                     queue);

    GstPad* sinkpad = gst_element_get_static_pad(decode, "sink");
    gst_pad_link(pad, sinkpad);
    gst_object_unref(sinkpad);

    // Decoded+converted frames, just before the appsink queue.
    GstPad* statPad = gst_element_get_static_pad(sink, "sink");
    st->stats.attachProbe(statPad, vkr::kStageSink);
    gst_object_unref(statPad);
    return st;
}

// rtsp_room callback: source removed or reconnecting - the pipeline is already
// at NULL, so close the producer connection (the renderer frees that stream's
// slot and the layout re-flows).
void onStreamClosed(void* handle, unsigned sourceId, void*) {
    Stream* st = static_cast<Stream*>(handle);
    if (!st) return;
    printf("rtsp: [source %u] closed after %llu frames\n", sourceId,
           (unsigned long long)st->frames.load());
    // Detaching frees the slot for the next source. The channel itself is the
    // renderer's and outlives us.
    if (st->channel) flClose(st->channel);
    delete st;
}

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    int port = 8890;
    int maxSources = 8;
    std::vector<const char*> urls;

    const char* usage =
        "usage: vkr_rtsp [--pipe name] [--port N] [--max N] [--url rtsp://...]...\n";
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--pipe") && i + 1 < argc) g_pipe = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max") && i + 1 < argc) maxSources = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--url") && i + 1 < argc) urls.push_back(argv[++i]);
        else {
            fprintf(stderr, "%s", usage);
            return 1;
        }
    }

    gst_init(&argc, &argv);

    // M28: wait for the renderer rather than dying on a startup race. Exiting
    // here silently costs the appliance a whole modality, and there are two
    // easy ways to lose the probe: the launcher starts us seconds after the
    // renderer, and a service already holding its 12-client maximum rejects
    // the probe too (observed: killed WHIP presenters keep their slots until
    // the WebRTC timeout, and vkr_rtsp launched into that window died for
    // good). Per-source connections are lazy, so once there is room we work.
    //
    // M38: the probe is now "does slot 0 exist", and FL_BUSY answers that
    // without occupying anything - an occupied slot proves the renderer is up
    // just as well as a free one does. The old probe took a real client
    // connection, which is exactly why it could fail against a full service and
    // kill this receiver for good.
    bool serviceUp = false;
    const std::string slot0 = std::string(g_pipe) + ".in.0";
    for (int attempt = 0; attempt < 30 && !serviceUp; ++attempt) {
        flChannel* probe = nullptr;
        const flResult r = flOpenProducer(slot0.c_str(), &probe);
        if (r == FL_OK) flClose(probe);
        if (r == FL_OK || r == FL_BUSY) {
            serviceUp = true;
            break;
        }
        if (attempt == 0)
            printf("rtsp: waiting for the vkrenderer service on pipe '%s'...\n", g_pipe);
        Sleep(1000);
    }
    if (!serviceUp)
        fprintf(stderr,
                "rtsp: vkrenderer service on pipe '%s' still unreachable - continuing "
                "anyway; sources will attach once it has room\n",
                g_pipe);

    RtspRoomCallbacks cbs{};
    cbs.onStream = onStream;
    cbs.onStreamClosed = onStreamClosed;
    RtspRoom* room = rtsp_room_new(port, maxSources, &cbs, nullptr);
    if (!room) return 1;
    printf("rtsp: control API on http://0.0.0.0:%d/sources - room for %d sources\n", port,
           maxSources);

    for (const char* u : urls) rtsp_room_add(room, u);

    // The room does all pipeline work here on the main thread: builds pending
    // sources, pumps each source's bus, and runs the reconnect backoff.
    for (;;) {
        rtsp_room_poll(room);
        g_usleep(100 * 1000);
    }
}

#endif // _WIN32
