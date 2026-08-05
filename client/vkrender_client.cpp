// vkrender client library implementation (M14). See vkrender_client.h.

#define VKR_CLIENT_EXPORTS
#include "vkrender_client.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d11_4.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

#include <cstring>
#include <vector>

#include "ipc.h"
#include "log.h"
#include "protocol.h"

using Microsoft::WRL::ComPtr;

struct vkrClient {
    vkr::IpcChannel channel;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext4> ctx;
    ComPtr<ID3D11Fence> readyFence, consumedFence;
    HANDLE consumedEvent = nullptr;
    std::vector<ComPtr<ID3D11Texture2D>> textures;
    std::vector<uint64_t> slotReadyValue; // last value signaled per slot
    uint64_t readyValue = 0;
};

namespace {

ComPtr<ID3D11Device> createDeviceOnLuid(const uint8_t luid[8], bool luidValid) {
    ComPtr<IDXGIFactory1> factory;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    ComPtr<IDXGIAdapter1> adapter, pick;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; ++i) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (!luidValid || memcmp(&desc.AdapterLuid, luid, 8) == 0) {
            pick = adapter;
            break;
        }
    }
    ComPtr<ID3D11Device> dev;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
    D3D11CreateDevice(pick.Get(), pick ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                      nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, nullptr);
    return dev;
}

} // namespace

extern "C" {

VKR_API vkrClient* vkrConnect(const char* pipeName, ID3D11Device* device, unsigned width,
                              unsigned height) {
    auto client = new vkrClient();
    client->channel = vkr::IpcChannel::connect(pipeName ? pipeName : "vkrender", 5000);
    if (!client->channel.connected()) {
        delete client;
        return nullptr;
    }

    vkr::proto::ClientHelloMsg hello{};
    hello.clientPid = GetCurrentProcessId();
    hello.width = width;
    hello.height = height;
    hello.format = vkr::proto::SharedFormat::BGRA8;
    hello.poolSize = 4;
    if (!client->channel.send(vkr::proto::MsgType::ClientHello, hello)) {
        delete client;
        return nullptr;
    }

    // Service replies Hello (its adapter LUID) then PoolDesc (our handles).
    auto helloMsg = client->channel.recv();
    if (!helloMsg || helloMsg->type != vkr::proto::MsgType::Hello) {
        delete client;
        return nullptr;
    }
    const auto* srvHello = helloMsg->as<vkr::proto::HelloMsg>();

    if (device) {
        client->device = device;
    } else if (srvHello) {
        client->device = createDeviceOnLuid(srvHello->deviceLUID, srvHello->luidValid != 0);
    }
    if (!client->device) {
        VKR_ERR("vkrClient: no D3D11 device");
        delete client;
        return nullptr;
    }
    ComPtr<ID3D11DeviceContext> ctx;
    client->device->GetImmediateContext(&ctx);
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(client->device.As(&mt))) mt->SetMultithreadProtected(TRUE);
    if (FAILED(ctx.As(&client->ctx))) {
        VKR_ERR("vkrClient: ID3D11DeviceContext4 unavailable");
        delete client;
        return nullptr;
    }

    auto poolMsg = client->channel.recv();
    if (!poolMsg || poolMsg->type != vkr::proto::MsgType::PoolDesc) {
        delete client;
        return nullptr;
    }
    const auto* desc = poolMsg->as<vkr::proto::PoolDescMsg>();
    if (!desc || desc->poolSize == 0) {
        delete client;
        return nullptr;
    }

    ComPtr<ID3D11Device1> dev1;
    ComPtr<ID3D11Device5> dev5;
    if (FAILED(client->device.As(&dev1)) || FAILED(client->device.As(&dev5))) {
        delete client;
        return nullptr;
    }
    for (uint32_t i = 0; i < desc->poolSize; ++i) {
        ComPtr<ID3D11Texture2D> tex;
        if (FAILED(dev1->OpenSharedResource1((HANDLE)(uintptr_t)desc->textureHandle[i],
                                             IID_PPV_ARGS(&tex)))) {
            VKR_ERR("vkrClient: OpenSharedResource1 failed (adapter mismatch?)");
            delete client;
            return nullptr;
        }
        client->textures.push_back(tex);
        client->slotReadyValue.push_back(0);
    }
    if (FAILED(dev5->OpenSharedFence((HANDLE)(uintptr_t)desc->readyFenceHandle,
                                     IID_PPV_ARGS(&client->readyFence))) ||
        FAILED(dev5->OpenSharedFence((HANDLE)(uintptr_t)desc->consumedFenceHandle,
                                     IID_PPV_ARGS(&client->consumedFence)))) {
        VKR_ERR("vkrClient: OpenSharedFence failed");
        delete client;
        return nullptr;
    }
    client->consumedEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    VKR_LOG("vkrClient: connected, ring of %u %ux%u", desc->poolSize, desc->width,
            desc->height);
    return client;
}

VKR_API ID3D11Device* vkrGetDevice(vkrClient* client) {
    return client ? client->device.Get() : nullptr;
}

VKR_API int vkrDequeueBuffer(vkrClient* client, vkrBuffer* out, unsigned timeoutMs) {
    if (!client || !client->channel.connected()) return -1;
    DWORD deadline = GetTickCount() + timeoutMs;
    for (;;) {
        uint64_t consumed = client->consumedFence->GetCompletedValue();
        for (size_t i = 0; i < client->textures.size(); ++i) {
            // Free once the renderer promised to never again read the value
            // this slot last carried.
            if (client->slotReadyValue[i] <= consumed) {
                out->slot = (unsigned)i;
                out->texture = client->textures[i].Get();
                return 0;
            }
        }
        DWORD now = GetTickCount();
        if (now >= deadline) return 1;
        // Wake when the consumed fence advances past the oldest outstanding
        // value (backpressure wait, GPU-driven).
        uint64_t oldest = UINT64_MAX;
        for (uint64_t v : client->slotReadyValue)
            if (v > consumed && v < oldest) oldest = v;
        if (oldest == UINT64_MAX) continue;
        client->consumedFence->SetEventOnCompletion(oldest, client->consumedEvent);
        WaitForSingleObject(client->consumedEvent, deadline - now);
    }
}

VKR_API int vkrQueueBuffer(vkrClient* client, const vkrBuffer* buffer, long long ptsNs) {
    if (!client || !client->channel.connected() || !buffer ||
        buffer->slot >= client->textures.size())
        return -1;
    uint64_t value = ++client->readyValue;
    client->ctx->Signal(client->readyFence.Get(), value);
    client->ctx->Flush(); // missing Flush after Signal = intermittent hangs (M2)
    client->slotReadyValue[buffer->slot] = value;

    vkr::proto::FrameReadyMsg fr{};
    fr.slot = buffer->slot;
    fr.readyValue = value;
    fr.ptsNs = ptsNs;
    return client->channel.send(vkr::proto::MsgType::FrameReady, fr) ? 0 : -1;
}

VKR_API void vkrDisconnect(vkrClient* client) {
    if (!client) return;
    if (client->channel.connected()) client->channel.send(vkr::proto::MsgType::Bye);
    if (client->consumedEvent) CloseHandle(client->consumedEvent);
    delete client;
}

} // extern "C"
