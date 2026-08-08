// M27: multi-source RTSP ingest. See rtsp_room.h for the design and the
// control-API surface.

#include "rtsp_room.h"

#include <gio/gio.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

// Reconnect backoff: cameras reboot, networks blip. Start fast so a transient
// drop is invisible, cap so a permanently-dead URL costs nothing.
const gint64 kRetryFirstMs = 1000;
const gint64 kRetryMaxMs = 30000;

// A dead RTSP source usually does NOT produce an error. Measured against
// mediamtx: kill the publisher and the server holds the reader session open,
// so rtspsrc sees a healthy connection that simply stops delivering - no
// ERROR, no EOS, and the source sits in "live" forever with a frozen tile.
// Real cameras do the same on a PoE flap or an encoder crash. So liveness is
// judged by FRAME ARRIVAL, not by connection state.
const gint64 kStallMsDefault = 8000;
// Separately: a URL that connects but never produces a video pad (wrong path,
// audio-only, publisher not up yet) must not hang in "connecting" forever.
const gint64 kConnectMsDefault = 15000;

enum SourceState {
    kPending,    // queued, pipeline not built yet
    kConnecting, // pipeline PLAYING, no decoded pad yet
    kLive,       // onStream fired, frames expected
    kRetrying,   // failed; waiting for retryAtUs
    kRemoving,   // queued for teardown
};

const char* stateName(SourceState s) {
    switch (s) {
        case kPending: return "pending";
        case kConnecting: return "connecting";
        case kLive: return "live";
        case kRetrying: return "retrying";
        default: return "removing";
    }
}

struct RtspSource {
    unsigned id = 0;
    std::string url;
    SourceState state = kPending;
    GstElement* pipeline = nullptr;
    void* streamHandle = nullptr;
    gint64 retryAtUs = 0;
    int retryMs = 0;
    guint64 failures = 0;
    std::string lastError;
    // Liveness. `lastFrameUs` is written from a pad probe on the streaming
    // thread and read in poll(); `startedUs` marks when this attempt began.
    std::atomic<gint64> lastFrameUs{0};
    gint64 startedUs = 0;
};

} // namespace

struct RtspRoom {
    GMutex lock;
    std::vector<RtspSource*> sources;
    unsigned nextId = 1;
    int maxSources = 8;
    RtspRoomCallbacks cbs{};
    void* user = nullptr;

    GSocketListener* listener = nullptr;
    GThread* acceptThread = nullptr;
    volatile gint quit = 0;

    // rtspsrc tuning, read once from the environment.
    guint latencyMs = 200;
    std::string protocols;
    gint64 stallUs = kStallMsDefault * 1000;
    gint64 connectUs = kConnectMsDefault * 1000;
};

