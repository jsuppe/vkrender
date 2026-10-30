#include "perf_trace.h"
// M15 scene host implementation - see scene_host.h.

#ifndef __ANDROID__

#include "scene_host.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "dump_util.h"
#include "log.h"
#include "scene_math.h"

namespace vkr {

namespace {
int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

const char* layoutName(SceneLayout l) {
    switch (l) {
        case SceneLayout::TwoCubes: return "two-cubes";
        case SceneLayout::SingleCube: return "single-cube";
        case SceneLayout::Wall: return "wall";
        case SceneLayout::Presentation: return "presentation";
        default: return "?";
    }
}
} // namespace

namespace {

bool buildSwapchain(VkContext& vk, GLFWwindow* window, VkSurfaceKHR surface,
                    SceneHost::Window* out); // fwd

bool createWindowSwapchain(VkContext& vk, SceneHost::Window& w) {
    return buildSwapchain(vk, w.glfw, w.surface, &w);
}

bool buildSwapchain(VkContext& vk, GLFWwindow* window, VkSurfaceKHR surface,
                    SceneHost::Window* out) {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.phys, surface, &caps);

    uint32_t fmtCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk.phys, surface, &fmtCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fmtCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk.phys, surface, &fmtCount, formats.data());
    VkSurfaceFormatKHR chosen = formats[0];
    for (const auto& f : formats)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            break;
        }
    // Must match the primary: one VideoPass pipeline targets one format.
    if (chosen.format != vk.swapFormat) {
        VKR_ERR("scene: window surface format %d != primary %d", chosen.format, vk.swapFormat);
        return false;
    }

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        int fbw = 0, fbh = 0;
        glfwGetFramebufferSize(window, &fbw, &fbh);
        extent = {(uint32_t)fbw, (uint32_t)fbh};
    }
    if (extent.width == 0 || extent.height == 0) return false;

    // FIFO everywhere: the scene loop is vsync-paced, not latency-critical.
    // M28: VKR_PRESENT_MODE overrides it. FIFO paces on the compositor, so on
    // a box whose display is asleep or driven remotely, vkQueuePresentKHR can
    // block far longer than a refresh interval and drag the whole scene down
    // (measured: 134 ms/frame on an idle RTX 5090 - the GPU was finishing
    // instantly, presentation was the entire cost). `immediate` or `mailbox`
    // decouples the scene from the display for headless/appliance use.
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (const char* pm = getenv("VKR_PRESENT_MODE")) {
        VkPresentModeKHR want = presentMode;
        if (!strcmp(pm, "immediate")) want = VK_PRESENT_MODE_IMMEDIATE_KHR;
        else if (!strcmp(pm, "mailbox")) want = VK_PRESENT_MODE_MAILBOX_KHR;
        uint32_t modeCount = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(vk.phys, surface, &modeCount, nullptr);
        std::vector<VkPresentModeKHR> modes(modeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(vk.phys, surface, &modeCount,
                                                  modes.data());
        for (VkPresentModeKHR m : modes)
            if (m == want) presentMode = want;
        if (presentMode != want)
            VKR_WARN("scene: present mode '%s' unsupported, staying on FIFO", pm);
    }

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount && imageCount > caps.maxImageCount) imageCount = caps.maxImageCount;

    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = surface;
    sci.minImageCount = imageCount;
    sci.imageFormat = chosen.format;
    sci.imageColorSpace = chosen.colorSpace;
    sci.imageExtent = extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                           ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                           : caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = presentMode;
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = out->swapchain;

    VkSwapchainKHR newChain = VK_NULL_HANDLE;
    if (vkCreateSwapchainKHR(vk.device, &sci, nullptr, &newChain) != VK_SUCCESS) {
        VKR_ERR("scene: vkCreateSwapchainKHR (extra window) failed");
        return false;
    }
    for (VkImageView v : out->views) vkDestroyImageView(vk.device, v, nullptr);
    out->views.clear();
    if (out->swapchain) vkDestroySwapchainKHR(vk.device, out->swapchain, nullptr);
    out->swapchain = newChain;
    out->format = chosen.format;
    out->extent = extent;

    uint32_t count = 0;
    vkGetSwapchainImagesKHR(vk.device, out->swapchain, &count, nullptr);
    out->images.resize(count);
    vkGetSwapchainImagesKHR(vk.device, out->swapchain, &count, out->images.data());
    out->views.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ivci.image = out->images[i];
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format = out->format;
        ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(vk.device, &ivci, nullptr, &out->views[i]) != VK_SUCCESS)
            return false;
    }
    return true;
}

void keyCallback(GLFWwindow* window, int key, int, int action, int) {
    if (action != GLFW_PRESS) return;
    auto* host = (SceneHost*)glfwGetWindowUserPointer(window);
    if (host) host->handleKey(key);
}

} // namespace

