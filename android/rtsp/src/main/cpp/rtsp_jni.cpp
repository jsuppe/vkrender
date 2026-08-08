// M27: RTSP multi-source receiver on Android.
//
// The shared client/rtsp_room.cpp core owns the ingest (one rtspsrc pipeline
// per camera, HTTP control API, backoff reconnect, stall watchdog). This file
// supplies the Android sink: each source's H.264 track is depayloaded to
// access units and fed to its OWN AMediaCodec (an M18 AirplayVideoSink
// instance) decoding into its OWN renderer slot Surface (M22
// acquireInputSurfaceSlot) - so each camera lands on its own cube face,
// exactly like Windows (per-connection ring) and Linux (per-waylandsink
// surface).
//
// This is the WHIP module's sink almost verbatim, which is the point:
// rtsp_room hands over an ENCODED RTP pad, the same contract as whip_room's
// onTrack, precisely so this zero-copy MediaCodec path is reusable. Decoding
// centrally in the room would have forced a raw-frame handoff and thrown it
// away.
//
// RTSP is PULL, so unlike the WHIP module there is nothing to advertise over
// mDNS and no PIN - the camera authenticates us via credentials in the URL.
// Sources come from the Java side (persisted list / Intent extra) through
// nativeAddSource, or at runtime over the room's HTTP control API.

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
#include "rtsp_room.h"

#define VKR_LOG(...) __android_log_print(ANDROID_LOG_INFO, "vkrender", __VA_ARGS__)
#define VKR_ERR(...) __android_log_print(ANDROID_LOG_ERROR, "vkrender", __VA_ARGS__)

extern "C" {
// Base + transport plugins (shared with the decoder module).
GST_PLUGIN_STATIC_DECLARE(coreelements);
GST_PLUGIN_STATIC_DECLARE(app);
GST_PLUGIN_STATIC_DECLARE(rtp);
GST_PLUGIN_STATIC_DECLARE(rtpmanager);
GST_PLUGIN_STATIC_DECLARE(udp);
GST_PLUGIN_STATIC_DECLARE(tcp);
GST_PLUGIN_STATIC_DECLARE(typefindfunctions);
GST_PLUGIN_STATIC_DECLARE(videoparsersbad);
// The RTSP client itself.
GST_PLUGIN_STATIC_DECLARE(rtsp);
}

