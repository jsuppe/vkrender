// M20/M22: WebRTC (WHIP) receiver -> vkrender producer (Windows).
//
// M22 turned this into a multi-presenter room: client/whip_room.cpp runs the
// WHIP ingest (own HTTP listener, one webrtcbin per presenter - replacing the
// single-producer whipserversrc), and this file supplies the Windows sink:
// each presenter's video track is decoded and uploaded into its OWN
// vkrender_client connection. The M14 service keys one scene stream per pipe
// connection, so every presenter lands on their own cube face / wall tile
// with zero renderer changes.
//
//   vkr_webrtc [pipe] [port] [name] [maxPresenters]
//
// It advertises itself over mDNS as `_vkrender-whip._tcp` (TXT path=/whip/
// endpoint) so the Flutter sender discovers it with no manual address entry.
// Any WHIP sender works: the Flutter app, a browser, OBS, whipclientsink.
//
// The SAME whip_room core drives the Linux and Android receivers (there the
// sink differs: waylandsink into the compositor / MediaCodec into the
// renderer Surface); this is the shared WebRTC design.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d11.h>
#include <wrl/client.h>

#define GST_USE_UNSTABLE_API
#include <gst/app/gstappsink.h>
#include <gst/gst.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "recv_stats.h"
#include "vkrender_client.h"
#include "whip_mdns_win.h"
#include "whip_room.h"

using Microsoft::WRL::ComPtr;

