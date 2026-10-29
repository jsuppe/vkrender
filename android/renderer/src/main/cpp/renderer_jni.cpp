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
std::string vkrControlCapabilities();
int vkrControl(const std::string& action,int slot,const std::string& value);
extern "C" JNIEXPORT jstring JNICALL
Java_dev_vkrender_renderer_RendererService_nativeControlStatus(JNIEnv* env,jclass) {
    return env->NewStringUTF(vkrControlStatus().c_str());
}
extern "C" JNIEXPORT jstring JNICALL
Java_dev_vkrender_renderer_RendererService_nativeControlCapabilities(JNIEnv* env,jclass) {
    return env->NewStringUTF(vkrControlCapabilities().c_str());
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

// The window reference acquired here is released by the render-thread output registry.
#include "android_displays.h"
extern "C" JNIEXPORT void JNICALL
Java_dev_vkrender_renderer_DisplayOutputs_nativeSetDisplaySurface(JNIEnv* env,jclass,jint id,
        jobject surface,jfloat x,jfloat y,jfloat width,jfloat height,jboolean recreate) {
    ANativeWindow* window=surface?ANativeWindow_fromSurface(env,surface):nullptr;
    vkrSetDisplayWindow(id,window,x,y,width,height,recreate);
}

extern "C" JNIEXPORT jstring JNICALL
Java_dev_vkrender_renderer_DisplayOutputs_nativeDisplayStatus(JNIEnv* env,jclass) {
    return env->NewStringUTF(vkrDisplayStatus().c_str());
}

#include "preview_mailbox.h"
vkr::PreviewMailbox::Result vkrPreview();
extern "C" JNIEXPORT jintArray JNICALL
Java_dev_vkrender_renderer_RendererService_nativePreview(JNIEnv* env,jclass) {
    auto frame=vkrPreview();if(frame.argb.empty())return nullptr;
    const jint header[]={static_cast<jint>(frame.width),static_cast<jint>(frame.height),static_cast<jint>(frame.sequence)};
    jintArray out=env->NewIntArray(frame.argb.size()+3);if(!out)return nullptr;
    env->SetIntArrayRegion(out,0,3,header);
    env->SetIntArrayRegion(out,3,frame.argb.size(),reinterpret_cast<const jint*>(frame.argb.data()));
    return out;
}
