// M30: three surfaces, one activity, one VkDevice - the Android half of the
// spanning wall (docs/MULTIDISPLAY.md).
//
// This is the Android counterpart to what M29 proved on Windows: N surfaces
// arranged over a shared virtual canvas, each rendering the SAME scene through
// its own off-centre sub-frustum, so content crosses panel boundaries without
// a seam. Here the three SurfaceViews live on one display, which is the
// stepping stone to three real displays - the canvas maths is identical, only
// where the surfaces come from changes.
//
// It also carries the presenter harness: "streams" are procedurally drawn
// tiles rather than decoded video, so layout reflow and (next) the entry/exit
// animation can be exercised with a button press instead of a decoder, a
// network sender and a camera. Real producers still arrive through the
// existing AIDL path in the renderer app; this deliberately does not duplicate
// that.

#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <volk.h>

#include "scene_math.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "vkrender-wall", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "vkrender-wall", __VA_ARGS__)

using vkr::Mat4;

namespace {

constexpr uint32_t kMaxPanels = 4;
constexpr uint32_t kFramesInFlight = 2;

// One panel = one SurfaceView = one swapchain, plus its slice of the canvas.
struct Panel {
    ANativeWindow* window = nullptr;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    VkExtent2D extent{};
    VkFormat format = VK_FORMAT_UNDEFINED;
    // Canvas region, normalised. Derived from the SurfaceViews' on-screen
    // coordinates, so the bezel model matches the desktop one exactly: the
    // rect is the panel's physical footprint, and `bezel` is how much of its
    // own edge is hidden.
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    int px = 0, py = 0, pw = 0, ph = 0;
    int bezel = 0;
    bool ready = false;
};

struct Frame {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence inFlight = VK_NULL_HANDLE;
    std::vector<VkSemaphore> acquire;
    std::vector<VkSemaphore> done;
};

struct Push {
    float mvp[16];
    float color[4];
};

struct Wall {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;

    Panel panels[kMaxPanels];
    uint32_t panelCount = 0;
    float canvasAspect = 1.0f;

    Frame frames[kFramesInFlight];
    uint64_t frameIndex = 0;

    std::mutex lock;
    int presenters = 0; // simulated streams currently "connected"
    std::atomic<bool> running{false};
    std::atomic<bool> dirty{true}; // panels changed -> rebuild canvas
    std::thread thread;
};

Wall g;

// ------------------------------------------------------------------ helpers

VkShaderModule loadShader(AAssetManager* mgr, const char* name) {
    AAsset* a = AAssetManager_open(mgr, name, AASSET_MODE_BUFFER);
    if (!a) {
        LOGE("asset %s missing", name);
        return VK_NULL_HANDLE;
    }
    size_t len = (size_t)AAsset_getLength(a);
    std::vector<uint32_t> code((len + 3) / 4, 0);
    memcpy(code.data(), AAsset_getBuffer(a), len);
    AAsset_close(a);
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = len;
    ci.pCode = code.data();
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(g.device, &ci, nullptr, &m) != VK_SUCCESS) {
        LOGE("vkCreateShaderModule(%s) failed", name);
        return VK_NULL_HANDLE;
    }
    return m;
}