namespace {

// ------------------------------------------------------------------ sources

// Configure rtspsrc for live, low-latency, fail-fast behaviour.
void configureSrc(RtspRoom* room, GstElement* src) {
    GObjectClass* klass = G_OBJECT_GET_CLASS(src);
    // rtspsrc defaults to 2000 ms of jitter buffer - far too much for a
    // compositor that is otherwise sub-100 ms. VKR_RTSP_LATENCY_MS overrides
    // (mirrors VKR_WEBRTC_LATENCY_MS).
    if (g_object_class_find_property(klass, "latency"))
        g_object_set(src, "latency", room->latencyMs, nullptr);
    // Drop rather than stall when the buffer overruns: live wins over complete.
    if (g_object_class_find_property(klass, "drop-on-latency"))
        g_object_set(src, "drop-on-latency", TRUE, nullptr);
    // Notice a silently dead camera instead of hanging forever.
    if (g_object_class_find_property(klass, "timeout"))
        g_object_set(src, "timeout", (guint64)5000000, nullptr); // us
    if (g_object_class_find_property(klass, "tcp-timeout"))
        g_object_set(src, "tcp-timeout", (guint64)5000000, nullptr);
    if (!room->protocols.empty() && g_object_class_find_property(klass, "protocols")) {
        // Accepts the nick ("tcp", "udp-mcast", ...) via gst_util_set_object_arg.
        gst_util_set_object_arg(G_OBJECT(src), "protocols", room->protocols.c_str());
    }
}

struct PadCtx {
    RtspRoom* room;
    RtspSource* src;
};

// Liveness tick. Runs on the streaming thread for every decoded frame, so it
// stays a single relaxed store - poll() does the actual judging.
GstPadProbeReturn onFrameProbe(GstPad*, GstPadProbeInfo*, gpointer data) {
    static_cast<RtspSource*>(data)->lastFrameUs.store(g_get_monotonic_time(),
                                                      std::memory_order_relaxed);
    return GST_PAD_PROBE_OK;
}

// rtspsrc exposes one pad per SDP stream, still RTP-encoded. We want the video
// one; audio pads are left unlinked (rtspsrc drops them harmlessly).
void onPadAdded(GstElement*, GstPad* pad, gpointer data) {
    PadCtx* ctx = static_cast<PadCtx*>(data);
    GstCaps* caps = gst_pad_get_current_caps(pad);
    if (!caps) caps = gst_pad_query_caps(pad, nullptr);
    bool isVideo = false;
    if (caps && gst_caps_get_size(caps) > 0) {
        GstStructure* st = gst_caps_get_structure(caps, 0);
        const char* name = gst_structure_get_name(st);
        const char* media = gst_structure_get_string(st, "media");
        // application/x-rtp with media=video. Fall back to a raw/encoded video
        // caps name in case a source ever hands over an already-parsed pad.
        isVideo = (name && strncmp(name, "video/", 6) == 0) ||
                  (media && strcmp(media, "video") == 0);
    }
    if (caps) gst_caps_unref(caps);
    if (!isVideo) return;

    RtspRoom* room = ctx->room;
    RtspSource* s = ctx->src;
    g_mutex_lock(&room->lock);
    bool wanted = (s->state == kConnecting) && !s->streamHandle;
    g_mutex_unlock(&room->lock);
    if (!wanted) return; // already have a video stream for this source

    // Arm the stall watchdog BEFORE the sink branch exists, so the very first
    // frame counts and a source that never delivers is caught.
    s->lastFrameUs.store(g_get_monotonic_time(), std::memory_order_relaxed);
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, onFrameProbe, s, nullptr);

    GstElement* elements[8] = {};
    void* handle = room->cbs.onStream ? room->cbs.onStream(s->pipeline, pad, s->id, elements,
                                                           8, room->user)
                                      : nullptr;
    g_mutex_lock(&room->lock);
    s->streamHandle = handle;
    s->state = kLive;
    s->retryMs = 0; // a good connection resets the backoff
    g_mutex_unlock(&room->lock);
    printf("rtsp-room: [source %u] live - %s\n", s->id, s->url.c_str());
}

// Build (or rebuild) one source's private pipeline. Caller must NOT hold the
// lock: element construction can call back in.
void buildSource(RtspRoom* room, RtspSource* s) {
    char name[64];
    snprintf(name, sizeof(name), "rtsp-source-%u", s->id);
    GstElement* pipeline = gst_pipeline_new(name);
    GstElement* src = gst_element_factory_make("rtspsrc", nullptr);
    if (!pipeline || !src) {
        fprintf(stderr, "rtsp-room: [source %u] rtspsrc missing\n", s->id);
        if (pipeline) gst_object_unref(pipeline);
        if (src) gst_object_unref(src);
        g_mutex_lock(&room->lock);
        s->state = kRetrying;
        s->lastError = "rtspsrc missing";
        s->retryMs = (int)kRetryMaxMs;
        s->retryAtUs = g_get_monotonic_time() + kRetryMaxMs * 1000;
        g_mutex_unlock(&room->lock);
        return;
    }

    g_object_set(src, "location", s->url.c_str(), nullptr);
    configureSrc(room, src);
    PadCtx* ctx = new PadCtx{room, s};
    g_signal_connect_data(src, "pad-added", G_CALLBACK(onPadAdded), ctx,
                          [](gpointer d, GClosure*) { delete static_cast<PadCtx*>(d); },
                          (GConnectFlags)0);
    gst_bin_add(GST_BIN(pipeline), src);

    g_mutex_lock(&room->lock);
    s->pipeline = pipeline;
    s->state = kConnecting;
    s->startedUs = g_get_monotonic_time();
    s->lastFrameUs.store(0, std::memory_order_relaxed);
    g_mutex_unlock(&room->lock);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    printf("rtsp-room: [source %u] connecting to %s\n", s->id, s->url.c_str());
}

