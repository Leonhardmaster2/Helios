// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// RAII GPU buffer (VMA-backed).
#pragma once

#include "helios/core/error.hpp"
#include "helios/vk/device.hpp"

#include <cstddef>
#include <cstdint>

#include <vk_mem_alloc.h>

namespace helios::vk {

class Buffer {
  public:
    Buffer() = default;
    // Allocates a buffer with the given usage/flags. When host_visible is
    // set, the buffer is mappable (VMA_MEMORY_USAGE_AUTO_PREFER_HOST).
    [[nodiscard]] static core::Result<Buffer> create(Device& device, VkDeviceSize size,
                                                     VkBufferUsageFlags usage, bool host_visible,
                                                     std::string_view debug_name);
    Buffer(Buffer&& other) noexcept;
    Buffer& operator=(Buffer&& other) noexcept;
    Buffer(Buffer const&) = delete;
    Buffer& operator=(Buffer const&) = delete;
    ~Buffer();

    [[nodiscard]] VkBuffer handle() const noexcept {
        return buffer_;
    }
    [[nodiscard]] VkDeviceSize size() const noexcept {
        return size_;
    }
    [[nodiscard]] bool valid() const noexcept {
        return buffer_ != VK_NULL_HANDLE;
    }

    // Maps and copies data into the buffer (host-visible only).
    [[nodiscard]] core::Result<void> upload(void const* data, VkDeviceSize size, VkDeviceSize offset = 0);

    // Maps the buffer and returns a pointer (host-visible only).
    [[nodiscard]] core::Result<void*> map();
    void unmap();

  private:
    void destroy();

    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    VkDeviceSize size_ = 0;
    void* mapped_ = nullptr;
};

} // namespace helios::vk
