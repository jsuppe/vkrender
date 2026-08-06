// See whip_room.h. Multi-session WHIP ingest on webrtcbin: one tiny HTTP
// listener, one webrtcbin per presenter, non-trickle ICE. Pure GLib/GIO +
// GStreamer -- no platform code, shared by the Windows/Linux/Android
// receivers.

#define GST_USE_UNSTABLE_API
#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>

#include <gio/gio.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "whip_room.h"

namespace {

constexpr int kMaxTrackElements = 8;
constexpr gint64 kIceGatherTimeoutUs = 5 * G_USEC_PER_SEC;

struct RoomSession {
    unsigned id = 0;
    GstElement* webrtc = nullptr;              // owned by the pipeline bin
    std::vector<GstElement*> branch;           // sink branches (ours + onTrack's)
    void* trackHandle = nullptr;
    bool haveTrack = false;
    volatile gboolean dead = FALSE;

    // Non-trickle ICE: the HTTP thread waits here for gathering to finish.
    GMutex lock;
    GCond cond;
};

} // namespace

struct WhipRoom {
    GstElement* pipeline = nullptr;
    WhipRoomCallbacks cbs{};
    void* user = nullptr;
    int maxSessions = 12;
    int port = 0;

    GMutex lock;
    std::vector<RoomSession*> sessions;   // live + dead-awaiting-reap
    unsigned nextId = 1;

    GSocketListener* listener = nullptr;
    GCancellable* cancel = nullptr;
    GThread* acceptThread = nullptr;
};

namespace {

// ---------------------------------------------------------------- sessions

void onIceGatheringNotify(GObject* obj, GParamSpec*, gpointer data) {
    (void)obj;
    RoomSession* ses = static_cast<RoomSession*>(data);
    g_mutex_lock(&ses->lock);
    g_cond_broadcast(&ses->cond);
    g_mutex_unlock(&ses->lock);
}

void onConnectionStateNotify(GObject* obj, GParamSpec*, gpointer data) {
    RoomSession* ses = static_cast<RoomSession*>(data);
    GstWebRTCPeerConnectionState st;
    g_object_get(obj, "connection-state", &st, nullptr);
    if (st == GST_WEBRTC_PEER_CONNECTION_STATE_FAILED ||
        st == GST_WEBRTC_PEER_CONNECTION_STATE_CLOSED) {
        // Presenter vanished without a DELETE (closed tab, killed sender).
        g_atomic_int_set(&ses->dead, TRUE);
    }
}

struct PadCtx {
    WhipRoom* room;
    RoomSession* ses;
};

// A remote track pad appeared on a session's webrtcbin. Video goes to the
// platform sink via onTrack; everything else is discarded into a fakesink so
// the session stays healthy.
void onPadAdded(GstElement*, GstPad* pad, gpointer data) {
    PadCtx* ctx = static_cast<PadCtx*>(data);
    WhipRoom* room = ctx->room;
    RoomSession* ses = ctx->ses;
    if (g_atomic_int_get(&ses->dead)) return;

    bool isVideo = false;
    GstCaps* caps = gst_pad_get_current_caps(pad);
    if (caps && gst_caps_get_size(caps) > 0) {
        const gchar* media =
            gst_structure_get_string(gst_caps_get_structure(caps, 0), "media");
        isVideo = media && strcmp(media, "video") == 0;
    }
    if (caps) gst_caps_unref(caps);

    if (isVideo && room->cbs.onTrack && !ses->haveTrack) {
        GstElement* elems[kMaxTrackElements + 1] = {};
        void* handle = room->cbs.onTrack(room->pipeline, pad, ses->id, elems,
                                         kMaxTrackElements, room->user);
        g_mutex_lock(&room->lock);
        ses->trackHandle = handle;
        ses->haveTrack = true;
        for (int i = 0; i < kMaxTrackElements && elems[i]; i++)
            ses->branch.push_back(elems[i]);
        g_mutex_unlock(&room->lock);
        return;
    }

    // Discard: audio tracks, or a second video track on one session.
    GstElement* sink = gst_element_factory_make("fakesink", nullptr);
    g_object_set(sink, "sync", FALSE, "async", FALSE, nullptr);
    gst_bin_add(GST_BIN(room->pipeline), sink);
    gst_element_sync_state_with_parent(sink);
    GstPad* sinkpad = gst_element_get_static_pad(sink, "sink");
    gst_pad_link(pad, sinkpad);
    gst_object_unref(sinkpad);
    g_mutex_lock(&room->lock);
    ses->branch.push_back(sink);
    g_mutex_unlock(&room->lock);
}

// Run one presenter's offer through a fresh webrtcbin: set remote, create
// answer, set local, wait for ICE gathering, return the answer SDP text.
// Returns empty string on failure (caller responds 500).
std::string negotiate(WhipRoom* room, RoomSession* ses, const std::string& offerSdp) {
    GstElement* webrtc = ses->webrtc;

    GstSDPMessage* sdp = nullptr;
    if (gst_sdp_message_new(&sdp) != GST_SDP_OK ||
        gst_sdp_message_parse_buffer((const guint8*)offerSdp.data(),
                                     (guint)offerSdp.size(), sdp) != GST_SDP_OK) {
        if (sdp) gst_sdp_message_free(sdp);
        return {};
    }
    GstWebRTCSessionDescription* offer =
        gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_OFFER, sdp); // takes sdp
    GstPromise* p = gst_promise_new();
    g_signal_emit_by_name(webrtc, "set-remote-description", offer, p);
    gst_promise_wait(p);
    gst_promise_unref(p);
    gst_webrtc_session_description_free(offer);

