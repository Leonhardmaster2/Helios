// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "helios/vk/swapchain.hpp"

#include "helios/core/log.hpp"

#include <algorithm>
#include <volk.h>

using namespace helios::core;

namespace helios::vk {

namespace {

VkSurfaceFormatKHR pickFormat(Device& device, VkSurfaceKHR surface) {
    std::uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device.physicalDevice(), surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(device.physicalDevice(), surface, &count, formats.data());
    for (auto const& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return f;
        }
    }
    for (auto const& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM) {
            return f;
        }
    }
    return formats.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
                           : formats.front();
}

VkPresentModeKHR pickPresentMode(Device& device, VkSurfaceKHR surface) {
    std::uint32_t count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(device.physicalDevice(), surface, &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(device.physicalDevice(), surface, &count, modes.data());
    if (std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_MAILBOX_KHR) != modes.end()) {
        return VK_PRESENT_MODE_MAILBOX_KHR;
    }
    return VK_PRESENT_MODE_FIFO_KHR; // universally required
}

} // namespace

core::Result<std::unique_ptr<Swapchain>> Swapchain::create(Device& device, VkSurfaceKHR surface,
                                                           std::uint32_t width, std::uint32_t height) {
    auto swapchain = std::unique_ptr<Swapchain>(new Swapchain());
    swapchain->device_ = &device;
    swapchain->surface_ = surface;
    auto result = swapchain->createInternal(width, height);
    if (result.failed()) {
        return result.error();
    }
    return swapchain;
}

core::Result<void> Swapchain::createInternal(std::uint32_t width, std::uint32_t height) {
    VkSurfaceCapabilitiesKHR caps{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device_->physicalDevice(), surface_, &caps) != VK_SUCCESS) {
        return core::makeError(core::ErrorCode::Vulkan, "surface capabilities query failed");
    }
    const VkSurfaceFormatKHR format = pickFormat(*device_, surface_);
    const VkPresentModeKHR present_mode = pickPresentMode(*device_, surface_);

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFU || extent.height == 0xFFFFFFFFU) {
        extent.width = std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }

    std::uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0) {
        image_count = std::min(image_count, caps.maxImageCount);
    }

    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = surface_;
    info.minImageCount = image_count;
    info.imageFormat = format.format;
    info.imageColorSpace = format.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = present_mode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = swapchain_;

    VkSwapchainKHR new_swapchain = VK_NULL_HANDLE;
    const VkResult r = vkCreateSwapchainKHR(device_->handle(), &info, nullptr, &new_swapchain);
    if (r != VK_SUCCESS) {
        return core::makeError(core::ErrorCode::Vulkan,
                               "vkCreateSwapchainKHR failed: " + std::to_string(static_cast<int>(r)));
    }

    // Destroy old swapchain after successful creation.
    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_->handle(), swapchain_, nullptr);
    }
    swapchain_ = new_swapchain;

    std::uint32_t count = 0;
    vkGetSwapchainImagesKHR(device_->handle(), swapchain_, &count, nullptr);
    images_.resize(count);
    vkGetSwapchainImagesKHR(device_->handle(), swapchain_, &count, images_.data());

    info_.format = format.format;
    info_.color_space = format.colorSpace;
    info_.present_mode = present_mode;
    info_.extent = extent;
    info_.image_count = count;

    core::logInfo("Swapchain: {}x{}, format {}, present mode {}, images {}", extent.width, extent.height,
                  std::to_string(static_cast<int>(format.format)), std::to_string(static_cast<int>(present_mode)),
                  count);
    return Result<void>{};
}

core::Result<std::uint32_t> Swapchain::acquireNextImage(VkSemaphore signal_semaphore, VkFence fence) {
    std::uint32_t index = 0;
    const VkResult r = vkAcquireNextImageKHR(device_->handle(), swapchain_, UINT64_MAX, signal_semaphore, fence,
                                             &index);
    if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
        return index;
    }
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        return core::makeError(core::ErrorCode::Vulkan, "swapchain out of date");
    }
    return core::makeError(core::ErrorCode::Vulkan,
                           "vkAcquireNextImageKHR failed: " + std::to_string(static_cast<int>(r)));
}

core::Result<VkResult> Swapchain::present(std::uint32_t image_index, VkSemaphore wait_semaphore) {
    VkPresentInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &wait_semaphore;
    info.swapchainCount = 1;
    info.pSwapchains = &swapchain_;
    info.pImageIndices = &image_index;
    const VkResult r = vkQueuePresentKHR(device_->graphicsQueue(), &info);
    return r;
}

core::Result<void> Swapchain::recreate(std::uint32_t width, std::uint32_t height) {
    vkDeviceWaitIdle(device_->handle());
    return createInternal(width, height);
}

void Swapchain::destroy() {
    if (swapchain_ != VK_NULL_HANDLE && device_ != nullptr) {
        vkDestroySwapchainKHR(device_->handle(), swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

Swapchain::~Swapchain() {
    destroy();
}

} // namespace helios::vk