bool createDevice() {
    if (volkInitialize() != VK_SUCCESS) {
        LOGE("volkInitialize failed");
        return false;
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    // 1.3, and the dynamicRendering feature below, are not optional: this
    // renderer uses vkCmdBeginRendering. Ask for 1.1 and volk leaves that
    // entry point NULL, which crashes with rip=0 the first time the render
    // thread records a frame - which is exactly what happened here. Matches
    // renderer/vk_context.cpp.
    app.apiVersion = VK_API_VERSION_1_3;
    app.pApplicationName = "vkrender-wall";
    const char* instExt[] = {VK_KHR_SURFACE_EXTENSION_NAME,
                             VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = instExt;
    if (vkCreateInstance(&ici, nullptr, &g.instance) != VK_SUCCESS) {
        LOGE("vkCreateInstance failed");
        return false;
    }
    volkLoadInstance(g.instance);

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g.instance, &n, nullptr);
    if (!n) {
        LOGE("no Vulkan physical device");
        return false;
    }
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(g.instance, &n, devs.data());
    g.phys = VK_NULL_HANDLE;
    for (VkPhysicalDevice d : devs) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(d, &props);
        if (props.apiVersion >= VK_API_VERSION_1_3) {
            g.phys = d; // one GPU on the target SoC; see docs/MULTIDISPLAY.md
            LOGI("device: %s (Vulkan %u.%u)", props.deviceName,
                 VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion));
            break;
        }
    }
    if (!g.phys) {
        LOGE("no Vulkan 1.3 device (dynamic rendering required)");
        return false;
    }

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qs(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &qn, qs.data());
    bool found = false;
    for (uint32_t i = 0; i < qn; ++i)
        if (qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            g.queueFamily = i;
            found = true;
            break;
        }
    if (!found) {
        LOGE("no graphics queue");
        return false;
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = g.queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char* devExt[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkPhysicalDeviceVulkan13Features feat13{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    feat13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceFeatures2 feat2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    feat2.pNext = &feat13;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &feat2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = devExt;
    if (vkCreateDevice(g.phys, &dci, nullptr, &g.device) != VK_SUCCESS) {
        LOGE("vkCreateDevice failed");
        return false;
    }
    volkLoadDevice(g.device);
    vkGetDeviceQueue(g.device, g.queueFamily, 0, &g.queue);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = g.queueFamily;
    if (vkCreateCommandPool(g.device, &pci, nullptr, &g.pool) != VK_SUCCESS) return false;
    return true;
}

bool buildPipeline() {
    VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                            sizeof(Push)};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(g.device, &plci, nullptr, &g.layout) != VK_SUCCESS) return false;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = g.vs;
    stages[0].pName = "main";
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = g.fs;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;

    // Dynamic rendering: no VkRenderPass object, and each panel's swapchain
    // format is declared at record time.
    VkFormat colorFormat = g.panels[0].format;
    VkPipelineRenderingCreateInfo pr{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    pr.colorAttachmentCount = 1;
    pr.pColorAttachmentFormats = &colorFormat;

    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.pNext = &pr;
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &ds;
    gp.layout = g.layout;
    if (vkCreateGraphicsPipelines(g.device, VK_NULL_HANDLE, 1, &gp, nullptr, &g.pipeline) !=
        VK_SUCCESS) {
        LOGE("vkCreateGraphicsPipelines failed");
        return false;
    }
    return true;
}

bool buildSwapchain(Panel& p) {
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.phys, p.surface, &caps);
    uint32_t fn = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g.phys, p.surface, &fn, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(fn);
    vkGetPhysicalDeviceSurfaceFormatsKHR(g.phys, p.surface, &fn, fmts.data());
    VkSurfaceFormatKHR chosen = fmts[0];
    for (const auto& f : fmts)
        if (f.format == VK_FORMAT_R8G8B8A8_UNORM || f.format == VK_FORMAT_B8G8R8A8_UNORM) {
            chosen = f;
            break;
        }

    p.extent = caps.currentExtent;
    if (p.extent.width == UINT32_MAX)
        p.extent = {(uint32_t)ANativeWindow_getWidth(p.window),
                    (uint32_t)ANativeWindow_getHeight(p.window)};
    if (!p.extent.width || !p.extent.height) return false;

    // One over the minimum: BufferQueue-backed swapchains stall in acquire
    // when SurfaceFlinger holds a buffer across a latch (M7.1 lesson).
    uint32_t count = caps.minImageCount + 2;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;

    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = p.surface;
    sci.minImageCount = count;
    sci.imageFormat = chosen.format;
    sci.imageColorSpace = chosen.colorSpace;
    sci.imageExtent = p.extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    // IDENTITY, not currentTransform. A portrait-native panel running a
    // landscape activity reports ROTATE_90; passing that through claims the
    // content is already pre-rotated, and it is not - the whole scene comes
    // out turned 90 degrees, which shows up as the panels' HORIZONTAL offsets
    // appearing vertical. Same reasoning as renderer/vk_context.cpp.
    sci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                           ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                           : caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = p.swapchain;

    VkSwapchainKHR chain = VK_NULL_HANDLE;
    if (vkCreateSwapchainKHR(g.device, &sci, nullptr, &chain) != VK_SUCCESS) {
        LOGE("vkCreateSwapchainKHR failed");
        return false;
    }
    for (VkImageView v : p.views) vkDestroyImageView(g.device, v, nullptr);
    p.views.clear();
    if (p.swapchain) vkDestroySwapchainKHR(g.device, p.swapchain, nullptr);
    p.swapchain = chain;
    p.format = chosen.format;

    uint32_t ic = 0;
    vkGetSwapchainImagesKHR(g.device, p.swapchain, &ic, nullptr);
    p.images.resize(ic);
    vkGetSwapchainImagesKHR(g.device, p.swapchain, &ic, p.images.data());
    p.views.resize(ic);
    for (uint32_t i = 0; i < ic; ++i) {
        VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ivci.image = p.images[i];
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format = p.format;
        ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(g.device, &ivci, nullptr, &p.views[i]) != VK_SUCCESS)
            return false;
    }
    p.ready = true;
    return true;
}

