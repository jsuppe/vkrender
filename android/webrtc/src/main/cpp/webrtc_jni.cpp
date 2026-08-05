// M20: WebRTC (WHIP) receiver on Android. GStreamer whipserversrc runs the
// WHIP HTTP endpoint + full WebRTC stack; the negotiated H.264 track is
// depayloaded to access units and fed to AMediaCodec decoding into the
// renderer-provided Surface (reusing M18's AirplayVideoSink). Same
// whipserversrc core as the Windows/Linux receivers - shared design.

#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <atomic>
#include <cstring>
#include <string>
#include <thread>

#include <gst/app/gstappsink.h>
#include <gst/gst.h>

#include "airplay_video.h" // reused: H.264 AUs -> MediaCodec into Surface

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
// WebRTC stack.
GST_PLUGIN_STATIC_DECLARE(webrtc);
GST_PLUGIN_STATIC_DECLARE(nice);
GST_PLUGIN_STATIC_DECLARE(dtls);
GST_PLUGIN_STATIC_DECLARE(srtp);
GST_PLUGIN_STATIC_DECLARE(rtpmanagerbad);
GST_PLUGIN_STATIC_DECLARE(rswebrtc);
GST_PLUGIN_STATIC_DECLARE(rsrtp);
GST_PLUGIN_STATIC_DECLARE(webrtchttp);
}

namespace {

vkr::AirplayVideoSink g_sink;
GstElement* g_pipeline = nullptr;
std::atomic<bool> g_ready{false};

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
    GST_PLUGIN_STATIC_REGISTER(rswebrtc);
    GST_PLUGIN_STATIC_REGISTER(rsrtp);
    GST_PLUGIN_STATIC_REGISTER(webrtchttp);
}

void gstLogToLogcat(GstDebugCategory*, GstDebugLevel level, const gchar* file, const gchar*,
                    gint line, GObject*, GstDebugMessage* message, gpointer) {
    __android_log_print(level <= GST_LEVEL_ERROR ? ANDROID_LOG_ERROR : ANDROID_LOG_WARN,
                        "vkrender-webrtc", "%s:%d %s", file, line,
                        gst_debug_message_get(message));
}

// whipserversrc exposes video_%u / audio_%u (application/x-rtp) once the peer
// track arrives; for the video track, depay -> parse (byte-stream, in-band
// SPS/PPS) -> appsink emitting access units for the codec.
void onPadAdded(GstElement*, GstPad* pad, gpointer) {
    gchar* name = gst_pad_get_name(pad);
    bool isVideo = name && strncmp(name, "video", 5) == 0;
    VKR_LOG("webrtc: track '%s'%s", name ? name : "?", isVideo ? " (video)" : "");
    g_free(name);
    if (!isVideo) return;

    GstElement* depay = gst_element_factory_make("rtph264depay", nullptr);
    GstElement* parse = gst_element_factory_make("h264parse", nullptr);
    GstElement* sink = gst_element_factory_make("appsink", nullptr);
    if (!depay || !parse || !sink) {
        VKR_ERR("webrtc: failed to make depay/parse/appsink");
        return;
    }
    g_object_set(parse, "config-interval", -1, nullptr);
    GstCaps* caps =
        gst_caps_from_string("video/x-h264,stream-format=byte-stream,alignment=au");
    g_object_set(sink, "caps", caps, "emit-signals", FALSE, "max-buffers", 8, "drop", FALSE,
                 "sync", FALSE, nullptr);
    gst_caps_unref(caps);

    gst_bin_add_many(GST_BIN(g_pipeline), depay, parse, sink, nullptr);
    gst_element_sync_state_with_parent(depay);
    gst_element_sync_state_with_parent(parse);
    gst_element_sync_state_with_parent(sink);
    gst_element_link_many(depay, parse, sink, nullptr);
    GstPad* dsink = gst_element_get_static_pad(depay, "sink");
    gst_pad_link(pad, dsink);
    gst_object_unref(dsink);

    // Pull thread: appsink AUs -> the MediaCodec sink.
    std::thread([sink]() {
        while (g_ready.load() && !g_sink.surfaceDead()) {
            GstSample* sample =
                gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 100 * GST_MSECOND);
            if (!sample) continue;
            GstBuffer* buffer = gst_sample_get_buffer(sample);
            GstMapInfo map;
            if (buffer && gst_buffer_map(buffer, &map, GST_MAP_READ)) {
                int64_t pts = GST_BUFFER_PTS_IS_VALID(buffer)
                                  ? (int64_t)(GST_BUFFER_PTS(buffer) / 1000)
                                  : 0;
                g_sink.feed(map.data, (int)map.size, pts);
                gst_buffer_unmap(buffer, &map);
            }
            gst_sample_unref(sample);
        }
    }).detach();
}

} // namespace

extern "C" JNIEXPORT jboolean JNICALL
Java_dev_vkrender_webrtc_WebrtcService_nativeStart(JNIEnv* env, jobject, jobject jsurface,
                                                   jint port) {
    static bool inited = false;
    if (!inited) {
        gst_init(nullptr, nullptr);
        registerPlugins();
        gst_debug_remove_log_function(gst_debug_log_default);
        gst_debug_add_log_function(gstLogToLogcat, nullptr, nullptr);
        gst_debug_set_default_threshold(GST_LEVEL_WARNING);
        inited = true;
    }
    ANativeWindow* window = ANativeWindow_fromSurface(env, jsurface);
    if (!window) {
        VKR_ERR("webrtc: no surface");
        return JNI_FALSE;
    }
    g_sink.start(window);

    g_pipeline = gst_pipeline_new("webrtc");
    GstElement* src = gst_element_factory_make("whipserversrc", "whip");
    if (!src) {
        VKR_ERR("webrtc: whipserversrc unavailable");
        return JNI_FALSE;
    }
    // Constrain to H.264 so the depay->MediaCodec path always applies.
    g_object_set(src, "video-codecs", "H264", nullptr);
    GObject* signaller = nullptr;
    g_object_get(src, "signaller", &signaller, nullptr);
    if (signaller) {
        char addr[64];
        snprintf(addr, sizeof(addr), "http://0.0.0.0:%d", (int)port);
        g_object_set(signaller, "host-addr", addr, nullptr);
        g_object_unref(signaller);
    }
    g_signal_connect(src, "pad-added", G_CALLBACK(onPadAdded), nullptr);
    gst_bin_add(GST_BIN(g_pipeline), src);

    g_ready = true;
    if (gst_element_set_state(g_pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        VKR_ERR("webrtc: pipeline failed to start");
        return JNI_FALSE;
    }
    VKR_LOG("webrtc: WHIP endpoint on port %d - POST an SDP offer to stream", (int)port);
    return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_dev_vkrender_webrtc_WebrtcService_nativeSurfaceDead(JNIEnv*, jobject) {
    return g_sink.surfaceDead() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_dev_vkrender_webrtc_WebrtcService_nativeStop(JNIEnv*, jobject) {
    g_ready = false;
    if (g_pipeline) {
        gst_element_set_state(g_pipeline, GST_STATE_NULL);
        gst_object_unref(g_pipeline);
        g_pipeline = nullptr;
    }
    g_sink.stop();
    VKR_LOG("webrtc: stopped");
}
