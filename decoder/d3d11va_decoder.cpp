#include "d3d11va_decoder.h"

#include <chrono>
#include <cstring>
#include <thread>

#include "log.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

namespace vkr {

namespace {

enum AVPixelFormat pickD3D11Format(AVCodecContext*, const enum AVPixelFormat* fmts) {
    for (const enum AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == AV_PIX_FMT_D3D11) return *p;
    }
    VKR_ERR("decoder: AV_PIX_FMT_D3D11 not offered by codec; hardware decode unavailable");
    return AV_PIX_FMT_NONE;
}

proto::Matrix mapMatrix(const AVFrame* f) {
    switch (f->colorspace) {
        case AVCOL_SPC_BT709: return proto::Matrix::BT709;
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M: return proto::Matrix::BT601;
        default:
            // Unspecified: HD content is almost always BT.709.
            return f->height >= 720 ? proto::Matrix::BT709 : proto::Matrix::BT601;
    }
}

proto::Range mapRange(const AVFrame* f) {
    return f->color_range == AVCOL_RANGE_JPEG ? proto::Range::Full : proto::Range::Limited;
}

} // namespace

D3D11VADecoder::D3D11VADecoder(const proto::HelloMsg& hello, uint32_t poolSize)
    : poolSize_(poolSize < 2 ? 2 : (poolSize > proto::kMaxPoolSize ? proto::kMaxPoolSize : poolSize)) {
    rendererProcess_ = OpenProcess(PROCESS_DUP_HANDLE, FALSE, hello.rendererPid);
    if (!rendererProcess_)
        fatal("OpenProcess(renderer) failed - cannot duplicate shared handles");
    createDevice(hello);
}

D3D11VADecoder::~D3D11VADecoder() {
    closeStream();
    destroyPool();
    if (hwDevice_) av_buffer_unref(&hwDevice_);
    if (rendererProcess_) CloseHandle(rendererProcess_);
}

void D3D11VADecoder::createDevice(const proto::HelloMsg& hello) {
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
        fatal("CreateDXGIFactory2 failed");

    // The renderer's Vulkan device LUID drives adapter selection so both
    // processes are guaranteed to be on the same GPU.
    ComPtr<IDXGIAdapter1> adapter;
    if (hello.luidValid) {
        LUID luid;
        static_assert(sizeof(luid) == 8);
        memcpy(&luid, hello.deviceLUID, sizeof(luid));
        if (FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)))) {
            VKR_WARN("decoder: no adapter matches renderer LUID, falling back to default");
            adapter.Reset();
        }
    }
    if (!adapter) {
        factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                            IID_PPV_ARGS(&adapter));
    }
    if (adapter) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        VKR_LOG("decoder: adapter: %ls", desc.Description);
    }

    UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(adapter.Get(),
                                   adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                                   nullptr, flags, nullptr, 0, D3D11_SDK_VERSION,
                                   &dev, &fl, &ctx);
    if (FAILED(hr)) fatal("D3D11CreateDevice failed");
    if (FAILED(dev.As(&device_)) || FAILED(ctx.As(&ctx_)))
        fatal("D3D11.4 interfaces (ID3D11Device5/DeviceContext4) unavailable - Windows 10 1703+ required");

    // FFmpeg only enables multithread protection on devices it creates itself;
    // required when the decoder worker threads and our submission thread share
    // the immediate context.
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(device_.As(&mt))) mt->SetMultithreadProtected(TRUE);

    if (FAILED(device_.As(&videoDev_)) || FAILED(ctx_.As(&videoCtx_)))
        fatal("ID3D11VideoDevice/VideoContext unavailable");
}

