#include "perf_trace.h"
#include "nv12_output.h"
#ifdef _WIN32
#include "external_video_image.h"
#include "log.h"
namespace vkr {
using Microsoft::WRL::ComPtr;
bool Nv12Output::init(VkContext &vk, ID3D11Device *device,
                      ID3D11Texture2D *output, uint32_t w, uint32_t h) {
  if (!device || !output || !w || !h || (w & 1) || (h & 1))
    return false;
  auto fail = [&]() {
    destroy(vk);
    return false;
  };
  ComPtr<ID3D11Device5> dev;
  ComPtr<ID3D11VideoDevice> video;
  ComPtr<ID3D11DeviceContext> ctx;
  device->GetImmediateContext(&ctx);
  if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dev))) ||
      FAILED(device->QueryInterface(IID_PPV_ARGS(&video))) ||
      FAILED(ctx.As(&context_)) || FAILED(ctx.As(&videoContext_)))
    return fail();
  D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
  content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
  content.InputWidth = content.OutputWidth = w;
  content.InputHeight = content.OutputHeight = h;
  content.InputFrameRate = content.OutputFrameRate = {60, 1};
  content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
  if (FAILED(video->CreateVideoProcessorEnumerator(&content, &enumerator_)))
    return fail();
  UINT flags = 0;
  if (FAILED(enumerator_->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                                    &flags)) ||
      !(flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT))
    return fail();
  if (FAILED(
          enumerator_->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &flags)) ||
      !(flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT))
    return fail();
  ComPtr<ID3D11VideoProcessorEnumerator1> extended;
  BOOL supported = FALSE;
  if (FAILED(enumerator_.As(&extended)) ||
      FAILED(extended->CheckVideoProcessorFormatConversion(
          DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,
          DXGI_FORMAT_NV12, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709,
          &supported)) ||
      !supported)
    return fail();
  if (FAILED(video->CreateVideoProcessor(enumerator_.Get(), 0, &processor_)))
    return fail();
  D3D11_TEXTURE2D_DESC td{};
  td.Width = w;
  td.Height = h;
  td.MipLevels = td.ArraySize = 1;
  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  td.MiscFlags =
      D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
  if (FAILED(device->CreateTexture2D(&td, nullptr, &texture_)))
    return fail();
  ComPtr<IDXGIResource1> resource;
  if (FAILED(texture_.As(&resource)) ||
      FAILED(resource->CreateSharedHandle(
          nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
          nullptr, &textureHandle_)))
    return fail();
  if (!importWin32BlitTarget(vk, (uint64_t)(uintptr_t)textureHandle_, w, h,
                             &image, &memory_))
    return fail();
  if (FAILED(dev->CreateFence(0, D3D11_FENCE_FLAG_SHARED,
                              IID_PPV_ARGS(&fence_))) ||
      FAILED(fence_->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr,
                                        &fenceHandle_)) ||
      !importTimelineSemaphoreWin32(vk, (uint64_t)(uintptr_t)fenceHandle_,
                                    &inputReady))
    return fail();
  if (FAILED(dev->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&done_))))
    return fail();
  D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv{};
  iv.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
  D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ov{};
  ov.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
  if (FAILED(video->CreateVideoProcessorInputView(
          texture_.Get(), enumerator_.Get(), &iv, &input_)) ||
      FAILED(video->CreateVideoProcessorOutputView(output, enumerator_.Get(),
                                                   &ov, &output_)))
    return fail();
  RECT rect{0, 0, (LONG)w, (LONG)h};
  videoContext_->VideoProcessorSetStreamFrameFormat(
      processor_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
  videoContext_->VideoProcessorSetStreamSourceRect(processor_.Get(), 0, TRUE,
                                                   &rect);
  videoContext_->VideoProcessorSetStreamDestRect(processor_.Get(), 0, TRUE,
                                                 &rect);
  videoContext_->VideoProcessorSetOutputTargetRect(processor_.Get(), TRUE,
                                                   &rect);
  videoContext_->VideoProcessorSetStreamAutoProcessingMode(processor_.Get(), 0,
                                                           FALSE);
  videoContext_->VideoProcessorSetStreamColorSpace1(
      processor_.Get(), 0, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
  videoContext_->VideoProcessorSetOutputColorSpace1(
      processor_.Get(), DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
  return true;
}
bool Nv12Output::finish(uint64_t value) {
  vkrtrace::Scope trace("nv12.queue_conversion_cpu",(uint64_t)(uintptr_t)this);
  if(trace.active) trace.event.fence=value;
  if(trace.active && lastDone_) {
    const uint64_t completed=done_->GetCompletedValue();
    // This is a readiness observation, NOT video-engine execution duration.
    vkrtrace::record({"nv12.fence_observation",vkrtrace::clockNs(),0,(uint64_t)(uintptr_t)this,completed,lastDone_,0,(int64_t)traceSubmitNs_,completed==UINT64_MAX?-1:(completed>=lastDone_?0:1)});
  }
  if (FAILED(context_->Wait(fence_.Get(), value)))
    return trace.result(false);
  D3D11_VIDEO_PROCESSOR_STREAM stream{};
  stream.Enable = TRUE;
  stream.pInputSurface = input_.Get();
  const HRESULT conversion=videoContext_->VideoProcessorBlt(processor_.Get(), output_.Get(),0,1,&stream);
  if(FAILED(conversion))return trace.result(false);
  if (FAILED(context_->Signal(done_.Get(), value)))
    return trace.result(false);
  lastDone_ = value;
  if(trace.active)traceSubmitNs_=vkrtrace::clockNs();
  context_->Flush();
  return trace.result(true);
}
void Nv12Output::destroy(VkContext &vk) {
  if (done_ && lastDone_ && done_->GetCompletedValue() < lastDone_) {
    HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (event) {
      if (SUCCEEDED(done_->SetEventOnCompletion(lastDone_, event)))
        WaitForSingleObject(event, INFINITE);
      CloseHandle(event);
    }
  }
  if (image)
    vkDestroyImage(vk.device, image, nullptr);
  if (memory_)
    vkFreeMemory(vk.device, memory_, nullptr);
  if (inputReady)
    vkDestroySemaphore(vk.device, inputReady, nullptr);
  if (textureHandle_)
    CloseHandle(textureHandle_);
  if (fenceHandle_)
    CloseHandle(fenceHandle_);
  *this = Nv12Output{};
}
} // namespace vkr
#endif
