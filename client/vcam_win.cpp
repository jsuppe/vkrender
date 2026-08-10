// M24: virtual camera fed from the scene. Negotiates a view with the renderer
// (consumer API), reads each delivered frame back and pumps it into a virtual
// camera, so the vkrender scene becomes a webcam.
//
//   vkr_vcam [pipe] [width] [height] [fps]   DirectShow softcam (default)
//   vkr_vcam --mf [...]                      Media Foundation camera
//   vkr_vcam --unregister                    remove the MF camera (elevated)
//
// Default tier is the softcam DirectShow filter: Chrome, OBS, Zoom, classic
// Teams. --mf uses MFCreateVirtualCamera instead, which Windows bridges into
// DirectShow too - so one registration covers MF/UWP apps *and* DirectShow
// apps, and it is the tier anything that persists a camera choice needs.
//
// One-time setup (elevated):  regsvr32 softcam.dll
//                             regsvr32 vkrender_vcam_source.dll   (--mf)
// The installer does both. Registration details, measured instance paths and
// the Teams Rooms content-camera analysis: docs/VIRTUAL_CAMERA.md

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
#include "framelink.h"
#include "framelink_d3d11.h"

using Microsoft::WRL::ComPtr;

// The vkrender camera media source CLSID (vkrender_vcam_source.dll,
// registered once with elevated regsvr32).
static const wchar_t* kVcamClsid = L"{9D8BF3A1-5F2C-4C6B-B2A4-7E1D0C3E5A21}";

// M26: create the MF virtual camera, preferring a registration that OUTLIVES
// this process.
//
// Teams Rooms (and anything else that persists a camera choice) stores a
// device instance path and health-checks it, so a camera that only exists
// while vkr_vcam runs reads as "missing" whenever the appliance is down. A
// System-lifetime camera stays registered across process exit and reboot; the
// Camera Frame Server activates vkrender_vcam_source.dll on demand.
//
// System lifetime + AllUsers access need elevation. Fall back to the original
// session-scoped registration rather than failing, so a non-elevated run keeps
// working exactly as it did before.  Measured instance path either way:
//   SWD\VCAMDEVAPI\<stable hash of name+CLSID>   (class SoftwareDevice,
//   registered under KSCATEGORY_VIDEO_CAMERA - see docs/VIRTUAL_CAMERA.md)
static HRESULT createRaw(ComPtr<IMFVirtualCamera>& cam, MFVirtualCameraLifetime life,
                         MFVirtualCameraAccess access) {
    return MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource, life, access,
                                 L"vkrender Camera", kVcamClsid, nullptr, 0, &cam);
}

// Measured: MFCreateVirtualCamera(System, AllUsers) SUCCEEDS unelevated - it is
// Start() that then fails with E_ACCESSDENIED (0x80070005). So the privilege
// check has to happen around Start, not around Create, or an unelevated run
// silently ends up with no camera at all.
static HRESULT startVirtualCamera(ComPtr<IMFVirtualCamera>& cam, bool* persistent) {
    HRESULT hr = createRaw(cam, MFVirtualCameraLifetime_System, MFVirtualCameraAccess_AllUsers);
    if (SUCCEEDED(hr)) {
        hr = cam->Start(nullptr);
        if (SUCCEEDED(hr)) {
            *persistent = true;
            return S_OK;
        }
        cam->Remove(); // never leave a registered-but-dead system camera behind
        cam->Shutdown();
        cam.Reset();
    }
    *persistent = false;
    hr = createRaw(cam, MFVirtualCameraLifetime_Session, MFVirtualCameraAccess_CurrentUser);
    if (FAILED(hr)) return hr;
    return cam->Start(nullptr);
}

