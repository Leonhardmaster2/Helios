// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "helios/vk/image.hpp"

#include "helios/vk/buffer.hpp"
#include "helios/vk/vma_allocator.hpp"
#include "helios/vk/command.hpp"

#include <volk.h>

#include <cstring>

namespace helios::vk {

namespace {
VkImageAspectFlags aspectForFormat(VkFormat format) {
    switch (format) {
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    default:
        return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}
} // namespace

core::Result<Image> Image::create(Device& device, VkExtent2D extent, VkFormat format, VkImageUsageFlags usage,
                                  bool host_visible, std::string_view debug_name) {
    Image image;
    image.device_ = &device;
    image.extent_ = extent;
    image.format_ = format;

    VkImage handle = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    handle = createImage(device.allocator(), VkExtent3D{extent.width, extent.height, 1}, format, usage,
                         host_visible ? VMA_MEMORY_USAGE_AUTO_PREFER_HOST : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                         handle, allocation);
    if (handle == VK_NULL_HANDLE) {
        return core::makeError(core::ErrorCode::Vulkan, "image allocation failed: " + std::string(debug_name));
    }
    image.image_ = handle;
    image.allocation_ = allocation;
    device.setDebugName(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<std::uint64_t>(image.image_), debug_name);
    return image;
}

Image::Image(Image&& other) noexcept {
    *this = std::move(other);
}

Image& Image::operator=(Image&& other) noexcept {
    if (this != &other) {
        destroy();
        device_ = other.device_;
        image_ = other.image_;
        allocation_ = other.allocation_;
        extent_ = other.extent_;
        format_ = other.format_;
        other.device_ = nullptr;
        other.image_ = VK_NULL_HANDLE;
        other.allocation_ = VK_NULL_HANDLE;
    }
    return *this;
}

void Image::destroy() {
    if (image_ != VK_NULL_HANDLE && device_ != nullptr) {
        vmaDestroyImage(device_->allocator(), image_, allocation_);
        image_ = VK_NULL_HANDLE;
        allocation_ = VK_NULL_HANDLE;
    }
}

Image::~Image() {
    destroy();
}

core::Result<std::vector<std::uint8_t>> Image::readback(Device& device) const {
    const VkDeviceSize size = static_cast<VkDeviceSize>(extent_.width) * extent_.height * 4;
    auto staging = Buffer::create(device, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, "readback-staging");
    if (staging.failed()) {
        return staging.error();
    }
    const VkImageAspectFlags aspect = aspectForFormat(format_);

    const auto result = runOneShot(device, [&](VkCommandBuffer cmd) {
        // Image -> transfer source.
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image_;
        barrier.subresourceRange = {aspect, 0, 1, 0, 1};
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dep);

        VkBufferImageCopy region{};
        region.imageSubresource = {aspect, 0, 0, 1};
        region.imageExtent = {extent_.width, extent_.height, 1};
        vkCmdCopyImageToBuffer(cmd, image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.value().handle(), 1,
                               &region);

        VkBufferMemoryBarrier2 buf_barrier{};
        buf_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        buf_barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        buf_barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        buf_barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        buf_barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        buf_barrier.buffer = staging.value().handle();
        buf_barrier.offset = 0;
        buf_barrier.size = size;
        VkDependencyInfo dep2{};
        dep2.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep2.bufferMemoryBarrierCount = 1;
        dep2.pBufferMemoryBarriers = &buf_barrier;
        vkCmdPipelineBarrier2(cmd, &dep2);
    });
    if (result.failed()) {
        return result.error();
    }

    auto mapped = staging.value().map();
    if (mapped.failed()) {
        return mapped.error();
    }
    std::vector<std::uint8_t> out(static_cast<std::size_t>(size));
    std::memcpy(out.data(), mapped.value(), out.size());
    staging.value().unmap();
    return out;
}

core::Result<void> Image::uploadFromHost(Device& device, void const* data, std::size_t size) const {
    const VkDeviceSize expected = static_cast<VkDeviceSize>(extent_.width) * extent_.height * 4;
    if (size != expected) {
        return core::makeError(core::ErrorCode::InvalidArgument, "image upload size mismatch");
    }
    auto staging = Buffer::create(device, expected, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, "upload-staging");
    if (staging.failed()) {
        return staging.error();
    }
    auto upload_result = staging.value().upload(data, expected, 0);
    if (upload_result.failed()) {
        return upload_result.error();
    }

    const VkImageAspectFlags aspect = aspectForFormat(format_);
    return runOneShot(device, [&](VkCommandBuffer cmd) {
        VkBufferMemoryBarrier2 buf_barrier{};
        buf_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        buf_barrier.srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        buf_barrier.srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT;
        buf_barrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        buf_barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        buf_barrier.buffer = staging.value().handle();
        buf_barrier.offset = 0;
        buf_barrier.size = expected;
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.bufferMemoryBarrierCount = 1;
        dep.pBufferMemoryBarriers = &buf_barrier;
        vkCmdPipelineBarrier2(cmd, &dep);

        VkImageMemoryBarrier2 img_barrier{};
        img_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        img_barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        img_barrier.srcAccessMask = 0;
        img_barrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        img_barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        img_barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        img_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        img_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        img_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        img_barrier.image = image_;
        img_barrier.subresourceRange = {aspect, 0, 1, 0, 1};
        VkDependencyInfo dep2{};
        dep2.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep2.imageMemoryBarrierCount = 1;
        dep2.pImageMemoryBarriers = &img_barrier;
        vkCmdPipelineBarrier2(cmd, &dep2);

        VkBufferImageCopy region{};
        region.imageSubresource = {aspect, 0, 0, 1};
        region.imageExtent = {extent_.width, extent_.height, 1};
        vkCmdCopyBufferToImage(cmd, staging.value().handle(), image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &region);
    });
}

} // namespace helios::vk
