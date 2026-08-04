// JNI entry for the decoder client app's DecoderService: runs one decode
// session into the renderer-app-provided Surface (M10 - BufferQueue carries
// the frames across the app boundary, no custom frame IPC).

#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <atomic>
#include <string>

#include "gst_surface_decoder.h"
#include "log.h"

// Set from the Binder thread when the renderer app goes away: a MediaCodec
// feeding an abandoned surface can stall without ever erroring, so the
// session must be aborted from outside.
static std::atomic<bool> g_abortSession{false};

extern "C" JNIEXPORT void JNICALL
Java_dev_vkrender_decoder_DecoderService_nativeAbortSession(JNIEnv*, jobject) {
    g_abortSession = true;
}

extern "C" JNIEXPORT void JNICALL
Java_dev_vkrender_decoder_DecoderService_nativeRunSession(JNIEnv* env, jobject, jstring jurl,
                                                          jobject jsurface) {
    const char* urlChars = env->GetStringUTFChars(jurl, nullptr);
    std::string url(urlChars ? urlChars : "");
    env->ReleaseStringUTFChars(jurl, urlChars);
    if (url.empty() || !jsurface) {
        VKR_ERR("decoder session missing url or surface");
        return;
    }
    ANativeWindow* window = ANativeWindow_fromSurface(env, jsurface);
    if (!window) {
        VKR_ERR("ANativeWindow_fromSurface failed");
        return;
    }
    g_abortSession = false;
    vkr::GstSurfaceDecoder decoder;
    decoder.run(url, window, &g_abortSession);
    ANativeWindow_release(window);
}
