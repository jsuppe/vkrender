#pragma once

#include <cstdint>
#include <vector>

#include <volk.h>

#include "vk_context.h"

// Writes the headless offscreen target (swapImages[0], GENERAL layout) as a
// binary PPM for SSH-only verification. Shared by the IPC renderer and the
// Wayland compositor mode.
bool vkrDumpTarget(vkr::VkContext& vk, VkCommandPool cmdPool, const char* path);

// Generalized dump of any BGRA image (M15 snapshot control: presented
// swapchain images). Transitions srcLayout -> TRANSFER_SRC -> srcLayout
// around the copy; the caller must ensure no GPU work is using the image.
bool vkrDumpImage(vkr::VkContext& vk, VkCommandPool cmdPool, VkImage image, VkExtent2D extent,
                  VkImageLayout srcLayout, const char* path);

// M42: the same readback into memory (tightly-packed BGRA) instead of a file -
// the moderator console's live preview. Same caller rules: render thread,
// between frames, no GPU work touching the image.
bool vkrReadImage(vkr::VkContext& vk, VkCommandPool cmdPool, VkImage image, VkExtent2D extent,
                  VkImageLayout srcLayout, std::vector<uint8_t>& bgraOut);

// M42: the same readback, but into memory (tightly-packed BGRA) instead of a
// file - the moderator console's live preview. Caller must ensure no GPU work
// is using the image (call between frames on the render thread).
#include <cstdint>
#include <vector>
bool vkrReadImage(vkr::VkContext& vk, VkCommandPool cmdPool, VkImage image, VkExtent2D extent,
                  VkImageLayout srcLayout, std::vector<uint8_t>& bgraOut);
