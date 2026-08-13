#include "vk_context.h"

#ifndef __ANDROID__
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#endif

#include <algorithm>
#include <cstring>

#include "log.h"

namespace vkr {

namespace {

// Interop extensions differ per OS: Windows imports D3D11 NT handles and
// D3D fences; Linux imports dma-buf fds with explicit DRM format modifiers;
// Android imports AHardwareBuffers. Linux/Android use the FOREIGN queue
// family for ownership transfers.
const char* kDeviceExtensions[] = {
#if defined(_WIN32)
    VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
#elif defined(__ANDROID__)
    VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
    VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
#else
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
    VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
#endif
};

bool hasAllExtensions(VkPhysicalDevice dev, bool needSwapchain) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> props(count);
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, props.data());
    auto has = [&](const char* want) {
        for (const auto& p : props)
            if (!strcmp(p.extensionName, want)) return true;
        return false;
    };
    for (const char* want : kDeviceExtensions)
        if (!has(want)) return false;
    if (needSwapchain && !has(VK_KHR_SWAPCHAIN_EXTENSION_NAME)) return false;
    return true;
}

} // namespace

bool VkContext::init(uint32_t width, uint32_t height, const char* title, bool headlessMode) {
    headless = headlessMode;

#ifndef __ANDROID__
    if (!headless) {
        if (!glfwInit()) {
            VKR_ERR("glfwInit failed");
            return false;
        }
        if (!glfwVulkanSupported()) {
            VKR_ERR("GLFW reports no Vulkan support (is the Vulkan loader present?)");
            return false;
        }
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        window = glfwCreateWindow((int)width, (int)height, title, nullptr, nullptr);
        if (!window) {
            VKR_ERR("glfwCreateWindow failed");
            return false;
        }
        canDisplay = true;
    } else {
        // M43: headless still initializes the window system (no window is
        // created) so the appliance can attach real displays at RUNTIME - the
        // moderator toggles a monitor on and a borderless window appears. If
        // there is no display session at all (a true server), this fails
        // quietly and the service runs exactly as before.
        if (glfwInit() && glfwVulkanSupported()) {
            canDisplay = true;
        } else {
            VKR_LOG("renderer: headless without a window system - display outputs unavailable");
        }
    }
#endif

    if (volkInitialize() != VK_SUCCESS) {
        VKR_ERR("volkInitialize failed - Vulkan loader not found");
        return false;
    }

    // Validation layer is only present if a Vulkan SDK (or the layer package)
    // is installed; enable opportunistically in Debug builds.
    uint32_t layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> layers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, layers.data());
    for (const auto& l : layers)
        if (!strcmp(l.layerName, "VK_LAYER_KHRONOS_validation")) validation_ = true;
#ifdef NDEBUG
    validation_ = false;
