// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
// VMA_IMPLEMENTATION must be defined before any vk_mem_alloc.h include
// in this TU; the header is not include-guarded. With volk (function
// pointers, VK_NO_PROTOTYPES) VMA must use dynamic function loading and
// see volk.h before vk_mem_alloc.h (VMA documentation, "Usage with volk").
#define VMA_IMPLEMENTATION
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#include <volk.h>
#include "helios/vk/vma_allocator.hpp"

#include "helios/core/log.hpp"

#include <volk.h>

namespace helios::vk {

VkBuffer createBuffer(VmaAllocator allocator, VkDeviceSize size, VkBufferUsageFlags usage,
                      VmaMemoryUsage memory_usage, VkMemoryPropertyFlags required_flags, VkBuffer& out_buffer,
                      VmaAllocation& out_allocation, VmaAllocationInfo* out_info) {
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo alloc{};
    alloc.usage = memory_usage;
    alloc.requiredFlags = required_flags;

    VkBuffer buffer = VK_NULL_HANDLE;
    const VkResult r = vmaCreateBuffer(allocator, &info, &alloc, &buffer, &out_allocation, out_info);
    if (r != VK_SUCCESS) {
        core::logError("vmaCreateBuffer failed: {}", static_cast<int>(r));
        return VK_NULL_HANDLE;
    }
    out_buffer = buffer;
    return buffer;
}

VkImage createImage(VmaAllocator allocator, VkExtent3D extent, VkFormat format, VkImageUsageFlags usage,
                    VmaMemoryUsage memory_usage, VkImage& out_image, VmaAllocation& out_allocation) {
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.extent = extent;
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.format = format;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    info.usage = usage;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo alloc{};
    alloc.usage = memory_usage;

    VkImage image = VK_NULL_HANDLE;
    const VkResult r = vmaCreateImage(allocator, &info, &alloc, &image, &out_allocation, nullptr);
    if (r != VK_SUCCESS) {
        core::logError("vmaCreateImage failed: {}", static_cast<int>(r));
        return VK_NULL_HANDLE;
    }
    out_image = image;
    return image;
}

void destroyBuffer(VmaAllocator allocator, VkBuffer buffer, VmaAllocation allocation) {
    if (buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(allocator, buffer, allocation);
    }
}

void destroyImage(VmaAllocator allocator, VkImage image, VmaAllocation allocation) {
    if (image != VK_NULL_HANDLE) {
        vmaDestroyImage(allocator, image, allocation);
    }
}

} // namespace helios::vk