    p = gst_promise_new();
    g_signal_emit_by_name(webrtc, "create-answer", nullptr, p);
    gst_promise_wait(p);
    const GstStructure* reply = gst_promise_get_reply(p);
    GstWebRTCSessionDescription* answer = nullptr;
    if (reply)
        gst_structure_get(reply, "answer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &answer,
                          nullptr);
    gst_promise_unref(p);
    if (!answer) return {};

    p = gst_promise_new();
    g_signal_emit_by_name(webrtc, "set-local-description", answer, p);
    gst_promise_wait(p);
    gst_promise_unref(p);
    gst_webrtc_session_description_free(answer);

    // Non-trickle: wait until our candidates are in the local description.
    gint64 deadline = g_get_monotonic_time() + kIceGatherTimeoutUs;
    for (;;) {
        GstWebRTCICEGatheringState st;
        g_object_get(webrtc, "ice-gathering-state", &st, nullptr);
        if (st == GST_WEBRTC_ICE_GATHERING_STATE_COMPLETE) break;
        g_mutex_lock(&ses->lock);
        g_cond_wait_until(&ses->cond, &ses->lock, g_get_monotonic_time() + 200000);
        g_mutex_unlock(&ses->lock);
        if (g_get_monotonic_time() > deadline) break; // answer with what we have
    }

    GstWebRTCSessionDescription* local = nullptr;
    g_object_get(webrtc, "local-description", &local, nullptr);
    if (!local) return {};
    gchar* text = gst_sdp_message_as_text(local->sdp);
    std::string out = text ? text : "";
    g_free(text);
    gst_webrtc_session_description_free(local);
    return out;
}

// ------------------------------------------------------------------- HTTP

struct HttpRequest {
    std::string method;
    std::string path;
    std::string body;
};

// Minimal blocking HTTP/1.1 request reader (headers capped at 16 KB, body by
// Content-Length up to 256 KB).
bool readRequest(GInputStream* in, HttpRequest* out) {
    std::string buf;
    char c;
    gssize n;
    while (buf.size() < 16384) {
        n = g_input_stream_read(in, &c, 1, nullptr, nullptr);
        if (n <= 0) return false;
        buf.push_back(c);
        if (buf.size() >= 4 && buf.compare(buf.size() - 4, 4, "\r\n\r\n") == 0) break;
    }
    size_t lineEnd = buf.find("\r\n");
    if (lineEnd == std::string::npos) return false;
    std::string reqLine = buf.substr(0, lineEnd);
    size_t sp1 = reqLine.find(' ');
    size_t sp2 = reqLine.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return false;
    out->method = reqLine.substr(0, sp1);
    out->path = reqLine.substr(sp1 + 1, sp2 - sp1 - 1);

    // Content-Length (case-insensitive scan).
    size_t contentLength = 0;
    std::string lower = buf;
    for (auto& ch : lower) ch = (char)tolower((unsigned char)ch);
    size_t clPos = lower.find("content-length:");
    if (clPos != std::string::npos)
        contentLength = strtoul(lower.c_str() + clPos + 15, nullptr, 10);
    if (contentLength > 262144) return false;

    out->body.resize(contentLength);
    size_t got = 0;
    while (got < contentLength) {
        n = g_input_stream_read(in, &out->body[got], contentLength - got, nullptr, nullptr);
        if (n <= 0) return false;
        got += (size_t)n;
    }
    return true;
}

