#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif
#ifdef __ANDROID__
#include <android/hardware_buffer.h>
#include <sys/system_properties.h>
#endif

#include "external_video_image.h"

#include <cstring>

#include <vector>

#include "log.h"

namespace vkr {

namespace {

VkFormat vkFormatFor(proto::SharedFormat f) {
    switch (f) {
        case proto::SharedFormat::NV12: return VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
        case proto::SharedFormat::BGRA8: return VK_FORMAT_B8G8R8A8_UNORM;
        // DXGI R10G10B10A2 (R in bits 0-9) == Vulkan A2B10G10R10 packed layout
        case proto::SharedFormat::RGB10A2: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    }
    return VK_FORMAT_UNDEFINED;
}

int pickMemoryType(VkPhysicalDevice phys, uint32_t allowedBits) {
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(phys, &memProps);
    int memType = -1;
    for (uint32_t t = 0; t < memProps.memoryTypeCount; ++t) {
        if (!(allowedBits & (1u << t))) continue;
        if (memProps.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
            return (int)t;
        if (memType < 0) memType = (int)t;
    }
    return memType;
}

bool createYcbcrConversion(VkContext& vk, VkFormat format, proto::Matrix matrix,
                           proto::Range range, VkSamplerYcbcrConversion* out) {
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
    if (vkCreateSamplerYcbcrConversion(vk.device, &convInfo, nullptr, out) != VK_SUCCESS) {
        VKR_ERR("vkCreateSamplerYcbcrConversion failed");
        return false;
    }
    return true;
}

bool makeView(VkContext& vk, VkImage image, VkFormat format, VkSamplerYcbcrConversion conversion,
              VkImageView* out) {
    VkSamplerYcbcrConversionInfo viewConv{VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO};
    viewConv.conversion = conversion;
    VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    if (conversion) ivci.pNext = &viewConv;
    ivci.image = image;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = format;
    ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return vkCreateImageView(vk.device, &ivci, nullptr, out) == VK_SUCCESS;
}

#ifdef _WIN32

constexpr VkExternalMemoryHandleTypeFlagBits kWin32HandleType =
    VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT_KHR;
constexpr VkImageUsageFlags kImageUsage =
    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

bool checkImportCapability(VkPhysicalDevice phys, VkFormat format) {
    VkPhysicalDeviceExternalImageFormatInfo extInfo{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
    extInfo.handleType = kWin32HandleType;

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
        VKR_ERR("driver rejects D3D11 import config for format %d (query = %d)", format, r);
        return false;
    }
    if (!(extProps.externalMemoryProperties.externalMemoryFeatures &
          VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) {
        VKR_ERR("driver reports D3D11 texture handles not importable for format %d", format);
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
    return true;
}

bool importWin32Slot(VkContext& vk, VkFormat format, uint32_t width, uint32_t height,
                     uint64_t handleValue, ExternalVideoPool::Slot* slot,
                     VkSamplerYcbcrConversion conversion) {
    VkExternalMemoryImageCreateInfo extImg{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    extImg.handleTypes = kWin32HandleType;

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
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(vk.device, &ici, nullptr, &slot->image[0]) != VK_SUCCESS) {
        VKR_ERR("vkCreateImage(imported D3D11) failed");
        return false;
    }

    VkMemoryDedicatedRequirements dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 req{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    req.pNext = &dedicated;
    VkImageMemoryRequirementsInfo2 reqInfo{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
    reqInfo.image = slot->image[0];
    vkGetImageMemoryRequirements2(vk.device, &reqInfo, &req);

    HANDLE handle = (HANDLE)(uintptr_t)handleValue;
    VkMemoryWin32HandlePropertiesKHR handleProps{
        VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
    if (vkGetMemoryWin32HandlePropertiesKHR(vk.device, kWin32HandleType, handle, &handleProps) !=
        VK_SUCCESS) {
        VKR_ERR("vkGetMemoryWin32HandlePropertiesKHR failed");
        return false;
    }

    // The valid memory types are the intersection of what the image needs and
    // what the imported handle allows; skipping the intersection works on
    // NVIDIA and breaks on other vendors.
    int memType = pickMemoryType(vk.phys,
                                 req.memoryRequirements.memoryTypeBits & handleProps.memoryTypeBits);
    if (memType < 0) {
        VKR_ERR("no compatible memory type for imported texture");
        return false;
    }

    VkImportMemoryWin32HandleInfoKHR importInfo{
        VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
    importInfo.handleType = kWin32HandleType;
    importInfo.handle = handle;
    // Imported D3D11 textures are dedicated-only on all desktop drivers.
    VkMemoryDedicatedAllocateInfo dedicatedInfo{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicatedInfo.pNext = &importInfo;
    dedicatedInfo.image = slot->image[0];

    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.pNext = &dedicatedInfo;
    alloc.allocationSize = req.memoryRequirements.size;
    alloc.memoryTypeIndex = (uint32_t)memType;
    if (vkAllocateMemory(vk.device, &alloc, nullptr, &slot->memory[0]) != VK_SUCCESS) {
        VKR_ERR("vkAllocateMemory(import D3D11 texture) failed");
        return false;
    }
    if (vkBindImageMemory(vk.device, slot->image[0], slot->memory[0], 0) != VK_SUCCESS) {
        VKR_ERR("vkBindImageMemory failed");
        return false;
    }
    return makeView(vk, slot->image[0], format, conversion, &slot->view[0]);
}

#endif // _WIN32

} // namespace

bool ExternalVideoPool::create(VkContext& vk, const proto::PoolDescMsg& desc) {
    format = vkFormatFor(desc.format);
    if (format == VK_FORMAT_UNDEFINED) {
        VKR_ERR("unsupported shared format %u", (uint32_t)desc.format);
        return false;
    }
    width = desc.width;
    height = desc.height;
    matrix = desc.matrix;
    range = desc.range;

    if (desc.format == proto::SharedFormat::NV12) {
        if (!createYcbcrConversion(vk, format, matrix, range, &conversion)) return false;
    }

#ifdef _WIN32
    if (desc.poolSize > proto::kMaxPoolSize) return false;
    if (!checkImportCapability(vk.phys, format)) return false;
    if (desc.poolSize == 0) {
        // M14 service mode: header only; slots arrive via addSurfaceWin32 and
        // the fence pairs live per client, outside the pool.
        VKR_LOG("renderer: stream header %ux%u fmt=%d, dynamic win32 surfaces", width, height,
                (int)format);
    } else {
        for (uint32_t i = 0; i < desc.poolSize; ++i) {
            Slot slot;
            if (!importWin32Slot(vk, format, width, height, desc.textureHandle[i], &slot,
                                 conversion)) {
                destroy(vk);
                return false;
            }
            slots.emplace(i, slot);
        }
        if (!importTimelineSemaphore(vk.device, desc.readyFenceHandle, &readySem) ||
            !importTimelineSemaphore(vk.device, desc.consumedFenceHandle, &consumedSem)) {
            destroy(vk);
            return false;
        }
        VKR_LOG("renderer: imported pool %ux%u fmt=%d x%u (zero-copy)", width, height,
                (int)format, (uint32_t)slots.size());
    }
#else
    // Linux: stream header only; surfaces arrive incrementally as SurfaceDesc.
    VKR_LOG("renderer: stream header %ux%u fmt=%d, awaiting dma-buf surfaces", width, height,
            (int)format);
#endif

    headerValid = true;
    return true;
}

bool ExternalVideoPool::addSurface(VkContext& vk, const proto::SurfaceDescMsg& desc,
                                   const int* fds, size_t fdCount) {
#ifdef _WIN32
    (void)vk; (void)desc; (void)fds; (void)fdCount;
    VKR_ERR("SurfaceDesc is not expected on Windows");
    return false;
#else
    auto closeAll = [&]() {
        for (size_t i = 0; i < fdCount; ++i) ::close(fds[i]);
    };
    if (!headerValid) {
        VKR_ERR("SurfaceDesc before PoolDesc");
        closeAll();
        return false;
    }
    if (slots.count(desc.slotKey)) { // decoder resent a known surface; ignore
        closeAll();
        return true;
    }
    if (desc.numObjects == 0 || desc.numObjects > 4 || fdCount != desc.numObjects ||
        desc.numPlanes == 0 || desc.numPlanes > 4) {
        VKR_ERR("bad dma-buf topology: %u objects / %zu fds / %u planes", desc.numObjects,
                fdCount, desc.numPlanes);
        closeAll();
        return false;
    }
    if (slots.size() >= proto::kMaxSurfaces) {
        VKR_ERR("too many surfaces (%zu)", slots.size());
        closeAll();
        return false;
    }

    Slot slot;
    auto fail = [&](const char* what) {
        VKR_ERR("%s (surface %u, modifier=0x%llx)", what, desc.slotKey,
                (unsigned long long)desc.drmModifier);
        for (auto& v : slot.view)
            if (v) vkDestroyImageView(vk.device, v, nullptr);
        for (auto& img : slot.image)
            if (img) vkDestroyImage(vk.device, img, nullptr);
        for (auto& m : slot.memory)
            if (m) vkFreeMemory(vk.device, m, nullptr);
        return false;
    };

    // Mode decision, once, on the first surface (needs the actual modifier):
    // if the driver cannot sample multi-planar NV12 with this modifier
    // (NVIDIA only lists LINEAR for NV12 but supports its block-linear
    // modifiers for R8/R8G8), fall back to per-plane images + shader YUV.
    if (slots.empty() && format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM) {
        VkDrmFormatModifierPropertiesListEXT list{
            VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
        VkFormatProperties2 props{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
        props.pNext = &list;
        vkGetPhysicalDeviceFormatProperties2(vk.phys, format, &props);
        std::vector<VkDrmFormatModifierPropertiesEXT> mods(list.drmFormatModifierCount);
        list.pDrmFormatModifierProperties = mods.data();
        vkGetPhysicalDeviceFormatProperties2(vk.phys, format, &props);
        bool supported = false;
        for (const auto& m : mods) {
            if (m.drmFormatModifier == desc.drmModifier &&
                (m.drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
                supported = true;
        }
        planar = !supported;
        if (planar)
            VKR_LOG("renderer: modifier 0x%llx not sampleable as multi-planar NV12; "
                    "using per-plane R8/R8G8 import",
                    (unsigned long long)desc.drmModifier);
    }

    if (planar) {
        if (desc.numPlanes != 2) {
            VKR_ERR("planar import expects 2 planes, got %u", desc.numPlanes);
            closeAll();
            return false;
        }
        // Import each buffer object once; both plane images may share one.
        for (uint32_t o = 0; o < desc.numObjects; ++o) {
            VkMemoryFdPropertiesKHR fdProps{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
            if (vkGetMemoryFdPropertiesKHR(vk.device,
                                           VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                                           fds[o], &fdProps) != VK_SUCCESS) {
                for (uint32_t r = o; r < fdCount; ++r) ::close(fds[r]);
                return fail("vkGetMemoryFdPropertiesKHR failed");
            }
            int memType = pickMemoryType(vk.phys, fdProps.memoryTypeBits);
            if (memType < 0) {
                for (uint32_t r = o; r < fdCount; ++r) ::close(fds[r]);
                return fail("no compatible memory type for dma-buf");
            }
            VkImportMemoryFdInfoKHR importInfo{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
            importInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
            importInfo.fd = fds[o];
            VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            alloc.pNext = &importInfo;
            alloc.allocationSize = desc.objectSize[o];
            alloc.memoryTypeIndex = (uint32_t)memType;
            if (vkAllocateMemory(vk.device, &alloc, nullptr, &slot.memory[o]) != VK_SUCCESS) {
                for (uint32_t r = o; r < fdCount; ++r) ::close(fds[r]);
                return fail("vkAllocateMemory(import dma-buf) failed");
            }
            // fd owned by Vulkan from here on.
        }

        const VkFormat planeFormats[2] = {VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM};
        for (uint32_t p = 0; p < 2; ++p) {
            VkSubresourceLayout layout{};
            layout.offset = desc.plane[p].offset;
            layout.rowPitch = desc.plane[p].pitch;
            VkImageDrmFormatModifierExplicitCreateInfoEXT modInfo{
                VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
            modInfo.drmFormatModifier = desc.drmModifier;
            modInfo.drmFormatModifierPlaneCount = 1;
            modInfo.pPlaneLayouts = &layout;

            VkExternalMemoryImageCreateInfo extImg{
                VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
            extImg.pNext = &modInfo;
            extImg.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

            VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            ici.pNext = &extImg;
            ici.imageType = VK_IMAGE_TYPE_2D;
            ici.format = planeFormats[p];
            ici.extent = {p == 0 ? width : width / 2, p == 0 ? height : height / 2, 1};
            ici.mipLevels = 1;
            ici.arrayLayers = 1;
            ici.samples = VK_SAMPLE_COUNT_1_BIT;
            ici.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
            ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
            ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (vkCreateImage(vk.device, &ici, nullptr, &slot.image[p]) != VK_SUCCESS)
                return fail("vkCreateImage(plane, dma-buf) failed");
            if (vkBindImageMemory(vk.device, slot.image[p],
                                  slot.memory[desc.planeObject[p]], 0) != VK_SUCCESS)
                return fail("vkBindImageMemory(plane) failed");
            if (!makeView(vk, slot.image[p], planeFormats[p], VK_NULL_HANDLE, &slot.view[p]))
                return fail("plane view creation failed");
        }

        slots.emplace(desc.slotKey, slot);
        VKR_LOG("renderer: imported dma-buf surface key=%u modifier=0x%llx planar (%zu total, "
                "zero-copy)",
                desc.slotKey, (unsigned long long)desc.drmModifier, slots.size());
        return true;
    }

    // Multi-object exports (one buffer per plane, e.g. nvidia-vaapi-driver)
    // need a DISJOINT image with per-plane memory binds; single-object
    // exports (Mesa) bind one allocation for the whole image.
    const bool disjoint = desc.numObjects > 1;

    VkSubresourceLayout planeLayouts[4]{};
    for (uint32_t p = 0; p < desc.numPlanes; ++p) {
        planeLayouts[p].offset = desc.plane[p].offset;
        planeLayouts[p].rowPitch = desc.plane[p].pitch;
    }
    VkImageDrmFormatModifierExplicitCreateInfoEXT modInfo{
        VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
    modInfo.drmFormatModifier = desc.drmModifier;
    modInfo.drmFormatModifierPlaneCount = desc.numPlanes;
    modInfo.pPlaneLayouts = planeLayouts;

    VkExternalMemoryImageCreateInfo extImg{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    extImg.pNext = &modInfo;
    extImg.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.pNext = &extImg;
    ici.flags = disjoint ? VK_IMAGE_CREATE_DISJOINT_BIT : 0;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {width, height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(vk.device, &ici, nullptr, &slot.image[0]) != VK_SUCCESS) {
        closeAll();
        return fail("vkCreateImage(dma-buf) failed - driver may not support this "
                    "modifier for sampling");
    }

    if (!disjoint) {
        VkMemoryFdPropertiesKHR fdProps{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
        if (vkGetMemoryFdPropertiesKHR(vk.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                                       fds[0], &fdProps) != VK_SUCCESS) {
            closeAll();
            return fail("vkGetMemoryFdPropertiesKHR failed");
        }
        VkMemoryDedicatedRequirements dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
        VkMemoryRequirements2 req{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
        req.pNext = &dedicated;
        VkImageMemoryRequirementsInfo2 reqInfo{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
        reqInfo.image = slot.image[0];
        vkGetImageMemoryRequirements2(vk.device, &reqInfo, &req);

        int memType = pickMemoryType(
            vk.phys, req.memoryRequirements.memoryTypeBits & fdProps.memoryTypeBits);
        if (memType < 0) {
            closeAll();
            return fail("no compatible memory type for dma-buf import");
        }
        VkImportMemoryFdInfoKHR importInfo{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
        importInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        importInfo.fd = fds[0];
        VkMemoryDedicatedAllocateInfo dedicatedInfo{
            VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicatedInfo.pNext = &importInfo;
        dedicatedInfo.image = slot.image[0];
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.pNext = dedicated.requiresDedicatedAllocation || dedicated.prefersDedicatedAllocation
                          ? (void*)&dedicatedInfo
                          : (void*)&importInfo;
        alloc.allocationSize =
            desc.objectSize[0] ? desc.objectSize[0] : req.memoryRequirements.size;
        alloc.memoryTypeIndex = (uint32_t)memType;
        if (vkAllocateMemory(vk.device, &alloc, nullptr, &slot.memory[0]) != VK_SUCCESS) {
            closeAll();
            return fail("vkAllocateMemory(import dma-buf) failed");
        }
        // Vulkan owns the fd now.
        if (vkBindImageMemory(vk.device, slot.image[0], slot.memory[0], 0) != VK_SUCCESS)
            return fail("vkBindImageMemory failed");
    } else {
        // One import per buffer object, one bind per plane.
        static const VkImageAspectFlagBits kPlaneAspects[4] = {
            VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT, VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT,
            VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT, VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT};

        for (uint32_t o = 0; o < desc.numObjects; ++o) {
            VkMemoryFdPropertiesKHR fdProps{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
            if (vkGetMemoryFdPropertiesKHR(vk.device,
                                           VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                                           fds[o], &fdProps) != VK_SUCCESS) {
                for (uint32_t r = o; r < fdCount; ++r) ::close(fds[r]);
                return fail("vkGetMemoryFdPropertiesKHR failed");
            }
            // Plane requirements: use the first plane backed by this object.
            uint32_t planeForObject = 0;
            for (uint32_t p = 0; p < desc.numPlanes; ++p)
                if (desc.planeObject[p] == o) { planeForObject = p; break; }
            VkImagePlaneMemoryRequirementsInfo planeInfo{
                VK_STRUCTURE_TYPE_IMAGE_PLANE_MEMORY_REQUIREMENTS_INFO};
            planeInfo.planeAspect = kPlaneAspects[planeForObject];
            VkImageMemoryRequirementsInfo2 reqInfo{
                VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
            reqInfo.pNext = &planeInfo;
            reqInfo.image = slot.image[0];
            VkMemoryRequirements2 req{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
            vkGetImageMemoryRequirements2(vk.device, &reqInfo, &req);

            int memType = pickMemoryType(
                vk.phys, req.memoryRequirements.memoryTypeBits & fdProps.memoryTypeBits);
            if (memType < 0) {
                for (uint32_t r = o; r < fdCount; ++r) ::close(fds[r]);
                return fail("no compatible memory type for dma-buf plane import");
            }
            VkImportMemoryFdInfoKHR importInfo{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
            importInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
            importInfo.fd = fds[o];
            VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            alloc.pNext = &importInfo;
            alloc.allocationSize =
                desc.objectSize[o] ? desc.objectSize[o] : req.memoryRequirements.size;
            alloc.memoryTypeIndex = (uint32_t)memType;
            if (vkAllocateMemory(vk.device, &alloc, nullptr, &slot.memory[o]) != VK_SUCCESS) {
                for (uint32_t r = o; r < fdCount; ++r) ::close(fds[r]);
                return fail("vkAllocateMemory(import dma-buf plane) failed");
            }
            // fd owned by Vulkan from here on.
        }

        VkBindImagePlaneMemoryInfo planeBind[4]{};
        VkBindImageMemoryInfo binds[4]{};
        for (uint32_t p = 0; p < desc.numPlanes; ++p) {
            planeBind[p] = {VK_STRUCTURE_TYPE_BIND_IMAGE_PLANE_MEMORY_INFO};
            planeBind[p].planeAspect = kPlaneAspects[p];
            binds[p] = {VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO};
            binds[p].pNext = &planeBind[p];
            binds[p].image = slot.image[0];
            binds[p].memory = slot.memory[desc.planeObject[p]];
            binds[p].memoryOffset = 0; // plane offset lives in the modifier layout
        }
        if (vkBindImageMemory2(vk.device, desc.numPlanes, binds) != VK_SUCCESS)
            return fail("vkBindImageMemory2(disjoint planes) failed");
    }

    if (!makeView(vk, slot.image[0], format, conversion, &slot.view[0]))
        return fail("view creation failed for imported dma-buf surface");

    slots.emplace(desc.slotKey, slot);
    VKR_LOG("renderer: imported dma-buf surface key=%u modifier=0x%llx (%zu total, zero-copy)",
            desc.slotKey, (unsigned long long)desc.drmModifier, slots.size());
    return true;
#endif
}

#ifdef __ANDROID__
// MediaCodec output buffers are almost always vendor-opaque YUV: import goes
// through VkExternalFormatANDROID and a YCbCr conversion created from the
// buffer's reported format properties (the mandatory path for external
// formats). The conversion is created once, from the first buffer.
bool ExternalVideoPool::addSurfaceAHB(VkContext& vk, uint32_t key, AHardwareBuffer* buffer) {
    if (!headerValid) {
        VKR_ERR("SurfaceDesc before PoolDesc");
        return false;
    }
    if (slots.count(key)) return true;
    if (slots.size() >= proto::kMaxSurfaces) {
        VKR_ERR("too many surfaces (%zu)", slots.size());
        return false;
    }

    VkAndroidHardwareBufferFormatPropertiesANDROID fmtProps{
        VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
    VkAndroidHardwareBufferPropertiesANDROID props{
        VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
    props.pNext = &fmtProps;
    if (vkGetAndroidHardwareBufferPropertiesANDROID(vk.device, buffer, &props) != VK_SUCCESS) {
        VKR_ERR("vkGetAndroidHardwareBufferPropertiesANDROID failed");
        return false;
    }

    if (!conversion) {
        VkExternalFormatANDROID extFmt{VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID};
        extFmt.externalFormat = fmtProps.externalFormat;
        VkSamplerYcbcrConversionCreateInfo convInfo{
            VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO};
        if (fmtProps.format == VK_FORMAT_UNDEFINED) convInfo.pNext = &extFmt;
        convInfo.format = fmtProps.format;
        convInfo.ycbcrModel = fmtProps.suggestedYcbcrModel;
        convInfo.ycbcrRange = fmtProps.suggestedYcbcrRange;
        convInfo.components = fmtProps.samplerYcbcrConversionComponents;
        // Note: on the emulator, gfxstream's external-format sampling routes
        // chroma incorrectly for YV12 buffers (colors shift; component
        // swizzle overrides are ignored). Cosmetic, emulator-only - real
        // devices use their vendor's CTS-validated conversion. See
        // docs/ANDROID.md.
        convInfo.xChromaOffset = fmtProps.suggestedXChromaOffset;
        convInfo.yChromaOffset = fmtProps.suggestedYChromaOffset;
        convInfo.chromaFilter = VK_FILTER_LINEAR;
        convInfo.forceExplicitReconstruction = VK_FALSE;
        if (vkCreateSamplerYcbcrConversion(vk.device, &convInfo, nullptr, &conversion) !=
            VK_SUCCESS) {
            VKR_ERR("vkCreateSamplerYcbcrConversion(external format 0x%llx) failed",
                    (unsigned long long)fmtProps.externalFormat);
            return false;
        }
        format = fmtProps.format; // may stay UNDEFINED (external format)
    }

    AHardwareBuffer_Desc ahbDesc{};
    AHardwareBuffer_describe(buffer, &ahbDesc);

    Slot slot;
    auto fail = [&](const char* what) {
        VKR_ERR("%s (surface %u)", what, key);
        for (auto& v : slot.view)
            if (v) vkDestroyImageView(vk.device, v, nullptr);
        for (auto& img : slot.image)
            if (img) vkDestroyImage(vk.device, img, nullptr);
        for (auto& m : slot.memory)
            if (m) vkFreeMemory(vk.device, m, nullptr);
        return false;
    };

    VkExternalFormatANDROID extFmt{VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID};
    extFmt.externalFormat = fmtProps.format == VK_FORMAT_UNDEFINED ? fmtProps.externalFormat : 0;
    VkExternalMemoryImageCreateInfo extImg{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    extImg.pNext = &extFmt;
    extImg.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.pNext = &extImg;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = fmtProps.format;
    ici.extent = {ahbDesc.width, ahbDesc.height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(vk.device, &ici, nullptr, &slot.image[0]) != VK_SUCCESS)
        return fail("vkCreateImage(AHardwareBuffer) failed");

    int memType = pickMemoryType(vk.phys, props.memoryTypeBits);
    if (memType < 0) return fail("no compatible memory type for AHardwareBuffer");

    VkImportAndroidHardwareBufferInfoANDROID importInfo{
        VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID};
    importInfo.buffer = buffer;
    VkMemoryDedicatedAllocateInfo dedicatedInfo{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicatedInfo.pNext = &importInfo;
    dedicatedInfo.image = slot.image[0]; // AHB imports are dedicated-only

    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.pNext = &dedicatedInfo;
    alloc.allocationSize = props.allocationSize;
    alloc.memoryTypeIndex = (uint32_t)memType;
    if (vkAllocateMemory(vk.device, &alloc, nullptr, &slot.memory[0]) != VK_SUCCESS)
        return fail("vkAllocateMemory(import AHardwareBuffer) failed");
    if (vkBindImageMemory(vk.device, slot.image[0], slot.memory[0], 0) != VK_SUCCESS)
        return fail("vkBindImageMemory failed");
    if (!makeView(vk, slot.image[0], fmtProps.format, conversion, &slot.view[0]))
        return fail("view creation failed");

    slots.emplace(key, slot);
    VKR_LOG("renderer: imported AHardwareBuffer surface key=%u extFmt=0x%llx (%zu total, "
            "zero-copy)",
            key, (unsigned long long)fmtProps.externalFormat, slots.size());
    return true;
}
#endif // __ANDROID__

void ExternalVideoPool::removeSlot(VkContext& vk, uint32_t key) {
    auto it = slots.find(key);
    if (it == slots.end()) return;
    Slot& s = it->second;
    for (auto& v : s.view)
        if (v) vkDestroyImageView(vk.device, v, nullptr);
    for (auto& img : s.image)
        if (img) vkDestroyImage(vk.device, img, nullptr);
    for (auto& m : s.memory)
        if (m) vkFreeMemory(vk.device, m, nullptr);
    slots.erase(it);
}

#ifdef _WIN32
bool ExternalVideoPool::addSurfaceWin32(VkContext& vk, uint32_t key, uint64_t handleValue,
                                        uint32_t w, uint32_t h) {
    if (!headerValid) {
        VKR_ERR("addSurfaceWin32 before pool header");
        return false;
    }
    if (slots.count(key)) return true;
    if (slots.size() >= proto::kMaxSurfaces) {
        VKR_ERR("too many surfaces (%zu)", slots.size());
        return false;
    }
    Slot slot;
    if (!importWin32Slot(vk, format, w, h, handleValue, &slot, conversion)) return false;
    slots.emplace(key, slot);
    VKR_LOG("renderer: imported win32 surface key=%u %ux%u (%zu total, zero-copy)", key, w, h,
            slots.size());
    return true;
}

bool importTimelineSemaphoreWin32(VkContext& vk, uint64_t handleValue, VkSemaphore* out) {
    return importTimelineSemaphore(vk.device, handleValue, out);
}

bool importWin32BlitTarget(VkContext& vk, uint64_t handleValue, uint32_t width,
                           uint32_t height, VkImage* outImage, VkDeviceMemory* outMemory) {
    VkExternalMemoryImageCreateInfo extImg{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    extImg.handleTypes = kWin32HandleType;

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.pNext = &extImg;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_B8G8R8A8_UNORM;
    ici.extent = {width, height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(vk.device, &ici, nullptr, outImage) != VK_SUCCESS) {
        VKR_ERR("vkCreateImage(blit target) failed");
        return false;
    }

    VkMemoryDedicatedRequirements dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 req{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    req.pNext = &dedicated;
    VkImageMemoryRequirementsInfo2 reqInfo{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
    reqInfo.image = *outImage;
    vkGetImageMemoryRequirements2(vk.device, &reqInfo, &req);

    HANDLE handle = (HANDLE)(uintptr_t)handleValue;
    VkMemoryWin32HandlePropertiesKHR handleProps{
        VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
    if (vkGetMemoryWin32HandlePropertiesKHR(vk.device, kWin32HandleType, handle,
                                            &handleProps) != VK_SUCCESS) {
        VKR_ERR("vkGetMemoryWin32HandlePropertiesKHR (blit target) failed");
        return false;
    }
    int memType = pickMemoryType(
        vk.phys, req.memoryRequirements.memoryTypeBits & handleProps.memoryTypeBits);
    if (memType < 0) {
        VKR_ERR("no compatible memory type for blit target");
        return false;
    }

    VkImportMemoryWin32HandleInfoKHR importInfo{
        VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
    importInfo.handleType = kWin32HandleType;
    importInfo.handle = handle;
    VkMemoryDedicatedAllocateInfo dedicatedInfo{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicatedInfo.pNext = &importInfo;
    dedicatedInfo.image = *outImage;

    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.pNext = &dedicatedInfo;
    alloc.allocationSize = req.memoryRequirements.size;
    alloc.memoryTypeIndex = (uint32_t)memType;
    if (vkAllocateMemory(vk.device, &alloc, nullptr, outMemory) != VK_SUCCESS) {
        VKR_ERR("vkAllocateMemory(blit target) failed");
        return false;
    }
    if (vkBindImageMemory(vk.device, *outImage, *outMemory, 0) != VK_SUCCESS) {
        VKR_ERR("vkBindImageMemory(blit target) failed");
        return false;
    }
    return true;
}
#endif

bool ExternalVideoPool::addOwnedSurface(VkContext& vk, uint32_t key, uint32_t width,
                                        uint32_t height) {
    if (slots.count(key)) return true;
    if (slots.size() >= proto::kMaxSurfaces) {
        VKR_ERR("too many surfaces (%zu)", slots.size());
        return false;
    }

    Slot slot;
    slot.external = false;
    slot.width = width;
    slot.height = height;

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_B8G8R8A8_UNORM;
    ici.extent = {width, height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_LINEAR; // CPU-writable, sampled directly
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
    if (vkCreateImage(vk.device, &ici, nullptr, &slot.image[0]) != VK_SUCCESS) {
        VKR_ERR("owned surface image creation failed (%ux%u)", width, height);
        return false;
    }

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(vk.device, slot.image[0], &req);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(vk.phys, &mp);
    int memType = -1;
    for (uint32_t t = 0; t < mp.memoryTypeCount; ++t) {
        constexpr VkMemoryPropertyFlags want =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if ((req.memoryTypeBits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & want) == want) {
            memType = (int)t;
            break;
        }
    }
    auto fail = [&](const char* what) {
        VKR_ERR("%s (owned surface %u)", what, key);
        if (slot.view[0]) vkDestroyImageView(vk.device, slot.view[0], nullptr);
        vkDestroyImage(vk.device, slot.image[0], nullptr);
        if (slot.memory[0]) vkFreeMemory(vk.device, slot.memory[0], nullptr);
        return false;
    };
    if (memType < 0) return fail("no host-visible memory type");

    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = (uint32_t)memType;
    if (vkAllocateMemory(vk.device, &mai, nullptr, &slot.memory[0]) != VK_SUCCESS)
        return fail("owned surface allocation failed");
    if (vkBindImageMemory(vk.device, slot.image[0], slot.memory[0], 0) != VK_SUCCESS)
        return fail("owned surface bind failed");
    if (vkMapMemory(vk.device, slot.memory[0], 0, VK_WHOLE_SIZE, 0, &slot.mapped) != VK_SUCCESS)
        return fail("owned surface map failed");

    VkImageSubresource sub{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
    VkSubresourceLayout layout;
    vkGetImageSubresourceLayout(vk.device, slot.image[0], &sub, &layout);
    slot.rowPitch = layout.rowPitch;

    if (!makeView(vk, slot.image[0], VK_FORMAT_B8G8R8A8_UNORM, VK_NULL_HANDLE, &slot.view[0]))
        return fail("owned surface view failed");

    // One-time PREINITIALIZED -> GENERAL transition (sampling layout).
    VkCommandPool tmpPool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    cpci.queueFamilyIndex = vk.queueFamily;
    vkCreateCommandPool(vk.device, &cpci, nullptr, &tmpPool);
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = tmpPool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(vk.device, &cbai, &cmd);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    b.srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.image = slot.image[0];
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(vk.queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(vk.queue);
    vkDestroyCommandPool(vk.device, tmpPool, nullptr);

    slots.emplace(key, slot);
    VKR_LOG("renderer: created owned surface key=%u %ux%u (shm upload path, %zu total)", key,
            width, height, slots.size());
    return true;
}

bool ExternalVideoPool::updateOwnedSurface(VkContext& vk, uint32_t key, const void* pixels,
                                          uint64_t srcStride, uint32_t width, uint32_t height) {
    (void)vk;
    auto it = slots.find(key);
    if (it == slots.end() || it->second.external || !it->second.mapped) return false;
    Slot& s = it->second;
    uint32_t copyW = width < s.width ? width : s.width;
    uint32_t copyH = height < s.height ? height : s.height;
    const uint8_t* src = (const uint8_t*)pixels;
    uint8_t* dst = (uint8_t*)s.mapped;
    for (uint32_t y = 0; y < copyH; ++y)
        memcpy(dst + y * s.rowPitch, src + y * srcStride, (size_t)copyW * 4);
    return true;
}

void ExternalVideoPool::destroy(VkContext& vk) {
    for (auto& [key, s] : slots) {
        for (auto& v : s.view)
            if (v) vkDestroyImageView(vk.device, v, nullptr);
        for (auto& img : s.image)
            if (img) vkDestroyImage(vk.device, img, nullptr);
        for (auto& m : s.memory)
            if (m) vkFreeMemory(vk.device, m, nullptr);
    }
    slots.clear();
    if (conversion) vkDestroySamplerYcbcrConversion(vk.device, conversion, nullptr);
    conversion = VK_NULL_HANDLE;
    if (readySem) vkDestroySemaphore(vk.device, readySem, nullptr);
    if (consumedSem) vkDestroySemaphore(vk.device, consumedSem, nullptr);
    readySem = consumedSem = VK_NULL_HANDLE;
    width = height = 0;
    headerValid = false;
    format = VK_FORMAT_UNDEFINED;
}

} // namespace vkr

