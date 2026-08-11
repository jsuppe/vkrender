// M27: RTSP multi-source receiver -> framelink producer (Linux).
//
// The shared client/rtsp_room.cpp core owns the ingest (one private pipeline
// per RTSP URL, HTTP control API, backoff reconnect); this file supplies the
// Linux sink: each source's decoded video is written into its OWN framelink
// input slot (client/fl_sink_posix.h). The compositor keys one scene stream
// per slot, so every camera lands on its own tile with zero compositor
// changes, exactly like a WHIP presenter.
//
// M39: framelink, not waylandsink - one transport, shared with the Windows
// twin (docs/MIGRATION.md). A side benefit: this receiver no longer needs a
// Wayland connection at all, so it runs headless with no fakesink workaround.
//
//   vkr_rtsp [--port 8890] [--max 8] [--url rtsp://...]...
//     (WAYLAND_DISPLAY selects the scene: slots are <WAYLAND_DISPLAY>.in.N)
//
// Unlike the WHIP receiver this one is a CLIENT: it dials out to cameras and
// encoders, so there is no PIN and nothing to advertise over mDNS.
// Credentials ride in the URL (rtsp://user:pass@host/path).

#ifndef _WIN32

#include <gst/gst.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "fl_sink_posix.h"
#include "rtsp_room.h"

namespace {

// rtsp_room callback: a source's video pad appeared (rtspsrc's encoded RTP
// pad - the same contract as whip_room's onTrack, so this is the WHIP sink
// with the leaky queue added). decodebin handles depay + decode for whatever
// the camera negotiated; the BGRA appsink feeds this source's framelink slot.
void* onStream(GstElement* pipeline, GstPad* pad, unsigned sourceId, GstElement**, int,
               void*) {
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
        return nullptr;
    }
    auto* fs = new vkr::FlSink();
    fs->id = sourceId;
    fs->tag = "rtsp";
    fs->stats.init(pipeline, sourceId);
    fs->stats.attachProbe(pad, vkr::kStageIngress); // post-jitterbuffer RTP

    g_object_set(queue, "leaky", 2 /* downstream */, "max-size-buffers", 3,
                 "max-size-bytes", 0, "max-size-time", (guint64)0, nullptr);
    GstCaps* bgra = gst_caps_from_string("video/x-raw,format=BGRA");
    g_object_set(sink, "caps", bgra, "emit-signals", TRUE, "max-buffers", 4, "drop", TRUE,
                 "sync", FALSE, nullptr);
    gst_caps_unref(bgra);
    GstAppSinkCallbacks cbs{};
    cbs.new_sample = vkr::flSinkNewSample;
    gst_app_sink_set_callbacks(GST_APP_SINK(sink), &cbs, fs, nullptr);

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
    fs->stats.attachProbe(statPad, vkr::kStageSink);
    gst_object_unref(statPad);

    printf("rtsp: [source %u] video -> framelink slot\n", sourceId);
    return fs;
}

void onStreamClosed(void* handle, unsigned sourceId, void*) {
    vkr::flSinkClose(static_cast<vkr::FlSink*>(handle));
    printf("rtsp: [source %u] closed\n", sourceId);
}

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    int port = 8890;
    int maxSources = 8;
    std::vector<const char*> urls;

    const char* usage = "usage: vkr_rtsp [--port N] [--max N] [--url rtsp://...]...\n";
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max") && i + 1 < argc) maxSources = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--url") && i + 1 < argc) urls.push_back(argv[++i]);
        else {
            fprintf(stderr, "%s", usage);
            return 1;
        }
    }

    gst_init(&argc, &argv);

    RtspRoomCallbacks cbs{};
    cbs.onStream = onStream;
    cbs.onStreamClosed = onStreamClosed;
    RtspRoom* room = rtsp_room_new(port, maxSources, &cbs, nullptr);
    if (!room) return 1;
    printf("rtsp: control API on http://0.0.0.0:%d/sources - room for %d sources "
           "(slots on '%s')\n",
           port, maxSources, vkr::flSlotPrefix().c_str());

    for (const char* u : urls) rtsp_room_add(room, u);

    // The room does all pipeline work here on the main thread: builds pending
    // sources, pumps each source's bus, and runs the reconnect backoff.
    for (;;) {
        rtsp_room_poll(room);
        g_usleep(100 * 1000);
    }
}

#endif // !_WIN32
