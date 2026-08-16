// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "helios/vk/sync.hpp"

#include "helios/core/log.hpp"

#include <volk.h>

namespace helios::vk {

core::Result<Fence> Fence::create(Device& device, bool signaled, std::string_view debug_name) {
    VkFenceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    info.flags = signaled ? VK_FENCE_CREATE_SIGNALED_BIT : 0;
    VkFence fence = VK_NULL_HANDLE;
    const VkResult r = vkCreateFence(device.handle(), &info, nullptr, &fence);
    if (r != VK_SUCCESS) {
        return core::makeError(core::ErrorCode::Vulkan,
                               "vkCreateFence failed: " + std::to_string(static_cast<int>(r)));
    }
    device.setDebugName(VK_OBJECT_TYPE_FENCE, reinterpret_cast<std::uint64_t>(fence), debug_name);
    Fence out;
    out.device_ = &device;
    out.fence_ = fence;
    return out;
}

Fence::Fence(Fence&& other) noexcept {
    *this = std::move(other);
}

Fence& Fence::operator=(Fence&& other) noexcept {
    if (this != &other) {
        destroy();
        device_ = other.device_;
        fence_ = other.fence_;
        other.device_ = nullptr;
        other.fence_ = VK_NULL_HANDLE;
    }
    return *this;
}

void Fence::destroy() {
    if (fence_ != VK_NULL_HANDLE && device_ != nullptr) {
        vkDestroyFence(device_->handle(), fence_, nullptr);
        fence_ = VK_NULL_HANDLE;
    }
}

Fence::~Fence() {
    destroy();
}

core::Result<void> Fence::wait(std::uint64_t timeout_ns) const {
    const VkResult r = vkWaitForFences(device_->handle(), 1, &fence_, VK_TRUE, timeout_ns);
    if (r != VK_SUCCESS) {
        return core::makeError(core::ErrorCode::Vulkan,
                               "vkWaitForFences failed: " + std::to_string(static_cast<int>(r)));
    }
    return Result<void>{};
}

core::Result<void> Fence::reset() {
    const VkResult r = vkResetFences(device_->handle(), 1, &fence_);
    if (r != VK_SUCCESS) {
        return core::makeError(core::ErrorCode::Vulkan,
                               "vkResetFences failed: " + std::to_string(static_cast<int>(r)));
    }
    return Result<void>{};
}

core::Result<Semaphore> Semaphore::create(Device& device, std::string_view debug_name) {
    VkSemaphoreCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    const VkResult r = vkCreateSemaphore(device.handle(), &info, nullptr, &semaphore);
    if (r != VK_SUCCESS) {
        return core::makeError(core::ErrorCode::Vulkan,
                               "vkCreateSemaphore failed: " + std::to_string(static_cast<int>(r)));
    }
    device.setDebugName(VK_OBJECT_TYPE_SEMAPHORE, reinterpret_cast<std::uint64_t>(semaphore), debug_name);
    Semaphore out;
    out.device_ = &device;
    out.semaphore_ = semaphore;
    return out;
}

Semaphore::Semaphore(Semaphore&& other) noexcept {
    *this = std::move(other);
}

Semaphore& Semaphore::operator=(Semaphore&& other) noexcept {
    if (this != &other) {
        destroy();
        device_ = other.device_;
        semaphore_ = other.semaphore_;
        other.device_ = nullptr;
        other.semaphore_ = VK_NULL_HANDLE;
    }
    return *this;
}

void Semaphore::destroy() {
    if (semaphore_ != VK_NULL_HANDLE && device_ != nullptr) {
        vkDestroySemaphore(device_->handle(), semaphore_, nullptr);
        semaphore_ = VK_NULL_HANDLE;
    }
}

Semaphore::~Semaphore() {
    destroy();
}

core::Result<TimelineSemaphore> TimelineSemaphore::create(Device& device, std::string_view debug_name) {
    VkSemaphoreTypeCreateInfo type_info{};
    type_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    type_info.initialValue = 0;
    VkSemaphoreCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    info.pNext = &type_info;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    const VkResult r = vkCreateSemaphore(device.handle(), &info, nullptr, &semaphore);
    if (r != VK_SUCCESS) {
        return core::makeError(core::ErrorCode::Vulkan,
                               "vkCreateSemaphore failed: " + std::to_string(static_cast<int>(r)));
    }
    device.setDebugName(VK_OBJECT_TYPE_SEMAPHORE, reinterpret_cast<std::uint64_t>(semaphore), debug_name);
    TimelineSemaphore out;
    out.device_ = &device;
    out.semaphore_ = semaphore;
    return out;
}

TimelineSemaphore::TimelineSemaphore(TimelineSemaphore&& other) noexcept {
    *this = std::move(other);
}

TimelineSemaphore& TimelineSemaphore::operator=(TimelineSemaphore&& other) noexcept {
    if (this != &other) {
        destroy();
        device_ = other.device_;
        semaphore_ = other.semaphore_;
        other.device_ = nullptr;
        other.semaphore_ = VK_NULL_HANDLE;
    }
    return *this;
}

void TimelineSemaphore::destroy() {
    if (semaphore_ != VK_NULL_HANDLE && device_ != nullptr) {
        vkDestroySemaphore(device_->handle(), semaphore_, nullptr);
        semaphore_ = VK_NULL_HANDLE;
    }
}

TimelineSemaphore::~TimelineSemaphore() {
    destroy();
}

core::Result<std::uint64_t> TimelineSemaphore::value() const {
    std::uint64_t value = 0;
    const VkResult r = vkGetSemaphoreCounterValue(device_->handle(), semaphore_, &value);
    if (r != VK_SUCCESS) {
        return core::makeError(core::ErrorCode::Vulkan,
                               "vkGetSemaphoreCounterValue failed: " + std::to_string(static_cast<int>(r)));
    }
    return value;
}

core::Result<void> TimelineSemaphore::signalGraphics(std::uint64_t value) {
    VkSemaphoreSubmitInfo signal{};
    signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signal.semaphore = semaphore_;
    signal.value = value;
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signal;
    const VkResult r = vkQueueSubmit2(device_->graphicsQueue(), 1, &submit, VK_NULL_HANDLE);
    if (r != VK_SUCCESS) {
        return core::makeError(core::ErrorCode::Vulkan,
                               "vkQueueSubmit2 failed: " + std::to_string(static_cast<int>(r)));
    }
    return Result<void>{};
}

core::Result<QueryPool> QueryPool::create(Device& device, VkQueryType type, std::uint32_t count,
                                          std::string_view debug_name) {
    VkQueryPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    info.queryType = type;
    info.queryCount = count;
    VkQueryPool pool = VK_NULL_HANDLE;
    const VkResult r = vkCreateQueryPool(device.handle(), &info, nullptr, &pool);
    if (r != VK_SUCCESS) {
        return core::makeError(core::ErrorCode::Vulkan,
                               "vkCreateQueryPool failed: " + std::to_string(static_cast<int>(r)));
    }
    device.setDebugName(VK_OBJECT_TYPE_QUERY_POOL, reinterpret_cast<std::uint64_t>(pool), debug_name);
    QueryPool out;
    out.device_ = &device;
    out.pool_ = pool;
    return out;
}

QueryPool::QueryPool(QueryPool&& other) noexcept {
    *this = std::move(other);
}

QueryPool& QueryPool::operator=(QueryPool&& other) noexcept {
    if (this != &other) {
        destroy();
        device_ = other.device_;
        pool_ = other.pool_;
        other.device_ = nullptr;
        other.pool_ = VK_NULL_HANDLE;
    }
    return *this;
}

void QueryPool::destroy() {
    if (pool_ != VK_NULL_HANDLE && device_ != nullptr) {
        vkDestroyQueryPool(device_->handle(), pool_, nullptr);
        pool_ = VK_NULL_HANDLE;
    }
}

QueryPool::~QueryPool() {
    destroy();
}

core::Result<void> QueryPool::reset(std::uint32_t first, std::uint32_t count) const {
    const VkResult r = vkResetQueryPool(device_->handle(), pool_, first, count);
    if (r != VK_SUCCESS) {
        return core::makeError(core::ErrorCode::Vulkan,
                               "vkResetQueryPool failed: " + std::to_string(static_cast<int>(r)));
    }
    return Result<void>{};
}

core::Result<void> QueryPool::getResults(std::uint32_t first, std::uint32_t count,
                                         std::vector<std::uint64_t>& out) const {
    out.resize(count);
    const VkResult r = vkGetQueryPoolResults(device_->handle(), pool_, first, count, out.size() * sizeof(out[0]),
                                             out.data(), sizeof(out[0]), VK_QUERY_RESULT_64_BIT);
    if (r != VK_SUCCESS) {
        return core::makeError(core::ErrorCode::Vulkan,
                               "vkGetQueryPoolResults failed: " + std::to_string(static_cast<int>(r)));
    }
    return Result<void>{};
}

} // namespace helios::vk
