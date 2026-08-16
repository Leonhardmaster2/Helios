// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "helios/vk/smoke/smoke_context.hpp"

#include "helios/core/log.hpp"

#include <volk.h>

namespace helios::vk::smoke {

namespace {
core::Result<VkImageView> createView(Device& device, VkImage image, VkFormat format, VkImageAspectFlags aspect,
                                     std::string_view name) {
    VkImageViewCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = format;
    info.subresourceRange = {aspect, 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    const VkResult r = vkCreateImageView(device.handle(), &info, nullptr, &view);
    if (r != VK_SUCCESS) {
        return core::makeError(core::ErrorCode::Vulkan,
                               "vkCreateImageView failed: " + std::to_string(static_cast<int>(r)));
    }
    device.setDebugName(VK_OBJECT_TYPE_IMAGE_VIEW, reinterpret_cast<std::uint64_t>(view), name);
    return view;
}
} // namespace

core::Result<std::unique_ptr<SmokeContext>> SmokeContext::create(Device& device, SmokeTargetSpec const& spec) {
    auto ctx = std::unique_ptr<SmokeContext>(new SmokeContext());
    ctx->device_ = &device;
    ctx->extent_ = {spec.width, spec.height};
    ctx->color_format_ = spec.color_format;
    ctx->depth_format_ = spec.depth_format;

    auto color = Image::create(device, ctx->extent_, spec.color_format, spec.color_usage, false, "smoke-color");
    if (color.failed()) return color.error();
    ctx->color_ = std::make_unique<Image>(std::move(color.value()));

    auto depth = Image::create(device, ctx->extent_, spec.depth_format, spec.depth_usage, false, "smoke-depth");
    if (depth.failed()) return depth.error();
    ctx->depth_ = std::make_unique<Image>(std::move(depth.value()));

    auto color_view = createView(device, ctx->color_->handle(), spec.color_format, VK_IMAGE_ASPECT_COLOR_BIT,
                                 "smoke-color-view");
    if (color_view.failed()) return color_view.error();
    ctx->color_view_ = color_view.value();

    auto depth_view = createView(device, ctx->depth_->handle(), spec.depth_format, VK_IMAGE_ASPECT_DEPTH_BIT,
                                 "smoke-depth-view");
    if (depth_view.failed()) return depth_view.error();
    ctx->depth_view_ = depth_view.value();

    return ctx;
}

void SmokeContext::destroy() {
    if (device_ == nullptr) return;
    if (color_view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_->handle(), color_view_, nullptr);
        color_view_ = VK_NULL_HANDLE;
    }
    if (depth_view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_->handle(), depth_view_, nullptr);
        depth_view_ = VK_NULL_HANDLE;
    }
    color_.reset();
    depth_.reset();
    device_ = nullptr;
}

SmokeContext::~SmokeContext() {
    destroy();
}

core::Result<void> SmokeContext::clearColor(std::array<float, 4> const& value) const {
    return runOneShot(*device_, [&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        barrier.srcAccessMask = 0;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = color_->handle();
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dep);

        VkClearColorValue clear{};
        clear.float32[0] = value[0];
        clear.float32[1] = value[1];
        clear.float32[2] = value[2];
        clear.float32[3] = value[3];
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, color_->handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);

        VkImageMemoryBarrier2 barrier2{};
        barrier2.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier2.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barrier2.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier2.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier2.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
        barrier2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier2.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier2.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier2.image = color_->handle();
        barrier2.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep2{};
        dep2.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep2.imageMemoryBarrierCount = 1;
        dep2.pImageMemoryBarriers = &barrier2;
        vkCmdPipelineBarrier2(cmd, &dep2);
    });
}

} // namespace helios::vk::smoke