// The canvas is the union of the panels' on-screen rects, and each panel's
// sub-frustum is its rect INSET by its bezel - identical to the desktop model
// in scene_host.cpp, just fed from SurfaceView coordinates instead of
// --window flags.
void rebuildCanvas() {
    int minX = INT32_MAX, minY = INT32_MAX, maxX = INT32_MIN, maxY = INT32_MIN;
    for (uint32_t i = 0; i < g.panelCount; ++i) {
        const Panel& p = g.panels[i];
        minX = std::min(minX, p.px);
        minY = std::min(minY, p.py);
        maxX = std::max(maxX, p.px + p.pw);
        maxY = std::max(maxY, p.py + p.ph);
    }
    float cw = (float)(maxX - minX), ch = (float)(maxY - minY);
    if (cw <= 0 || ch <= 0) return;
    g.canvasAspect = cw / ch;
    for (uint32_t i = 0; i < g.panelCount; ++i) {
        Panel& p = g.panels[i];
        const float b = (float)p.bezel;
        p.u0 = ((float)(p.px - minX) + b) / cw;
        p.v0 = ((float)(p.py - minY) + b) / ch;
        p.u1 = ((float)(p.px - minX + p.pw) - b) / cw;
        p.v1 = ((float)(p.py - minY + p.ph) - b) / ch;
    }
    LOGI("canvas %.0fx%.0f (aspect %.2f) over %u panels", cw, ch, g.canvasAspect,
         g.panelCount);
}

// Same arrangement chooser as the desktop Presentation layout: score the
// candidate grids by their smallest tile and keep the winner, so the layout
// follows the wall's aspect instead of assuming 16:9.
struct Region {
    float cx, cy, hw, hh;
};

size_t layoutRegions(int n, float W, float H, Region* out) {
    if (n <= 0) return 0;
    n = std::min(n, 4);
    struct Arr {
        int cols, rows;
        bool tri;
    };
    const Arr cands[] = {{1, 1, false}, {2, 1, false}, {1, 2, false}, {3, 1, false},
                         {1, 3, false}, {0, 0, true},  {2, 2, false}, {4, 1, false},
                         {1, 4, false}};
    float best = -1.0f;
    Region keep[4];
    for (const Arr& a : cands) {
        int count = a.tri ? 3 : a.cols * a.rows;
        if (count != n) continue;
        Region t[4];
        if (a.tri) {
            t[0] = {0, H / 2, W, H / 2};
            t[1] = {-W / 2, -H / 2, W / 2, H / 2};
            t[2] = {W / 2, -H / 2, W / 2, H / 2};
        } else {
            float hw = W / (float)a.cols, hh = H / (float)a.rows;
            for (int r = 0; r < a.rows; ++r)
                for (int c = 0; c < a.cols; ++c)
                    t[r * a.cols + c] = {-W + hw * (2 * c + 1), H - hh * (2 * r + 1), hw, hh};
        }
        float worst = 1e9f;
        for (int i = 0; i < count; ++i)
            worst = std::min(worst, std::min(t[i].hw, t[i].hh * 16.0f / 9.0f));
        if (worst > best) {
            best = worst;
            for (int i = 0; i < count; ++i) keep[i] = t[i];
        }
    }
    if (best < 0) return 0;
    for (int i = 0; i < n; ++i) out[i] = keep[i];
    return (size_t)n;
}