bool SceneHost::init(VkContext& vk, const std::string& shaderDir,
                     std::vector<WindowSpec> specs) {
    if (specs.empty()) specs.push_back({});
    specs_ = std::move(specs);
    canDisplay_ = vk.canDisplay;
    if (vk.headless && specs_.size() > 1) {
        VKR_WARN("scene: headless mode uses a single offscreen canvas; extra --window ignored"
                 " (displays attach at runtime via the moderator)");
        specs_.resize(1);
    }

    // Virtual canvas = union of the window rects.
    int minX = specs_[0].x, minY = specs_[0].y;
    int maxX = specs_[0].x + specs_[0].width, maxY = specs_[0].y + specs_[0].height;
    for (const auto& s : specs_) {
        minX = std::min(minX, s.x);
        minY = std::min(minY, s.y);
        maxX = std::max(maxX, s.x + s.width);
        maxY = std::max(maxY, s.y + s.height);
    }
    float cw = (float)(maxX - minX), ch = (float)(maxY - minY);
    canvasAspect_ = ch > 0 ? cw / ch : 1.0f;
    // The canvas spans the panels' PHYSICAL footprints (above), but each panel
    // only shows the part not hidden by its own frame - so the sub-frustum is
    // built from the rect inset by `bezel`. Content landing under a frame is
    // never drawn, which is exactly what makes a shape crossing the wall look
    // like it passed behind the bezel rather than skipping over it.
    for (const auto& s : specs_) {
        const float b = (float)s.bezel;
        WindowRects r;
        r.u0 = ((float)(s.x - minX) + b) / cw;
        r.v0 = ((float)(s.y - minY) + b) / ch;
        r.u1 = ((float)(s.x - minX + s.width) - b) / cw;
        r.v1 = ((float)(s.y - minY + s.height) - b) / ch;
        rects_.push_back(r);
    }

    if (!vk.headless) {
        glfwSetWindowPos(vk.window, specs_[0].x, specs_[0].y);
        glfwSetWindowSize(vk.window, specs_[0].width, specs_[0].height);
        glfwSetWindowUserPointer(vk.window, this);
        glfwSetKeyCallback(vk.window, keyCallback);
        vk.preferFifo = true; // vsync-paced scene; MAILBOX would free-run
        if (!vk.recreateSwapchain()) return false;

        for (size_t i = 1; i < specs_.size(); ++i) {
            const auto& s = specs_[i];
            glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
            Window w;
            w.glfw = glfwCreateWindow(s.width, s.height, "vkrender", nullptr, nullptr);
            if (!w.glfw) return false;
            glfwSetWindowPos(w.glfw, s.x, s.y);
            glfwSetWindowUserPointer(w.glfw, this);
            glfwSetKeyCallback(w.glfw, keyCallback);
            if (glfwCreateWindowSurface(vk.instance, w.glfw, nullptr, &w.surface) !=
                VK_SUCCESS)
                return false;
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(vk.phys, vk.queueFamily, w.surface, &present);
            if (!present) {
                VKR_ERR("scene: queue family cannot present to extra window");
                return false;
            }
            if (!createWindowSwapchain(vk, w)) return false;
            extra_.push_back(w);
        }
    }

    if (!background_.init(vk, shaderDir)) VKR_WARN("scene: backdrop unavailable");

    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = vk.queueFamily;
    vkCreateCommandPool(vk.device, &cpci, nullptr, &cmdPool_);

    size_t windowCount = 1 + extra_.size();
    for (auto& f : frames_) {
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(vk.device, &fci, nullptr, &f.inFlight);
        VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbai.commandPool = cmdPool_;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        vkAllocateCommandBuffers(vk.device, &cbai, &f.cmd);
        f.acquireSems.resize(windowCount);
        f.renderDoneSems.resize(windowCount);
        for (size_t w = 0; w < windowCount; ++w) {
            VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            vkCreateSemaphore(vk.device, &sci, nullptr, &f.acquireSems[w]);
            vkCreateSemaphore(vk.device, &sci, nullptr, &f.renderDoneSems[w]);
        }
    }
    lastFrameNs_ = nowNs();
    rateWindowStart_ = nowNs();
    lastSnapshotNs_ = nowNs();
    lastImageIndex_.assign(windowCount, 0);
    if (const char* env = getenv("VKR_SNAPSHOT_SECONDS")) snapshotEvery_ = atof(env);
    // M25: starting layout (L still cycles at runtime), e.g. the installed
    // appliance launches with VKR_LAYOUT=presentation.
    if (const char* env = getenv("VKR_LAYOUT")) {
        for (int l = 0; l < (int)SceneLayout::Count; ++l) {
            if (strcmp(env, layoutName((SceneLayout)l)) == 0) {
                layout_ = (SceneLayout)l;
                break;
            }
        }
        VKR_LOG("scene: starting layout %s", layoutName(layout_));
    }
    VKR_LOG("scene: %zu window(s), canvas %.0fx%.0f (aspect %.2f). Controls: L layout, "
            "SPACE pause, -/= speed, B backdrop, 1-9 toggle stream, 0 all, ESC quit",
            windowCount, cw, ch, canvasAspect_);
    return true;
}