// Tear a source's pipeline down completely. Caller must NOT hold the lock
// (onStreamClosed is platform code). Safe to call repeatedly.
void teardownSource(RtspRoom* room, RtspSource* s) {
    g_mutex_lock(&room->lock);
    GstElement* pipeline = s->pipeline;
    void* handle = s->streamHandle;
    s->pipeline = nullptr;
    s->streamHandle = nullptr;
    g_mutex_unlock(&room->lock);

    if (pipeline) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
    }
    // After GST_STATE_NULL the sink branch is quiesced, so platform code can
    // safely drop its producer connection here.
    if (handle && room->cbs.onStreamClosed)
        room->cbs.onStreamClosed(handle, s->id, room->user);
}

// A source failed. Schedule the next attempt with exponential backoff.
void scheduleRetry(RtspRoom* room, RtspSource* s, const char* why) {
    g_mutex_lock(&room->lock);
    s->failures++;
    s->lastError = why ? why : "?";
    s->retryMs = s->retryMs ? (int)(s->retryMs * 2) : (int)kRetryFirstMs;
    if (s->retryMs > (int)kRetryMaxMs) s->retryMs = (int)kRetryMaxMs;
    s->retryAtUs = g_get_monotonic_time() + (gint64)s->retryMs * 1000;
    s->state = kRetrying;
    int delay = s->retryMs;
    g_mutex_unlock(&room->lock);
    fprintf(stderr, "rtsp-room: [source %u] %s - retrying in %d ms\n", s->id, why, delay);
}

// Drain one source's bus. Returns a reason string if the source died.
const char* pumpBus(RtspSource* s, std::string* detail) {
    if (!s->pipeline) return nullptr;
    GstBus* bus = gst_element_get_bus(s->pipeline);
    if (!bus) return nullptr;
    const char* died = nullptr;
    for (;;) {
        GstMessage* msg = gst_bus_pop_filtered(
            bus, (GstMessageType)(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
        if (!msg) break;
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            GError* err = nullptr;
            gst_message_parse_error(msg, &err, nullptr);
            if (detail) *detail = err && err->message ? err->message : "error";
            if (err) g_error_free(err);
            died = "stream error";
        } else {
            if (detail) *detail = "end of stream";
            died = "stream ended";
        }
        gst_message_unref(msg);
    }
    gst_object_unref(bus);
    return died;
}

// --------------------------------------------------------------------- HTTP

struct HttpRequest {
    std::string method;
    std::string path;
    std::string body;
};

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

    std::string lower = buf;
    for (auto& ch : lower) ch = (char)tolower((unsigned char)ch);
    size_t contentLength = 0;
    size_t clPos = lower.find("content-length:");
    if (clPos != std::string::npos)
        contentLength = strtoul(lower.c_str() + clPos + 15, nullptr, 10);
    if (contentLength > 65536) return false;

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
             "Content-Type: application/json\r\n"
             "Access-Control-Allow-Origin: *\r\n"
             "Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS\r\n"
             "Access-Control-Allow-Headers: content-type\r\n"
             "Content-Length: %u\r\n",
             status, statusText, (unsigned)body.size());
    std::string full = std::string(head) + extraHeaders + "\r\n" + body;
    g_output_stream_write_all(out, full.data(), full.size(), nullptr, nullptr, nullptr);
}

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// Body is either a bare URL or {"url":"..."} - accept both so `curl -d
// rtsp://...` works as readily as a JSON client.
std::string urlFromBody(const std::string& body) {
    std::string t = trim(body);
    if (t.empty() || t[0] != '{') return t;
    size_t k = t.find("\"url\"");
    if (k == std::string::npos) return "";
    size_t colon = t.find(':', k + 5);
    if (colon == std::string::npos) return "";
    size_t q1 = t.find('"', colon);
    if (q1 == std::string::npos) return "";
    size_t q2 = t.find('"', q1 + 1);
    if (q2 == std::string::npos) return "";
    return t.substr(q1 + 1, q2 - q1 - 1);
}