void writeResponse(GOutputStream* out, int status, const char* statusText,
                   const std::string& extraHeaders, const std::string& body) {
    char head[512];
    snprintf(head, sizeof(head),
             "HTTP/1.1 %d %s\r\n"
             "Connection: close\r\n"
             "Access-Control-Allow-Origin: *\r\n"
             "Access-Control-Allow-Methods: POST, OPTIONS, DELETE, PATCH\r\n"
             "Access-Control-Allow-Headers: content-type,authorization\r\n"
             "Access-Control-Expose-Headers: location\r\n"
             "Content-Length: %u\r\n",
             status, statusText, (unsigned)body.size());
    std::string full = std::string(head) + extraHeaders + "\r\n" + body;
    g_output_stream_write_all(out, full.data(), full.size(), nullptr, nullptr, nullptr);
}

// POST: admit a presenter. Creates the session webrtcbin, negotiates, and
// answers 201 with the SDP + Location.
void handlePost(WhipRoom* room, const HttpRequest& req, GOutputStream* out) {
    RoomSession* ses = nullptr;
    {
        g_mutex_lock(&room->lock);
        int live = 0;
        for (RoomSession* s : room->sessions)
            if (!g_atomic_int_get(&s->dead)) live++;
        if (live >= room->maxSessions) {
            g_mutex_unlock(&room->lock);
            printf("whip-room: presenter rejected - room full (%d)\n", room->maxSessions);
            writeResponse(out, 503, "Service Unavailable", "", "room full\n");
            return;
        }
        ses = new RoomSession();
        ses->id = room->nextId++;
        g_mutex_init(&ses->lock);
        g_cond_init(&ses->cond);
        room->sessions.push_back(ses);
        g_mutex_unlock(&room->lock);
    }

    GstElement* webrtc = gst_element_factory_make("webrtcbin", nullptr);
    if (!webrtc) {
        g_atomic_int_set(&ses->dead, TRUE);
        writeResponse(out, 500, "Internal Server Error", "", "no webrtcbin\n");
        return;
    }
    g_object_set(webrtc, "bundle-policy", 3 /* max-bundle */, nullptr);
    // M23: webrtcbin's rtpjitterbuffer latency (default 200 ms) dominates the
    // receive latency budget; make it tunable for profiling/low-latency runs.
    const gchar* lat = g_getenv("VKR_WEBRTC_LATENCY_MS");
    if (lat && *lat) g_object_set(webrtc, "latency", (guint)atoi(lat), nullptr);
    ses->webrtc = webrtc;

    PadCtx* ctx = g_new0(PadCtx, 1);
    ctx->room = room;
    ctx->ses = ses;
    g_signal_connect_data(webrtc, "pad-added", G_CALLBACK(onPadAdded), ctx,
                          [](gpointer d, GClosure*) { g_free(d); }, (GConnectFlags)0);
    g_signal_connect(webrtc, "notify::ice-gathering-state",
                     G_CALLBACK(onIceGatheringNotify), ses);
    g_signal_connect(webrtc, "notify::connection-state",
                     G_CALLBACK(onConnectionStateNotify), ses);

    gst_bin_add(GST_BIN(room->pipeline), webrtc);
    gst_element_sync_state_with_parent(webrtc);

    std::string answer = negotiate(room, ses, req.body);
    if (answer.empty()) {
        printf("whip-room: session %u negotiation failed\n", ses->id);
        g_atomic_int_set(&ses->dead, TRUE);
        writeResponse(out, 500, "Internal Server Error", "", "negotiation failed\n");
        return;
    }

    char loc[64];
    snprintf(loc, sizeof(loc), "Location: /session/%u\r\nContent-Type: application/sdp\r\n",
             ses->id);
    writeResponse(out, 201, "Created", loc, answer);
    printf("whip-room: presenter %u joined (%d live)\n", ses->id, whip_room_live_count(room));
}

void handleDelete(WhipRoom* room, const HttpRequest& req, GOutputStream* out) {
    unsigned id = 0;
    if (sscanf(req.path.c_str(), "/session/%u", &id) != 1) {
        writeResponse(out, 404, "Not Found", "", "");
        return;
    }
    g_mutex_lock(&room->lock);
    for (RoomSession* s : room->sessions) {
        if (s->id == id) {
            g_atomic_int_set(&s->dead, TRUE);
            break;
        }
    }
    g_mutex_unlock(&room->lock);
    writeResponse(out, 200, "OK", "", "");
    printf("whip-room: presenter %u left\n", id);
}

struct ConnCtx {
    WhipRoom* room;
    GSocketConnection* conn;
};