namespace {

JavaVM* g_vm = nullptr;
jobject g_service = nullptr;      // RtspService global ref
jmethodID g_midAcquire = nullptr; // Surface acquireSlotSurface(int sourceId)
jmethodID g_midRelease = nullptr; // void releaseSlotSurface(int sourceId)

RtspRoom* g_room = nullptr;

void registerPlugins() {
    GST_PLUGIN_STATIC_REGISTER(coreelements);
    GST_PLUGIN_STATIC_REGISTER(app);
    GST_PLUGIN_STATIC_REGISTER(rtp);
    GST_PLUGIN_STATIC_REGISTER(rtpmanager);
    GST_PLUGIN_STATIC_REGISTER(udp);
    GST_PLUGIN_STATIC_REGISTER(tcp);
    GST_PLUGIN_STATIC_REGISTER(typefindfunctions);
    GST_PLUGIN_STATIC_REGISTER(videoparsersbad);
    GST_PLUGIN_STATIC_REGISTER(rtsp);
}

void gstLogToLogcat(GstDebugCategory*, GstDebugLevel level, const gchar* file, const gchar*,
                    gint line, GObject*, GstDebugMessage* message, gpointer) {
    __android_log_print(level <= GST_LEVEL_ERROR ? ANDROID_LOG_ERROR : ANDROID_LOG_WARN,
                        "vkrender-rtsp", "%s:%d %s", file, line,
                        gst_debug_message_get(message));
}

// One source: its MediaCodec sink, renderer slot Surface, and the pull thread
// moving access units from the appsink into the codec.
struct Stream {
    unsigned sourceId = 0;
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

// JNI upcall: ask RtspService for a renderer slot Surface for this source.
ANativeWindow* acquireSlotWindow(unsigned sourceId) {
    JNIEnv* env = nullptr;
    if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return nullptr;
    jobject jsurface = env->CallObjectMethod(g_service, g_midAcquire, (jint)sourceId);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        jsurface = nullptr;
    }
    ANativeWindow* window = jsurface ? ANativeWindow_fromSurface(env, jsurface) : nullptr;
    if (jsurface) env->DeleteLocalRef(jsurface);
    return window; // thread stays attached; it's a GStreamer streaming thread
}

void releaseSlot(unsigned sourceId) {
    JNIEnv* env = nullptr;
    if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return;
    env->CallVoidMethod(g_service, g_midRelease, (jint)sourceId);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

// rtsp_room callback: a source's video pad appeared. H.264 only (the
// MediaCodec path); anything else is discarded, and the room's connect
// watchdog then retries the URL.
void* onStream(GstElement* pipeline, GstPad* pad, unsigned sourceId, GstElement**, int,
               void*) {
    GstCaps* caps = gst_pad_get_current_caps(pad);
    if (!caps) caps = gst_pad_query_caps(pad, nullptr);
    const gchar* enc = nullptr;
    if (caps && gst_caps_get_size(caps) > 0)
        enc = gst_structure_get_string(gst_caps_get_structure(caps, 0), "encoding-name");
    bool isH264 = enc && strcmp(enc, "H264") == 0;
    if (caps) gst_caps_unref(caps);
    if (!isH264) {
        VKR_ERR("rtsp: [source %u] stream is %s, not H264 - discarding", sourceId,
                enc ? enc : "?");
        return nullptr;
    }

    ANativeWindow* window = acquireSlotWindow(sourceId);
    if (!window) {
        VKR_ERR("rtsp: [source %u] no renderer slot surface - discarding", sourceId);
        return nullptr;
    }

    Stream* st = new Stream();
    st->sourceId = sourceId;
    st->window = window;
    st->sink.start(window);
    st->stats.init(pipeline, sourceId, statsToLogcat);
    st->stats.attachProbe(pad, vkr::kStageIngress); // post-jitterbuffer RTP

    GstElement* depay = gst_element_factory_make("rtph264depay", nullptr);
    GstElement* parse = gst_element_factory_make("h264parse", nullptr);
    GstElement* sink = gst_element_factory_make("appsink", nullptr);
    if (!depay || !parse || !sink) {
        VKR_ERR("rtsp: [source %u] missing elements (rtph264depay/h264parse/appsink)",
                sourceId);
        st->sink.stop();
        ANativeWindow_release(window);
        releaseSlot(sourceId);
        delete st;
        return nullptr;
    }
    // config-interval -1: re-send SPS/PPS on every keyframe, so MediaCodec can
    // configure itself after a mid-stream reconnect without waiting for the
    // camera's next parameter-set interval.
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
    st->appsink = sink;

    // Parsed H.264 AUs at the appsink door (pre-MediaCodec).
    GstPad* statPad = gst_element_get_static_pad(sink, "sink");
    st->stats.attachProbe(statPad, vkr::kStageSink);
    gst_object_unref(statPad);

    // Pull thread: appsink AUs -> this source's MediaCodec.
    st->running = true;
    st->pull = std::thread([st]() {
        while (st->running.load() && !st->sink.surfaceDead()) {
            GstSample* sample =
                gst_app_sink_try_pull_sample(GST_APP_SINK(st->appsink), 100 * GST_MSECOND);
            if (!sample) continue;
            st->stats.onPulled();
            GstBuffer* buffer = gst_sample_get_buffer(sample);
            GstMapInfo map;
            if (buffer && gst_buffer_map(buffer, &map, GST_MAP_READ)) {
                int64_t pts =
                    GST_BUFFER_PTS_IS_VALID(buffer) ? (int64_t)(GST_BUFFER_PTS(buffer) / 1000)
                                                    : 0;
                st->sink.feed(map.data, (int)map.size, pts);
                gst_buffer_unmap(buffer, &map);
            }
            gst_sample_unref(sample);
        }
    });

    VKR_LOG("rtsp: [source %u] H.264 -> renderer slot surface", sourceId);
    return st;
}

// rtsp_room callback: source removed or reconnecting - the pipeline is already
// at NULL, so stop the codec and give the renderer slot back. Called before
// every reconnect too, so it must pair cleanly with a later onStream.
void onStreamClosed(void* handle, unsigned sourceId, void*) {
    Stream* st = static_cast<Stream*>(handle);
    if (!st) return;
    st->running = false;
    if (st->pull.joinable()) st->pull.join();
    st->sink.stop();
    if (st->window) ANativeWindow_release(st->window);
    releaseSlot(sourceId);
    VKR_LOG("rtsp: [source %u] closed", sourceId);
    delete st;
}

} // namespace

extern "C" JNIEXPORT jboolean JNICALL Java_dev_vkrender_rtsp_RtspService_nativeStart(
    JNIEnv* env, jobject thiz, jint controlPort, jint maxSources) {
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
        VKR_ERR("rtsp: RtspService slot methods not found");
        return JNI_FALSE;
    }

