#include "airplay_video.h"

#include <android/log.h>
#include <android/native_window.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <sys/system_properties.h>

#include <cstring>

#define VKR_LOG(...) __android_log_print(ANDROID_LOG_INFO, "vkrender", __VA_ARGS__)
#define VKR_WARN(...) __android_log_print(ANDROID_LOG_WARN, "vkrender", __VA_ARGS__)
#define VKR_ERR(...) __android_log_print(ANDROID_LOG_ERROR, "vkrender", __VA_ARGS__)

namespace vkr {

bool AirplayVideoSink::start(ANativeWindow* window) {
    std::lock_guard<std::mutex> lock(mutex_);
    window_ = window;
    surfaceDead_ = false;
    decodedFrames_ = 0;
    return true;
}

bool AirplayVideoSink::ensureCodec() {
    if (codec_) return true;
    if (!window_) return false;

    // Hardware first, software fallback (concurrent-session caps; emulator
    // goldfish stalls in surface mode) - the M12 lesson.
    char qemu[92] = {0};
    __system_property_get("ro.boot.qemu", qemu);
    const bool emulator = qemu[0] == '1';
    for (int attempt = 0; attempt < 2 && !codec_; ++attempt) {
        bool wantSw = emulator || attempt == 1;
        codec_ = wantSw ? AMediaCodec_createCodecByName("c2.android.avc.decoder")
                        : AMediaCodec_createDecoderByType("video/avc");
        if (!codec_) continue;
        AMediaFormat* format = AMediaFormat_new();
        AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, "video/avc");
        // AirPlay mirrors are up to the sender's screen size; the decoder
        // re-configures itself from the in-band SPS.
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, 1920);
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, 1080);
        media_status_t ms = AMediaCodec_configure(codec_, format, window_, nullptr, 0);
        AMediaFormat_delete(format);
        if (ms == AMEDIA_OK && AMediaCodec_start(codec_) == AMEDIA_OK) {
            VKR_LOG("airplay: decoder up (%s)", wantSw ? "software" : "hardware");
            break;
        }
        VKR_WARN("airplay: %s codec start failed (%d)", wantSw ? "software" : "hardware", ms);
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
        if (emulator) break;
    }
    if (!codec_) {
        VKR_ERR("airplay: no usable H.264 decoder");
        surfaceDead_ = true;
        return false;
    }
    return true;
}

bool AirplayVideoSink::drainDecoder() {
    for (;;) {
        AMediaCodecBufferInfo info;
        ssize_t idx = AMediaCodec_dequeueOutputBuffer(codec_, &info, 0);
        if (idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER) return true;
        if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED ||
            idx == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED)
            continue;
        if (idx < AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
            VKR_ERR("airplay: dequeueOutputBuffer error %zd - surface likely dead", idx);
            surfaceDead_ = true;
            return false;
        }
        if (idx < 0) return true;
        if (AMediaCodec_releaseOutputBuffer(codec_, (size_t)idx, info.size != 0) != AMEDIA_OK) {
            surfaceDead_ = true;
            return false;
        }
        if (info.size != 0 && (++decodedFrames_ % 300) == 1)
            VKR_LOG("airplay: frame %llu -> surface", (unsigned long long)decodedFrames_);
    }
}

void AirplayVideoSink::feed(const uint8_t* data, int len, int64_t ptsUs) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (surfaceDead_ || len <= 0 || !ensureCodec()) return;
    for (int attempts = 0; attempts < 200; ++attempts) {
        ssize_t idx = AMediaCodec_dequeueInputBuffer(codec_, 10000);
        if (idx >= 0) {
            size_t cap = 0;
            uint8_t* dst = AMediaCodec_getInputBuffer(codec_, (size_t)idx, &cap);
            size_t n = (size_t)len <= cap ? (size_t)len : cap;
            memcpy(dst, data, n);
            AMediaCodec_queueInputBuffer(codec_, (size_t)idx, 0, n, ptsUs, 0);
            ++queued_;
            break;
        }
        if (!drainDecoder()) return;
    }
    drainDecoder();
}

void AirplayVideoSink::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (codec_) AMediaCodec_flush(codec_);
}

void AirplayVideoSink::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (codec_) {
        AMediaCodec_stop(codec_);
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
    }
    window_ = nullptr;
}

} // namespace vkr
