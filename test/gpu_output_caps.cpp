#include "vk_context.h"
#include <cstdio>
#include <cstring>
#ifdef __ANDROID__
#include <android/hardware_buffer.h>
#endif
int main() {
  vkr::VkContext vk;
  if (!vk.init(64, 64, "Output capability probe", true))
    return 1;
  VkPhysicalDeviceProperties p{};
  vkGetPhysicalDeviceProperties(vk.phys, &p);
  printf("device=%s\n", p.deviceName);
  uint32_t n = 0;
  vkEnumerateDeviceExtensionProperties(vk.phys, nullptr, &n, nullptr);
  std::vector<VkExtensionProperties> ex(n);
  vkEnumerateDeviceExtensionProperties(vk.phys, nullptr, &n, ex.data());
  for (auto &e : ex)
    if (strstr(e.extensionName, "external_format_resolve") ||
        strstr(e.extensionName, "external_memory"))
      printf("extension=%s\n", e.extensionName);
  for (auto fmt :
       {VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_G8_B8R8_2PLANE_420_UNORM}) {
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(vk.phys, fmt, &fp);
    printf("format=%u optimal=0x%x linear=0x%x\n", fmt,
           fp.optimalTilingFeatures, fp.linearTilingFeatures);
  }
#ifdef __ANDROID__
  for (uint32_t format : {AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
                          AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420})
    for (bool render : {false, true}) {
      AHardwareBuffer_Desc d{};
      d.width = 128;
      d.height = 72;
      d.layers = 1;
      d.format = format;
      d.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                AHARDWAREBUFFER_USAGE_CPU_READ_RARELY |
                AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;
      if (render)
        d.usage |= AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT;
      AHardwareBuffer *b = nullptr;
      int r = AHardwareBuffer_allocate(&d, &b);
      printf("AHB format=%u render=%d allocate=%d", format, render, r);
      if (b) {
        VkAndroidHardwareBufferFormatPropertiesANDROID f{
            VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
        VkAndroidHardwareBufferPropertiesANDROID q{
            VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
        q.pNext = &f;
        auto vr = vkGetAndroidHardwareBufferPropertiesANDROID(vk.device, b, &q);
        printf(" props=%d vkFormat=%u external=%llu features=0x%x", vr,
               f.format, (unsigned long long)f.externalFormat,
               f.formatFeatures);
        AHardwareBuffer_release(b);
      }
      puts("");
    }
#endif
  vk.destroy();
  return 0;
}
