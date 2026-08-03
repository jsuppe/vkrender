#include "gst_d3d11_decoder.h"

#include <cstring>

#include "log.h"

#include <gst/app/gstappsink.h>
#include <gst/video/video.h>

// The gstreamer-d3d11 library API is exported but versioned as unstable.
#define GST_USE_UNSTABLE_API
#include <gst/d3d11/gstd3d11.h>

namespace vkr {

namespace {

void sendPhase(IpcChannel& ch, proto::StreamPhase phase) {
    proto::StreamStatusMsg st{phase};
    ch.send(proto::MsgType::StreamStatus, st);
}

void gstInitOnce() {
    static bool done = false;
    if (done) return;
    gst_init(nullptr, nullptr);
    done = true;
}

} // namespace

GstD3D11Decoder::GstD3D11Decoder(const proto::HelloMsg& hello, uint32_t poolSize)
    : poolSize_(poolSize < 2 ? 2 : (poolSize > proto::kMaxPoolSize ? proto::kMaxPoolSize : poolSize)) {
    gstInitOnce();
    rendererProcess_ = OpenProcess(PROCESS_DUP_HANDLE, FALSE, hello.rendererPid);
    if (!rendererProcess_) fatal("OpenProcess(renderer) failed");
    createDevice(hello);
}

GstD3D11Decoder::~GstD3D11Decoder() {
    teardownPipeline();
    destroyPool();
    if (gstDevice_) gst_object_unref(gstDevice_);
    if (rendererProcess_) CloseHandle(rendererProcess_);
}

void GstD3D11Decoder::createDevice(const proto::HelloMsg& hello) {
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) fatal("CreateDXGIFactory2 failed");

    ComPtr<IDXGIAdapter1> adapter;
    if (hello.luidValid) {
        LUID luid;
        memcpy(&luid, hello.deviceLUID, sizeof(luid));
        if (FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)))) adapter.Reset();
    }
    if (!adapter)
        factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                            IID_PPV_ARGS(&adapter));

    UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL fl;
    if (FAILED(D3D11CreateDevice(adapter.Get(),
                                 adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                                 nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx)))
        fatal("D3D11CreateDevice failed");
    if (FAILED(dev.As(&device_)) || FAILED(ctx.As(&ctx_)))
        fatal("D3D11.4 interfaces unavailable");
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(device_.As(&mt))) mt->SetMultithreadProtected(TRUE);
    if (FAILED(device_.As(&videoDev_)) || FAILED(ctx_.As(&videoCtx_)))
        fatal("ID3D11VideoDevice/VideoContext unavailable");

    // Wrap OUR device for GStreamer so its d3d11 elements decode on it - the
    // decoded textures are then directly usable by our VideoProcessor.
    gstDevice_ = gst_d3d11_device_new_wrapped(device_.Get());
    if (!gstDevice_) fatal("gst_d3d11_device_new_wrapped failed");
}

bool GstD3D11Decoder::buildPipeline(const std::string& url) {
    GError* err = nullptr;
    std::string desc =
        "rtspsrc name=src protocols=tcp latency=100 location=" + url +
        " ! decodebin3 ! appsink name=sink max-buffers=4 drop=false sync=false";
    pipeline_ = gst_parse_launch(desc.c_str(), &err);
    if (!pipeline_ || err) {
        VKR_ERR("gst_parse_launch failed: %s", err ? err->message : "?");
        if (err) g_error_free(err);
        return false;
    }
    appsink_ = gst_bin_get_by_name(GST_BIN(pipeline_), "sink");
    GstCaps* caps = gst_caps_from_string(
        "video/x-raw(memory:D3D11Memory), format=(string){ NV12, P010_10LE }");
    gst_app_sink_set_caps(GST_APP_SINK(appsink_), caps);
    gst_caps_unref(caps);

    // Hand the pipeline our device before it starts.
    GstContext* ctx = gst_d3d11_context_new(GST_D3D11_DEVICE(gstDevice_));
    gst_element_set_context(pipeline_, ctx);
    gst_context_unref(ctx);

    bus_ = gst_element_get_bus(pipeline_);
    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        VKR_ERR("pipeline refused to start");
        return false;
    }
    return true;
}