bool D3D11VADecoder::openStream(const std::string& url) {
    avformat_network_init();

    // Low-latency receive path: no internal buffering, tight jitter budget,
    // and a short probe window (probing only delays startup).
    fmt_ = avformat_alloc_context();
    fmt_->flags |= AVFMT_FLAG_NOBUFFER | AVFMT_FLAG_FLUSH_PACKETS;

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "rtsp_transport", "tcp", 0);
    av_dict_set(&opts, "timeout", "5000000", 0);       // 5 s socket I/O timeout -> stalls become errors
    av_dict_set(&opts, "max_delay", "100000", 0);      // 100 ms reorder/jitter cap
    av_dict_set(&opts, "probesize", "1000000", 0);     // 1 MB is plenty for SPS/PPS
    av_dict_set(&opts, "analyzeduration", "500000", 0); // 0.5 s
    int err = avformat_open_input(&fmt_, url.c_str(), nullptr, &opts);
    av_dict_free(&opts);
    if (err < 0) {
        char buf[256];
        av_strerror(err, buf, sizeof(buf));
        VKR_ERR("avformat_open_input(%s): %s", url.c_str(), buf);
        return false;
    }
    if (avformat_find_stream_info(fmt_, nullptr) < 0) {
        VKR_ERR("avformat_find_stream_info failed");
        return false;
    }
    const AVCodec* dec = nullptr;
    videoStream_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
    if (videoStream_ < 0 || !dec) {
        VKR_ERR("no video stream found");
        return false;
    }
    AVStream* st = fmt_->streams[videoStream_];
    VKR_LOG("decoder: stream: %s %dx%d", avcodec_get_name(st->codecpar->codec_id),
            st->codecpar->width, st->codecpar->height);

    codec_ = avcodec_alloc_context3(dec);
    if (!codec_ || avcodec_parameters_to_context(codec_, st->codecpar) < 0) {
        VKR_ERR("codec context setup failed");
        return false;
    }

    // Wrap OUR device in an FFmpeg hw device context so decode surfaces live
    // on the adapter we picked (and multithread protection is already on).
    // Created once and reused across RTSP reconnects.
    if (!hwDevice_) {
        hwDevice_ = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        if (!hwDevice_) {
            VKR_ERR("av_hwdevice_ctx_alloc(D3D11VA) failed");
            return false;
        }
        auto* hwctx = reinterpret_cast<AVHWDeviceContext*>(hwDevice_->data);
        auto* d3dctx = reinterpret_cast<AVD3D11VADeviceContext*>(hwctx->hwctx);
        device_->AddRef(); // av_hwdevice_ctx frees this reference on release
        d3dctx->device = device_.Get();
        if (av_hwdevice_ctx_init(hwDevice_) < 0) {
            VKR_ERR("av_hwdevice_ctx_init failed");
            av_buffer_unref(&hwDevice_);
            return false;
        }
    }
    codec_->hw_device_ctx = av_buffer_ref(hwDevice_);
    codec_->get_format = pickD3D11Format;
    codec_->flags |= AV_CODEC_FLAG_LOW_DELAY; // emit frames without output lag

    if (avcodec_open2(codec_, dec, nullptr) < 0) {
        VKR_ERR("avcodec_open2 failed");
        return false;
    }
    return true;
}

void D3D11VADecoder::closeStream() {
    if (codec_) avcodec_free_context(&codec_);
    if (fmt_) avformat_close_input(&fmt_);
}

HANDLE D3D11VADecoder::duplicateToRenderer(HANDLE local) const {
    HANDLE remote = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), local, rendererProcess_, &remote, 0, FALSE,
                         DUPLICATE_SAME_ACCESS)) {
        VKR_ERR("DuplicateHandle into renderer failed: %lu", GetLastError());
        return nullptr;
    }
    return remote;
}

void D3D11VADecoder::destroyPool() {
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
    poolSwFormat_ = -1;
}

