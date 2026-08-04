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
    gst_debug_remove_log_function(gst_debug_log_default);
    gst_debug_add_log_function(gstLogToLogcat, nullptr, nullptr);
    gst_debug_set_default_threshold(GST_LEVEL_WARNING);
    for (const char* f : {"rtspsrc", "rtpbin", "rtpjitterbuffer", "rtph264depay", "rtph265depay",
                          "h264parse", "h265parse", "parsebin", "appsink", "typefind"}) {
        GstElementFactory* factory = gst_element_factory_find(f);
        if (!factory)
            VKR_ERR("gst: element '%s' MISSING from registry", f);
        else
            gst_object_unref(factory);
    }
    done = true;
}

} // namespace

bool GstSurfaceDecoder::buildPipeline(const std::string& url) {
    GError* err = nullptr;
    // parsebin's exposed pad sits behind an internal capsfilter pinned to
    // whatever the parser first negotiated (alignment=nal for RTP input), so
    // it cannot be linked straight to an au-only appsink. Instead we leave it
    // unlinked here and bridge it in onParsebinPad() through our own outer
    // h26xparse, which is free to re-align nal -> au for MediaCodec.
    std::string desc = "rtspsrc name=src protocols=tcp latency=100 location=" + url +
                       " ! parsebin name=parse  appsink name=sink max-buffers=8 drop=false"
                       " sync=false";
    pipeline_ = gst_parse_launch(desc.c_str(), &err);
    if (!pipeline_ || err) {
        VKR_ERR("gst_parse_launch failed: %s", err ? err->message : "?");
        if (err) g_error_free(err);
        return false;
    }
    if (GstElement* parse = gst_bin_get_by_name(GST_BIN(pipeline_), "parse")) {
        g_signal_connect(parse, "pad-added",
                         G_CALLBACK(+[](GstElement*, GstPad* pad, gpointer self) {
                             static_cast<GstSurfaceDecoder*>(self)->onParsebinPad(pad);
                         }),
                         this);
        g_signal_connect(parse, "unknown-type",
                         G_CALLBACK(+[](GstElement*, GstPad*, GstCaps* caps, gpointer) {
                             gchar* s = gst_caps_to_string(caps);
                             VKR_ERR("decoder: parsebin unknown-type: %s", s ? s : "?");
                             g_free(s);
                         }),
                         nullptr);
        gst_object_unref(parse);
    }
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

// A parsebin stream appeared: plug the matching outer parser (config-interval
// -1 keeps SPS/PPS in-band so MediaCodec can start/restart from any point)
// and complete the chain to the appsink.
void GstSurfaceDecoder::onParsebinPad(GstPad* pad) {
    GstCaps* caps = gst_pad_get_current_caps(pad);
    if (!caps) caps = gst_pad_query_caps(pad, nullptr);
    const GstStructure* s = caps && gst_caps_get_size(caps) ? gst_caps_get_structure(caps, 0)
                                                            : nullptr;
    const char* name = s ? gst_structure_get_name(s) : "";
    gchar* capsStr = caps ? gst_caps_to_string(caps) : nullptr;
    VKR_LOG("decoder: parsebin stream: %.120s", capsStr ? capsStr : "(no caps)");
    g_free(capsStr);

    const char* parserFactory = nullptr;
    if (g_str_equal(name, "video/x-h264"))
        parserFactory = "h264parse";
    else if (g_str_equal(name, "video/x-h265"))
        parserFactory = "h265parse";
    if (caps) gst_caps_unref(caps);
    if (!parserFactory) {
        VKR_WARN("decoder: ignoring non-h26x stream (%s)", name);
        return;
    }

    GstElement* parser = gst_element_factory_make(parserFactory, nullptr);
    if (!parser) {
        VKR_ERR("decoder: %s unavailable", parserFactory);
        return;
    }
    g_object_set(parser, "config-interval", -1, nullptr);
    gst_bin_add(GST_BIN(pipeline_), parser);
    gst_element_sync_state_with_parent(parser);

    GstPad* parserSink = gst_element_get_static_pad(parser, "sink");
    GstPadLinkReturn lr = gst_pad_link(pad, parserSink);
    gst_object_unref(parserSink);
    if (lr != GST_PAD_LINK_OK || !gst_element_link(parser, appsink_))
        VKR_ERR("decoder: failed to bridge parsebin -> %s -> appsink (link %d)", parserFactory,
                lr);
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

    const char* swName = g_str_equal(mime, "video/avc") ? "c2.android.avc.decoder"
                                                        : "c2.android.hevc.decoder";
    char qemu[92] = {0};
    __system_property_get("ro.boot.qemu", qemu);
    const bool emulator = qemu[0] == '1';

    // Hardware first (software on the emulator - goldfish stalls in surface
    // mode), then the software codec as fallback: vendors cap concurrent
    // hardware sessions, and with many client apps decoding at once (M12)
    // create/configure/start can fail on the instance limit.
    for (int attempt = 0; attempt < 2 && !codec_; ++attempt) {
        bool wantSw = emulator || attempt == 1;
        codec_ = wantSw ? AMediaCodec_createCodecByName(swName)
                        : AMediaCodec_createDecoderByType(mime);
        if (!codec_) continue;
        AMediaFormat* format = AMediaFormat_new();
        AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, mime);
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, w > 0 ? w : 1280);
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, h > 0 ? h : 720);
        media_status_t ms = AMediaCodec_configure(codec_, format, window, nullptr, 0);
        AMediaFormat_delete(format);
        if (ms == AMEDIA_OK && AMediaCodec_start(codec_) == AMEDIA_OK) {
            if (wantSw) VKR_LOG("decoder: using software codec %s", swName);
            break;
        }
        VKR_WARN("decoder: %s codec configure/start failed (%d), %s", wantSw ? "software" : "hardware",
                 ms, attempt == 0 ? "trying software" : "giving up");
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
        if (emulator) break; // sw already failed; nothing left to try
    }
    if (!codec_) {
        VKR_ERR("no usable codec for %s - surface may be dead", mime);
        surfaceDead_ = true;
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

int GstSurfaceDecoder::run(const std::string& url, ANativeWindow* window,
                           const std::atomic<bool>* abortFlag) {
    gstInitOnce();
    uint32_t backoffMs = 1000;
    surfaceDead_ = false;
    auto aborted = [abortFlag] { return abortFlag && abortFlag->load(); };

    while (!surfaceDead_ && !aborted()) {
        VKR_LOG("decoder: connecting to %s (gstreamer)", url.c_str());
        if (!buildPipeline(url)) {
            teardownPipeline();
            std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
            backoffMs = backoffMs * 2 > 10000 ? 10000 : backoffMs * 2;
            continue;
        }

        bool streamOk = true;
        bool firstSample = true;
        while (streamOk && !surfaceDead_ && !aborted()) {
            if (GstMessage* m = gst_bus_pop_filtered(bus_, GST_MESSAGE_ERROR)) {
                GError* e = nullptr;
                gchar* dbg = nullptr;
                gst_message_parse_error(m, &e, &dbg);
                VKR_LOG("decoder: stream interrupted (%s | %s), decoded=%llu",
                        e ? e->message : "?", dbg ? dbg : "-",
                        (unsigned long long)decodedFrames_);
                if (e) g_error_free(e);
                g_free(dbg);
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
        if (!surfaceDead_ && !aborted()) std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    VKR_LOG("decoder: session over (%s), decoded=%llu", aborted() ? "aborted" : "surface dead",
            (unsigned long long)decodedFrames_);
    return 0;
}

} // namespace vkr
