#include "vaapi_decoder.h"

#include <unistd.h>

#include <chrono>
#include <cstring>
#include <thread>

#include "log.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/pixdesc.h>
}

#include <va/va_drmcommon.h>

namespace vkr {

namespace {

enum AVPixelFormat pickVaapiFormat(AVCodecContext*, const enum AVPixelFormat* fmts) {
    for (const enum AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == AV_PIX_FMT_VAAPI) return *p;
    }
    VKR_ERR("decoder: AV_PIX_FMT_VAAPI not offered by codec; hardware decode unavailable");
    return AV_PIX_FMT_NONE;
}

proto::Matrix mapMatrix(const AVFrame* f) {
    switch (f->colorspace) {
        case AVCOL_SPC_BT709: return proto::Matrix::BT709;
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M: return proto::Matrix::BT601;
        default:
            return f->height >= 720 ? proto::Matrix::BT709 : proto::Matrix::BT601;
    }
}

proto::Range mapRange(const AVFrame* f) {
    return f->color_range == AVCOL_RANGE_JPEG ? proto::Range::Full : proto::Range::Limited;
}

void sendPhase(IpcChannel& ch, proto::StreamPhase phase) {
    proto::StreamStatusMsg st{phase};
    ch.send(proto::MsgType::StreamStatus, st);
}

} // namespace

VAAPIDecoder::VAAPIDecoder(const proto::HelloMsg&, uint32_t maxHeldFrames,
                           const std::string& drmDevice)
    : maxHeld_(maxHeldFrames < 2 ? 2 : maxHeldFrames), drmDevice_(drmDevice) {}

VAAPIDecoder::~VAAPIDecoder() {
    releaseConsumed(UINT64_MAX);
    closeStream();
    if (hwDevice_) av_buffer_unref(&hwDevice_);
}

bool VAAPIDecoder::openStream(const std::string& url) {
    avformat_network_init();

    if (!hwDevice_) {
        // The DRM render node picks the GPU; there is no LUID equivalent, so
        // multi-GPU selection is explicit via --device.
        int err = av_hwdevice_ctx_create(&hwDevice_, AV_HWDEVICE_TYPE_VAAPI,
                                         drmDevice_.empty() ? nullptr : drmDevice_.c_str(),
                                         nullptr, 0);
        if (err < 0) {
            char buf[256];
            av_strerror(err, buf, sizeof(buf));
            VKR_ERR("av_hwdevice_ctx_create(VAAPI, %s): %s "
                    "(is a VAAPI driver installed? check `vainfo`)",
                    drmDevice_.empty() ? "default" : drmDevice_.c_str(), buf);
            return false;
        }
        auto* hwctx = reinterpret_cast<AVHWDeviceContext*>(hwDevice_->data);
        display_ = reinterpret_cast<AVVAAPIDeviceContext*>(hwctx->hwctx)->display;
        VKR_LOG("decoder: VAAPI display ready (%s)",
                drmDevice_.empty() ? "default device" : drmDevice_.c_str());
    }

    // Low-latency receive path (same knobs as the Windows backend).
    fmt_ = avformat_alloc_context();
    fmt_->flags |= AVFMT_FLAG_NOBUFFER | AVFMT_FLAG_FLUSH_PACKETS;
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "rtsp_transport", "tcp", 0);
    av_dict_set(&opts, "timeout", "5000000", 0);
    av_dict_set(&opts, "max_delay", "100000", 0);
    av_dict_set(&opts, "probesize", "1000000", 0);
    av_dict_set(&opts, "analyzeduration", "500000", 0);
    int err = avformat_open_input(&fmt_, url.c_str(), nullptr, &opts);
    av_dict_free(&opts);
    if (err < 0) {
        char buf[256];
        av_strerror(err, buf, sizeof(buf));
        VKR_ERR("avformat_open_input(%s): %s", url.c_str(), buf);
        return false;
    }
    if (avformat_find_stream_info(fmt_, nullptr) < 0) {
        VKR_ERR("avformat_find_stream_info failed");
        return false;
    }
    const AVCodec* dec = nullptr;
    videoStream_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
    if (videoStream_ < 0 || !dec) {
        VKR_ERR("no video stream found");
        return false;
    }
    AVStream* st = fmt_->streams[videoStream_];
    VKR_LOG("decoder: stream: %s %dx%d", avcodec_get_name(st->codecpar->codec_id),
            st->codecpar->width, st->codecpar->height);

    codec_ = avcodec_alloc_context3(dec);
    if (!codec_ || avcodec_parameters_to_context(codec_, st->codecpar) < 0) {
        VKR_ERR("codec context setup failed");
        return false;
    }
    codec_->hw_device_ctx = av_buffer_ref(hwDevice_);
    codec_->get_format = pickVaapiFormat;
    codec_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    // The frames we hold for the renderer come out of the same fixed surface
    // pool the decoder uses; give it headroom for them or decoding stalls.
    codec_->extra_hw_frames = (int)maxHeld_ + 2;

    if (avcodec_open2(codec_, dec, nullptr) < 0) {
        VKR_ERR("avcodec_open2 failed");
        return false;
    }
    return true;
}