bool D3D11VADecoder::ensurePool(const AVFrame* frame, IpcChannel& ch) {
    const uint32_t w = (uint32_t)frame->width & ~1u;
    const uint32_t h = (uint32_t)frame->height & ~1u;
    int curSwFormat = AV_PIX_FMT_NV12;
    if (frame->hw_frames_ctx)
        curSwFormat = reinterpret_cast<AVHWFramesContext*>(frame->hw_frames_ctx->data)->sw_format;
    if (w == poolWidth_ && h == poolHeight_ && curSwFormat == poolSwFormat_ && !pool_.empty())
        return true;

    if (!pool_.empty()) {
        VKR_LOG("decoder: stream parameters changed (%ux%u -> %ux%u), resetting pool",
                poolWidth_, poolHeight_, w, h);
        ch.send(proto::MsgType::StreamReset);
        // The renderer promises to drop all imported state on StreamReset;
        // wait for GPU idle before releasing the old textures.
        ctx_->Flush();
        destroyPool();
    }

    // The VideoProcessor enumerator is needed first: it gates the output
    // format choice (10-bit sources get an RGB10A2 pool when supported).
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputWidth = (UINT)frame->width;
    cd.InputHeight = (UINT)frame->height;
    cd.OutputWidth = w;
    cd.OutputHeight = h;
    cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    if (FAILED(videoDev_->CreateVideoProcessorEnumerator(&cd, &vpEnum_)) ||
        FAILED(videoDev_->CreateVideoProcessor(vpEnum_.Get(), 0, &vp_))) {
        VKR_ERR("CreateVideoProcessor failed");
        return false;
    }

    const AVPixelFormat swFormat = (AVPixelFormat)curSwFormat;
    const bool tenBit = swFormat == AV_PIX_FMT_P010;

    DXGI_FORMAT poolFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    proto::SharedFormat sharedFormat = proto::SharedFormat::BGRA8;
    if (tenBit) {
        UINT flags = 0;
        if (SUCCEEDED(vpEnum_->CheckVideoProcessorFormat(DXGI_FORMAT_R10G10B10A2_UNORM, &flags)) &&
            (flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
            poolFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
            sharedFormat = proto::SharedFormat::RGB10A2;
        } else {
            VKR_WARN("decoder: 10-bit source but VideoProcessor lacks RGB10A2 output; using BGRA8");
        }
    }

    proto::PoolDescMsg desc{};
    desc.width = w;
    desc.height = h;
    desc.format = sharedFormat;
    desc.matrix = mapMatrix(frame);
    desc.range = mapRange(frame);
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
        if (FAILED(device_->CreateTexture2D(&td, nullptr, &tex))) {
            VKR_ERR("CreateTexture2D(shared NV12 %ux%u) failed", w, h);
            return false;
        }
        ComPtr<IDXGIResource1> res;
        HANDLE local = nullptr;
        if (FAILED(tex.As(&res)) ||
            FAILED(res->CreateSharedHandle(nullptr,
                                           DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                           nullptr, &local))) {
            VKR_ERR("CreateSharedHandle(texture) failed");
            return false;
        }
        HANDLE remote = duplicateToRenderer(local);
        CloseHandle(local);
        if (!remote) return false;
        desc.textureHandle[i] = (uint64_t)(uintptr_t)remote;
        pool_.push_back(std::move(tex));
    }

    auto makeFence = [&](ComPtr<ID3D11Fence>& fence, uint64_t& outHandle) {
        HANDLE local = nullptr;
        if (FAILED(device_->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence))) ||
            FAILED(fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &local))) {
            VKR_ERR("CreateFence(shared)/CreateSharedHandle failed");
            return false;
        }
        HANDLE remote = duplicateToRenderer(local);
        CloseHandle(local);
        if (!remote) return false;
        outHandle = (uint64_t)(uintptr_t)remote;
        return true;
    };
    if (!makeFence(readyFence_, desc.readyFenceHandle)) return false;
    if (!makeFence(consumedFence_, desc.consumedFenceHandle)) return false;

    // Program the VideoProcessor (created above): colorimetry and the
    // source-rect crop (coded size -> display size).
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE inCs{};
    inCs.YCbCr_Matrix = desc.matrix == proto::Matrix::BT709 ? 1 : 0;
    inCs.Nominal_Range = desc.range == proto::Range::Full
                             ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255
                             : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    videoCtx_->VideoProcessorSetStreamColorSpace(vp_.Get(), 0, &inCs);
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE outCs{};
    outCs.RGB_Range = 0; // full-range RGB
    videoCtx_->VideoProcessorSetOutputColorSpace(vp_.Get(), &outCs);
    RECT srcRect{0, 0, (LONG)w, (LONG)h};
    videoCtx_->VideoProcessorSetStreamSourceRect(vp_.Get(), 0, TRUE, &srcRect);

    poolOutViews_.resize(poolSize_);
    for (uint32_t i = 0; i < poolSize_; ++i) {
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
        ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        if (FAILED(videoDev_->CreateVideoProcessorOutputView(pool_[i].Get(), vpEnum_.Get(), &ovd,
                                                             &poolOutViews_[i]))) {
            VKR_ERR("CreateVideoProcessorOutputView failed (slot %u)", i);
            return false;
        }
    }

    slotReadyValue_.assign(poolSize_, 0);
    poolWidth_ = w;
    poolHeight_ = h;
    poolSwFormat_ = curSwFormat;

    if (!ch.send(proto::MsgType::PoolDesc, desc)) return false;
    VKR_LOG("decoder: shared pool ready: %ux%u %s x%u, matrix=%s range=%s (source %s)", w, h,
            sharedFormat == proto::SharedFormat::RGB10A2 ? "RGB10A2" : "BGRA8", poolSize_,
            desc.matrix == proto::Matrix::BT709 ? "BT.709" : "BT.601",
            desc.range == proto::Range::Full ? "full" : "limited",
            av_get_pix_fmt_name(swFormat));
    return true;
}

int D3D11VADecoder::pickFreeSlot() const {
    const uint64_t consumed = consumedFence_->GetCompletedValue();
    for (uint32_t i = 0; i < pool_.size(); ++i) {
        if (consumed >= slotReadyValue_[i]) return (int)i;
    }
    return -1;
}

