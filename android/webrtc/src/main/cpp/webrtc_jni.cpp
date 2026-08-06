// M20/M22: WebRTC (WHIP) multi-presenter receiver on Android.
//
// The shared client/whip_room.cpp core runs the WHIP ingest (own HTTP
// listener, one webrtcbin per presenter - replacing the single-producer
// whipserversrc and its Rust plugin closure). This file supplies the Android
// sink: each presenter's H.264 track is depayloaded to access units and fed
// to its OWN AMediaCodec (an M18 AirplayVideoSink instance) decoding into its
// OWN renderer slot Surface (M22 acquireInputSurfaceSlot) - so each presenter
// lands on their own cube face, exactly like Windows (per-connection ring)
// and Linux (per-waylandsink surface).

#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <atomic>
#include <cstring>
#include <thread>

#include <gst/app/gstappsink.h>
#include <gst/gst.h>

#include "airplay_video.h" // reused: H.264 AUs -> MediaCodec into Surface
#include "recv_stats.h"
#include "whip_room.h"

#define VKR_LOG(...) __android_log_print(ANDROID_LOG_INFO, "vkrender", __VA_ARGS__)
#define VKR_ERR(...) __android_log_print(ANDROID_LOG_ERROR, "vkrender", __VA_ARGS__)

extern "C" {
// Base + transport plugins (shared with the decoder module).
GST_PLUGIN_STATIC_DECLARE(coreelements);
GST_PLUGIN_STATIC_DECLARE(app);
GST_PLUGIN_STATIC_DECLARE(rtp);
GST_PLUGIN_STATIC_DECLARE(rtpmanager);
GST_PLUGIN_STATIC_DECLARE(udp);
GST_PLUGIN_STATIC_DECLARE(typefindfunctions);
GST_PLUGIN_STATIC_DECLARE(videoparsersbad);
// WebRTC stack (C plugins only - the Rust closure is gone with whipserversrc).
GST_PLUGIN_STATIC_DECLARE(webrtc);
GST_PLUGIN_STATIC_DECLARE(nice);
GST_PLUGIN_STATIC_DECLARE(dtls);
GST_PLUGIN_STATIC_DECLARE(srtp);
GST_PLUGIN_STATIC_DECLARE(rtpmanagerbad);
}

namespace {

JavaVM* g_vm = nullptr;
jobject g_service = nullptr; // WebrtcService global ref
jmethodID g_midAcquire = nullptr; // Surface acquireSlotSurface(int sessionId)
jmethodID g_midRelease = nullptr; // void releaseSlotSurface(int sessionId)

GstElement* g_pipeline = nullptr;
WhipRoom* g_room = nullptr;

void registerPlugins() {
    GST_PLUGIN_STATIC_REGISTER(coreelements);
    GST_PLUGIN_STATIC_REGISTER(app);
    GST_PLUGIN_STATIC_REGISTER(rtp);
    GST_PLUGIN_STATIC_REGISTER(rtpmanager);
    GST_PLUGIN_STATIC_REGISTER(udp);
    GST_PLUGIN_STATIC_REGISTER(typefindfunctions);
    GST_PLUGIN_STATIC_REGISTER(videoparsersbad);
    GST_PLUGIN_STATIC_REGISTER(webrtc);
    GST_PLUGIN_STATIC_REGISTER(nice);
    GST_PLUGIN_STATIC_REGISTER(dtls);
    GST_PLUGIN_STATIC_REGISTER(srtp);
    GST_PLUGIN_STATIC_REGISTER(rtpmanagerbad);
}

void gstLogToLogcat(GstDebugCategory*, GstDebugLevel level, const gchar* file, const gchar*,
                    gint line, GObject*, GstDebugMessage* message, gpointer) {
    __android_log_print(level <= GST_LEVEL_ERROR ? ANDROID_LOG_ERROR : ANDROID_LOG_WARN,
                        "vkrender-webrtc", "%s:%d %s", file, line,
                        gst_debug_message_get(message));
}

// One presenter: their MediaCodec sink, renderer slot Surface, and the pull
// thread moving access units from the appsink into the codec.
struct Track {
    unsigned sessionId = 0;
    vkr::AirplayVideoSink sink;
    ANativeWindow* window = nullptr;
    GstElement* appsink = nullptr;
    std::atomic<bool> running{false};
    std::thread pull;
    vkr::RecvStats stats; // M23: behind-live latency + drop-by-cause counters
};

void statsToLogcat(const char* line) {
    __android_log_print(ANDROID_LOG_INFO, "vkrender", "%s", line);
}

// JNI upcall: ask WebrtcService for a renderer slot Surface for this session.
ANativeWindow* acquireSlotWindow(unsigned sessionId) {
    JNIEnv* env = nullptr;
    if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return nullptr;
    jobject jsurface = env->CallObjectMethod(g_service, g_midAcquire, (jint)sessionId);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        jsurface = nullptr;
    }
    ANativeWindow* window = jsurface ? ANativeWindow_fromSurface(env, jsurface) : nullptr;
    if (jsurface) env->DeleteLocalRef(jsurface);
    return window; // thread stays attached; it's a GStreamer streaming thread
}

