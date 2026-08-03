#pragma once

#include <cstdint>
#include <vector>

#include <volk.h>

struct GLFWwindow;

namespace vkr {

// Owns instance, device, queue, and either a GLFW window + swapchain or (in
// headless mode, for machines without a display session) an offscreen render
// target. Exposes the physical device LUID so the decoder can pick the same
// adapter on Windows; on Linux adapter matching is by DRM render node instead.
class VkContext {
public:
    // headless: no window system at all - creates an offscreen BGRA target of
    // the given size instead of a swapchain. GLFW is not initialized.
    bool init(uint32_t width, uint32_t height, const char* title, bool headless = false);
    void destroy();
    bool recreateSwapchain();
#ifdef __ANDROID__
    // Rebuilds surface + swapchain on a replacement ANativeWindow (the
    // activity gets a new one after screen-off/task-switch cycles).
    bool recreateSurface(void* newWindow);
#endif

    bool headless = false;

    // Android: the ANativeWindow* from the NativeActivity, set before init()
    // (GLFW is not used there). Desktop: unused.
    void* nativeWindow = nullptr;

    GLFWwindow* window = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkQueue queue = VK_NULL_HANDLE;

    uint8_t deviceLUID[8] = {};
    bool luidValid = false;
    // VK_GOOGLE_display_timing available (Android): presents can be scheduled
    // for specific display times, giving exact video cadence.
    bool hasDisplayTiming = false;

    // Window mode: swapchain state. Headless mode: swapImages/swapViews hold
    // the single offscreen target, swapFormat/swapExtent describe it.
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D swapExtent{};
    std::vector<VkImage> swapImages;
    std::vector<VkImageView> swapViews;
    VkDeviceMemory offscreenMemory = VK_NULL_HANDLE;

private:
    bool createSwapchain();
    bool createOffscreenTarget(uint32_t width, uint32_t height);
    void destroySwapchain();
    bool validation_ = false;
};

} // namespace vkr