bool D3D11VADecoder::submitFrame(const AVFrame* frame, IpcChannel& ch) {
    if (!ensurePool(frame, ch)) return false;

    int slot = pickFreeSlot();
    if (slot < 0) {
        // Renderer is behind; keep latency bounded by dropping this frame.
        if ((++droppedFrames_ % 60) == 1)
            VKR_WARN("decoder: pool full, dropped %llu frames so far",
                     (unsigned long long)droppedFrames_);
        return true;
    }

    auto* src = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
    const UINT slice = (UINT)(uintptr_t)frame->data[1];

    // Decode surfaces are slices of one BIND_DECODER array texture at coded
    // size (e.g. 1920x1088). One fixed-function blit converts the display
    // rect to BGRA in the shared texture.
    auto& inView = inputViewCache_[{src, slice}];
    if (!inView) {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
        ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        ivd.Texture2D.ArraySlice = slice;
        if (FAILED(videoDev_->CreateVideoProcessorInputView(src, vpEnum_.Get(), &ivd, &inView))) {
            VKR_ERR("CreateVideoProcessorInputView failed (slice %u)", slice);
            inputViewCache_.erase({src, slice});
            return false;
        }
    }
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = inView.Get();
    if (FAILED(videoCtx_->VideoProcessorBlt(vp_.Get(), poolOutViews_[slot].Get(), 0, 1, &stream))) {
        VKR_ERR("VideoProcessorBlt failed");
        return false;
    }

    slotReadyValue_[slot] = ++readyCounter_;
    if (FAILED(ctx_->Signal(readyFence_.Get(), readyCounter_))) {
        VKR_ERR("ID3D11DeviceContext4::Signal failed");
        return false;
    }
    // Without the flush the signal can sit in the deferred command stream
    // indefinitely and the renderer waits forever.
    ctx_->Flush();

    proto::FrameReadyMsg msg{};
    msg.slot = (uint32_t)slot;
    msg.readyValue = readyCounter_;
    msg.ptsNs = frame->pts != AV_NOPTS_VALUE
                    ? av_rescale_q(frame->pts, fmt_->streams[videoStream_]->time_base,
                                   AVRational{1, 1000000000})
                    : -1;
    ++sentFrames_;
    return ch.send(proto::MsgType::FrameReady, msg);
}

namespace {

// Drain control messages; false once the renderer is gone.
bool pollRenderer(IpcChannel& ch) {
    while (auto msg = ch.tryRecv()) {
        if (msg->type == proto::MsgType::Bye) {
            VKR_LOG("decoder: renderer said goodbye");
            return false;
        }
    }
    return ch.connected();
}

// Sleep in small steps so a renderer disconnect interrupts the backoff.
bool backoffWait(IpcChannel& ch, uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 100) {
        if (!pollRenderer(ch)) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return true;
}

void sendPhase(IpcChannel& ch, proto::StreamPhase phase) {
    proto::StreamStatusMsg st{phase};
    ch.send(proto::MsgType::StreamStatus, st);
}

} // namespace

// Streams until the renderer goes away. RTSP failures (connect refused, mid-
// stream drop, server end) auto-reconnect with capped exponential backoff; the
// shared pool survives across reconnects so the renderer keeps its last frame.
int D3D11VADecoder::run(const std::string& url, IpcChannel& ch) {
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    uint32_t backoffMs = 1000;
    bool rendererAlive = true;

    while (rendererAlive) {
        sendPhase(ch, proto::StreamPhase::Connecting);
        VKR_LOG("decoder: connecting to %s", url.c_str());
        if (!openStream(url)) {
            closeStream();
            sendPhase(ch, proto::StreamPhase::Error);
            VKR_WARN("decoder: connect failed, retrying in %u ms", backoffMs);
            rendererAlive = backoffWait(ch, backoffMs);
            backoffMs = backoffMs * 2 > 10000 ? 10000 : backoffMs * 2;
            continue;
        }
        backoffMs = 1000;
        sendPhase(ch, proto::StreamPhase::Streaming);

        bool streamOk = true;
        while (streamOk && rendererAlive) {
            rendererAlive = pollRenderer(ch);
            if (!rendererAlive) break;

            int err = av_read_frame(fmt_, pkt);
            if (err == AVERROR(EAGAIN)) continue;
            if (err < 0) {
                char buf[256];
                av_strerror(err, buf, sizeof(buf));
                VKR_LOG("decoder: stream interrupted (%s), sent=%llu dropped=%llu", buf,
                        (unsigned long long)sentFrames_, (unsigned long long)droppedFrames_);
                streamOk = false;
                break;
            }
            if (pkt->stream_index != videoStream_) {
                av_packet_unref(pkt);
                continue;
            }
            err = avcodec_send_packet(codec_, pkt);
            av_packet_unref(pkt);
            if (err < 0 && err != AVERROR(EAGAIN)) {
                VKR_WARN("decoder: send_packet error %d", err);
                continue;
            }
            while (avcodec_receive_frame(codec_, frame) == 0) {
                if (frame->format == AV_PIX_FMT_D3D11) {
                    if (!submitFrame(frame, ch)) rendererAlive = ch.connected();
                }
                av_frame_unref(frame);
            }
        }
        closeStream();
        if (rendererAlive) {
            sendPhase(ch, proto::StreamPhase::Error);
            rendererAlive = backoffWait(ch, 1000);
        }
    }

    ch.send(proto::MsgType::Bye);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    return 0;
}

} // namespace vkr

