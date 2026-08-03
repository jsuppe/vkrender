#include "mediacodec_decoder.h"

#include <android/hardware_buffer.h>
#include <media/NdkImage.h>
#include <sys/system_properties.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include <chrono>
#include <cstring>
#include <thread>

#include "log.h"

extern "C" {
#include <libavcodec/bsf.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
}

namespace vkr {

namespace {

void sendPhase(IpcChannel& ch, proto::StreamPhase phase) {
    proto::StreamStatusMsg st{phase};
    ch.send(proto::MsgType::StreamStatus, st);
}

const char* mimeFor(int codecId) {
    switch (codecId) {
        case AV_CODEC_ID_H264: return "video/avc";
        case AV_CODEC_ID_HEVC: return "video/hevc";
        default: return nullptr;
    }
}

} // namespace

MediaCodecDecoder::MediaCodecDecoder(const proto::HelloMsg&, uint32_t maxHeldFrames)
    : maxHeld_(maxHeldFrames < 2 ? 2 : maxHeldFrames) {}

MediaCodecDecoder::~MediaCodecDecoder() {
    releaseConsumed(UINT64_MAX);
    closeStream();
}

bool MediaCodecDecoder::openStream(const std::string& url) {
    avformat_network_init();

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
    VKR_LOG("decoder: stream: %s %dx%d", mime, par->width, par->height);

    // MediaCodec wants Annex-B; RTSP normally delivers it already, but if the
    // stream carries avcC/hvcC extradata, convert.
    if (par->extradata && par->extradata_size > 0 && par->extradata[0] == 1) {
        const AVBitStreamFilter* f = av_bsf_get_by_name(
            par->codec_id == AV_CODEC_ID_H264 ? "h264_mp4toannexb" : "hevc_mp4toannexb");
        if (f && av_bsf_alloc(f, &bsf_) == 0) {
            avcodec_parameters_copy(bsf_->par_in, par);
            av_bsf_init(bsf_);
        }
    }

    // Decode target: an AImageReader whose buffers are GPU-sampleable. Its
    // gralloc buffers are exactly what we share with the renderer.
    media_status_t ms = AImageReader_newWithUsage(
        par->width, par->height, AIMAGE_FORMAT_PRIVATE,
        AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, (int32_t)maxHeld_ + 3, &reader_);
    if (ms != AMEDIA_OK || !reader_) {
        VKR_ERR("AImageReader_newWithUsage failed: %d", ms);
        return false;
    }
    ANativeWindow* window = nullptr;
    AImageReader_getWindow(reader_, &window);

    // The emulator's c2.goldfish decoders stall in surface mode; use the
    // software decoder there. Real devices get their hardware codec.
    char qemu[92] = {0};
    __system_property_get("ro.boot.qemu", qemu);
    if (qemu[0] == '1') {
        const char* swName =
            par->codec_id == AV_CODEC_ID_H264 ? "c2.android.avc.decoder" : "c2.android.hevc.decoder";
        codec_ = AMediaCodec_createCodecByName(swName);
        if (codec_) VKR_LOG("decoder: emulator detected, using %s", swName);
    }
    if (!codec_) codec_ = AMediaCodec_createDecoderByType(mime);
    if (!codec_) {
        VKR_ERR("AMediaCodec_createDecoderByType(%s) failed", mime);
        return false;
    }
    AMediaFormat* format = AMediaFormat_new();
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, mime);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, par->width);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, par->height);
    // csd is delivered in-band (annex-b SPS/PPS from the RTSP stream).
    ms = AMediaCodec_configure(codec_, format, window, nullptr, 0);
    AMediaFormat_delete(format);
    if (ms != AMEDIA_OK) {
        VKR_ERR("AMediaCodec_configure failed: %d", ms);
        return false;
    }
    if (AMediaCodec_start(codec_) != AMEDIA_OK) {
        VKR_ERR("AMediaCodec_start failed");
        return false;
    }
    inputDone_ = false;

    // Push extradata (annex-b SPS/PPS) ahead of the first frame if present.
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

