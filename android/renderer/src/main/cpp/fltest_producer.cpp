// M40: the native half of FlTestService - a framelink producer that draws an
// animated pattern with the CPU and submits at ~30 fps, stamping
// CLOCK_MONOTONIC so the renderer's hop measurement means something. The
// Android sibling of test/win_client_demo.cpp, scoped down to what an
// on-device smoke test needs.

#ifdef __ANDROID__

#include <jni.h>
#include <time.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "framelink.h"
#include "framelink_slots.h"

#include "log.h"

namespace {

int64_t monotonicNs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

} // namespace

extern "C" JNIEXPORT jint JNICALL Java_dev_vkrender_renderer_FlTestService_nativeProduce(
    JNIEnv* env, jclass, jstring jchannel, jint frames) {
    const char* cname = env->GetStringUTFChars(jchannel, nullptr);
    std::string name = cname ? cname : "vkrender.in.0";
    env->ReleaseStringUTFChars(jchannel, cname);

    flChannel* ch = nullptr;
    flResult r = FL_NOT_FOUND;
    for (int attempt = 0; attempt < 50 && r == FL_NOT_FOUND; ++attempt) {
        r = flOpenProducer(name.c_str(), &ch);
        if (r == FL_NOT_FOUND) usleep(100000);
    }
    if (r != FL_OK) {
        VKR_ERR("fltest: flOpenProducer('%s') -> %s", name.c_str(), flResultString(r));
        return 0;
    }

    // Ask for a size the renderer did not pre-allocate - the reallocation path
    // is part of what this smoke test exists to prove on-device.
    flRequestGeometry(ch, 960, 540, FL_FORMAT_RGBA8);
    flChannelInfo info{};
    flQuery(ch, &info);
    VKR_LOG("fltest: on '%s' %ux%u ring of %u gen %llu", name.c_str(), info.width, info.height,
            info.poolSize, (unsigned long long)info.generation);

    int submitted = 0;
    for (int i = 1; i <= frames; ++i) {
        flBuffer b{};
        if (flAcquireBuffer(ch, &b, 1000) != FL_OK) break;
        void* px = nullptr;
        uint64_t stride = 0;
        if (flImageMap(b.image, &px, &stride) == FL_OK && px) {
            // Hue-cycling field with a sweeping bar: obviously animated on the
            // cube, and each frame distinct.
            const uint8_t rr = (uint8_t)((i * 5) & 0xFF);
            const uint8_t gg = (uint8_t)((i * 11) & 0xFF);
            const uint8_t bb = (uint8_t)((i * 3) & 0xFF);
            const uint32_t bar = (uint32_t)((i * 12) % (int)info.width);
            uint8_t* row = (uint8_t*)px;
            for (uint32_t y = 0; y < info.height; ++y, row += stride) {
                for (uint32_t x = 0; x < info.width; ++x) {
                    const bool inBar = x >= bar && x < bar + 48;
                    row[x * 4 + 0] = inBar ? 255 : rr; // R (RGBA8 - the channel says so)
                    row[x * 4 + 1] = inBar ? 255 : gg;
                    row[x * 4 + 2] = inBar ? 255 : bb;
                    row[x * 4 + 3] = 255;
                }
            }
        }
        // flSubmit unmaps for us - the AHardwareBuffer_unlock cache boundary.
        if (flSubmit(ch, &b, monotonicNs()) != FL_OK) break;
        ++submitted;
        usleep(33000); // ~30 fps
    }
    flClose(ch);
    return submitted;
}

#endif // __ANDROID__
