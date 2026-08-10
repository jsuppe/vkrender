// framelink Windows backend (spike). See framelink.h.
//
// The producer half is the mechanism already proven in client/vkrender_client.cpp;
// the consumer half is new here, because in vkrender the ring owner is the
// renderer service rather than the client library.
//
// Reuses vkr::IpcChannel and vkr::proto for the spike so the transport under
// test is the one already in production. A standalone repo would vendor a
// trimmed copy - that is packaging, not design, and doing it now would only
// obscure whether the API shape works.

#define FL_EXPORTS
#include "framelink.h"
#include "framelink_d3d11.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d11_4.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ipc.h"
#include "log.h"
#include "protocol.h"

using Microsoft::WRL::ComPtr;

struct flImage {
    ComPtr<ID3D11Texture2D> tex;
    HANDLE shared = nullptr; // borrowed by callers; owned here
};

struct flChannel {
    bool isConsumer = false;
    std::string name;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext4> ctx;
    ComPtr<ID3D11Fence> readyFence, consumedFence;
    HANDLE readyShared = nullptr, consumedShared = nullptr;
    HANDLE waitEvent = nullptr;

    std::vector<flImage*> images;
    std::vector<uint64_t> slotReadyValue; // producer: last value signalled per slot
    uint64_t readyValue = 0;              // producer: monotonic submit counter
    uint64_t consumedValue = 0;           // consumer: monotonic release counter

    flChannelInfo info{};

    // --- consumer only ---
    vkr::IpcChannel peer;
    std::thread accept;
    std::atomic<bool> stop{false};
    std::atomic<bool> peerGone{false};
    std::mutex pendingMutex;
    bool hasPending = false;
    uint32_t pendingSlot = 0;
    uint64_t pendingReady = 0;
    int64_t pendingPts = -1;
    uint64_t sequence = 0;

    // --- producer only ---
    vkr::IpcChannel link;
};

namespace {

constexpr uint32_t kMaxPool = vkr::proto::kMaxPoolSize;

ComPtr<ID3D11Device> createDevice(const uint8_t luid[8], bool luidValid) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return nullptr;
    ComPtr<IDXGIAdapter1> adapter, pick;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; ++i) {
        DXGI_ADAPTER_DESC1 d;
        adapter->GetDesc1(&d);
        if (!luidValid || memcmp(&d.AdapterLuid, luid, 8) == 0) {
            pick = adapter;
            break;
        }
    }
    ComPtr<ID3D11Device> dev;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
    if (FAILED(D3D11CreateDevice(pick.Get(),
                                 pick ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                                 nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr,
                                 nullptr)))
        return nullptr;
    // Not optional: a user-supplied device without this gives intermittent
    // hangs that look like sync bugs (learned the hard way in M2).
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(dev.As(&mt))) mt->SetMultithreadProtected(TRUE);
    return dev;
}

bool finishDeviceSetup(flChannel* ch) {
    ComPtr<ID3D11DeviceContext> ctx;
    ch->device->GetImmediateContext(&ctx);
    if (FAILED(ctx.As(&ch->ctx))) return false;
    ch->waitEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return ch->waitEvent != nullptr;
}

// Consumer: allocate the ring. Everything the producer will ever draw into is
// created here, because the ring belongs to the consumer.
bool allocateRing(flChannel* ch, uint32_t w, uint32_t h, uint32_t poolSize) {
    ComPtr<ID3D11Device5> dev5;
    if (FAILED(ch->device.As(&dev5))) return false;

    for (uint32_t i = 0; i < poolSize; ++i) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        // NT handles, not the legacy KMT ones: required for DuplicateHandle
        // into another process and for Vulkan/D3D12 import.
        td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        auto* img = new flImage();
        if (FAILED(ch->device->CreateTexture2D(&td, nullptr, &img->tex))) {
            delete img;
            return false;
        }
        ComPtr<IDXGIResource1> res;
        if (FAILED(img->tex.As(&res)) ||
            FAILED(res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ |
                                                        DXGI_SHARED_RESOURCE_WRITE,
                                           nullptr, &img->shared))) {
            delete img;
            return false;
        }
        ch->images.push_back(img);
        ch->slotReadyValue.push_back(0);
    }

    if (FAILED(dev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&ch->readyFence))) ||
        FAILED(dev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED,
                                 IID_PPV_ARGS(&ch->consumedFence))))
        return false;
    if (FAILED(ch->readyFence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr,
                                                  &ch->readyShared)) ||
        FAILED(ch->consumedFence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr,
                                                     &ch->consumedShared)))
        return false;
    return true;
}