void GstD3D11Decoder::teardownPipeline() {
    if (pipeline_) gst_element_set_state(pipeline_, GST_STATE_NULL);
    if (bus_) gst_object_unref(bus_);
    if (appsink_) gst_object_unref(appsink_);
    if (pipeline_) gst_object_unref(pipeline_);
    bus_ = nullptr;
    appsink_ = nullptr;
    pipeline_ = nullptr;
}

HANDLE GstD3D11Decoder::duplicateToRenderer(HANDLE local) const {
    HANDLE remote = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), local, rendererProcess_, &remote, 0, FALSE,
                         DUPLICATE_SAME_ACCESS)) {
        VKR_ERR("DuplicateHandle failed: %lu", GetLastError());
        return nullptr;
    }
    return remote;
}

void GstD3D11Decoder::destroyPool() {
    inputViewCache_.clear();
    poolOutViews_.clear();
    vp_.Reset();
    vpEnum_.Reset();
    pool_.clear();
    slotReadyValue_.clear();
    readyFence_.Reset();
    consumedFence_.Reset();
    readyCounter_ = 0;
    poolWidth_ = poolHeight_ = 0;
    poolTenBit_ = -1;
}

bool GstD3D11Decoder::ensurePool(GstCaps* caps, IpcChannel& ch) {
    GstVideoInfo info;
    if (!gst_video_info_from_caps(&info, caps)) return false;
    const uint32_t w = (uint32_t)GST_VIDEO_INFO_WIDTH(&info) & ~1u;
    const uint32_t h = (uint32_t)GST_VIDEO_INFO_HEIGHT(&info) & ~1u;
    const int tenBit = GST_VIDEO_INFO_FORMAT(&info) == GST_VIDEO_FORMAT_P010_10LE ? 1 : 0;
    if (w == poolWidth_ && h == poolHeight_ && tenBit == poolTenBit_ && !pool_.empty())
        return true;

    if (!pool_.empty()) {
        VKR_LOG("decoder: stream parameters changed, resetting pool");
        ch.send(proto::MsgType::StreamReset);
        ctx_->Flush();
        destroyPool();
    }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputWidth = (UINT)GST_VIDEO_INFO_WIDTH(&info);
    cd.InputHeight = (UINT)GST_VIDEO_INFO_HEIGHT(&info);
    cd.OutputWidth = w;
    cd.OutputHeight = h;
    cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    if (FAILED(videoDev_->CreateVideoProcessorEnumerator(&cd, &vpEnum_)) ||
        FAILED(videoDev_->CreateVideoProcessor(vpEnum_.Get(), 0, &vp_))) {
        VKR_ERR("CreateVideoProcessor failed");
        return false;
    }

    DXGI_FORMAT poolFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    proto::SharedFormat sharedFormat = proto::SharedFormat::BGRA8;
    if (tenBit) {
        UINT flags = 0;
        if (SUCCEEDED(vpEnum_->CheckVideoProcessorFormat(DXGI_FORMAT_R10G10B10A2_UNORM, &flags)) &&
            (flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
            poolFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
            sharedFormat = proto::SharedFormat::RGB10A2;
        }
    }

    proto::PoolDescMsg desc{};
    desc.width = w;
    desc.height = h;
    desc.format = sharedFormat;
    desc.matrix = info.colorimetry.matrix == GST_VIDEO_COLOR_MATRIX_BT601 ? proto::Matrix::BT601
                                                                          : proto::Matrix::BT709;
    desc.range = info.colorimetry.range == GST_VIDEO_COLOR_RANGE_0_255 ? proto::Range::Full
                                                                       : proto::Range::Limited;
    desc.poolSize = poolSize_;
    desc.planeCount = 1;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = poolFormat;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

    for (uint32_t i = 0; i < poolSize_; ++i) {
        ComPtr<ID3D11Texture2D> tex;
        if (FAILED(device_->CreateTexture2D(&td, nullptr, &tex))) return false;
        ComPtr<IDXGIResource1> res;
        HANDLE local = nullptr;
        if (FAILED(tex.As(&res)) ||
            FAILED(res->CreateSharedHandle(nullptr,
                                           DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                           nullptr, &local)))
            return false;
        HANDLE remote = duplicateToRenderer(local);
        CloseHandle(local);
        if (!remote) return false;
        desc.textureHandle[i] = (uint64_t)(uintptr_t)remote;
        pool_.push_back(std::move(tex));
    }
    auto makeFence = [&](ComPtr<ID3D11Fence>& fence, uint64_t& outHandle) {
        HANDLE local = nullptr;
        if (FAILED(device_->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence))) ||
            FAILED(fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &local)))
            return false;
        HANDLE remote = duplicateToRenderer(local);
        CloseHandle(local);
        if (!remote) return false;
        outHandle = (uint64_t)(uintptr_t)remote;
        return true;
    };
    if (!makeFence(readyFence_, desc.readyFenceHandle)) return false;
    if (!makeFence(consumedFence_, desc.consumedFenceHandle)) return false;

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE inCs{};
    inCs.YCbCr_Matrix = desc.matrix == proto::Matrix::BT709 ? 1 : 0;
    inCs.Nominal_Range = desc.range == proto::Range::Full
                             ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255
                             : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    videoCtx_->VideoProcessorSetStreamColorSpace(vp_.Get(), 0, &inCs);
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE outCs{};
    videoCtx_->VideoProcessorSetOutputColorSpace(vp_.Get(), &outCs);
    RECT srcRect{0, 0, (LONG)w, (LONG)h};
    videoCtx_->VideoProcessorSetStreamSourceRect(vp_.Get(), 0, TRUE, &srcRect);

    poolOutViews_.resize(poolSize_);
    for (uint32_t i = 0; i < poolSize_; ++i) {
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
        ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        if (FAILED(videoDev_->CreateVideoProcessorOutputView(pool_[i].Get(), vpEnum_.Get(), &ovd,
                                                             &poolOutViews_[i])))
            return false;
    }

    slotReadyValue_.assign(poolSize_, 0);
    poolWidth_ = w;
    poolHeight_ = h;
    poolTenBit_ = tenBit;

    if (!ch.send(proto::MsgType::PoolDesc, desc)) return false;
    VKR_LOG("decoder: shared pool ready: %ux%u %s x%u (gstreamer)", w, h,
            sharedFormat == proto::SharedFormat::RGB10A2 ? "RGB10A2" : "BGRA8", poolSize_);
    return true;
}

