// M18: JNI host for the AirPlay receiver client app.
//
// Wraps the UxPlay raop server core: starts it against the renderer-provided
// Surface (video callback -> AirplayVideoSink -> MediaCodec), and exposes the
// dnssd service descriptions so AirplayService registers them with Android's
// NsdManager (see dnssd_android.c).

#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <cstring>
#include <string>

extern "C" {
#include "dnssd.h"
#include "logger.h"
#include "raop.h"
#include "stream.h"
}

#include "airplay_video.h"

#define VKR_LOG(...) __android_log_print(ANDROID_LOG_INFO, "vkrender", __VA_ARGS__)
#define VKR_ERR(...) __android_log_print(ANDROID_LOG_ERROR, "vkrender", __VA_ARGS__)

extern "C" {
const char* vkr_dnssd_service_name(dnssd_t* dnssd, int which);
unsigned short vkr_dnssd_service_port(dnssd_t* dnssd, int which);
int vkr_dnssd_txt_count(dnssd_t* dnssd, int which);
const char* vkr_dnssd_txt_entry(dnssd_t* dnssd, int which, int idx);
}

namespace {

vkr::AirplayVideoSink g_sink;
raop_t* g_raop = nullptr;
dnssd_t* g_dnssd = nullptr;
ANativeWindow* g_window = nullptr;

// ---- raop callbacks (invoked on raop's own threads) -------------------------

void cbVideoProcess(void*, raop_ntp_t*, h264_decode_struct* data) {
    g_sink.feed(data->data, data->data_len, (int64_t)(data->ntp_time_remote / 1000));
}
void cbAudioProcess(void*, raop_ntp_t*, audio_decode_struct*) {} // video-only scene
void cbVideoPause(void*) {}
void cbVideoResume(void*) {}
void cbVideoFlush(void*) { g_sink.flush(); }
void cbAudioFlush(void*) {}
void cbConnInit(void*) { VKR_LOG("airplay: client connected"); }
void cbConnDestroy(void*) { VKR_LOG("airplay: client disconnected"); }
void cbConnReset(void*, int, bool) { VKR_LOG("airplay: connection reset"); }
void cbConnTeardown(void*, bool*, bool*) {}
void cbReportClient(void*, char* deviceid, char*, char* name, bool* admit) {
    VKR_LOG("airplay: sender '%s' (%s)", name ? name : "?", deviceid ? deviceid : "?");
    *admit = true;
}
void cbLog(void*, int level, const char* msg) {
    __android_log_print(level <= LOGGER_ERR ? ANDROID_LOG_ERROR : ANDROID_LOG_INFO,
                        "vkrender-airplay", "%s", msg);
}

} // namespace