// Consumer: one producer, handshake then pump FrameReady. Spike scope - fan-out
// needs the back-pressure question answered first (docs/FRAMELINK.md).
void acceptLoop(flChannel* ch) {
    while (!ch->stop.load()) {
        vkr::IpcChannel link = vkr::IpcChannel::listen(ch->name);
        if (!link.connected()) return;
        if (ch->stop.load()) return;

        HANDLE peerProc = OpenProcess(PROCESS_DUP_HANDLE, FALSE, link.peerPid());
        if (!peerProc) {
            VKR_ERR("framelink: OpenProcess(pid %u) failed", link.peerPid());
            continue;
        }

        vkr::proto::HelloMsg hello{};
        hello.rendererPid = GetCurrentProcessId();
        hello.luidValid = 1;
        memcpy(hello.deviceLUID, ch->info.adapterLUID, 8);

        vkr::proto::PoolDescMsg pd{};
        pd.width = ch->info.width;
        pd.height = ch->info.height;
        pd.format = vkr::proto::SharedFormat::BGRA8;
        pd.poolSize = (uint32_t)ch->images.size();
        pd.planeCount = 1;
        bool ok = true;
        for (size_t i = 0; i < ch->images.size(); ++i) {
            HANDLE dup = nullptr;
            if (!DuplicateHandle(GetCurrentProcess(), ch->images[i]->shared, peerProc, &dup, 0,
                                 FALSE, DUPLICATE_SAME_ACCESS)) {
                ok = false;
                break;
            }
            pd.textureHandle[i] = (uint64_t)(uintptr_t)dup;
        }
        HANDLE dupReady = nullptr, dupConsumed = nullptr;
        ok = ok &&
             DuplicateHandle(GetCurrentProcess(), ch->readyShared, peerProc, &dupReady, 0, FALSE,
                             DUPLICATE_SAME_ACCESS) &&
             DuplicateHandle(GetCurrentProcess(), ch->consumedShared, peerProc, &dupConsumed, 0,
                             FALSE, DUPLICATE_SAME_ACCESS);
        pd.readyFenceHandle = (uint64_t)(uintptr_t)dupReady;
        pd.consumedFenceHandle = (uint64_t)(uintptr_t)dupConsumed;
        CloseHandle(peerProc);
        if (!ok) {
            VKR_ERR("framelink: DuplicateHandle into producer failed");
            continue;
        }
        if (!link.send(vkr::proto::MsgType::Hello, hello) ||
            !link.send(vkr::proto::MsgType::PoolDesc, pd))
            continue;

        ch->peerGone = false;
        VKR_LOG("framelink: producer pid %u attached to '%s'", link.peerPid(),
                ch->name.c_str());

        for (;;) {
            auto msg = link.recv();
            if (!msg || msg->type == vkr::proto::MsgType::Bye) break;
            if (msg->type != vkr::proto::MsgType::FrameReady) continue;
            const auto* fr = msg->as<vkr::proto::FrameReadyMsg>();
            if (!fr || fr->slot >= ch->images.size()) continue;
            std::lock_guard<std::mutex> hold(ch->pendingMutex);
            // Latest-frame-wins: a slow consumer must never stall the producer.
            ch->hasPending = true;
            ch->pendingSlot = fr->slot;
            ch->pendingReady = fr->readyValue;
            ch->pendingPts = fr->ptsNs;
            ++ch->sequence;
        }
        ch->peerGone = true;
        VKR_LOG("framelink: producer detached from '%s'", ch->name.c_str());
        if (ch->stop.load()) return;
    }
}

} // namespace

