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

#include <string>
std::string vkrControlStatus();
const char* vkrControlCapabilities();
int vkrControl(const std::string& action,int slot,const std::string& value);
extern "C" JNIEXPORT jstring JNICALL
Java_dev_vkrender_renderer_RendererService_nativeControlStatus(JNIEnv* env,jclass) {
    return env->NewStringUTF(vkrControlStatus().c_str());
}
extern "C" JNIEXPORT jstring JNICALL
Java_dev_vkrender_renderer_RendererService_nativeControlCapabilities(JNIEnv* env,jclass) {
    return env->NewStringUTF(vkrControlCapabilities());
}
extern "C" JNIEXPORT jint JNICALL
Java_dev_vkrender_renderer_RendererService_nativeControl(JNIEnv* env,jclass,jstring action,jint slot,jstring value) {
    if(!action || !value)return 1;
    const char* a=env->GetStringUTFChars(action,nullptr);if(!a)return 1;
    const char* v=env->GetStringUTFChars(value,nullptr);
    if(!v){env->ReleaseStringUTFChars(action,a);return 1;}
    int result=vkrControl(a,slot,v);
    env->ReleaseStringUTFChars(value,v);env->ReleaseStringUTFChars(action,a);return result;
}