int GstD3D11Decoder::pickFreeSlot() const {
    const uint64_t consumed = consumedFence_->GetCompletedValue();
    for (uint32_t i = 0; i < pool_.size(); ++i)
        if (consumed >= slotReadyValue_[i]) return (int)i;
    return -1;
}

bool GstD3D11Decoder::submitSample(GstSample* sample, IpcChannel& ch) {
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstCaps* caps = gst_sample_get_caps(sample);
    if (!buffer || !caps) return true;
    if (!ensurePool(caps, ch)) return false;

    GstMemory* mem = gst_buffer_peek_memory(buffer, 0);
    if (!gst_is_d3d11_memory(mem)) {
        VKR_ERR("sample is not D3D11 memory - caps negotiation fell back?");
        return false;
    }
    auto* dmem = GST_D3D11_MEMORY_CAST(mem);
    auto* tex = (ID3D11Texture2D*)gst_d3d11_memory_get_resource_handle(dmem);
    const UINT slice = gst_d3d11_memory_get_subresource_index(dmem);
    if (!tex) return false;

    // Sanity: the texture must live on our device (context sharing worked).
    ComPtr<ID3D11Device> texDev;
    tex->GetDevice(&texDev);
    if (texDev.Get() != (ID3D11Device*)device_.Get()) {
        VKR_ERR("decoded texture is on a foreign D3D11 device - GstContext device "
                "sharing failed; cannot blit zero-copy");
        return false;
    }

    int slot = pickFreeSlot();
    if (slot < 0) {
        if ((++droppedFrames_ % 60) == 1)
            VKR_WARN("decoder: pool full, dropped %llu frames so far",
                     (unsigned long long)droppedFrames_);
        return true;
    }

    auto& inView = inputViewCache_[{tex, slice}];
    if (!inView) {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
        ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        ivd.Texture2D.ArraySlice = slice;
        if (FAILED(videoDev_->CreateVideoProcessorInputView(tex, vpEnum_.Get(), &ivd, &inView))) {
            inputViewCache_.erase({tex, slice});
            return false;
        }
    }

    // gst d3d11 elements drive the SAME immediate context; their lock guards
    // every use of it.
    gst_d3d11_device_lock(GST_D3D11_DEVICE(gstDevice_));
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = inView.Get();
    HRESULT hr = videoCtx_->VideoProcessorBlt(vp_.Get(), poolOutViews_[slot].Get(), 0, 1, &stream);
    slotReadyValue_[slot] = ++readyCounter_;
    if (SUCCEEDED(hr)) hr = ctx_->Signal(readyFence_.Get(), readyCounter_);
    ctx_->Flush();
    gst_d3d11_device_unlock(GST_D3D11_DEVICE(gstDevice_));
    if (FAILED(hr)) {
        VKR_ERR("VideoProcessorBlt/Signal failed: 0x%08lx", hr);
        return false;
    }

    proto::FrameReadyMsg msg{};
    msg.slot = (uint32_t)slot;
    msg.readyValue = readyCounter_;
    msg.ptsNs = GST_BUFFER_PTS_IS_VALID(buffer) ? (int64_t)GST_BUFFER_PTS(buffer) : -1;
    ++sentFrames_;
    return ch.send(proto::MsgType::FrameReady, msg);
}

