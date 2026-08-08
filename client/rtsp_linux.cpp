// M27: RTSP multi-source receiver -> Wayland compositor (Linux).
//
// The shared client/rtsp_room.cpp core owns the ingest (one private pipeline
// per RTSP URL, HTTP control API, backoff reconnect); this file supplies the
// Linux sink: each source's decoded video goes to its OWN waylandsink. Every
// waylandsink opens its own connection to the M13 compositor
// (WAYLAND_DISPLAY, e.g. vkrender-0), and each compositor client surface is
// already its own scene stream - so every camera lands on its own tile with
// zero compositor changes, exactly like a WHIP presenter.
//
//   vkr_rtsp [--port 8890] [--max 8] [--url rtsp://...]...
//     (WAYLAND_DISPLAY selects the scene)
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

#include "recv_stats.h"
#include "rtsp_room.h"

namespace {

// rtsp_room callback: a source's DECODED video pad appeared (uridecodebin has
// already handled depay + decode, so unlike the WHIP sink there is no
// decodebin here). waylandsink hands frames to the compositor as this
// source's own surface.
void* onStream(GstElement* pipeline, GstPad* pad, unsigned sourceId, GstElement**, int,
               void*) {
    // leaky queue: a camera that outruns the compositor must drop, never stall
    // the RTSP session (a stalled session eventually drops the connection).
    GstElement* queue = gst_element_factory_make("queue", nullptr);
    GstElement* convert = gst_element_factory_make("videoconvert", nullptr);
    GstElement* sink = gst_element_factory_make("waylandsink", nullptr);
    if (!queue || !convert || !sink) {
        fprintf(stderr,
                "rtsp: [source %u] missing elements (queue/videoconvert/waylandsink)\n",
                sourceId);
        return nullptr;
    }
    g_object_set(queue, "leaky", 2 /* downstream */, "max-size-buffers", 3,
                 "max-size-bytes", 0, "max-size-time", (guint64)0, nullptr);
    g_object_set(sink, "sync", FALSE, nullptr);

    gst_bin_add_many(GST_BIN(pipeline), queue, convert, sink, nullptr);
    gst_element_sync_state_with_parent(queue);
    gst_element_sync_state_with_parent(convert);
    gst_element_sync_state_with_parent(sink);
    gst_element_link_many(queue, convert, sink, nullptr);

    GstPad* sinkpad = gst_element_get_static_pad(queue, "sink");
    gst_pad_link(pad, sinkpad);
    gst_object_unref(sinkpad);

    // M23: behind-live latency probes - decoded frames off the wire, and the
    // handoff into waylandsink.
    auto* stats = new vkr::RecvStats();
    stats->init(pipeline, sourceId);
    stats->attachProbe(pad, vkr::kStageIngress);
    GstPad* statPad = gst_element_get_static_pad(sink, "sink");
    stats->attachProbe(statPad, vkr::kStageSink);
    gst_object_unref(statPad);

    printf("rtsp: [source %u] video -> compositor surface\n", sourceId);
    return stats;
}

void onStreamClosed(void* handle, unsigned sourceId, void*) {
    delete static_cast<vkr::RecvStats*>(handle);
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
           "(WAYLAND_DISPLAY=%s)\n",
           port, maxSources, getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "?");

    for (const char* u : urls) rtsp_room_add(room, u);

    // The room does all pipeline work here on the main thread: builds pending
    // sources, pumps each source's bus, and runs the reconnect backoff.
    for (;;) {
        rtsp_room_poll(room);
        g_usleep(100 * 1000);
    }
}

#endif // !_WIN32
