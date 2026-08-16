// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Swapchain + WSI synchronization, quarantined inside helios_vk
// (docs/ARCHITECTURE.md). Binary semaphores are used for WSI only.
#pragma once

#include "helios/core/error.hpp"
#include "helios/vk/device.hpp"

#include <cstdint>
#include <memory>
#include <vector>

#include <vulkan/vulkan.h>

namespace helios::vk {

struct SwapchainInfo {
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    VkExtent2D extent{};
    std::uint32_t image_count = 0;
};

class Swapchain {
public:
    ~Swapchain();
    Swapchain(Swapchain const&) = delete;
    Swapchain& operator=(Swapchain const&) = delete;

    [[nodiscard]] static core::Result<std::unique_ptr<Swapchain>> create(
        Device& device, VkSurfaceKHR surface, std::uint32_t width, std::uint32_t height);

    // Acquires the next image (blocks on the acquire fence).
    // Returns VK_SUCCESS, VK_SUBOPTIMAL_KHR or VK_ERROR_OUT_OF_DATE_KHR.
    [[nodiscard]] core::Result<std::uint32_t> acquireNextImage(VkSemaphore signal_semaphore, VkFence fence);

    // Presents the image at index with the given wait semaphores.
    [[nodiscard]] core::Result<VkResult> present(std::uint32_t image_index, VkSemaphore wait_semaphore);

    // Recreates the swapchain (after resize or OUT_OF_DATE). Images change.
    [[nodiscard]] core::Result<void> recreate(std::uint32_t width, std::uint32_t height);

    [[nodiscard]] SwapchainInfo const& info() const noexcept { return info_; }
    [[nodiscard]] VkSwapchainKHR handle() const noexcept { return swapchain_; }
    [[nodiscard]] VkImage image(std::uint32_t index) const noexcept { return images_[index]; }
    [[nodiscard]] std::vector<VkImage> const& images() const noexcept { return images_; }
    [[nodiscard]] VkSurfaceKHR surface() const noexcept { return surface_; }

private:
    Swapchain() = default;
    [[nodiscard]] core::Result<void> createInternal(std::uint32_t width, std::uint32_t height);

    Device* device_ = nullptr;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE; // owned by caller (app surface)
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    std::vector<VkImage> images_;
    SwapchainInfo info_;
};

} // namespace helios::vk