#endif

    std::vector<const char*> instExts;
    if (!headless || canDisplay) {
#ifdef __ANDROID__
        if (!headless) instExts = {VK_KHR_SURFACE_EXTENSION_NAME, "VK_KHR_android_surface"};
#else
        uint32_t glfwExtCount = 0;
        const char** glfwExts = glfwGetRequiredInstanceExtensions(&glfwExtCount);
        if (glfwExts) instExts.assign(glfwExts, glfwExts + glfwExtCount);
        else canDisplay = false; // loader has no surface support after all
#endif
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "vkrenderer";
    app.apiVersion = VK_API_VERSION_1_3;

    const char* valLayer = "VK_LAYER_KHRONOS_validation";
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = (uint32_t)instExts.size();
    ici.ppEnabledExtensionNames = instExts.data();
    if (validation_) {
        ici.enabledLayerCount = 1;
        ici.ppEnabledLayerNames = &valLayer;
        VKR_LOG("renderer: validation layer enabled");
    }
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) {
        VKR_ERR("vkCreateInstance failed");
        return false;
    }
    volkLoadInstance(instance);

    if (!headless) {
#ifdef __ANDROID__
        VkAndroidSurfaceCreateInfoKHR asci{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
        asci.window = (ANativeWindow*)nativeWindow;
        if (vkCreateAndroidSurfaceKHR(instance, &asci, nullptr, &surface) != VK_SUCCESS) {
            VKR_ERR("vkCreateAndroidSurfaceKHR failed");
            return false;
        }
#else
        if (glfwCreateWindowSurface(instance, window, nullptr, &surface) != VK_SUCCESS) {
            VKR_ERR("glfwCreateWindowSurface failed");
            return false;
        }
#endif
    }

    // Pick a physical device: needs the interop extensions, Vulkan 1.3, and
    // (window mode) present support. Prefer discrete GPUs.
    uint32_t devCount = 0;
    vkEnumeratePhysicalDevices(instance, &devCount, nullptr);
    std::vector<VkPhysicalDevice> devs(devCount);
    vkEnumeratePhysicalDevices(instance, &devCount, devs.data());

    int bestScore = -1;
    for (VkPhysicalDevice d : devs) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(d, &props);
        if (props.apiVersion < VK_API_VERSION_1_3 || !hasAllExtensions(d, !headless)) continue;

        uint32_t qCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qCount, nullptr);
        std::vector<VkQueueFamilyProperties> qProps(qCount);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qCount, qProps.data());
        int family = -1;
        for (uint32_t i = 0; i < qCount; ++i) {
            if (!(qProps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            if (!headless) {
                VkBool32 present = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(d, i, surface, &present);
                if (!present) continue;
            }
#ifndef __ANDROID__
            // M43: headless-with-displays prefers a present-capable family but
            // never rejects a device over it (the box may have no monitors).
            else if (canDisplay &&
                     !glfwGetPhysicalDevicePresentationSupport(instance, d, i)) {
                if (family < 0) family = (int)i; // usable, just not for display
                continue;
            }
#endif
            family = (int)i;
            break;
        }
        if (family < 0) continue;

        int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
        if (score > bestScore) {
            bestScore = score;
            phys = d;
            queueFamily = (uint32_t)family;
        }
    }
    if (!phys) {
        VKR_ERR("no Vulkan 1.3 device with the required interop extensions%s",
                headless ? "" : " and present support");
        return false;
    }

    VkPhysicalDeviceVulkan11Properties props11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
    VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    props2.pNext = &props11;
    vkGetPhysicalDeviceProperties2(phys, &props2);
    luidValid = props11.deviceLUIDValid == VK_TRUE;
    memcpy(deviceLUID, props11.deviceLUID, sizeof(deviceLUID));
    VKR_LOG("renderer: device: %s%s", props2.properties.deviceName,
            headless ? " (headless)" : "");

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    VkPhysicalDeviceVulkan13Features feat13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    feat13.dynamicRendering = VK_TRUE;
    feat13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceVulkan12Features feat12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    feat12.timelineSemaphore = VK_TRUE;
    feat12.pNext = &feat13;
    VkPhysicalDeviceVulkan11Features feat11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    feat11.samplerYcbcrConversion = VK_TRUE;
    feat11.pNext = &feat12;
    VkPhysicalDeviceFeatures2 feat2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    feat2.pNext = &feat11;

#ifndef __ANDROID__
    // M43: the chosen device must actually be able to present AND offer
    // VK_KHR_swapchain for runtime display windows; otherwise stay headless.
    if (headless && canDisplay &&
        (!glfwGetPhysicalDevicePresentationSupport(instance, phys, queueFamily) ||
         !hasAllExtensions(phys, true))) {
        VKR_LOG("renderer: device cannot present - display outputs unavailable");
        canDisplay = false;
    }
#endif

    std::vector<const char*> devExts(kDeviceExtensions,
                                     kDeviceExtensions + sizeof(kDeviceExtensions) / sizeof(char*));
    if (!headless || canDisplay) devExts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
#ifdef __ANDROID__
    // Optional: scheduled presents for exact video cadence.
    {
        uint32_t count = 0;
        vkEnumerateDeviceExtensionProperties(phys, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> props(count);
        vkEnumerateDeviceExtensionProperties(phys, nullptr, &count, props.data());
        for (const auto& p : props) {
            if (!strcmp(p.extensionName, VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME)) {
                devExts.push_back(VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME);
                hasDisplayTiming = true;
                VKR_LOG("renderer: VK_GOOGLE_display_timing available");
            }
        }
    }
#endif

    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &feat2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = (uint32_t)devExts.size();
    dci.ppEnabledExtensionNames = devExts.data();
    if (vkCreateDevice(phys, &dci, nullptr, &device) != VK_SUCCESS) {
        VKR_ERR("vkCreateDevice failed");
        return false;
    }
    volkLoadDevice(device);
    vkGetDeviceQueue(device, queueFamily, 0, &queue);

    return headless ? createOffscreenTarget(width, height) : createSwapchain();
}

bool VkContext::createOffscreenTarget(uint32_t width, uint32_t height) {
    swapFormat = VK_FORMAT_B8G8R8A8_UNORM;
    swapExtent = {width, height};

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = swapFormat;
    ici.extent = {width, height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkImage image = VK_NULL_HANDLE;
    if (vkCreateImage(device, &ici, nullptr, &image) != VK_SUCCESS) return false;

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, image, &req);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    uint32_t type = 0;
    for (uint32_t t = 0; t < mp.memoryTypeCount; ++t) {
        if ((req.memoryTypeBits & (1u << t)) &&
            (mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            type = t;
            break;
        }
    }
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if (vkAllocateMemory(device, &mai, nullptr, &offscreenMemory) != VK_SUCCESS ||
        vkBindImageMemory(device, image, offscreenMemory, 0) != VK_SUCCESS) {
        return false;
    }

    VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ivci.image = image;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = swapFormat;
    ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(device, &ivci, nullptr, &view) != VK_SUCCESS) return false;

    swapImages = {image};
    swapViews = {view};
    return true;
}

bool VkContext::createSwapchain() {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps);

    uint32_t fmtCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fmtCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fmtCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fmtCount, formats.data());
    VkSurfaceFormatKHR chosen = formats[0];
    for (const auto& f : formats) {
        // Video is already gamma-encoded; UNORM target writes it through as-is.
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { chosen = f; break; }
    }
    swapFormat = chosen.format;

    swapExtent = caps.currentExtent;
