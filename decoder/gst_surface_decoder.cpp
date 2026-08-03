#include "gst_surface_decoder.h"

#include <android/log.h>
#include <android/native_window.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <sys/system_properties.h>

#include <chrono>
#include <cstring>
#include <thread>

#include "log.h"

#include <gst/app/gstappsink.h>

// Static GStreamer build: declare the linked-in plugins.
extern "C" {
GST_PLUGIN_STATIC_DECLARE(coreelements);
GST_PLUGIN_STATIC_DECLARE(app);
GST_PLUGIN_STATIC_DECLARE(rtsp);
GST_PLUGIN_STATIC_DECLARE(rtp);
GST_PLUGIN_STATIC_DECLARE(rtpmanager);
GST_PLUGIN_STATIC_DECLARE(udp);
GST_PLUGIN_STATIC_DECLARE(tcp);
GST_PLUGIN_STATIC_DECLARE(playback);
GST_PLUGIN_STATIC_DECLARE(typefindfunctions);
GST_PLUGIN_STATIC_DECLARE(videoparsersbad);
}

namespace vkr {

namespace {

void gstLogToLogcat(GstDebugCategory* category, GstDebugLevel level, const gchar* file,
                    const gchar*, gint line, GObject*, GstDebugMessage* message, gpointer) {
    if (level > GST_LEVEL_WARNING) return;
    __android_log_print(level <= GST_LEVEL_ERROR ? ANDROID_LOG_ERROR : ANDROID_LOG_WARN,
                        "vkrender-gst", "%s:%d %s: %s", file, line,
                        category ? gst_debug_category_get_name(category) : "?",
                        gst_debug_message_get(message));
}

void gstInitOnce() {
    static bool done = false;
    if (done) return;
    gst_init(nullptr, nullptr);
    GST_PLUGIN_STATIC_REGISTER(coreelements);
    GST_PLUGIN_STATIC_REGISTER(app);
    GST_PLUGIN_STATIC_REGISTER(rtsp);
    GST_PLUGIN_STATIC_REGISTER(rtp);
    GST_PLUGIN_STATIC_REGISTER(rtpmanager);
    GST_PLUGIN_STATIC_REGISTER(udp);
    GST_PLUGIN_STATIC_REGISTER(tcp);
    GST_PLUGIN_STATIC_REGISTER(playback);
    GST_PLUGIN_STATIC_REGISTER(typefindfunctions);
    GST_PLUGIN_STATIC_REGISTER(videoparsersbad);
    gst_debug_remove_default_log_function();
    gst_debug_add_log_function(gstLogToLogcat, nullptr, nullptr);
    done = true;
}

// parsebin plugs the parser dynamically; force periodic in-band SPS/PPS so
// MediaCodec can start (and restart) from any point.
void onDeepElementAdded(GstBin*, GstBin*, GstElement* element, gpointer) {
    gchar* name = gst_element_get_name(element);
    if (name && (strstr(name, "h264parse") || strstr(name, "h265parse")))
        g_object_set(element, "config-interval", -1, nullptr);
    g_free(name);
}

} // namespace

bool GstSurfaceDecoder::buildPipeline(const std::string& url) {
    GError* err = nullptr;
    std::string desc = "rtspsrc name=src protocols=tcp latency=100 location=" + url +
                       " ! parsebin ! appsink name=sink max-buffers=8 drop=false sync=false";
    pipeline_ = gst_parse_launch(desc.c_str(), &err);
    if (!pipeline_ || err) {
        VKR_ERR("gst_parse_launch failed: %s", err ? err->message : "?");
        if (err) g_error_free(err);
        return false;
    }
    g_signal_connect(pipeline_, "deep-element-added", G_CALLBACK(onDeepElementAdded), nullptr);
    appsink_ = gst_bin_get_by_name(GST_BIN(pipeline_), "sink");
    GstCaps* caps = gst_caps_from_string(
        "video/x-h264, stream-format=byte-stream, alignment=au; "
        "video/x-h265, stream-format=byte-stream, alignment=au");
    gst_app_sink_set_caps(GST_APP_SINK(appsink_), caps);
    gst_caps_unref(caps);

    bus_ = gst_element_get_bus(pipeline_);
    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        VKR_ERR("pipeline refused to start");
        return false;
    }
    return true;
}

void GstSurfaceDecoder::teardownPipeline() {
    if (pipeline_) gst_element_set_state(pipeline_, GST_STATE_NULL);
    if (bus_) gst_object_unref(bus_);
    if (appsink_) gst_object_unref(appsink_);
    if (pipeline_) gst_object_unref(pipeline_);
    bus_ = nullptr;
    appsink_ = nullptr;
    pipeline_ = nullptr;
}