void SceneHost::handleKey(int key) {
    switch (key) {
        case GLFW_KEY_L:
            layout_ = (SceneLayout)(((int)layout_ + 1) % (int)SceneLayout::Count);
            VKR_LOG("scene: layout -> %s", layoutName(layout_));
            break;
        case GLFW_KEY_SPACE:
            paused_ = !paused_;
            VKR_LOG("scene: %s", paused_ ? "paused" : "resumed");
            break;
        case GLFW_KEY_MINUS:
            speed_ = std::max(0.1f, speed_ * 0.8f);
            VKR_LOG("scene: speed %.2fx", speed_);
            break;
        case GLFW_KEY_EQUAL:
            speed_ = std::min(8.0f, speed_ * 1.25f);
            VKR_LOG("scene: speed %.2fx", speed_);
            break;
        case GLFW_KEY_B:
            backgroundOn_ = !backgroundOn_;
            VKR_LOG("scene: backdrop %s", backgroundOn_ ? "on" : "off");
            break;
        case GLFW_KEY_P:
            snapshotRequested_ = true;
            break;
        case GLFW_KEY_0:
            disabledIds_.clear();
            VKR_LOG("scene: all streams enabled");
            break;
        default:
            if (key >= GLFW_KEY_1 && key <= GLFW_KEY_9) {
                size_t idx = (size_t)(key - GLFW_KEY_1);
                if (idx < lastStreamIds_.size()) {
                    uint64_t id = lastStreamIds_[idx];
                    auto it = std::find(disabledIds_.begin(), disabledIds_.end(), id);
                    if (it == disabledIds_.end()) {
                        disabledIds_.push_back(id);
                        VKR_LOG("scene: stream %zu removed from scene", idx + 1);
                    } else {
                        disabledIds_.erase(it);
                        VKR_LOG("scene: stream %zu back in the scene", idx + 1);
                    }
                }
            }
            break;
    }
}

bool SceneHost::recreateWindow(VkContext& vk, size_t index) {
    vkDeviceWaitIdle(vk.device);
    if (index == 0) return vk.recreateSwapchain();
    return createWindowSwapchain(vk, extra_[index - 1]);
}

// ---- M43: runtime display outputs (headless appliance) ---------------------

std::vector<SceneHost::DisplayInfo> SceneHost::listDisplays() const {
    std::vector<DisplayInfo> out;
    if (!canDisplay_) return out;
    int count = 0;
    GLFWmonitor** mons = glfwGetMonitors(&count);
    GLFWmonitor* prim = glfwGetPrimaryMonitor();
    for (int i = 0; i < count; ++i) {
        DisplayInfo d;
        d.id = i;
        const char* n = glfwGetMonitorName(mons[i]);
        d.name = n ? n : "display";
        glfwGetMonitorPos(mons[i], &d.x, &d.y);
        if (const GLFWvidmode* vm = glfwGetVideoMode(mons[i])) {
            d.width = vm->width;
            d.height = vm->height;
        }
        d.primary = mons[i] == prim;
        for (size_t j = 0; j < displayOuts_.size(); ++j) {
            if (displayOuts_[j].monitorId != i) continue;
            d.active = true;
            if (1 + j < rects_.size()) {
                d.u0 = rects_[1 + j].u0;
                d.v0 = rects_[1 + j].v0;
                d.u1 = rects_[1 + j].u1;
                d.v1 = rects_[1 + j].v1;
            }
        }
        out.push_back(std::move(d));
    }
    return out;
}

void SceneHost::recomputeDisplayRects() {
    // The offscreen canvas (window 0) always shows everything - it is what
    // the camera broadcasts. The CANVAS is aspect-fit inside the union of the
    // active monitors' desktop rects: every display always sees the whole
    // composite along the fitted axis, and a display wider than the canvas
    // (e.g. one 32:9 panel) gets canvas coordinates beyond [0,1] - which the
    // off-center frustum renders as MORE SCENE (the backdrop extends), not a
    // crop. A lone 16:9 monitor mirrors the camera exactly.
    rects_.assign(1, {0.f, 0.f, 1.f, 1.f});
    if (displayOuts_.empty()) return;
    int minX = displayOuts_[0].x, minY = displayOuts_[0].y;
    int maxX = minX + displayOuts_[0].width, maxY = minY + displayOuts_[0].height;
    for (const auto& d : displayOuts_) {
        minX = std::min(minX, d.x);
        minY = std::min(minY, d.y);
        maxX = std::max(maxX, d.x + d.width);
        maxY = std::max(maxY, d.y + d.height);
    }
    const float uw = (float)(maxX - minX), uh = (float)(maxY - minY);
    const float ua = uh > 0 ? uw / uh : 1.0f;
    // The canvas's rect in desktop pixels, centered in the union.
    float cw, chh, cx, cy;
    if (ua >= canvasAspect_) {
        chh = uh;
        cw = uh * canvasAspect_;
        cx = (float)minX + (uw - cw) * 0.5f;
        cy = (float)minY;
    } else {
        cw = uw;
        chh = uw / canvasAspect_;
        cx = (float)minX;
        cy = (float)minY + (uh - chh) * 0.5f;
    }
    for (const auto& d : displayOuts_) {
        WindowRects r;
        r.u0 = ((float)d.x - cx) / cw;
        r.v0 = ((float)d.y - cy) / chh;
        r.u1 = ((float)(d.x + d.width) - cx) / cw;
        r.v1 = ((float)(d.y + d.height) - cy) / chh;
        rects_.push_back(r);
    }
}

