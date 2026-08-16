// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#pragma once

#include "helios/core/error.hpp"
#include "helios/vk/device.hpp"

#include <vulkan/vulkan.h>

namespace helios::render {

// Per-frame resources (frames in flight = 2 initially, per architecture).
struct FrameContext {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkSemaphore image_available = VK_NULL_HANDLE; // acquire -> submit
    VkFence in_flight = VK_NULL_HANDLE;           // submit completion
    bool submitted = false;
};
// Render-finished semaphores are indexed by SWAPCHAIN IMAGE (not frame
// slot): a present may still reference the semaphore when the frame slot is
// reused — per-image semaphores avoid the reuse hazard (Khronos swapchain
// semaphore reuse guidance).

// Creates the per-frame command pool + sync primitives.
[[nodiscard]] core::Result<FrameContext> createFrameContext(helios::vk::Device& device, std::string_view tag);

// Destroys frame resources (device must be idle).
void destroyFrameContext(helios::vk::Device& device, FrameContext& frame);

} // namespace helios::render
