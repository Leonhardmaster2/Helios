// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// RAII GPU image (VMA-backed) with optional host-visible staging readback.
#pragma once

#include "helios/core/error.hpp"
#include "helios/vk/device.hpp"

#include <cstdint>
#include <vector>

#include <vk_mem_alloc.h>

namespace helios::vk {

class Image {
  public:
    Image() = default;
    [[nodiscard]] static core::Result<Image> create(Device& device, VkExtent2D extent, VkFormat format,
                                                    VkImageUsageFlags usage, bool host_visible,
                                                    std::string_view debug_name);
    Image(Image&& other) noexcept;
    Image& operator=(Image&& other) noexcept;
    Image(Image const&) = delete;
    Image& operator=(Image const&) = delete;
    ~Image();

    [[nodiscard]] VkImage handle() const noexcept {
        return image_;
    }
    [[nodiscard]] VkExtent2D extent() const noexcept {
        return extent_;
    }
    [[nodiscard]] VkFormat format() const noexcept {
        return format_;
    }
    [[nodiscard]] bool valid() const noexcept {
        return image_ != VK_NULL_HANDLE;
    }

    // Reads back raw bytes via a staging buffer (device-local + host image).
    // Requires VK_IMAGE_USAGE_TRANSFER_SRC_BIT. Blocks until transfer done.
    [[nodiscard]] core::Result<std::vector<std::uint8_t>> readback(Device& device) const;

    // Copies host bytes into the image via a staging buffer.
    // Requires VK_IMAGE_USAGE_TRANSFER_DST_BIT. Blocks until transfer done.
    [[nodiscard]] core::Result<void> uploadFromHost(Device& device, void const* data, std::size_t size) const;

  private:
    void destroy();

    Device* device_ = nullptr;
    VkImage image_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    VkExtent2D extent_{};
    VkFormat format_ = VK_FORMAT_UNDEFINED;
};

} // namespace helios::vk