extern "C" JNIEXPORT jboolean JNICALL
Java_dev_vkrender_airplay_AirplayService_nativeStart(JNIEnv* env, jobject, jobject jsurface,
                                                     jstring jname, jbyteArray jhwaddr,
                                                     jstring jkeyfile) {
    if (g_raop) return JNI_TRUE; // already running
    g_window = ANativeWindow_fromSurface(env, jsurface);
    if (!g_window) {
        VKR_ERR("airplay: no surface");
        return JNI_FALSE;
    }
    g_sink.start(g_window);

    const char* name = env->GetStringUTFChars(jname, nullptr);
    const char* keyfile = env->GetStringUTFChars(jkeyfile, nullptr);
    jbyte hw[6] = {};
    env->GetByteArrayRegion(jhwaddr, 0, 6, hw);

    int error = 0;
    g_dnssd = dnssd_init(name, (int)strlen(name), (const char*)hw, 6, &error, 0);
    if (!g_dnssd) {
        VKR_ERR("airplay: dnssd_init failed (%d)", error);
        return JNI_FALSE;
    }

    raop_callbacks_t cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.video_process = cbVideoProcess;
    cbs.audio_process = cbAudioProcess;
    cbs.video_pause = cbVideoPause;
    cbs.video_resume = cbVideoResume;
    cbs.video_flush = cbVideoFlush;
    cbs.audio_flush = cbAudioFlush;
    cbs.conn_init = cbConnInit;
    cbs.conn_destroy = cbConnDestroy;
    cbs.conn_reset = cbConnReset;
    cbs.conn_teardown = cbConnTeardown;
    cbs.report_client_request = cbReportClient;

    g_raop = raop_init(&cbs);
    if (!g_raop) {
        VKR_ERR("airplay: raop_init failed");
        return JNI_FALSE;
    }
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X", (uint8_t)hw[0],
             (uint8_t)hw[1], (uint8_t)hw[2], (uint8_t)hw[3], (uint8_t)hw[4], (uint8_t)hw[5]);
    if (raop_init2(g_raop, 1, macStr, keyfile)) {
        VKR_ERR("airplay: raop_init2 failed");
        return JNI_FALSE;
    }
    raop_set_log_callback(g_raop, cbLog, nullptr);
    raop_set_log_level(g_raop, LOGGER_INFO);
    raop_set_plist(g_raop, "width", 1920);
    raop_set_plist(g_raop, "height", 1080);
    raop_set_plist(g_raop, "refreshRate", 60);
    raop_set_plist(g_raop, "maxFPS", 30);
    raop_set_plist(g_raop, "overscanned", 0);
    raop_set_plist(g_raop, "max_ntp_timeouts", 5);

    unsigned short port = 0;
    // httpd_start (via raop_start) returns 1 on success, <=0 on failure.
    if (raop_start(g_raop, &port) <= 0) {
        VKR_ERR("airplay: raop_start failed");
        return JNI_FALSE;
    }
    raop_set_port(g_raop, port);
    raop_set_dnssd(g_raop, g_dnssd);
    dnssd_register_raop(g_dnssd, port);
    dnssd_register_airplay(g_dnssd, port);

    env->ReleaseStringUTFChars(jname, name);
    env->ReleaseStringUTFChars(jkeyfile, keyfile);
    VKR_LOG("airplay: raop server on port %u", port);
    return JNI_TRUE;
}

// which: 0 = _raop._tcp, 1 = _airplay._tcp. Returns [name, port, k=v, ...].
extern "C" JNIEXPORT jobjectArray JNICALL
Java_dev_vkrender_airplay_AirplayService_nativeGetService(JNIEnv* env, jobject, jint which) {
    if (!g_dnssd) return nullptr;
    int count = vkr_dnssd_txt_count(g_dnssd, which);
    jobjectArray arr =
        env->NewObjectArray(2 + count, env->FindClass("java/lang/String"), nullptr);
    env->SetObjectArrayElement(arr, 0,
                               env->NewStringUTF(vkr_dnssd_service_name(g_dnssd, which)));
    char portStr[8];
    snprintf(portStr, sizeof(portStr), "%u", vkr_dnssd_service_port(g_dnssd, which));
    env->SetObjectArrayElement(arr, 1, env->NewStringUTF(portStr));
    for (int i = 0; i < count; ++i)
        env->SetObjectArrayElement(arr, 2 + i,
                                   env->NewStringUTF(vkr_dnssd_txt_entry(g_dnssd, which, i)));
    return arr;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_dev_vkrender_airplay_AirplayService_nativeSurfaceDead(JNIEnv*, jobject) {
    return g_sink.surfaceDead() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_dev_vkrender_airplay_AirplayService_nativeStop(JNIEnv*, jobject) {
    if (g_raop) {
        raop_stop(g_raop);
        raop_destroy(g_raop);
        g_raop = nullptr;
    }
    g_sink.stop();
    if (g_dnssd) {
        dnssd_destroy(g_dnssd);
        g_dnssd = nullptr;
    }
    if (g_window) {
        ANativeWindow_release(g_window);
        g_window = nullptr;
    }
    VKR_LOG("airplay: stopped");
}
