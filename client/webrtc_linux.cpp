// M22: WebRTC (WHIP) multi-presenter receiver -> Wayland compositor (Linux).
//
// The shared client/whip_room.cpp core runs the WHIP ingest (own HTTP
// listener, one webrtcbin per presenter - replacing the single-producer
// whipserversrc); this file supplies the Linux sink: each presenter's video
// track is decoded into its OWN waylandsink. Every waylandsink opens its own
// connection to the M13 compositor (WAYLAND_DISPLAY, e.g. vkrender-0), and
// each compositor client surface is already its own scene stream - so every
// presenter lands on their own cube face with zero compositor changes.
//
//   vkr_webrtc [port] [maxPresenters]     (WAYLAND_DISPLAY selects the scene)
//
// mDNS advertisement is avahi's job on Linux - run via
// scripts/webrtc_receiver_linux.sh which pairs this binary with
// avahi-publish-service on `_vkrender-whip._tcp`.

#ifndef _WIN32

#include <gst/gst.h>

#include <cstdio>
#include <cstdlib>

#include "whip_room.h"

namespace {

// whip_room callback: a presenter's video track appeared. decodebin handles
// whatever codec they negotiated; waylandsink hands the frames to the
// compositor as this presenter's own surface.
void* onTrack(GstElement* pipeline, GstPad* pad, unsigned sessionId, GstElement** elements,
              int maxElements, void*) {
    GstElement* decode = gst_element_factory_make("decodebin", nullptr);
    GstElement* convert = gst_element_factory_make("videoconvert", nullptr);
    GstElement* sink = gst_element_factory_make("waylandsink", nullptr);
    if (!decode || !convert || !sink) {
        fprintf(stderr, "webrtc: [session %u] missing elements (decodebin/videoconvert/"
                        "waylandsink)\n", sessionId);
        return nullptr;
    }
    g_object_set(sink, "sync", FALSE, nullptr);

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

    printf("webrtc: [session %u] video track -> compositor surface\n", sessionId);
    if (maxElements > 2) {
        elements[0] = decode;
        elements[1] = convert;
        elements[2] = sink;
    }
    return nullptr; // nothing beyond the branch to reap
}

void onTrackClosed(void*, unsigned sessionId, void*) {
    printf("webrtc: [session %u] closed\n", sessionId);
}

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    int port = argc > 1 ? atoi(argv[1]) : 8889;
    int maxPresenters = argc > 2 ? atoi(argv[2]) : 12;

    gst_init(&argc, &argv);

    GstElement* pipeline = gst_pipeline_new("webrtc-room");
    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    WhipRoomCallbacks cbs{};
    cbs.onTrack = onTrack;
    cbs.onTrackClosed = onTrackClosed;
    WhipRoom* room = whip_room_new(pipeline, port, maxPresenters, &cbs, nullptr);
    if (!room) return 1;
    printf("webrtc: WHIP room on http://0.0.0.0:%d - space for %d presenters "
           "(WAYLAND_DISPLAY=%s)\n",
           port, maxPresenters, getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "?");

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

#endif // !_WIN32