void releaseSlot(unsigned sessionId) {
    JNIEnv* env = nullptr;
    if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return;
    env->CallVoidMethod(g_service, g_midRelease, (jint)sessionId);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

// whip_room callback: a presenter's video track appeared. H.264 only (the
// MediaCodec path); anything else is discarded by the room.
void* onTrack(GstElement* pipeline, GstPad* pad, unsigned sessionId, GstElement** elements,
              int maxElements, void*) {
    GstCaps* caps = gst_pad_get_current_caps(pad);
    const gchar* enc = nullptr;
    if (caps && gst_caps_get_size(caps) > 0)
        enc = gst_structure_get_string(gst_caps_get_structure(caps, 0), "encoding-name");
    bool isH264 = enc && strcmp(enc, "H264") == 0;
    if (caps) gst_caps_unref(caps);
    if (!isH264) {
        VKR_ERR("webrtc: [session %u] track is %s, not H264 - discarding", sessionId,
                enc ? enc : "?");
        return nullptr;
    }

    ANativeWindow* window = acquireSlotWindow(sessionId);
    if (!window) {
        VKR_ERR("webrtc: [session %u] no renderer slot surface - discarding", sessionId);
        return nullptr;
    }

    Track* trk = new Track();
    trk->sessionId = sessionId;
    trk->window = window;
    trk->sink.start(window);
    trk->stats.init(pipeline, sessionId, statsToLogcat);
    trk->stats.attachProbe(pad, vkr::kStageIngress); // post-jitterbuffer

    GstElement* depay = gst_element_factory_make("rtph264depay", nullptr);
    GstElement* parse = gst_element_factory_make("h264parse", nullptr);
    GstElement* sink = gst_element_factory_make("appsink", nullptr);
    g_object_set(parse, "config-interval", -1, nullptr);
    GstCaps* aucaps =
        gst_caps_from_string("video/x-h264,stream-format=byte-stream,alignment=au");
    g_object_set(sink, "caps", aucaps, "emit-signals", FALSE, "max-buffers", 8, "drop", FALSE,
                 "sync", FALSE, nullptr);
    gst_caps_unref(aucaps);

    gst_bin_add_many(GST_BIN(pipeline), depay, parse, sink, nullptr);
    gst_element_sync_state_with_parent(depay);
    gst_element_sync_state_with_parent(parse);
    gst_element_sync_state_with_parent(sink);
    gst_element_link_many(depay, parse, sink, nullptr);
    GstPad* dsink = gst_element_get_static_pad(depay, "sink");
    gst_pad_link(pad, dsink);
    gst_object_unref(dsink);
    trk->appsink = sink;

    // Parsed H.264 AUs at the appsink door (pre-MediaCodec).
    GstPad* statPad = gst_element_get_static_pad(sink, "sink");
    trk->stats.attachProbe(statPad, vkr::kStageSink);
    gst_object_unref(statPad);

    // Pull thread: appsink AUs -> this presenter's MediaCodec.
    trk->running = true;
    trk->pull = std::thread([trk]() {
        while (trk->running.load() && !trk->sink.surfaceDead()) {
            GstSample* sample =
                gst_app_sink_try_pull_sample(GST_APP_SINK(trk->appsink), 100 * GST_MSECOND);
            if (!sample) continue;
            trk->stats.onPulled();
            GstBuffer* buffer = gst_sample_get_buffer(sample);
            GstMapInfo map;
            if (buffer && gst_buffer_map(buffer, &map, GST_MAP_READ)) {
                int64_t pts = GST_BUFFER_PTS_IS_VALID(buffer)
                                  ? (int64_t)(GST_BUFFER_PTS(buffer) / 1000)
                                  : 0;
                trk->sink.feed(map.data, (int)map.size, pts);
                gst_buffer_unmap(buffer, &map);
            }
            gst_sample_unref(sample);
        }
    });

    VKR_LOG("webrtc: [session %u] H.264 track -> renderer slot surface", sessionId);
    if (maxElements > 2) {
        elements[0] = depay;
        elements[1] = parse;
        elements[2] = sink;
    }
    return trk;
}

// whip_room callback: presenter left, branch already at NULL and out of the
// bin - stop their codec and give the renderer slot back.
void onTrackClosed(void* handle, unsigned sessionId, void*) {
    Track* trk = static_cast<Track*>(handle);
    if (!trk) return;
    trk->running = false;
    if (trk->pull.joinable()) trk->pull.join();
    trk->sink.stop();
    if (trk->window) ANativeWindow_release(trk->window);
    releaseSlot(sessionId);
    VKR_LOG("webrtc: [session %u] closed", sessionId);
    delete trk;
}

} // namespace

