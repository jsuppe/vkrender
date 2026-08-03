#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "ipc.h"
#include "protocol.h"

#include <gst/gst.h>

namespace vkr {

// M9 Windows decoder: GStreamer end-to-end (rtspsrc -> decodebin3 ->
// d3d11h264dec/d3d11h265dec) with an appsink constrained to
// memory:D3D11Memory, running on OUR D3D11 device (shared into the pipeline
// via GstContext). Decoded textures feed the same VideoProcessor blit into
// the shared BGRA/RGB10A2 pool, with the same shared fences and IPC protocol
// as the FFmpeg-era decoder - only the demux/decode source changed.
class GstD3D11Decoder {
public:
    GstD3D11Decoder(const proto::HelloMsg& hello, uint32_t poolSize);
    ~GstD3D11Decoder();

    int run(const std::string& url, IpcChannel& ch);

private:
    template <typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    void createDevice(const proto::HelloMsg& hello);
    bool buildPipeline(const std::string& url);
    void teardownPipeline();
    bool ensurePool(GstCaps* caps, IpcChannel& ch);
    void destroyPool();
    bool submitSample(GstSample* sample, IpcChannel& ch);
    int pickFreeSlot() const;
    HANDLE duplicateToRenderer(HANDLE local) const;

    // D3D11 (shared pool + VideoProcessor + fences - as in the FFmpeg era)
    ComPtr<ID3D11Device5> device_;
    ComPtr<ID3D11DeviceContext4> ctx_;
    ComPtr<ID3D11VideoDevice> videoDev_;
    ComPtr<ID3D11VideoContext> videoCtx_;
    ComPtr<ID3D11VideoProcessorEnumerator> vpEnum_;
    ComPtr<ID3D11VideoProcessor> vp_;
    ComPtr<ID3D11Fence> readyFence_;
    ComPtr<ID3D11Fence> consumedFence_;
    std::vector<ComPtr<ID3D11Texture2D>> pool_;
    std::vector<ComPtr<ID3D11VideoProcessorOutputView>> poolOutViews_;
    std::map<std::pair<void*, UINT>, ComPtr<ID3D11VideoProcessorInputView>> inputViewCache_;
    std::vector<uint64_t> slotReadyValue_;
    uint32_t poolSize_ = 4;
    uint64_t readyCounter_ = 0;
    uint32_t poolWidth_ = 0, poolHeight_ = 0;
    int poolTenBit_ = -1;
    uint64_t droppedFrames_ = 0, sentFrames_ = 0;
    HANDLE rendererProcess_ = nullptr;

    // GStreamer
    GstElement* pipeline_ = nullptr;
    GstElement* appsink_ = nullptr;
    GstBus* bus_ = nullptr;
    void* gstDevice_ = nullptr; // GstD3D11Device* wrapping device_
};

} // namespace vkr
