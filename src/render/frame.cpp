// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "helios/render/frame.hpp"

#include "helios/core/debug_name.hpp"

#include <volk.h>

namespace helios::render {

core::Result<FrameContext> createFrameContext(helios::vk::Device& device, std::string_view tag) {
    FrameContext frame;
    const std::string prefix = "frame-" + std::string(tag);

    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = device.graphicsQueueFamily();
    if (vkCreateCommandPool(device.handle(), &pool_info, nullptr, &frame.pool) != VK_SUCCESS) {
        return core::makeError(core::ErrorCode::Vulkan, "command pool creation failed");
    }
    device.setDebugName(VK_OBJECT_TYPE_COMMAND_POOL, reinterpret_cast<std::uint64_t>(frame.pool),
                        prefix + "-pool");

    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = frame.pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device.handle(), &alloc, &frame.cmd) != VK_SUCCESS) {
        vkDestroyCommandPool(device.handle(), frame.pool, nullptr);
        return core::makeError(core::ErrorCode::Vulkan, "command buffer allocation failed");
    }

    VkSemaphoreCreateInfo sem_info{};
    sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    if (vkCreateSemaphore(device.handle(), &sem_info, nullptr, &frame.image_available) != VK_SUCCESS) {
        vkDestroyCommandPool(device.handle(), frame.pool, nullptr);
        return core::makeError(core::ErrorCode::Vulkan, "semaphore creation failed");
    }
    device.setDebugName(VK_OBJECT_TYPE_SEMAPHORE, reinterpret_cast<std::uint64_t>(frame.image_available),
                        prefix + "-image-available");

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT; // first acquire must not block
    if (vkCreateFence(device.handle(), &fence_info, nullptr, &frame.in_flight) != VK_SUCCESS) {
        vkDestroyCommandPool(device.handle(), frame.pool, nullptr);
        return core::makeError(core::ErrorCode::Vulkan, "fence creation failed");
    }
    device.setDebugName(VK_OBJECT_TYPE_FENCE, reinterpret_cast<std::uint64_t>(frame.in_flight),
                        prefix + "-in-flight");
    return frame;
}

void destroyFrameContext(helios::vk::Device& device, FrameContext& frame) {
    if (frame.pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device.handle(), frame.pool, nullptr);
        frame.pool = VK_NULL_HANDLE;
    }
    if (frame.image_available != VK_NULL_HANDLE) {
        vkDestroySemaphore(device.handle(), frame.image_available, nullptr);
        frame.image_available = VK_NULL_HANDLE;
    }
    if (frame.in_flight != VK_NULL_HANDLE) {
        vkDestroyFence(device.handle(), frame.in_flight, nullptr);
        frame.in_flight = VK_NULL_HANDLE;
    }
}

} // namespace helios::render