void handleRequest(RtspRoom* room, const HttpRequest& req, GOutputStream* out) {
    if (req.method == "OPTIONS") {
        writeResponse(out, 204, "No Content", "", "");
        return;
    }
    if (req.method == "GET") {
        char* json = rtsp_room_list_json(room);
        writeResponse(out, 200, "OK", "", json ? json : "[]");
        g_free(json);
        return;
    }
    if (req.method == "POST") {
        std::string url = urlFromBody(req.body);
        if (url.empty()) {
            writeResponse(out, 400, "Bad Request", "",
                          "{\"error\":\"expected a URL, or {\\\"url\\\":\\\"...\\\"}\"}\n");
            return;
        }
        unsigned id = rtsp_room_add(room, url.c_str());
        if (!id) {
            writeResponse(out, 503, "Service Unavailable", "",
                          "{\"error\":\"room is full\"}\n");
            return;
        }
        char body[128], loc[128];
        snprintf(body, sizeof(body), "{\"id\":%u}\n", id);
        snprintf(loc, sizeof(loc), "Location: /sources/%u\r\n", id);
        writeResponse(out, 201, "Created", loc, body);
        return;
    }
    if (req.method == "DELETE") {
        // /sources/<id>
        size_t slash = req.path.find_last_of('/');
        unsigned id = slash == std::string::npos
                          ? 0u
                          : (unsigned)strtoul(req.path.c_str() + slash + 1, nullptr, 10);
        if (id && rtsp_room_remove(room, id))
            writeResponse(out, 204, "No Content", "", "");
        else
            writeResponse(out, 404, "Not Found", "", "{\"error\":\"no such source\"}\n");
        return;
    }
    writeResponse(out, 405, "Method Not Allowed", "", "{\"error\":\"unsupported\"}\n");
}

struct ConnCtx {
    RtspRoom* room;
    GSocketConnection* conn;
};

gpointer connectionThread(gpointer data) {
    ConnCtx* ctx = static_cast<ConnCtx*>(data);
    GInputStream* in = g_io_stream_get_input_stream(G_IO_STREAM(ctx->conn));
    GOutputStream* out = g_io_stream_get_output_stream(G_IO_STREAM(ctx->conn));
    HttpRequest req;
    if (readRequest(in, &req)) handleRequest(ctx->room, req, out);
    g_io_stream_close(G_IO_STREAM(ctx->conn), nullptr, nullptr);
    g_object_unref(ctx->conn);
    delete ctx;
    return nullptr;
}

gpointer acceptThread(gpointer data) {
    RtspRoom* room = static_cast<RtspRoom*>(data);
    while (!g_atomic_int_get(&room->quit)) {
        GSocketConnection* conn =
            g_socket_listener_accept(room->listener, nullptr, nullptr, nullptr);
        if (!conn) break;
        ConnCtx* ctx = new ConnCtx{room, conn};
        GThread* t = g_thread_new("rtsp-ctl", connectionThread, ctx);
        if (t) g_thread_unref(t);
    }
    return nullptr;
}

} // namespace