gpointer connectionThread(gpointer data) {
    ConnCtx* ctx = static_cast<ConnCtx*>(data);
    GSocket* sock = g_socket_connection_get_socket(ctx->conn);
    if (sock) g_socket_set_timeout(sock, 15);
    GInputStream* in = g_io_stream_get_input_stream(G_IO_STREAM(ctx->conn));
    GOutputStream* out = g_io_stream_get_output_stream(G_IO_STREAM(ctx->conn));

    HttpRequest req;
    if (readRequest(in, &req)) {
        if (req.method == "OPTIONS") {
            writeResponse(out, 204, "No Content", "", "");
        } else if (req.method == "POST") {
            handlePost(ctx->room, req, out);
        } else if (req.method == "DELETE") {
            handleDelete(ctx->room, req, out);
        } else {
            // PATCH = trickle ICE, unsupported (we answer non-trickle).
            writeResponse(out, 405, "Method Not Allowed", "", "");
        }
    }
    g_io_stream_close(G_IO_STREAM(ctx->conn), nullptr, nullptr);
    g_object_unref(ctx->conn);
    g_free(ctx);
    return nullptr;
}

gpointer acceptThread(gpointer data) {
    WhipRoom* room = static_cast<WhipRoom*>(data);
    for (;;) {
        GError* err = nullptr;
        GSocketConnection* conn =
            g_socket_listener_accept(room->listener, nullptr, room->cancel, &err);
        if (!conn) {
            if (err) g_error_free(err);
            return nullptr; // cancelled / listener closed
        }
        ConnCtx* ctx = g_new0(ConnCtx, 1);
        ctx->room = room;
        ctx->conn = conn;
        GThread* t = g_thread_new("whip-conn", connectionThread, ctx);
        g_thread_unref(t);
    }
}

void reapSession(WhipRoom* room, RoomSession* ses) {
    for (GstElement* e : ses->branch) {
        gst_element_set_state(e, GST_STATE_NULL); // waits out streaming threads
        gst_bin_remove(GST_BIN(room->pipeline), e);
    }
    if (ses->webrtc) {
        gst_element_set_state(ses->webrtc, GST_STATE_NULL);
        gst_bin_remove(GST_BIN(room->pipeline), ses->webrtc);
    }
    if (ses->haveTrack && room->cbs.onTrackClosed)
        room->cbs.onTrackClosed(ses->trackHandle, ses->id, room->user);
    g_mutex_clear(&ses->lock);
    g_cond_clear(&ses->cond);
    delete ses;
}

} // namespace

WhipRoom* whip_room_new(GstElement* pipeline, int port, int maxSessions,
                        const WhipRoomCallbacks* cbs, void* user) {
    WhipRoom* room = new WhipRoom();
    room->pipeline = pipeline;
    room->cbs = *cbs;
    room->user = user;
    room->maxSessions = maxSessions;
    room->port = port;
    g_mutex_init(&room->lock);

    room->listener = g_socket_listener_new();
    room->cancel = g_cancellable_new();
    GError* err = nullptr;
    if (!g_socket_listener_add_inet_port(room->listener, (guint16)port, nullptr, &err)) {
        fprintf(stderr, "whip-room: cannot listen on port %d: %s\n", port,
                err ? err->message : "?");
        if (err) g_error_free(err);
        g_object_unref(room->listener);
        g_object_unref(room->cancel);
        delete room;
        return nullptr;
    }
    room->acceptThread = g_thread_new("whip-accept", acceptThread, room);
    return room;
}

void whip_room_poll(WhipRoom* room) {
    std::vector<RoomSession*> dead;
    g_mutex_lock(&room->lock);
    for (auto it = room->sessions.begin(); it != room->sessions.end();) {
        if (g_atomic_int_get(&(*it)->dead)) {
            dead.push_back(*it);
            it = room->sessions.erase(it);
        } else {
            ++it;
        }
    }
    g_mutex_unlock(&room->lock);
    for (RoomSession* s : dead) reapSession(room, s);
}

int whip_room_live_count(WhipRoom* room) {
    g_mutex_lock(&room->lock);
    int live = 0;
    for (RoomSession* s : room->sessions)
        if (!g_atomic_int_get(&s->dead)) live++;
    g_mutex_unlock(&room->lock);
    return live;
}

void whip_room_free(WhipRoom* room) {
    g_cancellable_cancel(room->cancel);
    g_socket_listener_close(room->listener);
    g_thread_join(room->acceptThread);
    g_mutex_lock(&room->lock);
    std::vector<RoomSession*> all;
    all.swap(room->sessions);
    g_mutex_unlock(&room->lock);
    for (RoomSession* s : all) reapSession(room, s);
    g_object_unref(room->listener);
    g_object_unref(room->cancel);
    g_mutex_clear(&room->lock);
    delete room;
}