namespace {

const char* g_pipe = "vkrender";

// One presenter's frame path: their producer connection + frame counter. The
// connection is made on the FIRST decoded frame so the ring matches the
// presenter's actual resolution (a fixed-size ring would letterbox them into
// a corner of their cube face).
struct Track {
    vkrClient* client = nullptr;
    unsigned sessionId = 0;
    bool connectFailed = false;
    std::atomic<uint64_t> frames{0};
    vkr::RecvStats stats; // M23: behind-live latency + drop-by-cause counters
};

// A decoded BGRA frame arrived from one presenter's appsink.
GstFlowReturn onNewSample(GstAppSink* sink, gpointer user) {
    Track* trk = static_cast<Track*>(user);
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
        if (!trk->client && !trk->connectFailed) {
            trk->client = vkrConnect(g_pipe, nullptr, w, h);
            if (!trk->client) {
                trk->connectFailed = true; // service gone; drop frames quietly
                fprintf(stderr,
                        "webrtc: [session %u] cannot reach vkrenderer service on pipe "
                        "'%s' - discarding track\n",
                        trk->sessionId, g_pipe);
            } else {
                printf("webrtc: [session %u] producing %dx%d\n", trk->sessionId, w, h);
            }
        }
        trk->stats.onPulled();
        vkrBuffer buf;
        if (!trk->client) {
            // fallthrough: no service; frame discarded (counted as ringdrop)
            trk->stats.onRingDrop();
        } else if (vkrDequeueBuffer(trk->client, &buf, 5) != 0) {
            trk->stats.onRingDrop(); // ring starved: renderer not consuming
        } else {
            ComPtr<ID3D11Device> dev = vkrGetDevice(trk->client);
            ComPtr<ID3D11DeviceContext> ctx;
            dev->GetImmediateContext(&ctx);
            D3D11_TEXTURE2D_DESC td{};
            buf.texture->GetDesc(&td);
            UINT cw = (UINT)w < td.Width ? (UINT)w : td.Width;
            UINT chh = (UINT)h < td.Height ? (UINT)h : td.Height;
            D3D11_BOX box{0, 0, 0, cw, chh, 1};
            ctx->UpdateSubresource(buf.texture, 0, &box, map.data, (UINT)w * 4,
                                   (UINT)w * 4 * chh);
            vkrQueueBuffer(trk->client, &buf, -1);
            uint64_t n = ++trk->frames;
            if (n % 300 == 1)
                printf("webrtc: [session %u] %llu frames into the scene\n", trk->sessionId,
                       (unsigned long long)n);
        }
        gst_buffer_unmap(buffer, &map);
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

// whip_room callback: a presenter's video track appeared. Build the decode
// branch (decodebin handles whatever codec they negotiated) into a BGRA
// appsink feeding a fresh producer connection.
void* onTrack(GstElement* pipeline, GstPad* pad, unsigned sessionId, GstElement** elements,
              int maxElements, void*) {
    Track* trk = new Track();
    trk->sessionId = sessionId; // producer connection made on first frame
    trk->stats.init(pipeline, sessionId);
    trk->stats.attachProbe(pad, vkr::kStageIngress); // post-jitterbuffer

    GstElement* decode = gst_element_factory_make("decodebin", nullptr);
    GstElement* convert = gst_element_factory_make("videoconvert", nullptr);
    GstElement* sink = gst_element_factory_make("appsink", nullptr);
    GstCaps* bgra = gst_caps_from_string("video/x-raw,format=BGRA");
    g_object_set(sink, "caps", bgra, "emit-signals", TRUE, "max-buffers", 4, "drop", TRUE,
                 "sync", FALSE, nullptr);
    gst_caps_unref(bgra);
    GstAppSinkCallbacks cbs{};
    cbs.new_sample = onNewSample;
    gst_app_sink_set_callbacks(GST_APP_SINK(sink), &cbs, trk, nullptr);

    gst_bin_add_many(GST_BIN(pipeline), decode, convert, sink, nullptr);
    gst_element_sync_state_with_parent(decode);
    gst_element_sync_state_with_parent(convert);
    gst_element_sync_state_with_parent(sink);
    gst_element_link(convert, sink);
    // decodebin's src pad appears dynamically -> link to convert then.
    g_signal_connect(decode, "pad-added", G_CALLBACK(+[](GstElement*, GstPad* p, gpointer c) {
                         GstPad* q = gst_element_get_static_pad(GST_ELEMENT(c), "sink");
                         gst_pad_link(p, q);
                         gst_object_unref(q);
                     }),
                     convert);
    GstPad* sinkpad = gst_element_get_static_pad(decode, "sink");
    gst_pad_link(pad, sinkpad);
    gst_object_unref(sinkpad);

    // Decoded+converted frames, just before the appsink queue.
    GstPad* statPad = gst_element_get_static_pad(sink, "sink");
    trk->stats.attachProbe(statPad, vkr::kStageSink);
    gst_object_unref(statPad);

    if (maxElements > 2) {
        elements[0] = decode;
        elements[1] = convert;
        elements[2] = sink;
    }
    return trk;
}

// whip_room callback: presenter left, branch already reaped - close their
// producer connection (the renderer frees that stream's slots).
void onTrackClosed(void* handle, unsigned sessionId, void*) {
    Track* trk = static_cast<Track*>(handle);
    if (!trk) return;
    printf("webrtc: [session %u] closed after %llu frames\n", sessionId,
           (unsigned long long)trk->frames.load());
    if (trk->client) vkrDisconnect(trk->client);
    delete trk;
}

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    g_pipe = argc > 1 ? argv[1] : "vkrender";
    int port = argc > 2 ? atoi(argv[2]) : 8889;
    const char* name = argc > 3 ? argv[3] : "vkrender";
    int maxPresenters = argc > 4 ? atoi(argv[4]) : 12;

    gst_init(&argc, &argv);

    // Fail fast if the renderer service isn't up (presenter connections are
    // made lazily per session, but a dead service should be loud at launch).
    vkrClient* probe = vkrConnect(g_pipe, nullptr, 64, 64);
    if (!probe) {
        fprintf(stderr, "webrtc: cannot reach vkrenderer service on pipe '%s'\n", g_pipe);
        return 1;
    }
    vkrDisconnect(probe);

    GstElement* pipeline = gst_pipeline_new("webrtc-room");
    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    WhipRoomCallbacks cbs{};
    cbs.onTrack = onTrack;
    cbs.onTrackClosed = onTrackClosed;
    WhipRoom* room = whip_room_new(pipeline, port, maxPresenters, &cbs, nullptr);
    if (!room) return 1;
    printf("webrtc: WHIP room on http://0.0.0.0:%d - space for %d presenters\n", port,
           maxPresenters);

    // Advertise on the LAN so the Flutter sender auto-discovers us.
    whip_mdns_register(name, "/whip/endpoint", (unsigned short)port);

    // Pump the bus (per-presenter decode errors are logged, not fatal) and
    // reap departed presenters.
    GstBus* bus = gst_element_get_bus(pipeline);
    for (;;) {
        GstMessage* msg =
            gst_bus_timed_pop_filtered(bus, 200 * GST_MSECOND,
                                       (GstMessageType)(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
        if (msg) {
            GError* err = nullptr;
            gst_message_parse_error(msg, &err, nullptr);
            fprintf(stderr, "webrtc: pipeline error from %s: %s\n",
                    GST_OBJECT_NAME(GST_MESSAGE_SRC(msg)), err ? err->message : "?");
            if (err) g_error_free(err);
            gst_message_unref(msg);
        }
        whip_room_poll(room);
    }
}
