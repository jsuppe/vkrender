#pragma once

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
