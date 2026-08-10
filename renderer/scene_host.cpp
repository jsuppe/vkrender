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
    if (vk.headless && specs_.size() > 1) {
        VKR_WARN("scene: headless mode uses a single offscreen canvas; extra --window ignored");
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

bool SceneHost::frame(VkContext& vk, ExternalVideoPool& pool, VideoPass& pass, bool passBound,
                      const std::vector<StreamRef>& streams, const Extras& extras, bool* quit) {
    *quit = false;
    if (!vk.headless) {
        glfwPollEvents();
        if (glfwWindowShouldClose(vk.window)) *quit = true;
        for (auto& w : extra_)
            if (glfwWindowShouldClose(w.glfw)) *quit = true;
        if (glfwGetKey(vk.window, GLFW_KEY_ESCAPE) == GLFW_PRESS) *quit = true;
        if (*quit) return true;
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
    const bool wide = canvasAspect_ >= 1.0f;
    auto cycleFaces = [&](size_t faces) {
        for (size_t fIdx = 0; fIdx < faces; ++fIdx)
            faceSlots.push_back(enabled.empty() ? -1 : enabled[fIdx % enabled.size()]);
    };
    switch (layout_) {
        case SceneLayout::TwoCubes:
            models.push_back(cubeModel(t, wide ? -1.55f : 0.0f, wide ? 0.0f : 1.55f, 0.9f,
                                       0.37f, 0.0f, 0.85f, 0.85f, 0.85f));
            models.push_back(cubeModel(t, wide ? 1.55f : 0.0f, wide ? 0.0f : -1.55f, -0.63f,
                                       0.37f, 2.1f, 0.85f, 0.85f, 0.85f));
            cycleFaces(12);
            break;
        case SceneLayout::SingleCube:
            models.push_back(cubeModel(t, 0.0f, 0.0f, 0.9f, 0.37f, 0.0f, 1.35f, 1.35f, 1.35f));
            cycleFaces(6);
            break;
        case SceneLayout::Wall: {
            size_t n = enabled.size();
            if (n == 0) break;
            const float halfH = 3.11f, halfW = halfH * canvasAspect_;
            int cols = std::max(1, (int)std::lround(std::sqrt((double)n * canvasAspect_)));
            int rows = (int)((n + cols - 1) / cols);
            float tileW = 2.0f * halfW / (float)cols, tileH = 2.0f * halfH / (float)rows;
            float sx = std::min(tileW * 0.44f, tileH * 0.44f * 16.0f / 9.0f);
            float sy = sx * 9.0f / 16.0f;
            for (size_t i = 0; i < n; ++i) {
                int cIdx = (int)(i % cols), rIdx = (int)(i / cols);
                float cx = -halfW + tileW * ((float)cIdx + 0.5f);
                float cy = halfH - tileH * ((float)rIdx + 0.5f);
                // Gentle bob for parallax; face 0 (+Z) carries the stream.
                Mat4 m = cubeModel(t, cx, cy, 0.0f, 0.0f, 0.0f, sx, sy, 0.04f);
                m.m[14] += 0.12f * sinf(t * 0.8f + (float)i * 1.3f);
                models.push_back(m);
                faceSlots.push_back(enabled[i]);
                for (int f = 1; f < 6; ++f) faceSlots.push_back(-1);
            }
            break;
        }
        case SceneLayout::Presentation: {
            // M25: up to 4 presenters in fixed regions, scaled to fit
            // (16:9 aspect-preserving letterbox inside each region):
            //   1 -> full screen, 2 -> flat top/bottom, 3 -> triangle
            //   (one up top, two below), 4 -> quad (2x2). Extra streams
            //   beyond 4 wait off-scene until a slot frees.
            size_t n = std::min(enabled.size(), (size_t)4);
            if (n == 0) break;
            struct Region {
                float cx, cy, hw, hh;
            };
            const float H = 3.11f, W = H * canvasAspect_;

            // A tile takes its STREAM's aspect, not a hardcoded 16:9. Every
            // source tested before this was landscape, so the assumption held
            // by luck; the first real AirPlay cast was an iPad mirroring
            // PORTRAIT at 752x1080 (~0.70), and forcing that into a 16:9 quad
            // stretched it horizontally by 1.78/0.70 ~= 2.5x. Phones are
            // narrower still.
            //
            // 16:9 remains the fallback for the frames between a producer
            // connecting and its first real allocation, when the pool has no
            // dimensions yet (the ring starts 64x64).
            float aspects[4];
            for (size_t i = 0; i < n; ++i) {
                aspects[i] = 16.0f / 9.0f;
                if (const ExternalVideoPool::Slot* s = pool.find((uint32_t)enabled[i]))
                    if (s->width > 0 && s->height > 0)
                        aspects[i] = (float)s->width / (float)s->height;
            }

            // M29: pick the arrangement, don't hardcode it.
            //
            // The old table assumed a 16:9 canvas: 2 streams always went
            // top/bottom. But a 16:9 tile fitted into a half-height band is
            // limited by HEIGHT, so it can never be wider than half the canvas
            // - and on a three-panel wall (aspect 3.69) that left each tile at
            // ~24% of the width with both ends empty. Nothing to do with the
            // per-window frustum; this is purely where the tiles are placed in
            // canvas space.
            //
            // So evaluate the sensible arrangements and keep whichever makes
            // the tiles biggest. On 16:9 this reproduces the original choices;
            // on a wide wall 2 streams land side by side instead of stacked.
            struct Arrangement {
                int cols, rows;
                bool triangle; // 3-up: one across the top, two beneath
            };
            const Arrangement candidates[] = {
                {1, 1, false}, {2, 1, false}, {1, 2, false}, {3, 1, false},
                {1, 3, false}, {0, 0, true},  {2, 2, false}, {4, 1, false},
                {1, 4, false},
            };

            auto buildRegions = [&](const Arrangement& a, Region* out) -> float {
                if (a.triangle) {
                    out[0] = {0, H / 2, W, H / 2};
                    out[1] = {-W / 2, -H / 2, W / 2, H / 2};
                    out[2] = {W / 2, -H / 2, W / 2, H / 2};
                } else {
                    const float hw = W / (float)a.cols, hh = H / (float)a.rows;
                    for (int r = 0; r < a.rows; ++r)
                        for (int c = 0; c < a.cols; ++c)
                            out[r * a.cols + c] = {-W + hw * (2 * c + 1),
                                                   H - hh * (2 * r + 1), hw, hh};
                }
                // Score = the SMALLEST tile the arrangement produces, so a
                // layout is only preferred if it helps the worst-off stream.
                // Scored with each stream's OWN aspect: a portrait tile is
                // height-limited where a landscape one is width-limited, so
                // scoring them all as 16:9 would pick the wrong arrangement
                // for a mixed set (e.g. an iPad beside a laptop).
                float worst = 1e9f;
                // Only ever called with count == n (checked by the caller), so
                // aspects[i] is in range.
                const size_t count = a.triangle ? 3 : (size_t)(a.cols * a.rows);
                for (size_t i = 0; i < count; ++i)
                    worst = std::min(worst, std::min(out[i].hw, out[i].hh * aspects[i]));
                return worst;
            };

            Region regs[4];
            float best = -1.0f;
            for (const Arrangement& a : candidates) {
                const size_t count = a.triangle ? 3 : (size_t)(a.cols * a.rows);
                if (count != n) continue;
                Region trial[4];
                float score = buildRegions(a, trial);
                if (score > best) {
                    best = score;
                    for (size_t i = 0; i < n; ++i) regs[i] = trial[i];
                }
            }
            if (best < 0.0f) break; // no arrangement for this count

            // M31: the layout produces TARGETS; SceneMotion owns where tiles
            // actually are, so a join or leave makes everyone glide instead of
            // teleport. Shared with the Android wall demo via scene_motion.h.
            const float margin = 0.98f;
            TileTransform targets[4];
            for (size_t i = 0; i < n; ++i) {
                const Region& r = regs[i];
                float sx = std::min(r.hw, r.hh * aspects[i]) * margin;
                targets[i] = {r.cx, r.cy, sx, sx / aspects[i]};
            }
            motion_.update(enabledIds.data(), targets, n, (float)dtSec, W);

            // Remember each live stream's pool slot. A departing stream is no
            // longer in `streams` (that is how motion learns it is leaving),
            // so without this its texture could not be found and the exit
            // would pop. The service keeps the ring alive until
            // retainsStream() goes false, so the slot stays valid throughout.
            for (size_t i = 0; i < enabledIds.size(); ++i) {
                bool found = false;
                for (auto& e : tileSlots_)
                    if (e.first == enabledIds[i]) {
                        e.second = enabled[i];
                        found = true;
                        break;
                    }
                if (!found) tileSlots_.push_back({enabledIds[i], enabled[i]});
            }

            for (size_t k = 0; k < motion_.count(); ++k) {
                int64_t slot = -1;
                for (const auto& e : tileSlots_)
                    if (e.first == motion_.idAt(k)) {
                        slot = e.second;
                        break;
                    }
                if (slot < 0) continue;
                const TileTransform tr = motion_.at(k);
                // Static, face-on: zero rotation params, wafer-thin depth.
                models.push_back(
                    cubeModel(0.0f, tr.cx, tr.cy, 0.0f, 0.0f, 0.0f, tr.hw, tr.hh, 0.01f));
                faceSlots.push_back(slot);
                for (int f = 1; f < 6; ++f) faceSlots.push_back(-1);
            }
            // Drop cache entries for tiles the motion has finished with, so a
            // long session does not accumulate one per stream ever seen.
            for (size_t i = tileSlots_.size(); i-- > 0;)
                if (!motion_.has(tileSlots_[i].first))
                    tileSlots_.erase(tileSlots_.begin() + (long)i);
            break;
        }
        default: break;
    }
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

    vkWaitForFences(vk.device, 1, &f.inFlight, VK_TRUE, UINT64_MAX);
    const int64_t tAfterFence = kTiming ? nowNs() : 0;

    // Acquire every window's image.
    const size_t windowCount = vk.headless ? 1 : 1 + extra_.size();
    std::vector<uint32_t> imageIndex(windowCount, 0);
    if (!vk.headless) {
        for (size_t w = 0; w < windowCount; ++w) {
            VkSwapchainKHR chain = w == 0 ? vk.swapchain : extra_[w - 1].swapchain;
            VkResult ar = vkAcquireNextImageKHR(vk.device, chain, UINT64_MAX,
                                                f.acquireSems[w], VK_NULL_HANDLE,
                                                &imageIndex[w]);
            if (ar == VK_ERROR_OUT_OF_DATE_KHR || ar == VK_SUBOPTIMAL_KHR) {
                // Drain any acquire semaphores already signaled this frame so
                // they can be reused, then rebuild and skip the frame.
                if (w > 0) {
                    std::vector<VkPipelineStageFlags> stages(
                        w, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
                    VkSubmitInfo drain{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                    drain.waitSemaphoreCount = (uint32_t)w;
                    drain.pWaitSemaphores = f.acquireSems.data();
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
    }
    const int64_t tAfterAcquire = kTiming ? nowNs() : 0;
    vkResetFences(vk.device, 1, &f.inFlight);

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.cmd, &bi);

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
                         (uint32_t)specs_[w].bezel);

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
        fin.newLayout = vk.headless ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        fin.image = image;
        fin.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        dep.pImageMemoryBarriers = &fin;
        vkCmdPipelineBarrier2(f.cmd, &dep);
    }

    vkEndCommandBuffer(f.cmd);
    const int64_t tAfterRecord = kTiming ? nowNs() : 0;

    std::vector<VkSemaphore> waitSems;
    std::vector<VkPipelineStageFlags> waitStages;
    std::vector<uint64_t> waitValues;
    if (!vk.headless) {
        for (size_t w = 0; w < windowCount; ++w) {
            waitSems.push_back(f.acquireSems[w]);
            waitStages.push_back(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            waitValues.push_back(0);
        }
    }
    for (size_t i = 0; i < extras.waitSems.size(); ++i) {
        waitSems.push_back(extras.waitSems[i]);
        waitStages.push_back(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        waitValues.push_back(extras.waitValues[i]);
    }
    std::vector<VkSemaphore> signalSems;
    std::vector<uint64_t> signalValues;
    if (!vk.headless) {
        for (size_t w = 0; w < windowCount; ++w) {
            signalSems.push_back(f.renderDoneSems[w]);
            signalValues.push_back(0);
        }
    }
    for (size_t i = 0; i < extras.signalSems.size(); ++i) {
        signalSems.push_back(extras.signalSems[i]);
        signalValues.push_back(extras.signalValues[i]);
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
    if (vkQueueSubmit(vk.queue, 1, &si, f.inFlight) != VK_SUCCESS) {
        VKR_ERR("scene: submit failed");
        return false;
    }
    const int64_t tAfterSubmit = kTiming ? nowNs() : 0;

    if (!vk.headless) {
        std::vector<VkSwapchainKHR> chains;
        chains.push_back(vk.swapchain);
        for (auto& w : extra_) chains.push_back(w.swapchain);
        std::vector<VkResult> results(chains.size());
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = (uint32_t)windowCount;
        pi.pWaitSemaphores = f.renderDoneSems.data();
        pi.swapchainCount = (uint32_t)chains.size();
        pi.pSwapchains = chains.data();
        pi.pImageIndices = imageIndex.data();
        pi.pResults = results.data();
        vkQueuePresentKHR(vk.queue, &pi);
        for (size_t w = 0; w < results.size(); ++w)
            if (results[w] == VK_ERROR_OUT_OF_DATE_KHR || results[w] == VK_SUBOPTIMAL_KHR)
                recreateWindow(vk, w);

        lastImageIndex_ = imageIndex;
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
        vkrDumpImage(vk, cmdPool_, image, extent, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, path);
    }
}

void SceneHost::destroy(VkContext& vk) {
    vkDeviceWaitIdle(vk.device);
    for (auto& f : frames_) {
        for (VkSemaphore s : f.acquireSems) vkDestroySemaphore(vk.device, s, nullptr);
        for (VkSemaphore s : f.renderDoneSems) vkDestroySemaphore(vk.device, s, nullptr);
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