int GstD3D11Decoder::run(const std::string& url, IpcChannel& ch) {
    uint32_t backoffMs = 1000;
    bool rendererAlive = true;

    auto pollRenderer = [&]() {
        while (auto msg = ch.tryRecv()) {
            if (msg->type == proto::MsgType::Bye) return false;
        }
        return ch.connected();
    };
    auto backoffWait = [&](uint32_t ms) {
        for (uint32_t t = 0; t < ms; t += 100) {
            if (!pollRenderer()) return false;
            Sleep(100);
        }
        return true;
    };

    while (rendererAlive) {
        sendPhase(ch, proto::StreamPhase::Connecting);
        VKR_LOG("decoder: connecting to %s (gstreamer)", url.c_str());
        if (!buildPipeline(url)) {
            teardownPipeline();
            sendPhase(ch, proto::StreamPhase::Error);
            rendererAlive = backoffWait(backoffMs);
            backoffMs = backoffMs * 2 > 10000 ? 10000 : backoffMs * 2;
            continue;
        }
        sendPhase(ch, proto::StreamPhase::Streaming);
        bool streamOk = true;
        bool firstSample = true;

        while (streamOk && rendererAlive) {
            rendererAlive = pollRenderer();
            if (!rendererAlive) break;

            if (GstMessage* m = gst_bus_pop_filtered(bus_, GST_MESSAGE_ERROR)) {
                GError* e = nullptr;
                gst_message_parse_error(m, &e, nullptr);
                VKR_LOG("decoder: stream interrupted (%s), sent=%llu dropped=%llu",
                        e ? e->message : "?", (unsigned long long)sentFrames_,
                        (unsigned long long)droppedFrames_);
                if (e) g_error_free(e);
                gst_message_unref(m);
                streamOk = false;
                break;
            }

            GstSample* sample =
                gst_app_sink_try_pull_sample(GST_APP_SINK(appsink_), 100 * GST_MSECOND);
            if (!sample) {
                if (gst_app_sink_is_eos(GST_APP_SINK(appsink_))) streamOk = false;
                continue;
            }
            if (firstSample) {
                firstSample = false;
                backoffMs = 1000;
            }
            if (!submitSample(sample, ch)) rendererAlive = ch.connected();
            gst_sample_unref(sample);
        }
        teardownPipeline();
        if (rendererAlive) {
            sendPhase(ch, proto::StreamPhase::Error);
            rendererAlive = backoffWait(1000);
        }
    }

    ch.send(proto::MsgType::Bye);
    return 0;
}

} // namespace vkr
