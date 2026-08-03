// JNI entries for the two services.
//
// DecoderService (":decoder" process): runs one decode session into the
// renderer-provided Surface (M8 - BufferQueue carries the frames, no custom
// frame IPC).
//
// RendererService (main process): exposes the native renderer's AImageReader
// Surface to Binder clients (bridge state lives in android_main.cpp).

#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <string>

#include "gst_surface_decoder.h"
#include "log.h"

// Provided by android_main.cpp: blocks briefly until the renderer's reader
// exists, then returns its ANativeWindow (not owning).
ANativeWindow* vkrGetRendererInputWindow();

extern "C" JNIEXPORT void JNICALL
Java_dev_vkrender_DecoderService_nativeRunSession(JNIEnv* env, jobject, jstring jurl,
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
    vkr::GstSurfaceDecoder decoder;
    decoder.run(url, window);
    ANativeWindow_release(window);
}

extern "C" JNIEXPORT jobject JNICALL
Java_dev_vkrender_RendererService_nativeAcquireInputSurface(JNIEnv* env, jclass) {
    ANativeWindow* window = vkrGetRendererInputWindow();
    if (!window) return nullptr;
    return ANativeWindow_toSurface(env, window);
}
