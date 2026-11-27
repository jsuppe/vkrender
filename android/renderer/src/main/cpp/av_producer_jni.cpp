// FrameLink's A/V producer (samples/av_apps.cpp, built with main=flProducerMain)
// in the renderer APK's own ":avproducer" process, for the 6a identity
// captures (FrameLink decision 12). Same app UID as the renderer, because
// SELinux confines FrameLink's abstract sockets to one app's processes.
// Arguments come from AvProducerService's intent; stderr goes to a file the
// capture collects (Android discards a process's stderr otherwise).
#include <jni.h>
#include <android/log.h>
#include <cstdio>
#include <string>
#include <vector>

int flProducerMain(int, char**);

extern "C" JNIEXPORT jint JNICALL
Java_dev_vkrender_renderer_AvProducerService_nativeRun(JNIEnv* env, jclass, jobjectArray jargs, jstring jlog) {
    std::vector<std::string> args = {"framelink_av_producer"};
    const jsize n = env->GetArrayLength(jargs);
    for (jsize i = 0; i < n; ++i) {
        auto* s = (jstring)env->GetObjectArrayElement(jargs, i);
        const char* c = env->GetStringUTFChars(s, nullptr);
        args.emplace_back(c);
        env->ReleaseStringUTFChars(s, c);
        env->DeleteLocalRef(s);
    }
    const char* logPath = env->GetStringUTFChars(jlog, nullptr);
    if (!freopen(logPath, "w", stderr))
        __android_log_print(ANDROID_LOG_WARN, "vkrender", "avproducer: cannot open %s", logPath);
    env->ReleaseStringUTFChars(jlog, logPath);
    setvbuf(stderr, nullptr, _IOLBF, 0);
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    const int rc = flProducerMain((int)args.size(), argv.data());
    fflush(stderr);
    __android_log_print(ANDROID_LOG_INFO, "vkrender", "avproducer: exit %d", rc);
    return rc;
}
