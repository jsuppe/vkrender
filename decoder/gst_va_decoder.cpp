#include "gst_va_decoder.h"

#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <thread>

#include "log.h"

#include <gst/app/gstappsink.h>
#include <gst/va/gstva.h>
#include <gst/va/gstvautils.h>
#include <gst/video/video.h>
#include <va/va_drmcommon.h>

namespace vkr {

namespace {

void sendPhase(IpcChannel& ch, proto::StreamPhase phase) {
    proto::StreamStatusMsg st{phase};
    ch.send(proto::MsgType::StreamStatus, st);
}

void gstInitOnce() {
    static bool done = false;
    if (done) return;
    // The gst-va driver allowlist rejects unusual drivers (e.g. the NVIDIA
    // VAAPI shim); the project supports them, so opt in. No-overwrite: an
    // explicit user setting wins.
    setenv("GST_VA_ALL_DRIVERS", "1", 0);
    gst_init(nullptr, nullptr);
    done = true;
}

} // namespace

GstVaDecoder::GstVaDecoder(const proto::HelloMsg&, uint32_t maxHeldFrames)
    : maxHeld_(maxHeldFrames < 2 ? 2 : maxHeldFrames) {
    gstInitOnce();
}

GstVaDecoder::~GstVaDecoder() {
    releaseConsumed(UINT64_MAX);
    teardownPipeline();
}

bool GstVaDecoder::buildPipeline(const std::string& url) {
    GError* err = nullptr;
    std::string desc =
        "rtspsrc name=src protocols=tcp latency=100 location=" + url +
        " ! decodebin3 ! appsink name=sink max-buffers=4 drop=false sync=false";
    pipeline_ = gst_parse_launch(desc.c_str(), &err);
    if (!pipeline_ || err) {
        VKR_ERR("gst_parse_launch failed: %s", err ? err->message : "?");
        if (err) g_error_free(err);
        return false;
    }
    appsink_ = gst_bin_get_by_name(GST_BIN(pipeline_), "sink");
    // Constrain to VA surfaces so the decoder never downloads to system
    // memory; NV12 is what the shim/mesa produce for 8-bit streams.
    GstCaps* caps = gst_caps_from_string("video/x-raw(memory:VAMemory), format=NV12");
    gst_app_sink_set_caps(GST_APP_SINK(appsink_), caps);
    gst_caps_unref(caps);

    bus_ = gst_element_get_bus(pipeline_);
    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        VKR_ERR("pipeline refused to start");
        return false;
    }
    return true;
}

void GstVaDecoder::teardownPipeline() {
    // Held buffers belong to the pipeline's pool; drop them before the
    // pipeline (the renderer's dma-buf imports keep the memory alive).
    releaseConsumed(UINT64_MAX);
    announced_.clear();
    headerSent_ = false;
    if (pipeline_) gst_element_set_state(pipeline_, GST_STATE_NULL);
    if (bus_) gst_object_unref(bus_);
    if (appsink_) gst_object_unref(appsink_);
    if (gstVaDisplay_) gst_object_unref(gstVaDisplay_);
    if (pipeline_) gst_object_unref(pipeline_);
    bus_ = nullptr;
    appsink_ = nullptr;
    gstVaDisplay_ = nullptr;
    display_ = nullptr;
    pipeline_ = nullptr;
}

bool GstVaDecoder::resolveVaDisplay() {
    if (display_) return true;
    GstContext* ctx = gst_element_get_context(pipeline_, "gst.va.display.handle");
    if (!ctx) return false;
    const GstStructure* s = gst_context_get_structure(ctx);
    GstObject* obj = nullptr;
    gst_structure_get(s, "gst-display", GST_TYPE_OBJECT, &obj, nullptr);
    gst_context_unref(ctx);
    if (!obj) return false;
    gstVaDisplay_ = obj; // keep the ref
    display_ = gst_va_display_get_va_dpy(GST_VA_DISPLAY(obj));
    VKR_LOG("decoder: VA display resolved from gst context");
    return display_ != nullptr;
}

