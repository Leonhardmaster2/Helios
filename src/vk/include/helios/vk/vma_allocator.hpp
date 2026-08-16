// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

namespace helios::vk {

// VMA allocation helpers used across helios_vk. The allocator itself is
// owned by Device; these functions only wrap allocation calls.
[[nodiscard]] VkBuffer createBuffer(VmaAllocator allocator, VkDeviceSize size, VkBufferUsageFlags usage,
                                    VmaMemoryUsage memory_usage, VkMemoryPropertyFlags required_flags,
                                    VkBuffer& out_buffer, VmaAllocation& out_allocation,
                                    VmaAllocationInfo* out_info = nullptr);

[[nodiscard]] VkImage createImage(VmaAllocator allocator, VkExtent3D extent, VkFormat format,
                                  VkImageUsageFlags usage, VmaMemoryUsage memory_usage,
                                  VkImage& out_image, VmaAllocation& out_allocation);

void destroyBuffer(VmaAllocator allocator, VkBuffer buffer, VmaAllocation allocation);
void destroyImage(VmaAllocator allocator, VkImage image, VmaAllocation allocation);

} // namespace helios::vk