    RtspRoomCallbacks cbs{};
    cbs.onStream = onStream;
    cbs.onStreamClosed = onStreamClosed;
    g_room = rtsp_room_new((int)controlPort, (int)maxSources, &cbs, nullptr);
    if (!g_room) {
        VKR_ERR("rtsp: room failed to bind control port %d", (int)controlPort);
        return JNI_FALSE;
    }
    VKR_LOG("rtsp: room up, control API on port %d, room for %d sources", (int)controlPort,
            (int)maxSources);
    return JNI_TRUE;
}

// Seed a source from the Java side (persisted list / Intent extra). Runtime
// add/remove also works over the room's HTTP control API.
extern "C" JNIEXPORT jint JNICALL Java_dev_vkrender_rtsp_RtspService_nativeAddSource(
    JNIEnv* env, jobject, jstring jurl) {
    if (!g_room || !jurl) return 0;
    const char* url = env->GetStringUTFChars(jurl, nullptr);
    unsigned id = url ? rtsp_room_add(g_room, url) : 0;
    if (url) env->ReleaseStringUTFChars(jurl, url);
    return (jint)id;
}

extern "C" JNIEXPORT jboolean JNICALL Java_dev_vkrender_rtsp_RtspService_nativeRemoveSource(
    JNIEnv*, jobject, jint id) {
    if (!g_room) return JNI_FALSE;
    return rtsp_room_remove(g_room, (unsigned)id) ? JNI_TRUE : JNI_FALSE;
}

// Build pending sources, pump each source's bus, run the reconnect backoff and
// the stall watchdog. Called ~2 Hz from the service worker thread.
extern "C" JNIEXPORT void JNICALL
Java_dev_vkrender_rtsp_RtspService_nativePoll(JNIEnv*, jobject) {
    if (g_room) rtsp_room_poll(g_room);
}

extern "C" JNIEXPORT jstring JNICALL
Java_dev_vkrender_rtsp_RtspService_nativeListSources(JNIEnv* env, jobject) {
    if (!g_room) return env->NewStringUTF("[]");
    char* json = rtsp_room_list_json(g_room);
    jstring out = env->NewStringUTF(json ? json : "[]");
    g_free(json);
    return out;
}

extern "C" JNIEXPORT void JNICALL
Java_dev_vkrender_rtsp_RtspService_nativeStop(JNIEnv*, jobject) {
    if (g_room) {
        rtsp_room_free(g_room); // tears down every source (incl. slot release)
        g_room = nullptr;
    }
    VKR_LOG("rtsp: stopped");
}