void destroyPanelResources(Panel& p) {
    for (VkImageView v : p.views) vkDestroyImageView(g.device, v, nullptr);
    p.views.clear();
    if (p.swapchain) vkDestroySwapchainKHR(g.device, p.swapchain, nullptr);
    p.swapchain = VK_NULL_HANDLE;
    if (p.surface) vkDestroySurfaceKHR(g.instance, p.surface, nullptr);
    p.surface = VK_NULL_HANDLE;
    if (p.window) ANativeWindow_release(p.window);
    p.window = nullptr;
    p.ready = false;
}

// ------------------------------------------------------------------- render

void renderLoop() {
    auto start = std::chrono::steady_clock::now();
    uint64_t rendered = 0;
    auto lastReport = start;

    while (g.running.load()) {
        if (g.dirty.exchange(false)) {
            std::lock_guard<std::mutex> hold(g.lock);
            vkDeviceWaitIdle(g.device);
            for (uint32_t i = 0; i < g.panelCount; ++i)
                if (g.panels[i].window && !g.panels[i].ready) buildSwapchain(g.panels[i]);
            rebuildCanvas();
        }

        uint32_t live = 0;
        for (uint32_t i = 0; i < g.panelCount; ++i)
            if (g.panels[i].ready) live++;
        if (!live) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }

        Frame& f = g.frames[g.frameIndex % kFramesInFlight];
        ++g.frameIndex;
        vkWaitForFences(g.device, 1, &f.inFlight, VK_TRUE, UINT64_MAX);

        // Acquire every panel first; a panel that goes out of date is rebuilt
        // next pass rather than stalling the others.
        uint32_t imageIndex[kMaxPanels] = {};
        bool ok = true;
        for (uint32_t i = 0; i < g.panelCount && ok; ++i) {
            Panel& p = g.panels[i];
            if (!p.ready) continue;
            VkResult r = vkAcquireNextImageKHR(g.device, p.swapchain, UINT64_MAX,
                                               f.acquire[i], VK_NULL_HANDLE, &imageIndex[i]);
            if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
                p.ready = false;
                g.dirty = true;
                ok = false;
            } else if (r != VK_SUCCESS) {
                LOGE("acquire failed on panel %u: %d", i, r);
                ok = false;
            }
        }
        if (!ok) continue;

        vkResetFences(g.device, 1, &f.inFlight);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(f.cmd, &bi);

        int presenters;
        {
            std::lock_guard<std::mutex> hold(g.lock);
            presenters = g.presenters;
        }
        const float H = 3.11f, W = H * g.canvasAspect;
        Region regs[4];
        size_t tiles = layoutRegions(presenters, W, H, regs);
        float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();

        for (uint32_t i = 0; i < g.panelCount; ++i) {
            Panel& p = g.panels[i];
            if (!p.ready) continue;

            VkImageMemoryBarrier toColor{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            toColor.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            toColor.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            toColor.image = p.images[imageIndex[i]];
            toColor.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(f.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr,
                                 0, nullptr, 1, &toColor);

            VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            color.imageView = p.views[imageIndex[i]];
            color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            // The clear is also the bezel colour: the scene is drawn inset, so
            // what survives around it is the frame.
            color.clearValue.color = {{0.35f, 0.03f, 0.03f, 1.0f}};
            VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
            ri.renderArea = {{0, 0}, p.extent};
            ri.layerCount = 1;
            ri.colorAttachmentCount = 1;
            ri.pColorAttachments = &color;
            vkCmdBeginRendering(f.cmd, &ri);

            const uint32_t bez =
                (uint32_t)p.bezel * 2 < p.extent.width ? (uint32_t)p.bezel : 0;
            VkViewport vpt{(float)bez, (float)bez, (float)(p.extent.width - bez * 2),
                           (float)(p.extent.height - bez * 2), 0.0f, 1.0f};
            VkRect2D sc{{(int32_t)bez, (int32_t)bez},
                        {p.extent.width - bez * 2, p.extent.height - bez * 2}};
            vkCmdSetViewport(f.cmd, 0, 1, &vpt);
            vkCmdSetScissor(f.cmd, 0, 1, &sc);
            vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipeline);

            // Per-panel off-centre projection over the shared canvas: this is
            // what makes a tile cross a panel boundary without a seam.
            Mat4 proj = vkr::matFrustumSub(g.canvasAspect, p.u0, p.v0, p.u1, p.v1);
            for (size_t k = 0; k < tiles; ++k) {
                const Region& r = regs[k];
                float sx = std::min(r.hw, r.hh * 16.0f / 9.0f) * 0.94f;
                float sy = sx * 9.0f / 16.0f;
                // A slow drift so motion across seams is visible even before
                // the animation layer exists.
                float drift = 0.15f * sinf(t * 0.6f + (float)k);
                Mat4 model = vkr::cubeModel(0.0f, r.cx + drift, r.cy, 0, 0, 0, sx, sy, 0.01f);
                Push pc{};
                Mat4 mvp = vkr::matMul(proj, model);
                memcpy(pc.mvp, mvp.m, sizeof(mvp.m));
                const float hue = (float)k * 0.37f;
                pc.color[0] = 0.35f + 0.6f * fabsf(sinf(hue * 6.2831f));
                pc.color[1] = 0.35f + 0.6f * fabsf(sinf((hue + 0.33f) * 6.2831f));
                pc.color[2] = 0.35f + 0.6f * fabsf(sinf((hue + 0.66f) * 6.2831f));
                pc.color[3] = (float)(k + 1); // index -> label bars
                vkCmdPushConstants(f.cmd, g.layout,
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(pc), &pc);
                vkCmdDraw(f.cmd, 6, 1, 0, 0);
            }
            vkCmdEndRendering(f.cmd);

            VkImageMemoryBarrier toPresent{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            toPresent.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            toPresent.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            toPresent.image = p.images[imageIndex[i]];
            toPresent.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(f.cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                                 nullptr, 1, &toPresent);
        }
        vkEndCommandBuffer(f.cmd);

        // ONE submit for every panel - the shared render context. All panels
        // therefore carry the same logical frame by construction.
        std::vector<VkSemaphore> waits, signals;
        std::vector<VkPipelineStageFlags> stages;
        std::vector<VkSwapchainKHR> chains;
        std::vector<uint32_t> indices;
        for (uint32_t i = 0; i < g.panelCount; ++i) {
            if (!g.panels[i].ready) continue;
            waits.push_back(f.acquire[i]);
            stages.push_back(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            signals.push_back(f.done[i]);
            chains.push_back(g.panels[i].swapchain);
            indices.push_back(imageIndex[i]);
        }
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = (uint32_t)waits.size();
        si.pWaitSemaphores = waits.data();
        si.pWaitDstStageMask = stages.data();
        si.commandBufferCount = 1;
        si.pCommandBuffers = &f.cmd;
        si.signalSemaphoreCount = (uint32_t)signals.size();
        si.pSignalSemaphores = signals.data();
        if (vkQueueSubmit(g.queue, 1, &si, f.inFlight) != VK_SUCCESS) {
            LOGE("submit failed");
            break;
        }

        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = (uint32_t)signals.size();
        pi.pWaitSemaphores = signals.data();
        pi.swapchainCount = (uint32_t)chains.size();
        pi.pSwapchains = chains.data();
        pi.pImageIndices = indices.data();
        vkQueuePresentKHR(g.queue, &pi);

        ++rendered;
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<float>(now - lastReport).count() >= 5.0f) {
            LOGI("wall: %llu frames in 5s (%u panels, %d presenters)",
                 (unsigned long long)rendered, live, presenters);
            rendered = 0;
            lastReport = now;
        }
    }
}

} // namespace

