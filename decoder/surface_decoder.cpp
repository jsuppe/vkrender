#include "surface_decoder.h"

#include <android/native_window.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <sys/system_properties.h>

#include <chrono>
#include <cstring>
#include <ctime>
#include <thread>

#include "log.h"

extern "C" {
#include <libavcodec/bsf.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
}

namespace vkr {

namespace {
const char* mimeFor(int codecId) {
    switch (codecId) {
        case AV_CODEC_ID_H264: return "video/avc";
        case AV_CODEC_ID_HEVC: return "video/hevc";
        default: return nullptr;
    }
}
} // namespace

bool SurfaceDecoder::openStream(const std::string& url, ANativeWindow* window) {
    avformat_network_init();

    fmt_ = avformat_alloc_context();
    fmt_->flags |= AVFMT_FLAG_NOBUFFER | AVFMT_FLAG_FLUSH_PACKETS;
    fmt_->interrupt_callback.opaque = this;
    fmt_->interrupt_callback.callback = [](void* opaque) -> int {
        auto* self = (SurfaceDecoder*)opaque;
        return time(nullptr) > self->readDeadlineSec_ ? 1 : 0;
    };
    readDeadlineSec_ = time(nullptr) + 15;

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
    videoStream_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (videoStream_ < 0) {
        VKR_ERR("no video stream found");
        return false;
    }
    AVCodecParameters* par = fmt_->streams[videoStream_]->codecpar;
    const char* mime = mimeFor(par->codec_id);
    if (!mime) {
        VKR_ERR("unsupported codec id %d for MediaCodec", par->codec_id);
        return false;
    }
    VKR_LOG("decoder: stream: %s %dx%d -> renderer surface", mime, par->width, par->height);

    if (par->extradata && par->extradata_size > 0 && par->extradata[0] == 1) {
        const AVBitStreamFilter* f = av_bsf_get_by_name(
            par->codec_id == AV_CODEC_ID_H264 ? "h264_mp4toannexb" : "hevc_mp4toannexb");
        if (f && av_bsf_alloc(f, &bsf_) == 0) {
            avcodec_parameters_copy(bsf_->par_in, par);
            av_bsf_init(bsf_);
        }
    }

    // Emulator goldfish decoders stall in surface mode; use software there.
    char qemu[92] = {0};
    __system_property_get("ro.boot.qemu", qemu);
    if (qemu[0] == '1') {
        const char* swName = par->codec_id == AV_CODEC_ID_H264 ? "c2.android.avc.decoder"
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
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, par->width);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, par->height);
    media_status_t ms = AMediaCodec_configure(codec_, format, window, nullptr, 0);
    AMediaFormat_delete(format);
    if (ms != AMEDIA_OK) {
        VKR_ERR("AMediaCodec_configure(renderer surface) failed: %d", ms);
        surfaceDead_ = true; // a dead surface is the usual cause
        return false;
    }
    if (AMediaCodec_start(codec_) != AMEDIA_OK) {
        VKR_ERR("AMediaCodec_start failed");
        return false;
    }

    if (par->extradata && par->extradata_size > 0 && par->extradata[0] != 1) {
        ssize_t idx = AMediaCodec_dequeueInputBuffer(codec_, 100000);
        if (idx >= 0) {
            size_t cap = 0;
            uint8_t* dst = AMediaCodec_getInputBuffer(codec_, (size_t)idx, &cap);
            if (dst && cap >= (size_t)par->extradata_size) {
                memcpy(dst, par->extradata, par->extradata_size);
                AMediaCodec_queueInputBuffer(codec_, (size_t)idx, 0, par->extradata_size, 0,
                                             AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG);
            }
        }
    }
    return true;
}

void SurfaceDecoder::closeStream() {
    if (codec_) {
        AMediaCodec_stop(codec_);
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
    }
    if (bsf_) av_bsf_free(&bsf_);
    if (fmt_) avformat_close_input(&fmt_);
}