bool SceneHost::resizeFrameSync(VkContext& vk) {
    vkDeviceWaitIdle(vk.device);
    const size_t windowCount = 1 + extra_.size();
    for (auto& f : frames_) {
        if (f.testRelease.joinable()) f.testRelease.join();
        if (f.testGate) vkDestroySemaphore(vk.device, f.testGate, nullptr);
        f.testGate = VK_NULL_HANDLE;
        f.testGateValue = 0;
        for (VkSemaphore s : f.acquireSems) vkDestroySemaphore(vk.device, s, nullptr);
        for (VkSemaphore s : f.renderDoneSems) vkDestroySemaphore(vk.device, s, nullptr);
        f.acquireSems.assign(windowCount, VK_NULL_HANDLE);
        f.renderDoneSems.assign(windowCount, VK_NULL_HANDLE);
        for (size_t w = 0; w < windowCount; ++w) {
            VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            if (vkCreateSemaphore(vk.device, &sci, nullptr, &f.acquireSems[w]) != VK_SUCCESS ||
                vkCreateSemaphore(vk.device, &sci, nullptr, &f.renderDoneSems[w]) != VK_SUCCESS)
                return false;
        }
    }
    lastImageIndex_.assign(windowCount, 0);
    return true;
}

bool SceneHost::setDisplayActive(VkContext& vk, int id, bool on) {
    if (!vk.headless) {
        VKR_WARN("scene: display toggles are for the headless appliance; windows are"
                 " configured with --window");
        return false;
    }
    if (!canDisplay_) {
        VKR_ERR("scene: no window system available - cannot attach display %d", id);
        return false;
    }
    int have = -1;
    for (size_t j = 0; j < displayOuts_.size(); ++j)
        if (displayOuts_[j].monitorId == id) have = (int)j;
    if (on == (have >= 0)) return true; // already in the requested state

    if (on) {
        int count = 0;
        GLFWmonitor** mons = glfwGetMonitors(&count);
        if (id < 0 || id >= count) {
            VKR_ERR("scene: no display %d (%d present)", id, count);
            return false;
        }
        GLFWmonitor* m = mons[id];
        int mx = 0, my = 0;
        glfwGetMonitorPos(m, &mx, &my);
        const GLFWvidmode* vm = glfwGetVideoMode(m);
        if (!vm) return false;
        // Borderless window covering the monitor - no exclusive fullscreen, no
        // mode switch, and focus is NOT stolen from whoever is at the desk.
        glfwDefaultWindowHints();
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
        glfwWindowHint(GLFW_AUTO_ICONIFY, GLFW_FALSE);
        glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);
        glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
        Window w;
        w.glfw = glfwCreateWindow(vm->width, vm->height, "vkrender", nullptr, nullptr);
        glfwDefaultWindowHints();
        if (!w.glfw) {
            VKR_ERR("scene: display %d window creation failed", id);
            return false;
        }
        glfwSetWindowPos(w.glfw, mx, my);
        bool ok = glfwCreateWindowSurface(vk.instance, w.glfw, nullptr, &w.surface) ==
                  VK_SUCCESS;
        if (ok) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(vk.phys, vk.queueFamily, w.surface,
                                                 &present);
            ok = present == VK_TRUE && createWindowSwapchain(vk, w);
        }
        if (!ok) {
            VKR_ERR("scene: cannot present to display %d", id);
            vkDeviceWaitIdle(vk.device);
            for (VkImageView v : w.views) vkDestroyImageView(vk.device, v, nullptr);
            if (w.swapchain) vkDestroySwapchainKHR(vk.device, w.swapchain, nullptr);
            if (w.surface) vkDestroySurfaceKHR(vk.instance, w.surface, nullptr);
            glfwDestroyWindow(w.glfw);
            return false;
        }
        extra_.push_back(w);
        DisplayOut d;
        d.monitorId = id;
        const char* n = glfwGetMonitorName(m);
        d.name = n ? n : "display";
        d.x = mx;
        d.y = my;
        d.width = vm->width;
        d.height = vm->height;
        displayOuts_.push_back(std::move(d));
        VKR_LOG("scene: display %d '%s' attached (%dx%d at %d,%d), %zu active",
                id, displayOuts_.back().name.c_str(), vm->width, vm->height, mx, my,
                displayOuts_.size());
    } else {
        vkDeviceWaitIdle(vk.device);
        Window& w = extra_[have];
        for (VkImageView v : w.views) vkDestroyImageView(vk.device, v, nullptr);
        if (w.swapchain) vkDestroySwapchainKHR(vk.device, w.swapchain, nullptr);
        if (w.surface) vkDestroySurfaceKHR(vk.instance, w.surface, nullptr);
        if (w.glfw) glfwDestroyWindow(w.glfw);
        extra_.erase(extra_.begin() + have);
        displayOuts_.erase(displayOuts_.begin() + have);
        VKR_LOG("scene: display %d detached, %zu active", id, displayOuts_.size());
    }
    recomputeDisplayRects();
    return resizeFrameSync(vk);
}

