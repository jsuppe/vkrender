// JNI entry for the DecoderService: runs the MediaCodec decoder session loop
// on the calling (Java background) thread. Lives in the ":decoder" process,
// a real OS process boundary away from the renderer.

#include <android/log.h>
#include <jni.h>

#include <string>

#include "ipc.h"
#include "log.h"
#include "mediacodec_decoder.h"

extern "C" {
#include <libavutil/log.h>
}

// FFmpeg logs to stderr by default, which is invisible on Android; forward
// everything at info level or worse to logcat.
static void ffmpegLogToLogcat(void* avcl, int level, const char* fmt, va_list args) {
    if (level > av_log_get_level()) return;
    __android_log_vprint(level <= AV_LOG_ERROR ? ANDROID_LOG_ERROR : ANDROID_LOG_INFO,
                         "vkrender-ffmpeg", fmt, args);
    (void)avcl;
}

extern "C" JNIEXPORT void JNICALL
Java_dev_vkrender_DecoderService_nativeRun(JNIEnv* env, jobject, jstring jurl) {
    const char* urlChars = env->GetStringUTFChars(jurl, nullptr);
    std::string url(urlChars ? urlChars : "");
    env->ReleaseStringUTFChars(jurl, urlChars);
    if (url.empty()) {
        VKR_ERR("decoder service started without a url");
        return;
    }
    VKR_LOG("decoder service: url=%s", url.c_str());
    av_log_set_callback(ffmpegLogToLogcat);
    av_log_set_level(AV_LOG_WARNING);

    // Session loop: serve one renderer connection at a time, forever (the
    // service is stopped by the OS / stopService).
    for (;;) {
        vkr::IpcChannel ch = vkr::IpcChannel::listen("vkrender");
        if (!ch.connected()) return;
        auto hello = ch.recv();
        if (!hello || hello->type != vkr::proto::MsgType::Hello) continue;
        const auto* msg = hello->as<vkr::proto::HelloMsg>();
        if (!msg) continue;

        vkr::MediaCodecDecoder decoder(*msg, 4);
        decoder.run(url, ch);
        VKR_LOG("decoder service: session ended, waiting for a new renderer");
    }
}