void GstVaDecoder::releaseConsumed(uint64_t consumedValue) {
    while (!held_.empty() && held_.front().readyValue <= consumedValue) {
        gst_buffer_unref(held_.front().buffer);
        held_.pop_front();
    }
}

bool GstVaDecoder::pollRenderer(IpcChannel& ch) {
    while (auto msg = ch.tryRecv()) {
        if (msg->type == proto::MsgType::Bye) {
            VKR_LOG("decoder: renderer said goodbye");
            return false;
        }
        if (msg->type == proto::MsgType::FrameConsumed) {
            if (const auto* fc = msg->as<proto::FrameConsumedMsg>())
                releaseConsumed(fc->readyValue);
        }
    }
    return ch.connected();
}

bool GstVaDecoder::backoffWait(IpcChannel& ch, uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 100) {
        if (!pollRenderer(ch)) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return true;
}

bool GstVaDecoder::announceSurface(VASurfaceID sid, IpcChannel& ch) {
    if (announced_.count(sid)) return true;

    VADRMPRIMESurfaceDescriptor prime{};
    VAStatus vs = vaExportSurfaceHandle(
        display_, sid, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
        VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS, &prime);
    if (vs != VA_STATUS_SUCCESS) {
        VKR_ERR("vaExportSurfaceHandle(surface %u) failed: %s", sid, vaErrorStr(vs));
        return false;
    }

    proto::SurfaceDescMsg desc{};
    desc.slotKey = (uint32_t)sid;
    desc.numObjects = prime.num_objects;
    desc.drmModifier = prime.objects[0].drm_format_modifier;
    uint32_t planeCount = 0;
    for (uint32_t l = 0; l < prime.num_layers && planeCount < 4; ++l) {
        for (uint32_t p = 0; p < prime.layers[l].num_planes && planeCount < 4; ++p) {
            desc.plane[planeCount].offset = prime.layers[l].offset[p];
            desc.plane[planeCount].pitch = prime.layers[l].pitch[p];
            desc.planeObject[planeCount] = prime.layers[l].object_index[p];
            ++planeCount;
        }
    }
    desc.numPlanes = planeCount;
    int fds[4] = {-1, -1, -1, -1};
    for (uint32_t o = 0; o < prime.num_objects && o < 4; ++o) {
        fds[o] = prime.objects[o].fd;
        desc.objectSize[o] = prime.objects[o].size;
    }

    bool ok = prime.num_objects <= 4 && prime.fourcc == VA_FOURCC_NV12 &&
              ch.send(proto::MsgType::SurfaceDesc, desc, fds, prime.num_objects);
    if (!ok && prime.fourcc != VA_FOURCC_NV12)
        VKR_ERR("surface fourcc %08x not NV12", prime.fourcc);
    for (uint32_t o = 0; o < prime.num_objects && o < 4; ++o) close(fds[o]);
    if (ok) announced_.insert(sid);
    return ok;
}