bool GstSurfaceDecoder::ensureCodec(GstCaps* caps, ANativeWindow* window) {
    const GstStructure* s = gst_caps_get_structure(caps, 0);
    const char* mime =
        g_str_equal(gst_structure_get_name(s), "video/x-h265") ? "video/hevc" : "video/avc";
    int w = 0, h = 0;
    gst_structure_get_int(s, "width", &w);
    gst_structure_get_int(s, "height", &h);
    if (codec_ && mime == codecMime_ && w == codecW_ && h == codecH_) return true;
    closeCodec();

    VKR_LOG("decoder: stream: %s %dx%d -> renderer surface (gstreamer transport)", mime, w, h);

    char qemu[92] = {0};
    __system_property_get("ro.boot.qemu", qemu);
    if (qemu[0] == '1') {
        const char* swName = g_str_equal(mime, "video/avc") ? "c2.android.avc.decoder"
                                                            : "c2.android.hevc.decoder";
        codec_ = AMediaCodec_createCodecByName(swName);
        if (codec_) VKR_LOG("decoder: emulator detected, using %s", swName);
    }
    if (!codec_) codec_ = AMediaCodec_createDecoderByType(mime);
    if (!codec_) {
        VKR_ERR("AMediaCodec create failed (%s)", mime);
        return false;
    }
    AMediaFormat* format = AMediaFormat_new();
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, mime);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, w > 0 ? w : 1280);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, h > 0 ? h : 720);
    media_status_t ms = AMediaCodec_configure(codec_, format, window, nullptr, 0);
    AMediaFormat_delete(format);
    if (ms != AMEDIA_OK) {
        VKR_ERR("AMediaCodec_configure(renderer surface) failed: %d", ms);
        surfaceDead_ = true;
        return false;
    }
    if (AMediaCodec_start(codec_) != AMEDIA_OK) {
        VKR_ERR("AMediaCodec_start failed");
        return false;
    }
    codecMime_ = mime;
    codecW_ = w;
    codecH_ = h;
    return true;
}

void GstSurfaceDecoder::closeCodec() {
    if (codec_) {
        AMediaCodec_stop(codec_);
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
    }
    codecMime_.clear();
}

bool GstSurfaceDecoder::drainDecoder() {
    for (;;) {
        AMediaCodecBufferInfo info;
        ssize_t idx = AMediaCodec_dequeueOutputBuffer(codec_, &info, 0);
        if (idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER) return true;
        if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED ||
            idx == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED)
            continue;
        if (idx < AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
            VKR_ERR("dequeueOutputBuffer error %zd - surface likely dead", idx);
            surfaceDead_ = true;
            return false;
        }
        if (idx < 0) return true;
        media_status_t rs = AMediaCodec_releaseOutputBuffer(codec_, (size_t)idx, info.size != 0);
        if (rs != AMEDIA_OK) {
            surfaceDead_ = true;
            return false;
        }
        if (info.size != 0 && (++decodedFrames_ % 300) == 1)
            VKR_LOG("decoder: frame %llu -> surface (pts %lld us)",
                    (unsigned long long)decodedFrames_, (long long)info.presentationTimeUs);
    }
}

bool GstSurfaceDecoder::feedAccessUnit(GstBuffer* buffer) {
    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) return true;
    bool queued = false;
    for (int attempts = 0; attempts < 200 && !queued; ++attempts) {
        ssize_t idx = AMediaCodec_dequeueInputBuffer(codec_, 10000);
        if (idx >= 0) {
            size_t cap = 0;
            uint8_t* dst = AMediaCodec_getInputBuffer(codec_, (size_t)idx, &cap);
            size_t n = map.size <= cap ? map.size : cap;
            memcpy(dst, map.data, n);
            int64_t ptsUs =
                GST_BUFFER_PTS_IS_VALID(buffer) ? (int64_t)(GST_BUFFER_PTS(buffer) / 1000) : 0;
            AMediaCodec_queueInputBuffer(codec_, (size_t)idx, 0, n, ptsUs, 0);
            ++queuedPackets_;
            queued = true;
            break;
        }
        if (!drainDecoder()) break;
    }
    gst_buffer_unmap(buffer, &map);
    return drainDecoder();
}

int GstSurfaceDecoder::run(const std::string& url, ANativeWindow* window) {
    gstInitOnce();
    uint32_t backoffMs = 1000;
    surfaceDead_ = false;

    while (!surfaceDead_) {
        VKR_LOG("decoder: connecting to %s (gstreamer)", url.c_str());
        if (!buildPipeline(url)) {
            teardownPipeline();
            std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
            backoffMs = backoffMs * 2 > 10000 ? 10000 : backoffMs * 2;
            continue;
        }

        bool streamOk = true;
        bool firstSample = true;
        while (streamOk && !surfaceDead_) {
            if (GstMessage* m = gst_bus_pop_filtered(bus_, GST_MESSAGE_ERROR)) {
                GError* e = nullptr;
                gst_message_parse_error(m, &e, nullptr);
                VKR_LOG("decoder: stream interrupted (%s), decoded=%llu", e ? e->message : "?",
                        (unsigned long long)decodedFrames_);
                if (e) g_error_free(e);
                gst_message_unref(m);
                streamOk = false;
                break;
            }
            GstSample* sample =
                gst_app_sink_try_pull_sample(GST_APP_SINK(appsink_), 100 * GST_MSECOND);
            if (!sample) {
                if (gst_app_sink_is_eos(GST_APP_SINK(appsink_))) streamOk = false;
                continue;
            }
            if (firstSample) {
                firstSample = false;
                backoffMs = 1000;
            }
            GstBuffer* buffer = gst_sample_get_buffer(sample);
            GstCaps* caps = gst_sample_get_caps(sample);
            if (buffer && caps && ensureCodec(caps, window)) {
                if (!feedAccessUnit(buffer)) streamOk = false;
            } else if (surfaceDead_) {
                streamOk = false;
            }
            gst_sample_unref(sample);
        }
        teardownPipeline();
        closeCodec();
        if (!surfaceDead_) std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    VKR_LOG("decoder: session over (surface dead), decoded=%llu",
            (unsigned long long)decodedFrames_);
    return 0;
}

} // namespace vkr
