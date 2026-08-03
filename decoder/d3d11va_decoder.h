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

extern "C" {
#include <libavutil/frame.h>
}

struct AVFormatContext;
struct AVCodecContext;
struct AVBufferRef;

namespace vkr {

// RTSP -> D3D11VA decode -> shared NV12 texture ring, cross-process
// synchronized with two shared D3D11 fences (timeline semantics).
class D3D11VADecoder {
public:
    D3D11VADecoder(const proto::HelloMsg& hello, uint32_t poolSize);
    ~D3D11VADecoder();

    // Blocks until the stream ends, the renderer disconnects, or an error
    // occurs. Returns process exit code.
    int run(const std::string& url, IpcChannel& ch);

private:
    template <typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    void createDevice(const proto::HelloMsg& hello);
    bool openStream(const std::string& url);
    void closeStream();
    bool ensurePool(const AVFrame* frame, IpcChannel& ch);
    void destroyPool();
    bool submitFrame(const AVFrame* frame, IpcChannel& ch);
    int  pickFreeSlot() const;
    HANDLE duplicateToRenderer(HANDLE local) const;

    // D3D11
    ComPtr<ID3D11Device5> device_;
    ComPtr<ID3D11DeviceContext4> ctx_;
    ComPtr<ID3D11Fence> readyFence_;
    ComPtr<ID3D11Fence> consumedFence_;
    std::vector<ComPtr<ID3D11Texture2D>> pool_;

    // VideoProcessor: NV12 decode surface -> shared BGRA texture (colorimetry
    // handled by fixed-function; see protocol.h for why we don't share NV12)
    ComPtr<ID3D11VideoDevice> videoDev_;
    ComPtr<ID3D11VideoContext> videoCtx_;
    ComPtr<ID3D11VideoProcessorEnumerator> vpEnum_;
    ComPtr<ID3D11VideoProcessor> vp_;
    std::vector<ComPtr<ID3D11VideoProcessorOutputView>> poolOutViews_;
    std::map<std::pair<void*, UINT>, ComPtr<ID3D11VideoProcessorInputView>> inputViewCache_;
    std::vector<uint64_t> slotReadyValue_;
    uint32_t poolSize_ = 4;
    uint64_t readyCounter_ = 0;
    uint32_t poolWidth_ = 0, poolHeight_ = 0;
    int poolSwFormat_ = -1; // AVPixelFormat of the source when the pool was built
    uint64_t droppedFrames_ = 0, sentFrames_ = 0;

    HANDLE rendererProcess_ = nullptr;

    // FFmpeg
    AVFormatContext* fmt_ = nullptr;
    AVCodecContext* codec_ = nullptr;
    AVBufferRef* hwDevice_ = nullptr;
    int videoStream_ = -1;
};

} // namespace vkr