bool GstVaDecoder::submitBuffer(GstBuffer* buffer, GstCaps* caps, IpcChannel& ch) {
    if (!resolveVaDisplay()) {
        VKR_ERR("no VA display available from pipeline");
        return false;
    }

    if (!headerSent_) {
        GstVideoInfo info;
        if (!gst_video_info_from_caps(&info, caps)) return false;
        proto::PoolDescMsg desc{};
        desc.width = (uint32_t)GST_VIDEO_INFO_WIDTH(&info) & ~1u;
        desc.height = (uint32_t)GST_VIDEO_INFO_HEIGHT(&info) & ~1u;
        desc.format = proto::SharedFormat::NV12;
        desc.matrix = info.colorimetry.matrix == GST_VIDEO_COLOR_MATRIX_BT601
                          ? proto::Matrix::BT601
                          : proto::Matrix::BT709;
        desc.range = info.colorimetry.range == GST_VIDEO_COLOR_RANGE_0_255
                         ? proto::Range::Full
                         : proto::Range::Limited;
        desc.poolSize = maxHeld_;
        desc.planeCount = 2;
        if (!ch.send(proto::MsgType::PoolDesc, desc)) return false;
        headerSent_ = true;
        poolWidth_ = desc.width;
        poolHeight_ = desc.height;
        VKR_LOG("decoder: stream header sent: %ux%u NV12 (gstreamer)", desc.width, desc.height);
    }

    if (held_.size() >= maxHeld_) {
        if ((++droppedFrames_ % 60) == 1)
            VKR_WARN("decoder: %zu frames outstanding, dropped %llu so far", held_.size(),
                     (unsigned long long)droppedFrames_);
        return true;
    }

    VASurfaceID sid = gst_va_buffer_get_surface(buffer);
    if (sid == VA_INVALID_ID) {
        VKR_ERR("buffer carries no VA surface (caps negotiated system memory?)");
        return false;
    }
    if (!announceSurface(sid, ch)) return false;

    // Producer-side sync, as in M6: decode is complete before announcement.
    vaSyncSurface(display_, sid);

    gst_buffer_ref(buffer);
    held_.push_back({++readyCounter_, buffer});

    proto::FrameReadyMsg msg{};
    msg.slot = (uint32_t)sid;
    msg.readyValue = readyCounter_;
    msg.ptsNs = GST_BUFFER_PTS_IS_VALID(buffer) ? (int64_t)GST_BUFFER_PTS(buffer) : -1;
    ++sentFrames_;
    return ch.send(proto::MsgType::FrameReady, msg);
}

int GstVaDecoder::run(const std::string& url, IpcChannel& ch) {
    uint32_t backoffMs = 1000;
    bool rendererAlive = true;

    while (rendererAlive) {
        sendPhase(ch, proto::StreamPhase::Connecting);
        VKR_LOG("decoder: connecting to %s (gstreamer)", url.c_str());
        if (!buildPipeline(url)) {
            teardownPipeline();
            sendPhase(ch, proto::StreamPhase::Error);
            VKR_WARN("decoder: pipeline start failed, retrying in %u ms", backoffMs);
            rendererAlive = backoffWait(ch, backoffMs);
            backoffMs = backoffMs * 2 > 10000 ? 10000 : backoffMs * 2;
            continue;
        }
        sendPhase(ch, proto::StreamPhase::Streaming);
        bool streamOk = true;
        bool firstSample = true;

        while (streamOk && rendererAlive) {
            rendererAlive = pollRenderer(ch);
            if (!rendererAlive) break;

            // Bus errors (RTSP drop, negotiation failure) end the session.
            if (GstMessage* m = gst_bus_pop_filtered(bus_, GST_MESSAGE_ERROR)) {
                GError* e = nullptr;
                gst_message_parse_error(m, &e, nullptr);
                VKR_LOG("decoder: stream interrupted (%s), sent=%llu dropped=%llu",
                        e ? e->message : "?", (unsigned long long)sentFrames_,
                        (unsigned long long)droppedFrames_);
                if (e) g_error_free(e);
                gst_message_unref(m);
                streamOk = false;
                break;
            }

            GstSample* sample =
                gst_app_sink_try_pull_sample(GST_APP_SINK(appsink_), 100 * GST_MSECOND);
            if (!sample) {
                if (gst_app_sink_is_eos(GST_APP_SINK(appsink_))) {
                    VKR_LOG("decoder: EOS");
                    streamOk = false;
                }
                continue;
            }
            if (firstSample) {
                firstSample = false;
                backoffMs = 1000;
            }
            GstBuffer* buffer = gst_sample_get_buffer(sample);
            GstCaps* caps = gst_sample_get_caps(sample);
            if (buffer && caps) {
                if (!submitBuffer(buffer, caps, ch)) rendererAlive = ch.connected();
            }
            gst_sample_unref(sample);
        }
        if (rendererAlive) ch.send(proto::MsgType::StreamReset);
        teardownPipeline();
        if (rendererAlive) {
            sendPhase(ch, proto::StreamPhase::Error);
            rendererAlive = backoffWait(ch, 1000);
        }
    }

    ch.send(proto::MsgType::Bye);
    return 0;
}

} // namespace vkr