void VAAPIDecoder::closeStream() {
    // Held frames stay alive across RTSP reconnects only if the codec (and
    // its surface pool) survives; it does not, so release them first.
    releaseConsumed(UINT64_MAX);
    announced_.clear();
    headerSent_ = false;
    if (codec_) avcodec_free_context(&codec_);
    if (fmt_) avformat_close_input(&fmt_);
}

void VAAPIDecoder::releaseConsumed(uint64_t consumedValue) {
    while (!held_.empty() && held_.front().readyValue <= consumedValue) {
        av_frame_free(&held_.front().frame);
        held_.pop_front();
    }
}

bool VAAPIDecoder::pollRenderer(IpcChannel& ch) {
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

bool VAAPIDecoder::backoffWait(IpcChannel& ch, uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 100) {
        if (!pollRenderer(ch)) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return true;
}

bool VAAPIDecoder::announceSurface(VASurfaceID sid, IpcChannel& ch) {
    if (announced_.count(sid)) return true;

    VADRMPRIMESurfaceDescriptor prime{};
    VAStatus vs = vaExportSurfaceHandle(
        display_, sid, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
        VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS, &prime);
    if (vs != VA_STATUS_SUCCESS) {
        VKR_ERR("vaExportSurfaceHandle(surface %u) failed: %s (driver may not support "
                "DRM_PRIME_2 export)",
                sid, vaErrorStr(vs));
        return false;
    }

    // COMPOSED_LAYERS: one layer describing all planes. The renderer only
    // supports a single buffer object today (the common case for NV12).
    proto::SurfaceDescMsg desc{};
    desc.slotKey = (uint32_t)sid;
    desc.numObjects = prime.num_objects;
    desc.drmModifier = prime.objects[0].drm_format_modifier;
    const auto& layer = prime.layers[0];
    desc.numPlanes = layer.num_planes;
    for (uint32_t p = 0; p < layer.num_planes && p < 4; ++p) {
        desc.plane[p].offset = layer.offset[p];
        desc.plane[p].pitch = layer.pitch[p];
    }
    int fds[4] = {-1, -1, -1, -1};
    for (uint32_t o = 0; o < prime.num_objects && o < 4; ++o) {
        fds[o] = prime.objects[o].fd;
        desc.objectSize[o] = prime.objects[o].size;
    }

    bool ok = true;
    if (prime.num_objects != 1) {
        VKR_ERR("surface %u exports %u buffer objects; only 1 supported", sid,
                prime.num_objects);
        ok = false;
    } else if (prime.fourcc != VA_FOURCC_NV12) {
        VKR_ERR("surface fourcc %08x is not NV12 (10-bit Linux path not implemented yet)",
                prime.fourcc);
        ok = false;
    } else {
        ok = ch.send(proto::MsgType::SurfaceDesc, desc, fds, prime.num_objects);
    }
    // Our fd copies are duplicated into the socket on send; close ours either way.
    for (uint32_t o = 0; o < prime.num_objects && o < 4; ++o) close(fds[o]);
    if (ok) announced_.insert(sid);
    return ok;
}

bool VAAPIDecoder::submitFrame(AVFrame* frame, IpcChannel& ch) {
    const uint32_t w = (uint32_t)frame->width & ~1u;
    const uint32_t h = (uint32_t)frame->height & ~1u;
    if (!headerSent_ || w != poolWidth_ || h != poolHeight_) {
        if (headerSent_) {
            VKR_LOG("decoder: stream geometry changed, resetting");
            ch.send(proto::MsgType::StreamReset);
            releaseConsumed(UINT64_MAX);
            announced_.clear();
        }
        proto::PoolDescMsg desc{};
        desc.width = w;
        desc.height = h;
        desc.format = proto::SharedFormat::NV12;
        desc.matrix = mapMatrix(frame);
        desc.range = mapRange(frame);
        desc.poolSize = maxHeld_;
        desc.planeCount = 2;
        if (!ch.send(proto::MsgType::PoolDesc, desc)) return false;
        headerSent_ = true;
        poolWidth_ = w;
        poolHeight_ = h;
        VKR_LOG("decoder: stream header sent: %ux%u NV12, matrix=%s range=%s", w, h,
                desc.matrix == proto::Matrix::BT709 ? "BT.709" : "BT.601",
                desc.range == proto::Range::Full ? "full" : "limited");
    }

    if (held_.size() >= maxHeld_) {
        // Renderer is behind; keep latency bounded by dropping this frame.
        if ((++droppedFrames_ % 60) == 1)
            VKR_WARN("decoder: %zu frames outstanding, dropped %llu so far", held_.size(),
                     (unsigned long long)droppedFrames_);
        return true;
    }

    VASurfaceID sid = (VASurfaceID)(uintptr_t)frame->data[3];
    if (!announceSurface(sid, ch)) return false;

    // Producer-side sync: decode for this surface has certainly finished
    // before the renderer is told about it. (Explicit sync via exported
    // sync_file is the planned upgrade; see docs/LINUX.md.)
    vaSyncSurface(display_, sid);

    // Hold a ref so FFmpeg cannot recycle the surface while the renderer may
    // still read it. Released on FrameConsumed.
    AVFrame* held = av_frame_clone(frame);
    if (!held) return false;
    held_.push_back({++readyCounter_, held});

    proto::FrameReadyMsg msg{};
    msg.slot = (uint32_t)sid;
    msg.readyValue = readyCounter_;
    msg.ptsNs = frame->pts != AV_NOPTS_VALUE
                    ? av_rescale_q(frame->pts, fmt_->streams[videoStream_]->time_base,
                                   AVRational{1, 1000000000})
                    : -1;
    ++sentFrames_;
    return ch.send(proto::MsgType::FrameReady, msg);
}

int VAAPIDecoder::run(const std::string& url, IpcChannel& ch) {
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    uint32_t backoffMs = 1000;
    bool rendererAlive = true;

    while (rendererAlive) {
        sendPhase(ch, proto::StreamPhase::Connecting);
        VKR_LOG("decoder: connecting to %s", url.c_str());
        if (!openStream(url)) {
            closeStream();
            sendPhase(ch, proto::StreamPhase::Error);
            VKR_WARN("decoder: connect failed, retrying in %u ms", backoffMs);
            rendererAlive = backoffWait(ch, backoffMs);
            backoffMs = backoffMs * 2 > 10000 ? 10000 : backoffMs * 2;
            continue;
        }
        backoffMs = 1000;
        sendPhase(ch, proto::StreamPhase::Streaming);

        bool streamOk = true;
        while (streamOk && rendererAlive) {
            rendererAlive = pollRenderer(ch);
            if (!rendererAlive) break;

            int err = av_read_frame(fmt_, pkt);
            if (err == AVERROR(EAGAIN)) continue;
            if (err < 0) {
                char buf[256];
                av_strerror(err, buf, sizeof(buf));
                VKR_LOG("decoder: stream interrupted (%s), sent=%llu dropped=%llu", buf,
                        (unsigned long long)sentFrames_, (unsigned long long)droppedFrames_);
                streamOk = false;
                break;
            }
            if (pkt->stream_index != videoStream_) {
                av_packet_unref(pkt);
                continue;
            }
            err = avcodec_send_packet(codec_, pkt);
            av_packet_unref(pkt);
            if (err < 0 && err != AVERROR(EAGAIN)) {
                VKR_WARN("decoder: send_packet error %d", err);
                continue;
            }
            while (avcodec_receive_frame(codec_, frame) == 0) {
                if (frame->format == AV_PIX_FMT_VAAPI) {
                    if (!submitFrame(frame, ch)) rendererAlive = ch.connected();
                }
                av_frame_unref(frame);
            }
        }
        // StreamReset before closeStream: the renderer must stop touching
        // surfaces whose backing pool is about to be freed.
        if (rendererAlive) ch.send(proto::MsgType::StreamReset);
        closeStream();
        if (rendererAlive) {
            sendPhase(ch, proto::StreamPhase::Error);
            rendererAlive = backoffWait(ch, 1000);
        }
    }

    ch.send(proto::MsgType::Bye);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    return 0;
}

} // namespace vkr
