// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "helios/vk/buffer.hpp"

#include "helios/core/debug_name.hpp"

#include <cstring>

namespace helios::vk {

core::Result<Buffer> Buffer::create(Device& device, VkDeviceSize size, VkBufferUsageFlags usage,
                                    bool host_visible, std::string_view debug_name) {
    Buffer buffer;
    buffer.allocator_ = device.allocator();
    buffer.size_ = size;

    const VkBuffer handle = createBuffer(
        device.allocator(), size, usage,
        host_visible ? VMA_MEMORY_USAGE_AUTO_PREFER_HOST : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        host_visible ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                     : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        buffer.buffer_, buffer.allocation_);
    if (handle == VK_NULL_HANDLE) {
        return core::makeError(core::ErrorCode::Vulkan, "buffer allocation failed: " + std::string(debug_name));
    }
    device.setDebugName(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<std::uint64_t>(buffer.buffer_), debug_name);
    return buffer;
}

Buffer::Buffer(Buffer&& other) noexcept {
    *this = std::move(other);
}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
    if (this != &other) {
        destroy();
        allocator_ = other.allocator_;
        buffer_ = other.buffer_;
        allocation_ = other.allocation_;
        size_ = other.size_;
        mapped_ = other.mapped_;
        other.allocator_ = VK_NULL_HANDLE;
        other.buffer_ = VK_NULL_HANDLE;
        other.allocation_ = VK_NULL_HANDLE;
        other.size_ = 0;
        other.mapped_ = nullptr;
    }
    return *this;
}

void Buffer::destroy() {
    if (buffer_ != VK_NULL_HANDLE) {
        if (mapped_ != nullptr) {
            vmaUnmapMemory(allocator_, allocation_);
            mapped_ = nullptr;
        }
        vmaDestroyBuffer(allocator_, buffer_, allocation_);
        buffer_ = VK_NULL_HANDLE;
        allocation_ = VK_NULL_HANDLE;
    }
}

Buffer::~Buffer() {
    destroy();
}

core::Result<void> Buffer::upload(void const* data, VkDeviceSize size, VkDeviceSize offset) {
    if (mapped_ == nullptr) {
        void* ptr = nullptr;
        const VkResult r = vmaMapMemory(allocator_, allocation_, &ptr);
        if (r != VK_SUCCESS) {
            return core::makeError(core::ErrorCode::Vulkan,
                                   "vmaMapMemory failed: " + std::to_string(static_cast<int>(r)));
        }
        mapped_ = ptr;
    }
    if (offset + size > size_) {
        return core::makeError(core::ErrorCode::OutOfRange, "buffer upload out of range");
    }
    std::memcpy(static_cast<std::uint8_t*>(mapped_) + offset, data, static_cast<std::size_t>(size));
    return Result<void>{};
}

core::Result<void*> Buffer::map() {
    if (mapped_ == nullptr) {
        void* ptr = nullptr;
        const VkResult r = vmaMapMemory(allocator_, allocation_, &ptr);
        if (r != VK_SUCCESS) {
            return core::makeError(core::ErrorCode::Vulkan,
                                   "vmaMapMemory failed: " + std::to_string(static_cast<int>(r)));
        }
        mapped_ = ptr;
    }
    return mapped_;
}

void Buffer::unmap() {
    if (mapped_ != nullptr) {
        vmaUnmapMemory(allocator_, allocation_);
        mapped_ = nullptr;
    }
}

} // namespace helios::vk