void MediaCodecDecoder::closeStream() {
    releaseConsumed(UINT64_MAX);
    announced_.clear();
    headerSent_ = false;
    if (codec_) {
        AMediaCodec_stop(codec_);
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
    }
    if (reader_) {
        AImageReader_delete(reader_);
        reader_ = nullptr;
    }
    if (bsf_) av_bsf_free(&bsf_);
    if (fmt_) avformat_close_input(&fmt_);
}

void MediaCodecDecoder::releaseConsumed(uint64_t consumedValue) {
    while (!held_.empty() && held_.front().readyValue <= consumedValue) {
        AImage_delete(held_.front().image);
        held_.pop_front();
    }
}

bool MediaCodecDecoder::pollRenderer(IpcChannel& ch) {
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

bool MediaCodecDecoder::backoffWait(IpcChannel& ch, uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 100) {
        if (!pollRenderer(ch)) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return true;
}

bool MediaCodecDecoder::submitImage(AImage* image, int64_t ptsUs, IpcChannel& ch) {
    int32_t w = 0, h = 0;
    AImage_getWidth(image, &w);
    AImage_getHeight(image, &h);
    const uint32_t dw = (uint32_t)w & ~1u, dh = (uint32_t)h & ~1u;

    if (!headerSent_ || dw != poolWidth_ || dh != poolHeight_) {
        if (headerSent_) {
            ch.send(proto::MsgType::StreamReset);
            releaseConsumed(UINT64_MAX);
            announced_.clear();
        }
        proto::PoolDescMsg desc{};
        desc.width = dw;
        desc.height = dh;
        desc.format = proto::SharedFormat::NV12; // semantic; real format is the AHB's
        desc.matrix = dh >= 720 ? proto::Matrix::BT709 : proto::Matrix::BT601;
        desc.range = proto::Range::Limited;
        desc.poolSize = maxHeld_;
        desc.planeCount = 2;
        if (!ch.send(proto::MsgType::PoolDesc, desc)) return false;
        headerSent_ = true;
        poolWidth_ = dw;
        poolHeight_ = dh;
        VKR_LOG("decoder: stream header sent: %ux%u (AHardwareBuffer)", dw, dh);
    }

    if (held_.size() >= maxHeld_) {
        if ((++droppedFrames_ % 60) == 1)
            VKR_WARN("decoder: %zu frames outstanding, dropped %llu so far", held_.size(),
                     (unsigned long long)droppedFrames_);
        AImage_delete(image);
        return true;
    }

    AHardwareBuffer* ahb = nullptr;
    if (AImage_getHardwareBuffer(image, &ahb) != AMEDIA_OK || !ahb) {
        VKR_ERR("AImage_getHardwareBuffer failed");
        AImage_delete(image);
        return false;
    }
    const uint64_t key64 = (uint64_t)(uintptr_t)ahb;
    const uint32_t key = (uint32_t)(key64 ^ (key64 >> 32)); // fold pointer to slot key

    if (!announced_.count(key64)) {
        // SurfaceDesc with numObjects == 0 announces "an AHardwareBuffer
        // follows out-of-band on this socket".
        proto::SurfaceDescMsg desc{};
        desc.slotKey = key;
        desc.numPlanes = 2;
        desc.numObjects = 0;
        if (!ch.send(proto::MsgType::SurfaceDesc, desc)) {
            AImage_delete(image);
            return false;
        }
        int err = AHardwareBuffer_sendHandleToUnixSocket(ahb, ch.nativeSocket());
        if (err != 0) {
            VKR_ERR("AHardwareBuffer_sendHandleToUnixSocket failed: %d", err);
            AImage_delete(image);
            return false;
        }
        announced_.insert(key64);
    }

    held_.push_back({++readyCounter_, image});

    proto::FrameReadyMsg msg{};
    msg.slot = key;
    msg.readyValue = readyCounter_;
    msg.ptsNs = ptsUs >= 0 ? ptsUs * 1000 : -1;
    ++sentFrames_;
    return ch.send(proto::MsgType::FrameReady, msg);
}

// Pull all ready output frames out of the codec.
bool MediaCodecDecoder::drainDecoder(IpcChannel& ch) {
    for (;;) {
        AMediaCodecBufferInfo info;
        ssize_t idx = AMediaCodec_dequeueOutputBuffer(codec_, &info, 0);
        if (idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER) return true;
        if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED ||
            idx == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED)
            continue;
        if (idx < 0) return true;

        // render=true pushes the frame to the AImageReader surface...
        AMediaCodec_releaseOutputBuffer(codec_, (size_t)idx, info.size != 0);
        if (info.size == 0) continue;
        if ((++decodedFrames_ % 120) == 1)
            VKR_LOG("decoder: codec output frame %llu (pts %lld us)",
                    (unsigned long long)decodedFrames_, (long long)info.presentationTimeUs);

        // ...where we acquire it as an AImage. Queueing to the surface is
        // asynchronous, so poll briefly.
        AImage* image = nullptr;
        media_status_t ms = AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE;
        for (int tries = 0; tries < 50 && !image; ++tries) {
            ms = AImageReader_acquireNextImage(reader_, &image);
            if (ms == AMEDIA_OK && image) break;
            image = nullptr;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (!image) {
            if ((++acquireFailures_ % 60) == 1)
                VKR_WARN("decoder: AImageReader acquire failed (%d), %llu misses", ms,
                         (unsigned long long)acquireFailures_);
            continue;
        }
        if (!submitImage(image, info.presentationTimeUs, ch)) return false;
    }
}

int MediaCodecDecoder::run(const std::string& url, IpcChannel& ch) {
    AVPacket* pkt = av_packet_alloc();
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
            if (bsf_) {
                av_bsf_send_packet(bsf_, pkt);
                if (av_bsf_receive_packet(bsf_, pkt) < 0) continue;
            }

            // Feed the packet into MediaCodec (retry while its input is full,
            // draining outputs so it can make progress).
            bool queued = false;
            for (int attempts = 0; attempts < 100 && !queued; ++attempts) {
                ssize_t idx = AMediaCodec_dequeueInputBuffer(codec_, 10000);
                if (idx >= 0) {
                    size_t cap = 0;
                    uint8_t* dst = AMediaCodec_getInputBuffer(codec_, (size_t)idx, &cap);
                    size_t n = (size_t)pkt->size <= cap ? (size_t)pkt->size : cap;
                    memcpy(dst, pkt->data, n);
                    media_status_t qs = AMediaCodec_queueInputBuffer(
                        codec_, (size_t)idx, 0, n,
                        pkt->pts != AV_NOPTS_VALUE
                            ? av_rescale_q(pkt->pts, fmt_->streams[videoStream_]->time_base,
                                           AVRational{1, 1000000})
                            : 0,
                        0);
                    queued = qs == AMEDIA_OK;
                    if ((++queuedPackets_ % 120) == 1)
                        VKR_LOG("decoder: queued packet %llu (%zu bytes, first=%02x %02x %02x %02x %02x, qs=%d)",
                                (unsigned long long)queuedPackets_, n,
                                n > 0 ? pkt->data[0] : 0, n > 1 ? pkt->data[1] : 0,
                                n > 2 ? pkt->data[2] : 0, n > 3 ? pkt->data[3] : 0,
                                n > 4 ? pkt->data[4] : 0, qs);
                    break;
                }
                if (!drainDecoder(ch)) { rendererAlive = ch.connected(); break; }
            }
            if (!queued && rendererAlive && (++inputStalls_ % 30) == 1)
                VKR_WARN("decoder: MediaCodec input stalled (%llu times)",
                         (unsigned long long)inputStalls_);
            av_packet_unref(pkt);

            if (!drainDecoder(ch)) rendererAlive = ch.connected();
        }
        if (rendererAlive) ch.send(proto::MsgType::StreamReset);
        closeStream();
        if (rendererAlive) {
            sendPhase(ch, proto::StreamPhase::Error);
            rendererAlive = backoffWait(ch, 1000);
        }
    }

    ch.send(proto::MsgType::Bye);
    av_packet_free(&pkt);
    return 0;
}

} // namespace vkr