// A System-lifetime camera survives uninstall unless something removes it, so
// the uninstaller calls this to avoid leaving a ghost camera behind.
static int unregisterVirtualCamera() {
    if (FAILED(MFStartup(MF_VERSION))) return 1;
    ComPtr<IMFVirtualCamera> cam;
    HRESULT hr = createRaw(cam, MFVirtualCameraLifetime_System, MFVirtualCameraAccess_AllUsers);
    if (SUCCEEDED(hr)) hr = cam->Remove();
    if (FAILED(hr))
        fprintf(stderr, "vcam: could not remove the virtual camera (0x%08lx) - "
                        "run this elevated to remove a system-wide registration\n", hr);
    else
        printf("vcam: virtual camera registration removed\n");
    cam.Reset();
    MFShutdown();
    return SUCCEEDED(hr) ? 0 : 1;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    bool mfMode = false;
    // strip --mf / --unregister from argv
    int argn = 1;
    const char* args[8] = {};
    for (int i = 1; i < argc && argn < 8; ++i) {
        if (strcmp(argv[i], "--mf") == 0) {
            mfMode = true;
        } else if (strcmp(argv[i], "--unregister") == 0) {
            return unregisterVirtualCamera();
        } else {
            args[argn++] = argv[i];
        }
    }
    const char* pipe = argn > 1 ? args[1] : "vkrender";
    unsigned w = argn > 2 ? (unsigned)atoi(args[2]) : 1280;
    unsigned h = argn > 3 ? (unsigned)atoi(args[3]) : 720;
    unsigned fps = argn > 4 ? (unsigned)atoi(args[4]) : 30;
    if (mfMode) { w = 1280; h = 720; } // must match the camera's advertised format

    // framelink: the camera OWNS the ring and the renderer attaches as a
    // producer. That is the inversion the library forces, and it is why the
    // renderer now dials out to us rather than us negotiating a view from it -
    // the ring belongs to whoever consumes it, on every platform.
    //
    // Consequence worth knowing: this no longer fails when the renderer is
    // down. The channel exists as soon as we create it; the renderer attaches
    // whenever it comes up, and flAcquireFrame simply times out until then.
    char channel[128];
    snprintf(channel, sizeof(channel), "%s.scene", pipe);
    flChannel* c = nullptr;
    flResult fr = flCreateChannel(channel, w, h, FL_FORMAT_BGRA8, 4, &c);
    if (fr != FL_OK) {
        fprintf(stderr, "vcam: flCreateChannel('%s') -> %s\n", channel, flResultString(fr));
        return 1;
    }
    printf("vcam: channel '%s' %ux%u @%u fps - waiting for the renderer\n", channel, w, h, fps);

    // softcam wants multiples of four.
    if ((w & 3) || (h & 3)) {
        fprintf(stderr, "vcam: view %ux%u not a multiple of 4\n", w, h);
        return 1;
    }

    scCamera cam = nullptr;
    ComPtr<IMFVirtualCamera> mfCam;
    bool persistent = false;
    HANDLE shm = nullptr, shmMutex = nullptr;
    void* shmView = nullptr;

    if (mfMode) {
        // Windows 11 virtual camera: visible to MF/UWP *and* DirectShow apps.
        // Our media source (vkrender_vcam_source.dll, registered) runs in the
        // Camera Frame Server; frames travel via the Global\ shared ring the
        // source creates once a camera app opens the stream.
        if (FAILED(MFStartup(MF_VERSION))) return 1;
        HRESULT hr = startVirtualCamera(mfCam, &persistent);
        if (FAILED(hr)) {
            fprintf(stderr,
                    "vcam: could not start the MF virtual camera (0x%08lx) - is "
                    "vkrender_vcam_source.dll registered (elevated regsvr32)?\n", hr);
            return 1;
        }
        printf("vcam: MF virtual camera 'vkrender Camera' up - visible to all apps\n");
        printf("vcam: registration is %s\n",
               persistent ? "system-wide and persists after this process exits "
                            "(needed by Teams Rooms content camera)"
                          : "session-scoped - it disappears when this process exits; "
                            "run elevated for a persistent camera");
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

    ID3D11Device* dev = flChannelD3D11Device(c);
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
        flFrame frame{};
        flResult r = flAcquireFrame(c, &frame, 2000);
        // Neither an idle renderer nor a departed one is fatal now: we own the
        // channel, so the camera stays up and the renderer can reattach. Before
        // the port a disconnect ended the process and took the camera with it.
        if (r == FL_TIMEOUT || r == FL_DISCONNECTED) continue;
        if (r != FL_OK) {
            fprintf(stderr, "vcam: flAcquireFrame -> %s\n", flResultString(r));
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

        ctx->CopyResource(staging.Get(), flImageD3D11(frame.image));
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
        flRelease(c, &frame);

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
        // Only stop a session camera. Stopping a system-wide one would defeat
        // the point: the registration must stay visible while the appliance is
        // down, so persistent cameras are released, never stopped.
        // `--unregister` is the way to actually remove one.
        if (!persistent) mfCam->Stop();
        mfCam->Shutdown();
        MFShutdown();
    }
    if (shmView) UnmapViewOfFile(shmView);
    if (shmMutex) CloseHandle(shmMutex);
    if (shm) CloseHandle(shm);
    if (cam) scDeleteCamera(cam);
    flClose(c);
    return 0;
}

#endif // _WIN32