// ----------------------------------------------------------------- room API

RtspRoom* rtsp_room_new(int controlPort, int maxSources, const RtspRoomCallbacks* cbs,
                        void* user) {
    RtspRoom* room = new RtspRoom();
    g_mutex_init(&room->lock);
    room->maxSources = maxSources > 0 ? maxSources : 8;
    if (cbs) room->cbs = *cbs;
    room->user = user;

    if (const char* lat = getenv("VKR_RTSP_LATENCY_MS")) {
        int v = atoi(lat);
        if (v >= 0) room->latencyMs = (guint)v;
    }
    if (const char* proto = getenv("VKR_RTSP_PROTOCOLS")) room->protocols = proto;
    if (const char* stall = getenv("VKR_RTSP_STALL_MS")) {
        int v = atoi(stall);
        if (v > 0) room->stallUs = (gint64)v * 1000;
    }
    if (const char* ct = getenv("VKR_RTSP_CONNECT_MS")) {
        int v = atoi(ct);
        if (v > 0) room->connectUs = (gint64)v * 1000;
    }

    if (controlPort > 0) {
        room->listener = g_socket_listener_new();
        GError* err = nullptr;
        if (!g_socket_listener_add_inet_port(room->listener, (guint16)controlPort, nullptr,
                                             &err)) {
            fprintf(stderr, "rtsp-room: cannot bind control port %d: %s\n", controlPort,
                    err && err->message ? err->message : "?");
            if (err) g_error_free(err);
            g_object_unref(room->listener);
            g_mutex_clear(&room->lock);
            delete room;
            return nullptr;
        }
        room->acceptThread = g_thread_new("rtsp-accept", acceptThread, room);
    }
    return room;
}

unsigned rtsp_room_add(RtspRoom* room, const char* url) {
    if (!room || !url || !*url) return 0;
    g_mutex_lock(&room->lock);
    int configured = 0;
    for (RtspSource* s : room->sources)
        if (s->state != kRemoving) configured++;
    if (configured >= room->maxSources) {
        g_mutex_unlock(&room->lock);
        return 0;
    }
    RtspSource* s = new RtspSource();
    s->id = room->nextId++;
    s->url = url;
    s->state = kPending;
    room->sources.push_back(s);
    unsigned id = s->id;
    g_mutex_unlock(&room->lock);
    printf("rtsp-room: [source %u] added %s\n", id, url);
    return id;
}

int rtsp_room_remove(RtspRoom* room, unsigned id) {
    if (!room) return 0;
    g_mutex_lock(&room->lock);
    int found = 0;
    for (RtspSource* s : room->sources) {
        if (s->id == id && s->state != kRemoving) {
            s->state = kRemoving; // poll() does the teardown
            found = 1;
            break;
        }
    }
    g_mutex_unlock(&room->lock);
    return found;
}

