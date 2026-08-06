// M24: virtual camera fed from the scene. Negotiates a view with the
// renderer (consumer API), reads each delivered frame back and pumps it into
// the softcam DirectShow virtual camera - the vkrender scene becomes a
// webcam in Chrome / OBS / Zoom / classic Teams (DirectShow enumeration;
// MF-only UWP apps need the future MFCreateVirtualCamera source).
//
//   vkr_vcam [pipe] [width] [height] [fps]
//
// One-time setup (elevated):  regsvr32 softcam.dll

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d11.h>
#include <mfapi.h>
#include <mfvirtualcamera.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "softcam.h"
#include "vcam_shared.h"
#include "vkrender_client.h"

using Microsoft::WRL::ComPtr;

// The vkrender camera media source CLSID (vkrender_vcam_source.dll,
// registered once with elevated regsvr32).
static const wchar_t* kVcamClsid = L"{9D8BF3A1-5F2C-4C6B-B2A4-7E1D0C3E5A21}";

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    bool mfMode = false;
    // strip --mf from argv
    int argn = 1;
    const char* args[8] = {};
    for (int i = 1; i < argc && argn < 8; ++i) {
        if (strcmp(argv[i], "--mf") == 0) {
            mfMode = true;
        } else {
            args[argn++] = argv[i];
        }
    }
    const char* pipe = argn > 1 ? args[1] : "vkrender";
    unsigned w = argn > 2 ? (unsigned)atoi(args[2]) : 1280;
    unsigned h = argn > 3 ? (unsigned)atoi(args[3]) : 720;
    unsigned fps = argn > 4 ? (unsigned)atoi(args[4]) : 30;
    if (mfMode) { w = 1280; h = 720; } // must match the camera's advertised format

    vkrClient* c = vkrConnectConsumer(pipe, nullptr, &w, &h, fps);
    if (!c) {
        fprintf(stderr, "vcam: cannot reach vkrenderer service on pipe '%s'\n", pipe);
        return 1;
    }
    printf("vcam: negotiated scene view %ux%u @%u fps\n", w, h, fps);

    // softcam wants multiples of four.
    if ((w & 3) || (h & 3)) {
        fprintf(stderr, "vcam: view %ux%u not a multiple of 4\n", w, h);
        return 1;
    }

    scCamera cam = nullptr;
    ComPtr<IMFVirtualCamera> mfCam;
    HANDLE shm = nullptr, shmMutex = nullptr;
    void* shmView = nullptr;

    if (mfMode) {
        // Windows 11 virtual camera: visible to MF/UWP *and* DirectShow apps.
        // Our media source (vkrender_vcam_source.dll, registered) runs in the
        // Camera Frame Server; frames travel via the Global\ shared ring the
        // source creates once a camera app opens the stream.
        if (FAILED(MFStartup(MF_VERSION))) return 1;
        HRESULT hr = MFCreateVirtualCamera(
            MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_Session,
            MFVirtualCameraAccess_CurrentUser, L"vkrender Camera", kVcamClsid, nullptr, 0,
            &mfCam);
        if (FAILED(hr)) {
            fprintf(stderr,
                    "vcam: MFCreateVirtualCamera failed (0x%08lx) - is "
                    "vkrender_vcam_source.dll registered (elevated regsvr32)?\n", hr);
            return 1;
        }
        hr = mfCam->Start(nullptr);
        if (FAILED(hr)) {
            fprintf(stderr, "vcam: IMFVirtualCamera::Start failed (0x%08lx)\n", hr);
            return 1;
        }
        printf("vcam: MF virtual camera 'vkrender Camera' up - visible to all apps\n");
    } else {
        // framerate 0: we push in real time as frames arrive (renderer paces).
        cam = scCreateCamera((int)w, (int)h, 0.0f);
        if (!cam) {
            fprintf(stderr,
                    "vcam: scCreateCamera failed - is softcam.dll registered "
                    "(elevated: regsvr32 softcam.dll) and no other instance running?\n");
            return 1;
        }
        printf("vcam: virtual camera 'DirectShow Softcam' up - open it in a camera app\n");
    }

    ID3D11Device* dev = vkrGetDevice(c);
    ComPtr<ID3D11DeviceContext> ctx;
    dev->GetImmediateContext(&ctx);

    D3D11_TEXTURE2D_DESC sd{};
    sd.Width = w;
    sd.Height = h;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(dev->CreateTexture2D(&sd, nullptr, &staging))) return 1;

    std::vector<unsigned char> bgr((size_t)w * h * 3);
    uint64_t frames = 0, lastLog = 0;

    for (;;) {
        vkrFrame frame;
        int r = vkrAcquireFrame(c, &frame, 2000);
        if (r == 1) continue; // renderer idle; keep the camera alive
        if (r != 0) {
            fprintf(stderr, "vcam: renderer disconnected\n");
            break;
        }
        // MF mode: the transport appears once a camera app opens the stream
        // (the media source creates it in the frame server).
        if (mfMode && !shmView) {
            if (!shm) shm = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, VKR_VCAM_SHM);
            if (shm && !shmMutex) shmMutex = OpenMutexW(SYNCHRONIZE, FALSE, VKR_VCAM_MUTEX);
            if (shm && shmMutex) {
                shmView = MapViewOfFile(shm, FILE_MAP_ALL_ACCESS, 0, 0, 0);
                if (shmView) printf("vcam: transport open - an app opened the camera\n");
            }
        }

        ctx->CopyResource(staging.Get(), frame.texture);
        D3D11_MAPPED_SUBRESOURCE map;
        if (SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) {
            if (mfMode) {
                if (shmView && WaitForSingleObject(shmMutex, 10) == WAIT_OBJECT_0) {
                    auto* hd = (vkr::VcamHeader*)shmView;
                    unsigned char* px = (unsigned char*)shmView + sizeof(vkr::VcamHeader);
                    for (unsigned y = 0; y < h; ++y)
                        memcpy(px + (size_t)y * w * 4,
                               (const unsigned char*)map.pData + y * map.RowPitch,
                               (size_t)w * 4);
                    hd->width = w;
                    hd->height = h;
                    hd->frameCounter++;
                    ReleaseMutex(shmMutex);
                    ++frames;
                }
            } else {
                // BGRA -> tight 24-bit BGR (softcam's shared-memory format).
                for (unsigned y = 0; y < h; ++y) {
                    const unsigned char* src =
                        (const unsigned char*)map.pData + y * map.RowPitch;
                    unsigned char* dst = bgr.data() + (size_t)y * w * 3;
                    for (unsigned x = 0; x < w; ++x) {
                        dst[x * 3 + 0] = src[x * 4 + 0];
                        dst[x * 3 + 1] = src[x * 4 + 1];
                        dst[x * 3 + 2] = src[x * 4 + 2];
                    }
                }
                scSendFrame(cam, bgr.data());
                ++frames;
            }
            ctx->Unmap(staging.Get(), 0);
        }
        vkrReleaseFrame(c, &frame);

        if (frames - lastLog >= 150) {
            if (mfMode) {
                printf("vcam: %llu frames -> camera (transport %s)\n",
                       (unsigned long long)frames, shmView ? "live" : "waiting for an app");
            } else {
                printf("vcam: %llu frames -> camera (%s)\n", (unsigned long long)frames,
                       scIsConnected(cam) ? "an app is watching" : "no app connected");
            }
            lastLog = frames;
        }
    }

    if (mfCam) {
        mfCam->Stop();
        mfCam->Shutdown();
        MFShutdown();
    }
    if (shmView) UnmapViewOfFile(shmView);
    if (shmMutex) CloseHandle(shmMutex);
    if (shm) CloseHandle(shm);
    if (cam) scDeleteCamera(cam);
    vkrDisconnect(c);
    return 0;
}

#endif // _WIN32
