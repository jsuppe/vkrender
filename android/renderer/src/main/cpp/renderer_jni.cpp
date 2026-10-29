// JNI entry for the renderer app's RendererService: exposes per-client input
// Surfaces to Binder callers (stream registry lives in android_main.cpp).
// M22: streams are keyed (uid, slot) so one client app - the WHIP room - can
// hold several presenter streams; slot 0 is the classic per-app stream.

#include <android/native_window_jni.h>
#include <jni.h>

// Provided by android_main.cpp: waits briefly for renderer readiness, then
// returns the caller's stream window (creating it on first call; caller owns one reference).
ANativeWindow* vkrAcquireStreamWindow(int uid, int slot);
void vkrReleaseStream(int uid, int slot);

extern "C" JNIEXPORT jobject JNICALL
Java_dev_vkrender_renderer_RendererService_nativeAcquireInputSurface(JNIEnv* env, jclass,
                                                                     jint uid, jint slot) {
    ANativeWindow* window = vkrAcquireStreamWindow((int)uid, (int)slot);
    if (!window) return nullptr;
    jobject surface = ANativeWindow_toSurface(env, window);
    ANativeWindow_release(window);
    return surface;
}

extern "C" JNIEXPORT void JNICALL
Java_dev_vkrender_renderer_RendererService_nativeReleaseInputSurface(JNIEnv*, jclass, jint uid,
                                                                     jint slot) {
    vkrReleaseStream((int)uid, (int)slot);
}
