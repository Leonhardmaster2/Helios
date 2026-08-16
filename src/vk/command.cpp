// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "helios/vk/command.hpp"

#include "helios/core/log.hpp"

#include <volk.h>

namespace helios::vk {

core::Result<void> runOneShot(Device& device, std::function<void(VkCommandBuffer)> const& record) {
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = device.graphicsQueueFamily();
    VkCommandPool pool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(device.handle(), &pool_info, nullptr, &pool) != VK_SUCCESS) {
        return core::makeError(core::ErrorCode::Vulkan, "vkCreateCommandPool failed (one-shot)");
    }

    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device.handle(), &alloc, &cmd) != VK_SUCCESS) {
        vkDestroyCommandPool(device.handle(), pool, nullptr);
        return core::makeError(core::ErrorCode::Vulkan, "vkAllocateCommandBuffers failed (one-shot)");
    }

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    record(cmd);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    const VkResult sr = vkQueueSubmit(device.graphicsQueue(), 1, &submit, VK_NULL_HANDLE);
    if (sr != VK_SUCCESS) {
        vkDestroyCommandPool(device.handle(), pool, nullptr);
        return core::makeError(core::ErrorCode::Vulkan,
                               "vkQueueSubmit failed (one-shot): " + std::to_string(static_cast<int>(sr)));
    }
    vkQueueWaitIdle(device.graphicsQueue());

    vkDestroyCommandPool(device.handle(), pool, nullptr);
    return Result<void>{};
}

} // namespace helios::vk