#ifndef __ANDROID__
    if (swapExtent.width == UINT32_MAX) {
        int fbw = 0, fbh = 0;
        glfwGetFramebufferSize(window, &fbw, &fbh);
        swapExtent.width = std::clamp((uint32_t)fbw, caps.minImageExtent.width, caps.maxImageExtent.width);
        swapExtent.height = std::clamp((uint32_t)fbh, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
#endif
    if (swapExtent.width == 0 || swapExtent.height == 0) return false; // minimized

    // MAILBOX when available: newest-image-wins presentation without tearing,
    // so a late frame never queues behind stale ones (lowest display latency).
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    // M28: VKR_PRESENT_MODE forces a mode on the PRIMARY window (scene_host
    // applies the same override to extra windows). This is the one that
    // matters for a single-window appliance - patching only the extra-window
    // path looks like it works and changes nothing.
    const char* pmEnv = getenv("VKR_PRESENT_MODE");
    if (pmEnv || !preferFifo) {
        VkPresentModeKHR want = VK_PRESENT_MODE_MAILBOX_KHR;
        if (pmEnv) {
            if (!strcmp(pmEnv, "immediate")) want = VK_PRESENT_MODE_IMMEDIATE_KHR;
            else if (!strcmp(pmEnv, "fifo")) want = VK_PRESENT_MODE_FIFO_KHR;
            else if (!strcmp(pmEnv, "mailbox")) want = VK_PRESENT_MODE_MAILBOX_KHR;
        }
        uint32_t pmCount = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &pmCount, nullptr);
        std::vector<VkPresentModeKHR> modes(pmCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &pmCount, modes.data());
        for (VkPresentModeKHR m : modes)
            if (m == want) presentMode = m;
    }

#ifdef __ANDROID__
    // One extra image over desktop: BufferQueue-backed swapchains stall in
    // acquire when SurfaceFlinger holds buffers across a latch.
    uint32_t imageCount = caps.minImageCount + 2;
#else
    uint32_t imageCount = caps.minImageCount + 1;
#endif
    if (caps.maxImageCount && imageCount > caps.maxImageCount) imageCount = caps.maxImageCount;

    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = surface;
    sci.minImageCount = imageCount;
    sci.imageFormat = chosen.format;
    sci.imageColorSpace = chosen.colorSpace;
    sci.imageExtent = swapExtent;
    sci.imageArrayLayers = 1;
    // TRANSFER_SRC enables the M15 snapshot dump of presented frames.
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    // IDENTITY lets the compositor apply display rotation (we don't rotate in
    // the shader); currentTransform without shader-side handling shows the
    // image rotated on rotated displays (seen on the TCL tablet).
    sci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                           ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                           : caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = presentMode;
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = swapchain;

    VkSwapchainKHR newSwapchain = VK_NULL_HANDLE;
    if (vkCreateSwapchainKHR(device, &sci, nullptr, &newSwapchain) != VK_SUCCESS) {
        VKR_ERR("vkCreateSwapchainKHR failed");
        return false;
    }
    destroySwapchain();
    swapchain = newSwapchain;

    uint32_t count = 0;
    vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr);
    swapImages.resize(count);
    vkGetSwapchainImagesKHR(device, swapchain, &count, swapImages.data());
    swapViews.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ivci.image = swapImages[i];
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format = swapFormat;
        ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(device, &ivci, nullptr, &swapViews[i]) != VK_SUCCESS) return false;
    }
    return true;
}

