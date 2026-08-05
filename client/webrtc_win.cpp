// M20: WebRTC (WHIP) receiver -> vkrender producer (Windows).
//
// GStreamer's whipserversrc runs a WHIP HTTP endpoint plus the entire WebRTC
// stack (ICE / DTLS-SRTP / RTP) internally; a browser (or OBS, or a GStreamer
// whipclientsink) POSTs an SDP offer to http://<host>:<port> and streams. We
// decode each received video track and upload the frames into a dequeued
// vkrender_client ring texture - the cast becomes one more scene stream.
//
//   vkr_webrtc [pipe] [port] [name]
//
// It advertises itself over mDNS as `_vkrender-whip._tcp` (TXT path=/whip/
// endpoint) so the Flutter sender discovers it with no manual address entry.
//
// The SAME whipserversrc core drives the Linux and Android receivers (there
// the sink differs: waylandsink into the compositor / MediaCodec into the
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
#include <string>

#include "vkrender_client.h"
#include "whip_mdns_win.h"

using Microsoft::WRL::ComPtr;

namespace {

vkrClient* g_client = nullptr;
GstElement* g_pipeline = nullptr;
std::atomic<uint64_t> g_frames{0};

// A decoded BGRA frame arrived from a video track's appsink.
GstFlowReturn onNewSample(GstAppSink* sink, gpointer) {
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
        vkrBuffer buf;
        if (vkrDequeueBuffer(g_client, &buf, 5) == 0) {
            ComPtr<ID3D11Device> dev = vkrGetDevice(g_client);
            ComPtr<ID3D11DeviceContext> ctx;
            dev->GetImmediateContext(&ctx);
            D3D11_TEXTURE2D_DESC td{};
            buf.texture->GetDesc(&td);
            UINT cw = (UINT)w < td.Width ? (UINT)w : td.Width;
            UINT chh = (UINT)h < td.Height ? (UINT)h : td.Height;
            D3D11_BOX box{0, 0, 0, cw, chh, 1};
            ctx->UpdateSubresource(buf.texture, 0, &box, map.data, (UINT)w * 4,
                                   (UINT)w * 4 * chh);
            vkrQueueBuffer(g_client, &buf, -1);
            uint64_t n = ++g_frames;
            if (n % 150 == 1) printf("webrtc: %llu frames into the scene\n",
                                     (unsigned long long)n);
        }
        gst_buffer_unmap(buffer, &map);
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

// whipserversrc adds a video_%u / audio_%u pad (application/x-rtp) when the
// remote track arrives. Build the depay+decode branch for video, ending in a
// BGRA appsink; ignore audio.
void onPadAdded(GstElement*, GstPad* pad, gpointer) {
    // whipserversrc names pads video_%u / audio_%u; the caps' media field can
    // still be unset at pad-added, so key off the pad name.
    gchar* padName = gst_pad_get_name(pad);
    bool isVideo = padName && strncmp(padName, "video", 5) == 0;
    printf("webrtc: track '%s'%s\n", padName ? padName : "?", isVideo ? " (video)" : "");
    g_free(padName);
    if (!isVideo) return;

    // decodebin depayloads + decodes whatever the browser negotiated (H.264,
    // VP8, VP9, ...) so we stay codec-agnostic. Its src pad -> videoconvert ->
    // BGRA appsink.
    GstElement* decode = gst_element_factory_make("decodebin", nullptr);
    GstElement* convert = gst_element_factory_make("videoconvert", nullptr);
    GstElement* sink = gst_element_factory_make("appsink", nullptr);
    GstCaps* bgra = gst_caps_from_string("video/x-raw,format=BGRA");
    g_object_set(sink, "caps", bgra, "emit-signals", TRUE, "max-buffers", 4, "drop", TRUE,
                 "sync", FALSE, nullptr);
    gst_caps_unref(bgra);
    GstAppSinkCallbacks cbs{};
    cbs.new_sample = onNewSample;
    gst_app_sink_set_callbacks(GST_APP_SINK(sink), &cbs, nullptr, nullptr);

    gst_bin_add_many(GST_BIN(g_pipeline), decode, convert, sink, nullptr);
    gst_element_sync_state_with_parent(decode);
    gst_element_sync_state_with_parent(convert);
    gst_element_sync_state_with_parent(sink);
    gst_element_link(convert, sink);
    // decodebin's src pad appears dynamically -> link to convert then.
    g_signal_connect(decode, "pad-added", G_CALLBACK(+[](GstElement*, GstPad* p, gpointer c) {
                         gst_pad_link(p, gst_element_get_static_pad(GST_ELEMENT(c), "sink"));
                     }),
                     convert);
    GstPad* sinkpad = gst_element_get_static_pad(decode, "sink");
    gst_pad_link(pad, sinkpad);
    gst_object_unref(sinkpad);
}

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char* pipe = argc > 1 ? argv[1] : "vkrender";
    int port = argc > 2 ? atoi(argv[2]) : 8889;
    const char* name = argc > 3 ? argv[3] : "vkrender";

    gst_init(&argc, &argv);

    g_client = vkrConnect(pipe, nullptr, 1280, 720);
    if (!g_client) {
        fprintf(stderr, "webrtc: cannot reach vkrenderer service on pipe '%s'\n", pipe);
        return 1;
    }

    g_pipeline = gst_pipeline_new("webrtc");
    GstElement* src = gst_element_factory_make("whipserversrc", "whip");
    if (!src) {
        fprintf(stderr, "webrtc: whipserversrc unavailable (gst-plugins-rs missing?)\n");
        return 1;
    }
    // Point the WHIP signaller's HTTP endpoint at all interfaces on `port`.
    GObject* signaller = nullptr;
    g_object_get(src, "signaller", &signaller, nullptr);
    if (signaller) {
        char addr[64];
        snprintf(addr, sizeof(addr), "http://0.0.0.0:%d", port);
        g_object_set(signaller, "host-addr", addr, nullptr);
        g_object_unref(signaller);
    }
    g_signal_connect(src, "pad-added", G_CALLBACK(onPadAdded), nullptr);
    gst_bin_add(GST_BIN(g_pipeline), src);

    if (gst_element_set_state(g_pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        fprintf(stderr, "webrtc: pipeline failed to start\n");
        return 1;
    }
    printf("webrtc: WHIP endpoint on http://0.0.0.0:%d  (POST an SDP offer to stream)\n", port);

    // Advertise on the LAN so the Flutter sender auto-discovers us.
    whip_mdns_register(name, "/whip/endpoint", (unsigned short)port);

    // Pump the bus for errors; frames flow via the appsink callback.
    GstBus* bus = gst_element_get_bus(g_pipeline);
    for (;;) {
        GstMessage* msg =
            gst_bus_timed_pop_filtered(bus, 200 * GST_MSECOND,
                                       (GstMessageType)(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
        if (msg) {
            GError* err = nullptr;
            gst_message_parse_error(msg, &err, nullptr);
            fprintf(stderr, "webrtc: pipeline error: %s\n", err ? err->message : "?");
            if (err) g_error_free(err);
            gst_message_unref(msg);
        }
    }
}