void rtsp_room_poll(RtspRoom* room) {
    if (!room) return;

    // Snapshot the work list under the lock, then act without it: buildSource,
    // teardownSource and the platform callbacks must never run locked.
    std::vector<RtspSource*> toBuild, toTeardown, toDelete;
    std::vector<std::pair<RtspSource*, std::string>> toRetry;
    gint64 now = g_get_monotonic_time();

    g_mutex_lock(&room->lock);
    for (RtspSource* s : room->sources) {
        switch (s->state) {
            case kPending: toBuild.push_back(s); break;
            case kRemoving: toTeardown.push_back(s); break;
            case kRetrying:
                if (now >= s->retryAtUs) toBuild.push_back(s);
                break;
            default: break;
        }
    }
    g_mutex_unlock(&room->lock);

    // Bus pump: a source that errored moves to Retrying with its pipeline torn
    // down, so the next poll rebuilds it after the backoff.
    g_mutex_lock(&room->lock);
    std::vector<RtspSource*> active;
    for (RtspSource* s : room->sources)
        if (s->state == kConnecting || s->state == kLive) active.push_back(s);
    g_mutex_unlock(&room->lock);
    for (RtspSource* s : active) {
        std::string detail;
        if (const char* why = pumpBus(s, &detail)) {
            teardownSource(room, s);
            scheduleRetry(room, s, detail.empty() ? why : detail.c_str());
            continue;
        }
        // Watchdogs. A source can fail without saying so (see kStallMsDefault),
        // so judge it on frames arriving, not on the connection being open.
        g_mutex_lock(&room->lock);
        SourceState st = s->state;
        gint64 started = s->startedUs;
        g_mutex_unlock(&room->lock);
        gint64 last = s->lastFrameUs.load(std::memory_order_relaxed);

        if (st == kLive && last && now - last > room->stallUs) {
            teardownSource(room, s);
            scheduleRetry(room, s, "stalled (no frames)");
        } else if (st == kConnecting && started && now - started > room->connectUs) {
            teardownSource(room, s);
            scheduleRetry(room, s, "no video within connect timeout");
        }
    }

    for (RtspSource* s : toBuild) {
        if (s->pipeline) teardownSource(room, s); // retry path: drop the old one
        buildSource(room, s);
    }

    for (RtspSource* s : toTeardown) {
        teardownSource(room, s);
        printf("rtsp-room: [source %u] removed\n", s->id);
        toDelete.push_back(s);
    }
    if (!toDelete.empty()) {
        g_mutex_lock(&room->lock);
        for (RtspSource* dead : toDelete) {
            for (size_t i = 0; i < room->sources.size(); ++i) {
                if (room->sources[i] == dead) {
                    room->sources.erase(room->sources.begin() + i);
                    break;
                }
            }
        }
        g_mutex_unlock(&room->lock);
        for (RtspSource* dead : toDelete) delete dead;
    }
}

int rtsp_room_live_count(RtspRoom* room) {
    if (!room) return 0;
    g_mutex_lock(&room->lock);
    int n = 0;
    for (RtspSource* s : room->sources)
        if (s->state == kLive) n++;
    g_mutex_unlock(&room->lock);
    return n;
}

char* rtsp_room_list_json(RtspRoom* room) {
    if (!room) return g_strdup("[]");
    GString* out = g_string_new("[");
    g_mutex_lock(&room->lock);
    bool first = true;
    for (RtspSource* s : room->sources) {
        if (!first) g_string_append_c(out, ',');
        first = false;
        char* url = g_strescape(s->url.c_str(), nullptr);
        char* err = g_strescape(s->lastError.c_str(), nullptr);
        g_string_append_printf(out,
                               "{\"id\":%u,\"url\":\"%s\",\"state\":\"%s\","
                               "\"failures\":%llu,\"lastError\":\"%s\"}",
                               s->id, url ? url : "", stateName(s->state),
                               (unsigned long long)s->failures, err ? err : "");
        g_free(url);
        g_free(err);
    }
    g_mutex_unlock(&room->lock);
    g_string_append_c(out, ']');
    return g_string_free(out, FALSE);
}

void rtsp_room_free(RtspRoom* room) {
    if (!room) return;
    g_atomic_int_set(&room->quit, 1);
    if (room->listener) g_socket_listener_close(room->listener);
    if (room->acceptThread) g_thread_join(room->acceptThread);
    if (room->listener) g_object_unref(room->listener);

    g_mutex_lock(&room->lock);
    std::vector<RtspSource*> all = room->sources;
    room->sources.clear();
    g_mutex_unlock(&room->lock);
    for (RtspSource* s : all) {
        teardownSource(room, s);
        delete s;
    }
    g_mutex_clear(&room->lock);
    delete room;
}