extern "C" {

FL_API const char* flResultString(flResult r) {
    switch (r) {
        case FL_OK: return "ok";
        case FL_TIMEOUT: return "timeout";
        case FL_DISCONNECTED: return "peer disconnected";
        case FL_VERSION_MISMATCH: return "protocol version mismatch";
        case FL_FORMAT_UNSUPPORTED: return "format unsupported";
        case FL_ADAPTER_MISMATCH: return "producer and consumer on different GPUs";
        case FL_ACCESS_DENIED: return "access denied";
        case FL_NOT_FOUND: return "channel not found";
        case FL_INVALID: return "invalid argument";
        case FL_OOM: return "out of memory";
        case FL_INTERNAL: return "internal error";
    }
    return "unknown";
}

FL_API flResult flCreateChannel(const char* name, uint32_t width, uint32_t height,
                                flFormat format, uint32_t poolSize, flChannel** out) {
    if (!name || !out || width == 0 || height == 0) return FL_INVALID;
    if (format != FL_FORMAT_BGRA8) return FL_FORMAT_UNSUPPORTED; // spike scope
    if (poolSize == 0 || poolSize > kMaxPool) return FL_INVALID;

    auto* ch = new flChannel();
    ch->isConsumer = true;
    ch->name = name;
    ch->device = createDevice(nullptr, false);
    if (!ch->device || !finishDeviceSetup(ch)) {
        flClose(ch);
        return FL_INTERNAL;
    }

    ComPtr<IDXGIDevice> dxgiDev;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC ad{};
    if (SUCCEEDED(ch->device.As(&dxgiDev)) && SUCCEEDED(dxgiDev->GetAdapter(&adapter)) &&
        SUCCEEDED(adapter->GetDesc(&ad)))
        memcpy(ch->info.adapterLUID, &ad.AdapterLuid, 8);

    ch->info.version = FL_VERSION;
    ch->info.width = width;
    ch->info.height = height;
    ch->info.format = format;
    ch->info.poolSize = poolSize;

    if (!allocateRing(ch, width, height, poolSize)) {
        flClose(ch);
        return FL_INTERNAL;
    }
    ch->accept = std::thread(acceptLoop, ch);
    *out = ch;
    return FL_OK;
}

FL_API flResult flOpenProducer(const char* name, flChannel** out) {
    if (!name || !out) return FL_INVALID;
    auto* ch = new flChannel();
    ch->isConsumer = false;
    ch->name = name;
    // Short timeout and no retry: a producer attaches to a channel that exists
    // or it does not. It never creates one.
    ch->link = vkr::IpcChannel::connect(name, 2000, true);
    if (!ch->link.connected()) {
        delete ch;
        return FL_NOT_FOUND;
    }

    vkr::proto::ClientHelloMsg hello{};
    hello.clientPid = GetCurrentProcessId();
    hello.format = vkr::proto::SharedFormat::BGRA8;
    if (!ch->link.send(vkr::proto::MsgType::ClientHello, hello)) {
        delete ch;
        return FL_INTERNAL;
    }

    auto helloMsg = ch->link.recv();
    if (!helloMsg || helloMsg->type != vkr::proto::MsgType::Hello) {
        delete ch;
        return FL_DISCONNECTED;
    }
    const auto* srv = helloMsg->as<vkr::proto::HelloMsg>();
    if (!srv) {
        delete ch;
        return FL_INTERNAL;
    }
    // Open on the consumer's adapter. Getting this wrong is the cross-adapter
    // failure the API promises to refuse rather than crash on.
    ch->device = createDevice(srv->deviceLUID, srv->luidValid != 0);
    if (!ch->device || !finishDeviceSetup(ch)) {
        flClose(ch);
        return FL_INTERNAL;
    }
    memcpy(ch->info.adapterLUID, srv->deviceLUID, 8);

    auto poolMsg = ch->link.recv();
    if (!poolMsg || poolMsg->type != vkr::proto::MsgType::PoolDesc) {
        flClose(ch);
        return FL_DISCONNECTED;
    }
    const auto* pd = poolMsg->as<vkr::proto::PoolDescMsg>();
    if (!pd || pd->poolSize == 0 || pd->poolSize > kMaxPool) {
        flClose(ch);
        return FL_INTERNAL;
    }

    ComPtr<ID3D11Device1> dev1;
    ComPtr<ID3D11Device5> dev5;
    if (FAILED(ch->device.As(&dev1)) || FAILED(ch->device.As(&dev5))) {
        flClose(ch);
        return FL_INTERNAL;
    }
    for (uint32_t i = 0; i < pd->poolSize; ++i) {
        auto* img = new flImage();
        img->shared = (HANDLE)(uintptr_t)pd->textureHandle[i];
        if (FAILED(dev1->OpenSharedResource1(img->shared, IID_PPV_ARGS(&img->tex)))) {
            delete img;
            flClose(ch);
            return FL_ADAPTER_MISMATCH;
        }
        ch->images.push_back(img);
        ch->slotReadyValue.push_back(0);
    }
    if (FAILED(dev5->OpenSharedFence((HANDLE)(uintptr_t)pd->readyFenceHandle,
                                     IID_PPV_ARGS(&ch->readyFence))) ||
        FAILED(dev5->OpenSharedFence((HANDLE)(uintptr_t)pd->consumedFenceHandle,
                                     IID_PPV_ARGS(&ch->consumedFence)))) {
        flClose(ch);
        return FL_INTERNAL;
    }

    ch->info.version = FL_VERSION;
    ch->info.width = pd->width;
    ch->info.height = pd->height;
    ch->info.format = FL_FORMAT_BGRA8;
    ch->info.poolSize = pd->poolSize;
    *out = ch;
    return FL_OK;
}

FL_API flResult flAcquireBuffer(flChannel* ch, flBuffer* out, uint32_t timeoutMs) {
    if (!ch || ch->isConsumer || !out) return FL_INVALID;
    if (!ch->link.connected()) return FL_DISCONNECTED;
    const DWORD deadline = GetTickCount() + timeoutMs;
    for (;;) {
        const uint64_t consumed = ch->consumedFence->GetCompletedValue();
        for (size_t i = 0; i < ch->images.size(); ++i) {
            // Free once the consumer has promised never to read this slot's
            // previous contents again.
            if (ch->slotReadyValue[i] <= consumed) {
                out->slot = (uint32_t)i;
                out->image = ch->images[i];
                return FL_OK;
            }
        }
        const DWORD now = GetTickCount();
        if (now >= deadline) return FL_TIMEOUT;
        uint64_t oldest = UINT64_MAX;
        for (uint64_t v : ch->slotReadyValue)
            if (v > consumed && v < oldest) oldest = v;
        if (oldest == UINT64_MAX) continue;
        ch->consumedFence->SetEventOnCompletion(oldest, ch->waitEvent);
        WaitForSingleObject(ch->waitEvent, deadline - now);
    }
}

FL_API flResult flSubmit(flChannel* ch, const flBuffer* buf, int64_t ptsNs) {
    if (!ch || ch->isConsumer || !buf || buf->slot >= ch->images.size()) return FL_INVALID;
    if (!ch->link.connected()) return FL_DISCONNECTED;
    const uint64_t value = ++ch->readyValue;
    ch->ctx->Signal(ch->readyFence.Get(), value);
    ch->ctx->Flush(); // no Flush after Signal == intermittent hangs (M2)
    ch->slotReadyValue[buf->slot] = value;

    vkr::proto::FrameReadyMsg fr{};
    fr.slot = buf->slot;
    fr.readyValue = value;
    fr.ptsNs = ptsNs;
    return ch->link.send(vkr::proto::MsgType::FrameReady, fr) ? FL_OK : FL_DISCONNECTED;
}

FL_API flResult flAcquireFrame(flChannel* ch, flFrame* out, uint32_t timeoutMs) {
    if (!ch || !ch->isConsumer || !out) return FL_INVALID;
    const DWORD deadline = GetTickCount() + timeoutMs;
    for (;;) {
        uint32_t slot = 0;
        uint64_t ready = 0;
        int64_t pts = -1;
        uint64_t seq = 0;
        bool got = false;
        {
            std::lock_guard<std::mutex> hold(ch->pendingMutex);
            if (ch->hasPending) {
                ch->hasPending = false;
                slot = ch->pendingSlot;
                ready = ch->pendingReady;
                pts = ch->pendingPts;
                seq = ch->sequence;
                got = true;
            }
        }
        if (got) {
            // The producer's GPU writes are not necessarily done yet; the fence
            // is what makes this zero-copy AND correct.
            if (ch->readyFence->GetCompletedValue() < ready) {
                ch->readyFence->SetEventOnCompletion(ready, ch->waitEvent);
                WaitForSingleObject(ch->waitEvent, 1000);
            }
            out->slot = slot;
            out->image = ch->images[slot];
            out->ptsNs = pts;
            out->sequence = seq;
            out->_readyValue = ready;
            return FL_OK;
        }
        if (ch->peerGone.load()) return FL_DISCONNECTED;
        const DWORD now = GetTickCount();
        if (now >= deadline) return FL_TIMEOUT;
        Sleep(1);
    }
}

FL_API flResult flRelease(flChannel* ch, const flFrame* frame) {
    if (!ch || !ch->isConsumer || !frame || frame->slot >= ch->images.size()) return FL_INVALID;
    // Signal THIS frame's value, not the newest one seen. Using the newest
    // (which an earlier cut of this did) tells the producer that slots are free
    // while we may still be reading them - a latent use-while-writing race that
    // the readback test happened not to trip because it copies out immediately.
    //
    // Still monotonic: the producer treats consumedFence as "everything up to
    // here is free", so it must never go backwards.
    if (frame->_readyValue <= ch->consumedValue) return FL_OK; // already released
    const uint64_t value = frame->_readyValue;
    ch->consumedValue = value;
    ch->ctx->Signal(ch->consumedFence.Get(), value);
    ch->ctx->Flush();
    return FL_OK;
}

FL_API flResult flQuery(flChannel* ch, flChannelInfo* out) {
    if (!ch || !out) return FL_INVALID;
    *out = ch->info;
    return FL_OK;
}

FL_API void flClose(flChannel* ch) {
    if (!ch) return;
    if (ch->isConsumer) {
        ch->stop = true;
        // Unblock ConnectNamedPipe in the accept thread by dialling ourselves.
        { vkr::IpcChannel poke = vkr::IpcChannel::connect(ch->name, 200, true); }
        if (ch->accept.joinable()) ch->accept.join();
    } else if (ch->link.connected()) {
        ch->link.send(vkr::proto::MsgType::Bye);
    }
    for (flImage* img : ch->images) {
        if (img->shared) CloseHandle(img->shared);
        delete img;
    }
    if (ch->readyShared) CloseHandle(ch->readyShared);
    if (ch->consumedShared) CloseHandle(ch->consumedShared);
    if (ch->waitEvent) CloseHandle(ch->waitEvent);
    delete ch;
}

// ---- D3D11 interop -----------------------------------------------------------

FL_API ID3D11Texture2D* flImageD3D11(flImage* img) { return img ? img->tex.Get() : nullptr; }
FL_API ID3D11Device* flChannelD3D11Device(flChannel* ch) {
    return ch ? ch->device.Get() : nullptr;
}
FL_API void* flImageSharedHandle(flImage* img) { return img ? img->shared : nullptr; }

} // extern "C"
