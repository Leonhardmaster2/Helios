// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "helios/vk/descriptor_set.hpp"

#include <volk.h>

namespace helios::vk {

core::Result<DescriptorSetLayout> DescriptorSetLayout::create(
    Device& device, std::vector<VkDescriptorSetLayoutBinding> const& bindings, std::string_view debug_name) {
    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = static_cast<std::uint32_t>(bindings.size());
    info.pBindings = bindings.data();

    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    const VkResult r = vkCreateDescriptorSetLayout(device.handle(), &info, nullptr, &layout);
    if (r != VK_SUCCESS) {
        return core::makeError(core::ErrorCode::Vulkan,
                               "vkCreateDescriptorSetLayout failed: " + std::to_string(static_cast<int>(r)));
    }
    device.setDebugName(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, reinterpret_cast<std::uint64_t>(layout), debug_name);

    DescriptorSetLayout out;
    out.device_ = &device;
    out.layout_ = layout;
    return out;
}

DescriptorSetLayout::DescriptorSetLayout(DescriptorSetLayout&& other) noexcept {
    *this = std::move(other);
}

DescriptorSetLayout& DescriptorSetLayout::operator=(DescriptorSetLayout&& other) noexcept {
    if (this != &other) {
        destroy();
        device_ = other.device_;
        layout_ = other.layout_;
        other.device_ = nullptr;
        other.layout_ = VK_NULL_HANDLE;
    }
    return *this;
}

void DescriptorSetLayout::destroy() {
    if (layout_ != VK_NULL_HANDLE && device_ != nullptr) {
        vkDestroyDescriptorSetLayout(device_->handle(), layout_, nullptr);
        layout_ = VK_NULL_HANDLE;
    }
}

DescriptorSetLayout::~DescriptorSetLayout() {
    destroy();
}

core::Result<DescriptorPool> DescriptorPool::create(Device& device, std::vector<VkDescriptorPoolSize> const& sizes,
                                                    std::uint32_t max_sets, std::string_view debug_name) {
    VkDescriptorPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    info.maxSets = max_sets;
    info.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
    info.pPoolSizes = sizes.data();

    VkDescriptorPool pool = VK_NULL_HANDLE;
    const VkResult r = vkCreateDescriptorPool(device.handle(), &info, nullptr, &pool);
    if (r != VK_SUCCESS) {
        return core::makeError(core::ErrorCode::Vulkan,
                               "vkCreateDescriptorPool failed: " + std::to_string(static_cast<int>(r)));
    }
    device.setDebugName(VK_OBJECT_TYPE_DESCRIPTOR_POOL, reinterpret_cast<std::uint64_t>(pool), debug_name);

    DescriptorPool out;
    out.device_ = &device;
    out.pool_ = pool;
    return out;
}

DescriptorPool::DescriptorPool(DescriptorPool&& other) noexcept {
    *this = std::move(other);
}

DescriptorPool& DescriptorPool::operator=(DescriptorPool&& other) noexcept {
    if (this != &other) {
        destroy();
        device_ = other.device_;
        pool_ = other.pool_;
        other.device_ = nullptr;
        other.pool_ = VK_NULL_HANDLE;
    }
    return *this;
}

void DescriptorPool::destroy() {
    if (pool_ != VK_NULL_HANDLE && device_ != nullptr) {
        vkDestroyDescriptorPool(device_->handle(), pool_, nullptr);
        pool_ = VK_NULL_HANDLE;
    }
}

DescriptorPool::~DescriptorPool() {
    destroy();
}

core::Result<VkDescriptorSet> DescriptorPool::allocate(VkDescriptorSetLayout layout) const {
    VkDescriptorSetAllocateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    info.descriptorPool = pool_;
    info.descriptorSetCount = 1;
    info.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    const VkResult r = vkAllocateDescriptorSets(device_->handle(), &info, &set);
    if (r != VK_SUCCESS) {
        return core::makeError(core::ErrorCode::Vulkan,
                               "vkAllocateDescriptorSets failed: " + std::to_string(static_cast<int>(r)));
    }
    return set;
}

core::Result<PipelineLayout> PipelineLayout::create(Device& device,
                                                    std::vector<VkDescriptorSetLayout> const& set_layouts,
                                                    std::vector<VkPushConstantRange> const& push_constants,
                                                    std::string_view debug_name) {
    VkPipelineLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.setLayoutCount = static_cast<std::uint32_t>(set_layouts.size());
    info.pSetLayouts = set_layouts.data();
    info.pushConstantRangeCount = static_cast<std::uint32_t>(push_constants.size());
    info.pPushConstantRanges = push_constants.data();

    VkPipelineLayout layout = VK_NULL_HANDLE;
    const VkResult r = vkCreatePipelineLayout(device.handle(), &info, nullptr, &layout);
    if (r != VK_SUCCESS) {
        return core::makeError(core::ErrorCode::Vulkan,
                               "vkCreatePipelineLayout failed: " + std::to_string(static_cast<int>(r)));
    }
    device.setDebugName(VK_OBJECT_TYPE_PIPELINE_LAYOUT, reinterpret_cast<std::uint64_t>(layout), debug_name);

    PipelineLayout out;
    out.device_ = &device;
    out.layout_ = layout;
    return out;
}

PipelineLayout::PipelineLayout(PipelineLayout&& other) noexcept {
    *this = std::move(other);
}

PipelineLayout& PipelineLayout::operator=(PipelineLayout&& other) noexcept {
    if (this != &other) {
        destroy();
        device_ = other.device_;
        layout_ = other.layout_;
        other.device_ = nullptr;
        other.layout_ = VK_NULL_HANDLE;
    }
    return *this;
}

void PipelineLayout::destroy() {
    if (layout_ != VK_NULL_HANDLE && device_ != nullptr) {
        vkDestroyPipelineLayout(device_->handle(), layout_, nullptr);
        layout_ = VK_NULL_HANDLE;
    }
}

PipelineLayout::~PipelineLayout() {
    destroy();
}

} // namespace helios::vk