extern "C" JNIEXPORT jboolean JNICALL
Java_dev_vkrender_webrtc_WebrtcService_nativeStart(JNIEnv* env, jobject thiz, jint port) {
    static bool inited = false;
    if (!inited) {
        gst_init(nullptr, nullptr);
        registerPlugins();
        gst_debug_remove_log_function(gst_debug_log_default);
        gst_debug_add_log_function(gstLogToLogcat, nullptr, nullptr);
        gst_debug_set_default_threshold(GST_LEVEL_WARNING);
        env->GetJavaVM(&g_vm);
        inited = true;
    }
    if (g_service) env->DeleteGlobalRef(g_service);
    g_service = env->NewGlobalRef(thiz);
    jclass cls = env->GetObjectClass(thiz);
    g_midAcquire = env->GetMethodID(cls, "acquireSlotSurface", "(I)Landroid/view/Surface;");
    g_midRelease = env->GetMethodID(cls, "releaseSlotSurface", "(I)V");
    if (!g_midAcquire || !g_midRelease) {
        VKR_ERR("webrtc: WebrtcService slot methods not found");
        return JNI_FALSE;
    }

    g_pipeline = gst_pipeline_new("webrtc-room");
    gst_element_set_state(g_pipeline, GST_STATE_PLAYING);

    WhipRoomCallbacks cbs{};
    cbs.onTrack = onTrack;
    cbs.onTrackClosed = onTrackClosed;
    g_room = whip_room_new(g_pipeline, (int)port, 12, &cbs, nullptr);
    if (!g_room) {
        VKR_ERR("webrtc: whip_room failed to listen on %d", (int)port);
        return JNI_FALSE;
    }
    VKR_LOG("webrtc: WHIP room on port %d - space for 12 presenters", (int)port);
    return JNI_TRUE;
}

// Reap departed presenters + surface pipeline errors. Called ~1 Hz from the
// service worker thread.
extern "C" JNIEXPORT void JNICALL
Java_dev_vkrender_webrtc_WebrtcService_nativePoll(JNIEnv*, jobject) {
    if (!g_pipeline || !g_room) return;
    GstBus* bus = gst_element_get_bus(g_pipeline);
    for (;;) {
        GstMessage* msg = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
        if (!msg) break;
        GError* err = nullptr;
        gst_message_parse_error(msg, &err, nullptr);
        VKR_ERR("webrtc: pipeline error from %s: %s", GST_OBJECT_NAME(GST_MESSAGE_SRC(msg)),
                err ? err->message : "?");
        if (err) g_error_free(err);
        gst_message_unref(msg);
    }
    gst_object_unref(bus);
    whip_room_poll(g_room);
}

extern "C" JNIEXPORT void JNICALL
Java_dev_vkrender_webrtc_WebrtcService_nativeStop(JNIEnv*, jobject) {
    if (g_room) {
        whip_room_free(g_room); // tears down every session (incl. slot release)
        g_room = nullptr;
    }
    if (g_pipeline) {
        gst_element_set_state(g_pipeline, GST_STATE_NULL);
        gst_object_unref(g_pipeline);
        g_pipeline = nullptr;
    }
    VKR_LOG("webrtc: stopped");
}
