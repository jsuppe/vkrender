// M22: WebRTC (WHIP) multi-presenter receiver -> framelink producer (Linux).
//
// The shared client/whip_room.cpp core runs the WHIP ingest (own HTTP
// listener, one webrtcbin per presenter - replacing the single-producer
// whipserversrc); this file supplies the Linux sink: each presenter's video
// track is decoded to BGRA and written into its OWN framelink input slot
// (client/fl_sink_posix.h). The compositor keys one scene stream per slot, so
// every presenter lands on their own cube face with zero compositor changes.
//
// M39: framelink, not waylandsink. The Wayland socket stays as the front door
// for arbitrary Wayland clients; the receivers now speak the same transport as
// their Windows twins, which is the entire point of the migration
// (docs/MIGRATION.md).
//
//   vkr_webrtc [port] [maxPresenters]     (WAYLAND_DISPLAY selects the scene:
//                                          slots are <WAYLAND_DISPLAY>.in.N)
//
// mDNS advertisement is avahi's job on Linux - run via
// scripts/webrtc_receiver_linux.sh which pairs this binary with
// avahi-publish-service on `_vkrender-whip._tcp`.

#ifndef _WIN32

#include <gst/gst.h>

#include <cstdio>
#include <cstdlib>

#include "fl_sink_posix.h"
#include "whip_room.h"

namespace {

// whip_room callback: a presenter's video track appeared. decodebin handles
// whatever codec they negotiated; the BGRA appsink feeds this presenter's
// framelink slot.
void* onTrack(GstElement* pipeline, GstPad* pad, unsigned sessionId, GstElement** elements,
              int maxElements, void*) {
    GstElement* decode = gst_element_factory_make("decodebin", nullptr);
    GstElement* convert = gst_element_factory_make("videoconvert", nullptr);
    GstElement* sink = gst_element_factory_make("appsink", nullptr);
    if (!decode || !convert || !sink) {
        fprintf(stderr, "webrtc: [session %u] missing elements (decodebin/videoconvert/"
                        "appsink)\n", sessionId);
        return nullptr;
    }
    auto* fs = new vkr::FlSink();
    fs->id = sessionId;
    fs->tag = "webrtc";
    fs->stats.init(pipeline, sessionId);
    fs->stats.attachProbe(pad, vkr::kStageIngress); // post-jitterbuffer

    GstCaps* bgra = gst_caps_from_string("video/x-raw,format=BGRA");
    g_object_set(sink, "caps", bgra, "emit-signals", TRUE, "max-buffers", 4, "drop", TRUE,
                 "sync", FALSE, nullptr);
    gst_caps_unref(bgra);
    GstAppSinkCallbacks cbs{};
    cbs.new_sample = vkr::flSinkNewSample;
    gst_app_sink_set_callbacks(GST_APP_SINK(sink), &cbs, fs, nullptr);

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
    fs->stats.attachProbe(statPad, vkr::kStageSink);
    gst_object_unref(statPad);

    printf("webrtc: [session %u] video track -> framelink slot\n", sessionId);
    if (maxElements > 2) {
        elements[0] = decode;
        elements[1] = convert;
        elements[2] = sink;
    }
    return fs;
}

void onTrackClosed(void* handle, unsigned sessionId, void*) {
    vkr::flSinkClose(static_cast<vkr::FlSink*>(handle));
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
           "(slots on '%s')\n",
           port, maxPresenters, vkr::flSlotPrefix().c_str());

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
