// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Offscreen rendering target for headless smoke paths: color + depth
// images with transfer-readback support (no swapchain required).
#pragma once

#include "helios/core/error.hpp"
#include "helios/vk/device.hpp"
#include "helios/vk/image.hpp"

#include <cstdint>
#include <memory>
#include <vector>

#include <vulkan/vulkan.h>

namespace helios::vk::smoke {

struct SmokeTargetSpec {
    std::uint32_t width = 640;
    std::uint32_t height = 480;
    VkFormat color_format = VK_FORMAT_R8G8B8A8_UNORM;
    VkFormat depth_format = VK_FORMAT_D32_SFLOAT;
    VkImageUsageFlags color_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImageUsageFlags depth_usage =
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
};

// Offscreen color+depth target with readback. Owns its images.
class SmokeContext {
  public:
    ~SmokeContext();
    SmokeContext(SmokeContext const&) = delete;
    SmokeContext& operator=(SmokeContext const&) = delete;

    [[nodiscard]] static core::Result<std::unique_ptr<SmokeContext>> create(Device& device,
                                                                            SmokeTargetSpec const& spec);

    [[nodiscard]] VkImageView colorView() const noexcept {
        return color_view_;
    }
    [[nodiscard]] VkImageView depthView() const noexcept {
        return depth_view_;
    }
    [[nodiscard]] VkExtent2D extent() const noexcept {
        return extent_;
    }
    [[nodiscard]] VkFormat colorFormat() const noexcept {
        return color_format_;
    }
    [[nodiscard]] VkFormat depthFormat() const noexcept {
        return depth_format_;
    }
    [[nodiscard]] Image& colorImage() noexcept {
        return *color_;
    }
    [[nodiscard]] Image& depthImage() noexcept {
        return *depth_;
    }

    // Clears the color attachment to (0,0,0,0) via transfer.
    [[nodiscard]] core::Result<void> clearColor(std::array<float, 4> const& value) const;

  private:
    SmokeContext() = default;
    void destroy();

    Device* device_ = nullptr;
    VkExtent2D extent_{};
    VkFormat color_format_ = VK_FORMAT_UNDEFINED;
    VkFormat depth_format_ = VK_FORMAT_UNDEFINED;
    std::unique_ptr<Image> color_;
    std::unique_ptr<Image> depth_;
    VkImageView color_view_ = VK_NULL_HANDLE;
    VkImageView depth_view_ = VK_NULL_HANDLE;
};

} // namespace helios::vk::smoke