// ---------------------------------------------------------------------- JNI

extern "C" JNIEXPORT jboolean JNICALL Java_dev_vkrender_wall_WallActivity_nativeInit(
    JNIEnv* env, jobject, jobject assetMgr) {
    if (g.device) return JNI_TRUE;
    if (!createDevice()) return JNI_FALSE;
    AAssetManager* mgr = AAssetManager_fromJava(env, assetMgr);
    g.vs = loadShader(mgr, "tile.vert.spv");
    g.fs = loadShader(mgr, "tile.frag.spv");
    if (!g.vs || !g.fs) return JNI_FALSE;
    LOGI("wall: device up");
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL Java_dev_vkrender_wall_WallActivity_nativeSetPanel(
    JNIEnv* env, jobject, jint index, jobject surface, jint x, jint y, jint w, jint h,
    jint bezel) {
    if (index < 0 || index >= (jint)kMaxPanels) return;
    std::lock_guard<std::mutex> hold(g.lock);
    Panel& p = g.panels[index];
    if (g.device) vkDeviceWaitIdle(g.device);
    destroyPanelResources(p);
    if (!surface) {
        g.dirty = true;
        return;
    }
    p.window = ANativeWindow_fromSurface(env, surface);
    p.px = x;
    p.py = y;
    p.pw = w;
    p.ph = h;
    p.bezel = bezel;
    VkAndroidSurfaceCreateInfoKHR asci{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    asci.window = p.window;
    if (vkCreateAndroidSurfaceKHR(g.instance, &asci, nullptr, &p.surface) != VK_SUCCESS) {
        LOGE("vkCreateAndroidSurfaceKHR failed for panel %d", index);
        return;
    }
    VkBool32 sup = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(g.phys, g.queueFamily, p.surface, &sup);
    if (!sup) {
        LOGE("panel %d surface not supported by queue family", index);
        return;
    }
    if ((uint32_t)index + 1 > g.panelCount) g.panelCount = (uint32_t)index + 1;
    LOGI("panel %d: %dx%d at (%d,%d) bezel %d", index, w, h, x, y, bezel);
    g.dirty = true;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_dev_vkrender_wall_WallActivity_nativeStart(JNIEnv*, jobject) {
    if (g.running.load()) return JNI_TRUE;
    {
        std::lock_guard<std::mutex> hold(g.lock);
        for (uint32_t i = 0; i < g.panelCount; ++i)
            if (g.panels[i].window && !g.panels[i].ready) buildSwapchain(g.panels[i]);
        rebuildCanvas();
    }
    if (!g.pipeline && !buildPipeline()) return JNI_FALSE;

    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = g.pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        Frame& f = g.frames[i];
        if (!f.cmd) vkAllocateCommandBuffers(g.device, &cbai, &f.cmd);
        if (!f.inFlight) {
            VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            vkCreateFence(g.device, &fci, nullptr, &f.inFlight);
        }
        f.acquire.resize(kMaxPanels, VK_NULL_HANDLE);
        f.done.resize(kMaxPanels, VK_NULL_HANDLE);
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (uint32_t k = 0; k < kMaxPanels; ++k) {
            if (!f.acquire[k]) vkCreateSemaphore(g.device, &sci, nullptr, &f.acquire[k]);
            if (!f.done[k]) vkCreateSemaphore(g.device, &sci, nullptr, &f.done[k]);
        }
    }
    g.running = true;
    g.thread = std::thread(renderLoop);
    LOGI("wall: render thread up (%u panels)", g.panelCount);
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_dev_vkrender_wall_WallActivity_nativeStop(JNIEnv*, jobject) {
    if (!g.running.exchange(false)) return;
    if (g.thread.joinable()) g.thread.join();
    if (g.device) vkDeviceWaitIdle(g.device);
    LOGI("wall: render thread stopped");
}

// The presenter harness: add/remove simulated streams. Deliberately just a
// count - the layout, and later the entry/exit animation, are what this is
// here to exercise.
extern "C" JNIEXPORT jint JNICALL
Java_dev_vkrender_wall_WallActivity_nativeAddPresenter(JNIEnv*, jobject) {
    std::lock_guard<std::mutex> hold(g.lock);
    if (g.presenters < 4) g.presenters++;
    return g.presenters;
}

extern "C" JNIEXPORT jint JNICALL
Java_dev_vkrender_wall_WallActivity_nativeRemovePresenter(JNIEnv*, jobject) {
    std::lock_guard<std::mutex> hold(g.lock);
    if (g.presenters > 0) g.presenters--;
    return g.presenters;
}
