// M27: a multi-source RTSP ingest ("the room") that pulls from RTSP servers.
//
// The other receivers are PUSH: a sender finds us and initiates (WHIP POST,
// AirPlay pairing, Miracast connect). RTSP is PULL - we dial out to a camera
// or encoder and keep the connection up. That inverts two things:
//
//   - There is no PIN. We are the client; the server authenticates us, via
//     credentials in the URL (rtsp://user:pass@host/path), not the reverse.
//   - Sources are configured, not discovered. They are added over a small
//     HTTP control API at runtime and reconnected with backoff forever, so a
//     camera that reboots comes back on its own.
//
// Each source owns its OWN GstPipeline rather than sharing one. RTSP endpoints
// fail independently and often (camera reboots, network drops, credential
// changes), and a private pipeline means one dead source cannot error out the
// others and reconnect is just NULL -> PLAYING on that pipeline alone.
//
// Control API (JSON; CORS-open like the WHIP endpoint):
//   GET    /sources        -> 200 [{"id":1,"url":"rtsp://...","state":"live"}]
//   POST   /sources        body: a bare URL, or {"url":"rtsp://..."}
//                          -> 201 {"id":N} + Location: /sources/N
//   DELETE /sources/<id>   -> 204
//   OPTIONS                -> 204 (browser preflight)
//
// Platform code supplies one callback: onStream, invoked when a source's
// decoded video pad appears; it builds the platform sink branch (Windows:
// -> vkrender_client ring; Linux: -> waylandsink; Android: -> MediaCodec /
// renderer Surface) exactly as the WHIP receivers do, so the two ingest paths
// share their sink code shape.
//
// Threading: the HTTP listener runs on its own threads and only ever queues
// intent. Every pipeline mutation - creation, teardown, reconnect - happens in
// rtsp_room_poll() on the caller's main thread. Same discipline as whip_room.
#pragma once

#include <gst/gst.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RtspRoom RtspRoom;

typedef struct {
    // A source's video pad appeared. Build and link the sink branch for `pad`
    // (elements added to `pipeline` by the callback) and return an opaque
    // per-source handle (may be NULL).
    //
    // `pad` is rtspsrc's ENCODED RTP pad - deliberately the same contract as
    // whip_room's onTrack, so a platform's sink code is near-identical between
    // the two ingest types. It matters most on Android, where the sink must
    // feed H.264 access units to MediaCodec decoding straight into a renderer
    // slot Surface; a centrally-decoded pad would have thrown that zero-copy
    // path away.
    //
    // `elements` need not be filled: a source owns its whole pipeline here, so
    // the room tears that down rather than reaping individual elements. The
    // parameter exists to keep this signature interchangeable with the WHIP
    // one.
    void* (*onStream)(GstElement* pipeline, GstPad* pad, unsigned sourceId,
                      GstElement** elements, int maxElements, void* user);
    // Reap time (main thread, elements already at GST_STATE_NULL): release
    // whatever onStream allocated. Called on removal AND before each
    // reconnect attempt, so it must be safe to pair with a later onStream.
    void (*onStreamClosed)(void* streamHandle, unsigned sourceId, void* user);
} RtspRoomCallbacks;

// Start the room. `controlPort` <= 0 disables the HTTP control API (sources
// can still be added programmatically). Returns NULL on bind failure.
RtspRoom* rtsp_room_new(int controlPort, int maxSources, const RtspRoomCallbacks* cbs,
                        void* user);

// Add a source. Returns its id, or 0 if the room is full or the URL is empty.
// The pipeline is built on the next rtsp_room_poll(), so this is safe to call
// from any thread.
unsigned rtsp_room_add(RtspRoom* room, const char* url);

// Remove a source by id. Returns 1 if it existed. Teardown happens in poll().
int rtsp_room_remove(RtspRoom* room, unsigned id);

// Build pending sources, pump each source's bus, and run reconnect backoff.
// Call regularly from the main loop.
void rtsp_room_poll(RtspRoom* room);

// Sources currently delivering frames (i.e. onStream has fired and no error
// since). Note this is "connected", not "configured".
int rtsp_room_live_count(RtspRoom* room);

// JSON array of every configured source, for the control API and logging.
// Caller frees with g_free().
char* rtsp_room_list_json(RtspRoom* room);

void rtsp_room_free(RtspRoom* room);

#ifdef __cplusplus
}
#endif