bool SurfaceDecoder::drainDecoder() {
    for (;;) {
        AMediaCodecBufferInfo info;
        ssize_t idx = AMediaCodec_dequeueOutputBuffer(codec_, &info, 0);
        if (idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER) return true;
        if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED ||
            idx == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED)
            continue;
        if (idx == AMEDIA_ERROR_UNKNOWN || idx < AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
            VKR_ERR("dequeueOutputBuffer error %zd - surface likely dead", idx);
            surfaceDead_ = true;
            return false;
        }
        if (idx < 0) return true;

        // render=true queues the frame into the renderer's BufferQueue with
        // its presentation timestamp; that IS the frame delivery.
        media_status_t rs = AMediaCodec_releaseOutputBuffer(codec_, (size_t)idx, info.size != 0);
        if (rs != AMEDIA_OK) {
            VKR_ERR("releaseOutputBuffer failed: %d - surface dead", rs);
            surfaceDead_ = true;
            return false;
        }
        if (info.size != 0 && (++decodedFrames_ % 300) == 1)
            VKR_LOG("decoder: frame %llu -> surface (pts %lld us)",
                    (unsigned long long)decodedFrames_, (long long)info.presentationTimeUs);
    }
}

int SurfaceDecoder::run(const std::string& url, ANativeWindow* window) {
    AVPacket* pkt = av_packet_alloc();
    uint32_t backoffMs = 1000;
    surfaceDead_ = false;

    while (!surfaceDead_) {
        VKR_LOG("decoder: connecting to %s", url.c_str());
        if (!openStream(url, window)) {
            closeStream();
            if (surfaceDead_) break;
            VKR_WARN("decoder: connect failed, retrying in %u ms", backoffMs);
            std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
            backoffMs = backoffMs * 2 > 10000 ? 10000 : backoffMs * 2;
            continue;
        }
        backoffMs = 1000;

        bool streamOk = true;
        while (streamOk && !surfaceDead_) {
            readDeadlineSec_ = time(nullptr) + 8;
            int err = av_read_frame(fmt_, pkt);
            if (err == AVERROR(EAGAIN)) continue;
            if (err < 0) {
                char buf[256];
                av_strerror(err, buf, sizeof(buf));
                VKR_LOG("decoder: stream interrupted (%s), decoded=%llu", buf,
                        (unsigned long long)decodedFrames_);
                streamOk = false;
                break;
            }
            if (pkt->stream_index != videoStream_) {
                av_packet_unref(pkt);
                continue;
            }
            if (bsf_) {
                av_bsf_send_packet(bsf_, pkt);
                if (av_bsf_receive_packet(bsf_, pkt) < 0) continue;
            }
            for (int attempts = 0; attempts < 200; ++attempts) {
                ssize_t idx = AMediaCodec_dequeueInputBuffer(codec_, 10000);
                if (idx >= 0) {
                    size_t cap = 0;
                    uint8_t* dst = AMediaCodec_getInputBuffer(codec_, (size_t)idx, &cap);
                    size_t n = (size_t)pkt->size <= cap ? (size_t)pkt->size : cap;
                    memcpy(dst, pkt->data, n);
                    AMediaCodec_queueInputBuffer(
                        codec_, (size_t)idx, 0, n,
                        pkt->pts != AV_NOPTS_VALUE
                            ? av_rescale_q(pkt->pts, fmt_->streams[videoStream_]->time_base,
                                           AVRational{1, 1000000})
                            : 0,
                        0);
                    ++queuedPackets_;
                    break;
                }
                if (!drainDecoder()) break; // frees output slots or detects death
            }
            av_packet_unref(pkt);
            if (!drainDecoder()) break;
        }
        closeStream();
        if (!surfaceDead_) std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    av_packet_free(&pkt);
    VKR_LOG("decoder: session over (surface dead), decoded=%llu",
            (unsigned long long)decodedFrames_);
    return 0;
}

} // namespace vkr
