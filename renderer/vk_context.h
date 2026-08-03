#pragma once

#include <cstdint>
#include <vector>

#include <volk.h>

struct GLFWwindow;

namespace vkr {

// Owns instance, device, queue, swapchain, and the GLFW window.
// Exposes the physical device LUID so the decoder can pick the same adapter.
class VkContext {
public:
    bool init(uint32_t width, uint32_t height, const char* title);
    void destroy();
    bool recreateSwapchain();

    GLFWwindow* window = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkQueue queue = VK_NULL_HANDLE;

    uint8_t deviceLUID[8] = {};
    bool luidValid = false;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D swapExtent{};
    std::vector<VkImage> swapImages;
    std::vector<VkImageView> swapViews;

private:
    bool createSwapchain();
    void destroySwapchain();
    bool validation_ = false;
};

} // namespace vkr
