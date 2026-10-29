#pragma once
#include "vk_context.h"
#include <memory>
#include <string>
#include <vector>
namespace vkr {
// GPU-only output. Publication follows the caller's existing frame fence;
// no CPU pixel mapping/conversion and no extra queue/device idle wait.
class GpuOutput {
public:
  GpuOutput();
  ~GpuOutput();
  void configure(const std::vector<std::string> &);
  void submitted(unsigned slot);
  void retire(VkContext &, unsigned);
  void record(VkContext &, unsigned, VkCommandBuffer, VkImage, VkExtent2D,
              VkFormat, VkImageLayout, int64_t);
  void destroy(VkContext &);

private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};
} // namespace vkr
