// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Synchronization primitives (RAII fences, semaphores, query pools).
// Binary semaphores are quarantined inside WSI; timeline semaphores are
// used where the profile supports them (docs/ARCHITECTURE.md).
#pragma once

#include "helios/core/error.hpp"
#include "helios/vk/device.hpp"

#include <vulkan/vulkan.h>

namespace helios::vk {

class Fence {
public:
    Fence() = default;
    [[nodiscard]] static core::Result<Fence> create(Device& device, bool signaled, std::string_view debug_name);
    Fence(Fence&& other) noexcept;
    Fence& operator=(Fence&& other) noexcept;
    Fence(Fence const&) = delete;
    Fence& operator=(Fence const&) = delete;
    ~Fence();

    [[nodiscard]] VkFence handle() const noexcept { return fence_; }
    [[nodiscard]] core::Result<void> wait(std::uint64_t timeout_ns) const;
    [[nodiscard]] core::Result<void> reset();

private:
    void destroy();
    Device* device_ = nullptr;
    VkFence fence_ = VK_NULL_HANDLE;
};

class Semaphore {
public:
    Semaphore() = default;
    [[nodiscard]] static core::Result<Semaphore> create(Device& device, std::string_view debug_name);
    Semaphore(Semaphore&& other) noexcept;
    Semaphore& operator=(Semaphore&& other) noexcept;
    Semaphore(Semaphore const&) = delete;
    Semaphore& operator=(Semaphore const&) = delete;
    ~Semaphore();

    [[nodiscard]] VkSemaphore handle() const noexcept { return semaphore_; }

private:
    void destroy();
    Device* device_ = nullptr;
    VkSemaphore semaphore_ = VK_NULL_HANDLE;
};

class TimelineSemaphore {
public:
    TimelineSemaphore() = default;
    [[nodiscard]] static core::Result<TimelineSemaphore> create(Device& device, std::string_view debug_name);
    TimelineSemaphore(TimelineSemaphore&& other) noexcept;
    TimelineSemaphore& operator=(TimelineSemaphore&& other) noexcept;
    TimelineSemaphore(TimelineSemaphore const&) = delete;
    TimelineSemaphore& operator=(TimelineSemaphore const&) = delete;
    ~TimelineSemaphore();

    [[nodiscard]] VkSemaphore handle() const noexcept { return semaphore_; }
    [[nodiscard]] core::Result<std::uint64_t> value() const;
    // Signals on the graphics queue (wait: none).
    [[nodiscard]] core::Result<void> signalGraphics(std::uint64_t value);

private:
    void destroy();
    Device* device_ = nullptr;
    VkSemaphore semaphore_ = VK_NULL_HANDLE;
};

class QueryPool {
public:
    QueryPool() = default;
    [[nodiscard]] static core::Result<QueryPool> create(Device& device, VkQueryType type,
                                                        std::uint32_t count, std::string_view debug_name);
    QueryPool(QueryPool&& other) noexcept;
    QueryPool& operator=(QueryPool&& other) noexcept;
    QueryPool(QueryPool const&) = delete;
    QueryPool& operator=(QueryPool const&) = delete;
    ~QueryPool();

    [[nodiscard]] VkQueryPool handle() const noexcept { return pool_; }
    [[nodiscard]] core::Result<void> reset(std::uint32_t first, std::uint32_t count) const;
    [[nodiscard]] core::Result<void> getResults(std::uint32_t first, std::uint32_t count,
                                                std::vector<std::uint64_t>& out) const;

private:
    void destroy();
    Device* device_ = nullptr;
    VkQueryPool pool_ = VK_NULL_HANDLE;
};

} // namespace helios::vk
