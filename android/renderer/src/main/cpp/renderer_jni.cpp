// JNI entry for the renderer app's RendererService: exposes the native
// renderer's AImageReader Surface to Binder clients (bridge state lives in
// android_main.cpp).

#include <android/native_window_jni.h>
#include <jni.h>

// Provided by android_main.cpp: blocks briefly until the renderer's reader
// exists, then returns its ANativeWindow (not owning).
ANativeWindow* vkrGetRendererInputWindow();

extern "C" JNIEXPORT jobject JNICALL
Java_dev_vkrender_renderer_RendererService_nativeAcquireInputSurface(JNIEnv* env, jclass) {
    ANativeWindow* window = vkrGetRendererInputWindow();
    if (!window) return nullptr;
    return ANativeWindow_toSurface(env, window);
}