void VkContext::destroySwapchain() {
    for (VkImageView v : swapViews) vkDestroyImageView(device, v, nullptr);
    if (headless) {
        for (VkImage img : swapImages) vkDestroyImage(device, img, nullptr);
        if (offscreenMemory) vkFreeMemory(device, offscreenMemory, nullptr);
        offscreenMemory = VK_NULL_HANDLE;
    }
    swapViews.clear();
    swapImages.clear();
}

#ifdef __ANDROID__
bool VkContext::recreateSurface(void* newWindow) {
    vkDeviceWaitIdle(device);
    destroySwapchain();
    if (swapchain) {
        vkDestroySwapchainKHR(device, swapchain, nullptr);
        swapchain = VK_NULL_HANDLE;
    }
    if (surface) vkDestroySurfaceKHR(instance, surface, nullptr);
    surface = VK_NULL_HANDLE;
    nativeWindow = newWindow;

    VkAndroidSurfaceCreateInfoKHR asci{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    asci.window = (ANativeWindow*)nativeWindow;
    if (vkCreateAndroidSurfaceKHR(instance, &asci, nullptr, &surface) != VK_SUCCESS) {
        VKR_ERR("vkCreateAndroidSurfaceKHR (recreate) failed");
        return false;
    }
    return createSwapchain();
}
#endif

bool VkContext::recreateSwapchain() {
    if (headless) return true;
    vkDeviceWaitIdle(device);
    VkSwapchainKHR old = swapchain;
    bool ok = createSwapchain();
    if (ok && old && old != swapchain) vkDestroySwapchainKHR(device, old, nullptr);
    return ok;
}

void VkContext::destroy() {
    if (device) {
        vkDeviceWaitIdle(device);
        destroySwapchain();
        if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
        vkDestroyDevice(device, nullptr);
        device = VK_NULL_HANDLE;
    }
    if (surface) vkDestroySurfaceKHR(instance, surface, nullptr);
    if (instance) vkDestroyInstance(instance, nullptr);
#ifndef __ANDROID__
    if (window) glfwDestroyWindow(window);
    if (!headless || canDisplay) glfwTerminate();
#endif
}

} // namespace vkr
