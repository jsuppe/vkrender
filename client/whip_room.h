// M22: a multi-session WHIP ingest server ("the room") built directly on
// webrtcbin.
//
// Why not whipserversrc: the gst-plugins-rs element shipped with GStreamer
// 1.26 is single-producer - while one WHIP session is live, new POSTs to the
// endpoint are never answered (verified empirically: HTTP timeouts for
// presenters 2..N). A 12-presenter room needs one WebRTC session per
// presenter, so this module owns the ingest instead: a tiny HTTP listener
// (GLib sockets - portable to Windows/Linux/Android) where each POSTed SDP
// offer creates its OWN webrtcbin in the caller's pipeline and is answered
// with that webrtcbin's SDP. Senders don't change: WHIP is WHIP.
//
// Protocol surface (per the WHIP spec, RFC 9725):
//   POST   <any path>       application/sdp offer -> 201 + answer,
//                           Location: /session/<id>
//   DELETE /session/<id>    presenter leaves -> session torn down
//   OPTIONS                 CORS preflight (browser senders) -> 204
// ICE is non-trickle both ways: the answer carries the server's candidates
// (gathered before responding; host candidates on a LAN are near-instant).
//
// Platform code supplies one callback: onTrack, invoked when a session's
// remote video track pad appears; it builds the platform sink branch
// (Windows: decode -> vkrender_client ring; Linux: decode -> waylandsink;
// Android: depay -> MediaCodec) and returns the branch elements so the room
// can reap them when the presenter leaves.
//
// Threading: HTTP connections are handled on their own threads; onTrack fires
// on a webrtcbin streaming thread (same as whipserversrc's pad-added did).
// Call whip_room_poll() from the main loop - all teardown (state changes,
// bin removal, onTrackClosed) happens there, never on streaming threads.
#pragma once

#include <gst/gst.h>

typedef struct WhipRoom WhipRoom;

typedef struct {
    // A session's remote video track appeared. Build and link the sink
    // branch for `pad` (elements added to `pipeline` by the callback), fill
    // `elements` (NULL-terminated, at most `maxElements`) with the branch
    // elements for later teardown, and return an opaque per-track handle
    // (may be NULL).
    void* (*onTrack)(GstElement* pipeline, GstPad* pad, unsigned sessionId,
                     GstElement** elements, int maxElements, void* user);
    // Reap time (main thread, elements already at GST_STATE_NULL and out of
    // the bin): release whatever onTrack allocated.
    void (*onTrackClosed)(void* trackHandle, unsigned sessionId, void* user);
} WhipRoomCallbacks;

// Start the room: listen on `port`, admit at most `maxSessions` concurrent
// presenters (extra POSTs get 503). Returns NULL on bind failure.
WhipRoom* whip_room_new(GstElement* pipeline, int port, int maxSessions,
                        const WhipRoomCallbacks* cbs, void* user);

// Reap departed sessions. Call regularly from the main loop.
void whip_room_poll(WhipRoom* room);

// Number of currently live presenter sessions.
int whip_room_live_count(WhipRoom* room);

void whip_room_free(WhipRoom* room);