bool SceneHost::frame(VkContext& vk, ExternalVideoPool& pool, VideoPass& pass, bool passBound,
                      const std::vector<StreamRef>& streams, const Extras& extras, bool* quit) {
    vkrtrace::Scope trace("scene.frame_cpu");
    if(trace.active) trace.event.sequence=frameIndex_;
    *quit = false;
    if (!vk.headless) {
        glfwPollEvents();
        if (glfwWindowShouldClose(vk.window)) *quit = true;
        for (auto& w : extra_)
            if (glfwWindowShouldClose(w.glfw)) *quit = true;
        if (glfwGetKey(vk.window, GLFW_KEY_ESCAPE) == GLFW_PRESS) *quit = true;
        if (*quit) return true;
    } else if (!extra_.empty()) {
        // M43: display windows need their message pumps serviced. A closed
        // display window (someone alt-F4ed it at the desk) detaches that
        // display rather than quitting the appliance.
        glfwPollEvents();
        for (size_t j = extra_.size(); j-- > 0;)
            if (glfwWindowShouldClose(extra_[j].glfw))
                setDisplayActive(vk, displayOuts_[j].monitorId, false);
    }

    int64_t now = nowNs();
    // Real elapsed time, NOT sceneTime_: the animation must keep settling
    // while the scene is paused or slowed, otherwise SPACE freezes a tile
    // halfway across the wall.
    const double dtSec = lastFrameNs_ ? (double)(now - lastFrameNs_) * 1e-9 : 0.0;
    if (!paused_) sceneTime_ += (double)(now - lastFrameNs_) * 1e-9 * (double)speed_;
    lastFrameNs_ = now;
    float t = (float)sceneTime_;

    lastStreamIds_.clear();
    std::vector<int64_t> enabled;
    std::vector<uint64_t> enabledIds; // M31: motion keys off identity, not order
    for (const auto& s : streams) {
        lastStreamIds_.push_back(s.id);
        if (std::find(disabledIds_.begin(), disabledIds_.end(), s.id) == disabledIds_.end() &&
            s.slot >= 0) {
            enabled.push_back(s.slot);
            enabledIds.push_back(s.id);
        }
    }

    // Build the scene: model matrices + per-face slots per instance.
    std::vector<Mat4> models;
    std::vector<int64_t> faceSlots;
    buildScene(layout_,t,canvasAspect_,dtSec,enabled,enabledIds,pool,motion_,tileSlots_,models,faceSlots);
    if (!passBound || models.empty()) {
        models.clear();
        faceSlots.clear();
    }

    FrameSync& f = frames_[frameIndex_ % kFramesInFlight];
    ++frameIndex_;

    // M28: where does a scene frame actually go? The receive side has had
    // recv_stats since M23; this is the render-side counterpart, because "the
    // scene is slow" is otherwise indistinguishable between waiting on the
    // previous frame's GPU work, waiting on the compositor for a swapchain
    // image, and our own recording. VKR_SCENE_TIMING=1 to enable.
    static const bool kTiming = getenv("VKR_SCENE_TIMING") != nullptr;
    const int64_t tFrameStart = kTiming ? nowNs() : 0;

    vkrtrace::call("scene.wait_inflight_fence", [&]{return vkWaitForFences(vk.device, 1, &f.inFlight, VK_TRUE, UINT64_MAX);});
    if (f.testRelease.joinable()) f.testRelease.join();
    f.gpuTrace.prepare(vk);
    portableOutput_.retire(vk,(frameIndex_-1)%kFramesInFlight);
    const int64_t tAfterFence = kTiming ? nowNs() : 0;

    // Acquire every window's image. Headless: window 0 is the offscreen
    // canvas (nothing to acquire); M43 display windows still acquire/present.
    const size_t windowCount = 1 + extra_.size();
    const size_t firstChain = vk.headless ? 1 : 0; // first window with a swapchain
    std::vector<uint32_t> imageIndex(windowCount, 0);
    for (size_t w = firstChain; w < windowCount; ++w) {
        VkSwapchainKHR chain = w == 0 ? vk.swapchain : extra_[w - 1].swapchain;
        VkResult ar = vkrtrace::call("scene.acquire_swapchain", [&]{return vkAcquireNextImageKHR(vk.device, chain, UINT64_MAX,
                                            f.acquireSems[w], VK_NULL_HANDLE,
                                            &imageIndex[w]);});
        if (ar == VK_ERROR_OUT_OF_DATE_KHR || ar == VK_SUBOPTIMAL_KHR) {
            // Drain any acquire semaphores already signaled this frame so
            // they can be reused, then rebuild and skip the frame.
            if (w > firstChain) {
                std::vector<VkPipelineStageFlags> stages(
                    w - firstChain, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
                VkSubmitInfo drain{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                drain.waitSemaphoreCount = (uint32_t)(w - firstChain);
                drain.pWaitSemaphores = f.acquireSems.data() + firstChain;
                drain.pWaitDstStageMask = stages.data();
                vkQueueSubmit(vk.queue, 1, &drain, VK_NULL_HANDLE);
                vkQueueWaitIdle(vk.queue);
            }
            if (!recreateWindow(vk, w)) glfwWaitEventsTimeout(0.1);
            return true;
        } else if (ar != VK_SUCCESS) {
            VKR_ERR("scene: acquire failed on window %zu: %d", w, ar);
            return false;
        }
    }
    const int64_t tAfterAcquire = kTiming ? nowNs() : 0;
    // Opt-in native ownership stress: the command buffer genuinely waits on
    // a host-signaled timeline semaphore. Sleeping only on CPU would not delay GPU reads.
    static const unsigned gateMs = [] {
        const char* value = getenv("VKR_TEST_GPU_GATE_MS");
        return value ? unsigned(std::clamp(std::atoi(value), 0, 1000)) : 0u;
    }();
    const bool gated = vk.headless && gateMs && frameIndex_ % 7 == 0;
    if (gated) {
        if (!f.testGate) {
            VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
            type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            VkSemaphoreCreateInfo ci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            ci.pNext = &type;
            if (vkCreateSemaphore(vk.device, &ci, nullptr, &f.testGate) != VK_SUCCESS) return false;
        }
        ++f.testGateValue;
    }
    vkResetFences(vk.device, 1, &f.inFlight);

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    {
    vkrtrace::Scope recording("scene.record_commands_cpu");
    vkBeginCommandBuffer(f.cmd, &bi);
    f.gpuTrace.begin(f.cmd);


    for (size_t w = 0; w < windowCount; ++w) {
        VkImage image = w == 0 ? vk.swapImages[imageIndex[0]] : extra_[w - 1].images[imageIndex[w]];
        VkImageView view = w == 0 ? vk.swapViews[imageIndex[0]] : extra_[w - 1].views[imageIndex[w]];
        VkExtent2D extent = w == 0 ? vk.swapExtent : extra_[w - 1].extent;
        const WindowRects& r = rects_[w];

        VkImageMemoryBarrier2 toColor{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        toColor.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        toColor.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        toColor.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        toColor.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toColor.image = image;
        toColor.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &toColor;
        vkCmdPipelineBarrier2(f.cmd, &dep);

        // Per-window off-center projection over the shared canvas.
        Mat4 proj = matFrustumSub(canvasAspect_, r.u0, r.v0, r.u1, r.v1);
        std::vector<float> mvps(models.size() * 16);
        for (size_t i = 0; i < models.size(); ++i) {
            Mat4 mvp = matMul(proj, models[i]);
            memcpy(mvps.data() + i * 16, mvp.m, sizeof(mvp.m));
        }
        pass.recordScene(f.cmd, pool, faceSlots.empty() ? nullptr : faceSlots.data(),
                         (uint32_t)models.size(), mvps.empty() ? nullptr : mvps.data(), view,
                         extent, vk.queueFamily,
                         backgroundOn_ && background_.valid() ? &background_ : nullptr, t,
                         canvasAspect_, r.u0, r.v0, r.u1 - r.u0, r.v1 - r.v0,
                         w < specs_.size() ? (uint32_t)specs_[w].bezel : 0u);

        if(w==0)portableOutput_.record(vk,(frameIndex_-1)%kFramesInFlight,f.cmd,image,extent,
            vk.swapFormat,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,nowNs());
        // M24: consumer-view deliveries - blit window 0's rendered scene
        // (scaled) into each consumer's shared ring texture before the final
        // present transition.
        if (w == 0 && !extras.blits.empty()) {
            VkImageMemoryBarrier2 toSrc{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            toSrc.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            toSrc.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            toSrc.dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
            toSrc.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            toSrc.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            toSrc.image = image;
            toSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            dep.pImageMemoryBarriers = &toSrc;
            vkCmdPipelineBarrier2(f.cmd, &dep);

            for (const auto& blit : extras.blits) {
                VkImageMemoryBarrier2 toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                toDst.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
                toDst.dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
                toDst.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; // full overwrite
                toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                toDst.image = blit.image;
                toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
                toDst.dstQueueFamilyIndex = vk.queueFamily;
                toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                dep.pImageMemoryBarriers = &toDst;
                vkCmdPipelineBarrier2(f.cmd, &dep);

                VkImageBlit region{};
                region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.srcOffsets[1] = {(int32_t)extent.width, (int32_t)extent.height, 1};
                region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.dstOffsets[1] = {(int32_t)blit.width, (int32_t)blit.height, 1};
                vkCmdBlitImage(f.cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, blit.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                               VK_FILTER_LINEAR);

                // Release to the external (D3D11) reader.
                VkImageMemoryBarrier2 release{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                release.srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
                release.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                release.dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
                release.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                release.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                release.srcQueueFamilyIndex = vk.queueFamily;
                release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
                release.image = blit.image;
                release.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                dep.pImageMemoryBarriers = &release;
                vkCmdPipelineBarrier2(f.cmd, &dep);
            }

            VkImageMemoryBarrier2 back{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            back.srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
            back.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            back.dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
            back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            back.newLayout =
                vk.headless ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            back.image = image;
            back.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            dep.pImageMemoryBarriers = &back;
            vkCmdPipelineBarrier2(f.cmd, &dep);
            continue; // final transition handled above
        }

        VkImageMemoryBarrier2 fin{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        fin.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        fin.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        fin.dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
        fin.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        fin.newLayout = (vk.headless && w == 0) ? VK_IMAGE_LAYOUT_GENERAL
                                                : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        fin.image = image;
        fin.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        dep.pImageMemoryBarriers = &fin;
        vkCmdPipelineBarrier2(f.cmd, &dep);
    }

    f.gpuTrace.end(f.cmd);
    vkEndCommandBuffer(f.cmd);
    }
    const int64_t tAfterRecord = kTiming ? nowNs() : 0;

    std::vector<VkSemaphore> waitSems;
    std::vector<VkPipelineStageFlags> waitStages;
    std::vector<uint64_t> waitValues;
    for (size_t w = firstChain; w < windowCount; ++w) {
        waitSems.push_back(f.acquireSems[w]);
        waitStages.push_back(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        waitValues.push_back(0);
    }
    for (size_t i = 0; i < extras.waitSems.size(); ++i) {
        waitSems.push_back(extras.waitSems[i]);
        waitStages.push_back(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        waitValues.push_back(extras.waitValues[i]);
    }
    std::vector<VkSemaphore> signalSems;
    std::vector<uint64_t> signalValues;
    for (size_t w = firstChain; w < windowCount; ++w) {
        signalSems.push_back(f.renderDoneSems[w]);
        signalValues.push_back(0);
    }
    for (size_t i = 0; i < extras.signalSems.size(); ++i) {
        signalSems.push_back(extras.signalSems[i]);
        signalValues.push_back(extras.signalValues[i]);
    }

    if (gated) {
        waitSems.push_back(f.testGate);
        waitValues.push_back(f.testGateValue);
        waitStages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    }
    VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    timeline.waitSemaphoreValueCount = (uint32_t)waitValues.size();
    timeline.pWaitSemaphoreValues = waitValues.data();
    timeline.signalSemaphoreValueCount = (uint32_t)signalValues.size();
    timeline.pSignalSemaphoreValues = signalValues.data();

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.pNext = &timeline;
    si.waitSemaphoreCount = (uint32_t)waitSems.size();
    si.pWaitSemaphores = waitSems.data();
    si.pWaitDstStageMask = waitStages.data();
    si.commandBufferCount = 1;
    si.pCommandBuffers = &f.cmd;
    si.signalSemaphoreCount = (uint32_t)signalSems.size();
    si.pSignalSemaphores = signalSems.data();
    if (gated) {
        // Start before submit: some drivers can block submission on external waits.
        f.testRelease = std::thread([device=vk.device, gate=f.testGate, value=f.testGateValue,
                                     fence=f.inFlight, delay=gateMs] {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            auto status = vkGetFenceStatus(device, fence);
            uint64_t counter=0;
            vkGetSemaphoreCounterValue(device,gate,&counter);
            VKR_LOG("gpu-test-gate: pre-release fence=%d counter=%llu target=%llu delay_ms=%u",
                    int(status), (unsigned long long)counter, (unsigned long long)value, delay);
            VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
            signal.semaphore=gate; signal.value=value;
            vkSignalSemaphore(device,&signal);
        });
    }
    if (vkrtrace::call("scene.queue_submit_cpu", [&]{return vkQueueSubmit(vk.queue, 1, &si, f.inFlight);}) != VK_SUCCESS) {
        VKR_ERR("scene: submit failed");
        return false;
    }
    f.gpuTrace.submitted(frameIndex_-1);
    portableOutput_.submitted((frameIndex_-1)%kFramesInFlight);
    const int64_t tAfterSubmit = kTiming ? nowNs() : 0;
    // Read while the swapchain image is still acquired, before presenting it.
    // A post-present read is not valid without another WSI acquire.
    if(previewRequested_) {
        previewRequested_=false;
        vkrReadImage(vk,cmdPool_,vk.swapImages[imageIndex[0]],vk.swapExtent,
            vk.headless?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,previewPixels_);
    }

    if (windowCount > firstChain) {
        std::vector<VkSwapchainKHR> chains;
        if (!vk.headless) chains.push_back(vk.swapchain);
        for (auto& w : extra_) chains.push_back(w.swapchain);
        std::vector<VkResult> results(chains.size());
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = (uint32_t)(windowCount - firstChain);
        pi.pWaitSemaphores = f.renderDoneSems.data() + firstChain;
        pi.swapchainCount = (uint32_t)chains.size();
        pi.pSwapchains = chains.data();
        pi.pImageIndices = imageIndex.data() + firstChain;
        pi.pResults = results.data();
        vkrtrace::call("scene.present_call_cpu", [&]{return vkQueuePresentKHR(vk.queue, &pi);});
        for (size_t w = 0; w < results.size(); ++w)
            if (results[w] == VK_ERROR_OUT_OF_DATE_KHR || results[w] == VK_SUBOPTIMAL_KHR)
                recreateWindow(vk, w + firstChain);
    }
    lastImageIndex_ = imageIndex;
    {
        bool periodic = snapshotEvery_ > 0.0 &&
                        (double)(nowNs() - lastSnapshotNs_) * 1e-9 >= snapshotEvery_;
        if (snapshotRequested_ || periodic) {
            snapshotRequested_ = false;
            lastSnapshotNs_ = nowNs();
            snapshot(vk);
        }
    }

    ++renders_;
    if (kTiming) {
        const int64_t tEnd = nowNs();
        fenceNs_ += tAfterFence - tFrameStart;
        acquireNs_ += tAfterAcquire - tAfterFence;
        recordNs_ += tAfterRecord - tAfterAcquire;
        submitNs_ += tAfterSubmit - tAfterRecord;
        presentNs_ += tEnd - tAfterSubmit;
    }
    if (nowNs() - rateWindowStart_ > 5000000000ll) {
        VKR_LOG("scene: %llu frames in 5s (%s, %zu/%zu streams in scene, %zu windows)",
                (unsigned long long)renders_, layoutName(layout_), enabled.size(),
                streams.size(), windowCount);
        if (kTiming && renders_) {
            // fence  = waiting for this frame slot's previous GPU work
            // acquire= waiting for the compositor to hand back a swapchain image
            //          (under FIFO this is where vsync pacing shows up)
            // record = our own CPU work: recording, submit, present call
            VKR_LOG("scene-timing: per frame avg fence=%.1f acquire=%.1f record=%.1f "
                    "submit=%.1f present=%.1f (ms)",
                    (double)fenceNs_ / renders_ * 1e-6, (double)acquireNs_ / renders_ * 1e-6,
                    (double)recordNs_ / renders_ * 1e-6, (double)submitNs_ / renders_ * 1e-6,
                    (double)presentNs_ / renders_ * 1e-6);
            fenceNs_ = acquireNs_ = recordNs_ = submitNs_ = presentNs_ = 0;
        }
        renders_ = 0;
        rateWindowStart_ = nowNs();
    }
    return true;
}

void SceneHost::snapshot(VkContext& vk) {
    // Rare, debug-grade: quiesce and copy each window's last presented image.
    vkQueueWaitIdle(vk.queue);
    const size_t windowCount = 1 + extra_.size();
    for (size_t w = 0; w < windowCount && w < lastImageIndex_.size(); ++w) {
        VkImage image =
            w == 0 ? vk.swapImages[lastImageIndex_[0]] : extra_[w - 1].images[lastImageIndex_[w]];
        VkExtent2D extent = w == 0 ? vk.swapExtent : extra_[w - 1].extent;
        char path[64];
        snprintf(path, sizeof(path), "scene-w%zu.ppm", w);
        vkrDumpImage(vk, cmdPool_, image, extent,
                     (vk.headless && w == 0) ? VK_IMAGE_LAYOUT_GENERAL
                                             : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                     path);
    }
}

void SceneHost::destroy(VkContext& vk) {
    vkDeviceWaitIdle(vk.device);
    portableOutput_.destroy(vk);
    for (auto& f : frames_) {
        if (f.testRelease.joinable()) f.testRelease.join();
        if (f.testGate) vkDestroySemaphore(vk.device, f.testGate, nullptr);
        f.testGate = VK_NULL_HANDLE;
        f.testGateValue = 0;
        for (VkSemaphore s : f.acquireSems) vkDestroySemaphore(vk.device, s, nullptr);
        for (VkSemaphore s : f.renderDoneSems) vkDestroySemaphore(vk.device, s, nullptr);
        f.gpuTrace.destroy(vk);
        if (f.inFlight) vkDestroyFence(vk.device, f.inFlight, nullptr);
        f.acquireSems.clear();
        f.renderDoneSems.clear();
        f.inFlight = VK_NULL_HANDLE;
    }
    if (cmdPool_) vkDestroyCommandPool(vk.device, cmdPool_, nullptr);
    cmdPool_ = VK_NULL_HANDLE;
    background_.destroy(vk);
    for (auto& w : extra_) {
        for (VkImageView v : w.views) vkDestroyImageView(vk.device, v, nullptr);
        if (w.swapchain) vkDestroySwapchainKHR(vk.device, w.swapchain, nullptr);
        if (w.surface) vkDestroySurfaceKHR(vk.instance, w.surface, nullptr);
        if (w.glfw) glfwDestroyWindow(w.glfw);
    }
    extra_.clear();
}

} // namespace vkr

#endif // !__ANDROID__
