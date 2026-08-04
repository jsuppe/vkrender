// JNI entry for the renderer app's RendererService: exposes per-client input
// Surfaces to Binder callers (stream registry lives in android_main.cpp).

#include <android/native_window_jni.h>
#include <jni.h>

// Provided by android_main.cpp: waits briefly for renderer readiness, then
// returns the caller's stream window (creating it on first call; not owning).
ANativeWindow* vkrAcquireStreamWindow(int uid);
void vkrReleaseStream(int uid);

extern "C" JNIEXPORT jobject JNICALL
Java_dev_vkrender_renderer_RendererService_nativeAcquireInputSurface(JNIEnv* env, jclass,
                                                                     jint uid) {
    ANativeWindow* window = vkrAcquireStreamWindow((int)uid);
    if (!window) return nullptr;
    return ANativeWindow_toSurface(env, window);
}

extern "C" JNIEXPORT void JNICALL
Java_dev_vkrender_renderer_RendererService_nativeReleaseInputSurface(JNIEnv*, jclass, jint uid) {
    vkrReleaseStream((int)uid);
}
