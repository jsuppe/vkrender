#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "external_video_image.h"

#include "log.h"

namespace vkr {

namespace {

constexpr VkExternalMemoryHandleTypeFlagBits kHandleType =
    VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT_KHR;
constexpr VkImageUsageFlags kImageUsage =
    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

VkFormat vkFormatFor(proto::SharedFormat f) {
    switch (f) {
        case proto::SharedFormat::NV12: return VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
        case proto::SharedFormat::BGRA8: return VK_FORMAT_B8G8R8A8_UNORM;
        // DXGI R10G10B10A2 (R in bits 0-9) == Vulkan A2B10G10R10 packed layout
        case proto::SharedFormat::RGB10A2: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    }
    return VK_FORMAT_UNDEFINED;
}

bool checkImportCapability(VkPhysicalDevice phys, VkFormat format) {
    VkPhysicalDeviceExternalImageFormatInfo extInfo{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
    extInfo.handleType = kHandleType;

    VkPhysicalDeviceImageFormatInfo2 fmtInfo{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
    fmtInfo.pNext = &extInfo;
    fmtInfo.format = format;
    fmtInfo.type = VK_IMAGE_TYPE_2D;
    fmtInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    fmtInfo.usage = kImageUsage;

    VkExternalImageFormatProperties extProps{
        VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 props{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
    props.pNext = &extProps;

    VkResult r = vkGetPhysicalDeviceImageFormatProperties2(phys, &fmtInfo, &props);
    if (r != VK_SUCCESS) {
        VKR_ERR("driver rejects D3D11 import config for format %d (vkGetPhysicalDeviceImageFormatProperties2 = %d)",
                format, r);
        return false;
    }
    const auto& emp = extProps.externalMemoryProperties;
    if (!(emp.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) {
        VKR_ERR("driver reports D3D11 texture handles are not importable for format %d", format);
        return false;
    }
    return true;
}

bool importTimelineSemaphore(VkDevice device, uint64_t handleValue, VkSemaphore* out) {
    VkSemaphoreTypeCreateInfo typeInfo{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    typeInfo.initialValue = 0;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    sci.pNext = &typeInfo;
    if (vkCreateSemaphore(device, &sci, nullptr, out) != VK_SUCCESS) return false;

    // D3D12_FENCE covers fences created by ID3D11Device5::CreateFence too.
    VkImportSemaphoreWin32HandleInfoKHR imp{
        VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
    imp.semaphore = *out;
    imp.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
    imp.handle = (HANDLE)(uintptr_t)handleValue;
    if (vkImportSemaphoreWin32HandleKHR(device, &imp) != VK_SUCCESS) {
        VKR_ERR("vkImportSemaphoreWin32HandleKHR(D3D12_FENCE) failed");
        return false;
    }
    // Keep the NT handle open for the pool's lifetime (see texture import note).
    return true;
}

} // namespace

bool ExternalVideoPool::create(VkContext& vk, const proto::PoolDescMsg& desc) {
    const VkFormat format = vkFormatFor(desc.format);
    if (format == VK_FORMAT_UNDEFINED) {
        VKR_ERR("unsupported shared format %u", (uint32_t)desc.format);
        return false;
    }
    if (desc.poolSize == 0 || desc.poolSize > proto::kMaxPoolSize) return false;
    if (!checkImportCapability(vk.phys, format)) return false;

    width = desc.width;
    height = desc.height;
    matrix = desc.matrix;
    range = desc.range;

    if (desc.format == proto::SharedFormat::NV12) {
        VkSamplerYcbcrConversionCreateInfo convInfo{
            VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO};
        convInfo.format = format;
        convInfo.ycbcrModel = matrix == proto::Matrix::BT709
                                  ? VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709
                                  : VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601;
        convInfo.ycbcrRange = range == proto::Range::Full ? VK_SAMPLER_YCBCR_RANGE_ITU_FULL
                                                          : VK_SAMPLER_YCBCR_RANGE_ITU_NARROW;
        convInfo.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                               VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
        convInfo.xChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;
        convInfo.yChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;
        convInfo.chromaFilter = VK_FILTER_LINEAR;
        convInfo.forceExplicitReconstruction = VK_FALSE;
        if (vkCreateSamplerYcbcrConversion(vk.device, &convInfo, nullptr, &conversion) != VK_SUCCESS) {
            VKR_ERR("vkCreateSamplerYcbcrConversion failed");
            return false;
        }
    }

    for (uint32_t i = 0; i < desc.poolSize; ++i) {
        Slot slot;

        VkExternalMemoryImageCreateInfo extImg{
            VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        extImg.handleTypes = kHandleType;

        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.pNext = &extImg;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = format;
        ici.extent = {width, height, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = kImageUsage;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(vk.device, &ici, nullptr, &slot.image) != VK_SUCCESS) {
            VKR_ERR("vkCreateImage(imported NV12) failed");
            destroy(vk);
            return false;
        }

        VkMemoryDedicatedRequirements dedicated{
            VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
        VkMemoryRequirements2 req{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
        req.pNext = &dedicated;
        VkImageMemoryRequirementsInfo2 reqInfo{
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
        reqInfo.image = slot.image;
        vkGetImageMemoryRequirements2(vk.device, &reqInfo, &req);

        HANDLE handle = (HANDLE)(uintptr_t)desc.textureHandle[i];
        VkMemoryWin32HandlePropertiesKHR handleProps{
            VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
        if (vkGetMemoryWin32HandlePropertiesKHR(vk.device, kHandleType, handle,
                                                &handleProps) != VK_SUCCESS) {
            VKR_ERR("vkGetMemoryWin32HandlePropertiesKHR failed (slot %u)", i);
            destroy(vk);
            return false;
        }

        // The valid memory types are the intersection of what the image needs
        // and what the imported handle allows; skipping the intersection works
        // on NVIDIA and breaks on other vendors.
        uint32_t memBits = req.memoryRequirements.memoryTypeBits & handleProps.memoryTypeBits;
        VkPhysicalDeviceMemoryProperties memProps;
        vkGetPhysicalDeviceMemoryProperties(vk.phys, &memProps);
        int memType = -1;
        for (uint32_t t = 0; t < memProps.memoryTypeCount; ++t) {
            if (!(memBits & (1u << t))) continue;
            if (memProps.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
                memType = (int)t;
                break;
            }
            if (memType < 0) memType = (int)t;
        }
        if (memType < 0) {
            VKR_ERR("no compatible memory type for imported texture (bits img=%08x handle=%08x)",
                    req.memoryRequirements.memoryTypeBits, handleProps.memoryTypeBits);
            destroy(vk);
            return false;
        }

        VkImportMemoryWin32HandleInfoKHR importInfo{
            VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
        importInfo.handleType = kHandleType;
        importInfo.handle = handle;
        // Imported D3D11 textures are dedicated-only on all desktop drivers.
        VkMemoryDedicatedAllocateInfo dedicatedInfo{
            VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicatedInfo.pNext = &importInfo;
        dedicatedInfo.image = slot.image;

        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.pNext = &dedicatedInfo;
        alloc.allocationSize = req.memoryRequirements.size;
        alloc.memoryTypeIndex = (uint32_t)memType;
        if (vkAllocateMemory(vk.device, &alloc, nullptr, &slot.memory) != VK_SUCCESS) {
            VKR_ERR("vkAllocateMemory(import D3D11 texture, slot %u) failed", i);
            destroy(vk);
            return false;
        }
        // Deliberately NOT closing 'handle': spec says NT handle ownership
        // stays with the app after import, but keep it open for the pool's
        // lifetime while diagnosing device-lost on first access.
        if (vkBindImageMemory(vk.device, slot.image, slot.memory, 0) != VK_SUCCESS) {
            VKR_ERR("vkBindImageMemory failed (slot %u)", i);
            destroy(vk);
            return false;
        }

        VkSamplerYcbcrConversionInfo viewConv{VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO};
        viewConv.conversion = conversion;
        VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        if (conversion) ivci.pNext = &viewConv;
        ivci.image = slot.image;
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format = format;
        ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(vk.device, &ivci, nullptr, &slot.view) != VK_SUCCESS) {
            VKR_ERR("image view creation failed (slot %u)", i);
            destroy(vk);
            return false;
        }
        slots.push_back(slot);
    }

    if (!importTimelineSemaphore(vk.device, desc.readyFenceHandle, &readySem) ||
        !importTimelineSemaphore(vk.device, desc.consumedFenceHandle, &consumedSem)) {
        destroy(vk);
        return false;
    }

    const char* fmtName = desc.format == proto::SharedFormat::NV12      ? "NV12"
                          : desc.format == proto::SharedFormat::RGB10A2 ? "RGB10A2"
                                                                        : "BGRA8";
    VKR_LOG("renderer: imported pool %ux%u %s x%u (zero-copy)", width, height, fmtName,
            (uint32_t)slots.size());
    return true;
}

void ExternalVideoPool::destroy(VkContext& vk) {
    for (auto& s : slots) {
        if (s.view) vkDestroyImageView(vk.device, s.view, nullptr);
        if (s.image) vkDestroyImage(vk.device, s.image, nullptr);
        if (s.memory) vkFreeMemory(vk.device, s.memory, nullptr);
    }
    slots.clear();
    if (conversion) vkDestroySamplerYcbcrConversion(vk.device, conversion, nullptr);
    conversion = VK_NULL_HANDLE;
    if (readySem) vkDestroySemaphore(vk.device, readySem, nullptr);
    if (consumedSem) vkDestroySemaphore(vk.device, consumedSem, nullptr);
    readySem = consumedSem = VK_NULL_HANDLE;
    width = height = 0;
}

} // namespace vkr
