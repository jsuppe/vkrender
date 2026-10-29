#pragma once
#ifdef _WIN32
#include "vk_context.h"
#include <d3d11_4.h>
#include <wrl/client.h>
namespace vkr {
// Per-slot shared BGRA input and D3D11 GPU video processor -> FrameLink NV12.
// The graphics submit signals inputReady; finish queues a GPU wait, conversion,
// and completion fence on FrameLink's own D3D11 context. No CPU pixel mapping.
class Nv12Output {
public:
  bool init(VkContext &, ID3D11Device *, ID3D11Texture2D *output, uint32_t w,
            uint32_t h);
  bool finish(uint64_t value);
  void destroy(VkContext &);
  VkImage image = VK_NULL_HANDLE;
  VkSemaphore inputReady = VK_NULL_HANDLE;

private:
  VkDeviceMemory memory_ = VK_NULL_HANDLE;
  HANDLE textureHandle_ = nullptr, fenceHandle_ = nullptr;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context_;
  Microsoft::WRL::ComPtr<ID3D11VideoContext1> videoContext_;
  Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
  Microsoft::WRL::ComPtr<ID3D11VideoProcessor> processor_;
  Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> input_;
  Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> output_;
  Microsoft::WRL::ComPtr<ID3D11Fence> fence_, done_;
  uint64_t lastDone_ = 0;
  uint64_t traceSubmitNs_ = 0;

};
} // namespace vkr
#endif
