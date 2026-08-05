#pragma once

#include <volk.h>

#include "vk_context.h"

// Writes the headless offscreen target (swapImages[0], GENERAL layout) as a
// binary PPM for SSH-only verification. Shared by the IPC renderer and the
// Wayland compositor mode.
bool vkrDumpTarget(vkr::VkContext& vk, VkCommandPool cmdPool, const char* path);
