// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Descriptor set layout + pool helpers with queried limits. M0 uses a
// simple UBO/SSBO scheme; bindless/update-after-bind arrives with M3/M4.
#pragma once

#include "helios/core/error.hpp"
#include "helios/vk/device.hpp"

#include <vulkan/vulkan.h>

namespace helios::vk {

class DescriptorSetLayout {
  public:
    DescriptorSetLayout() = default;
    [[nodiscard]] static core::Result<DescriptorSetLayout>
    create(Device& device, std::vector<VkDescriptorSetLayoutBinding> const& bindings,
           std::string_view debug_name);
    DescriptorSetLayout(DescriptorSetLayout&& other) noexcept;
    DescriptorSetLayout& operator=(DescriptorSetLayout&& other) noexcept;
    DescriptorSetLayout(DescriptorSetLayout const&) = delete;
    DescriptorSetLayout& operator=(DescriptorSetLayout const&) = delete;
    ~DescriptorSetLayout();

    [[nodiscard]] VkDescriptorSetLayout handle() const noexcept {
        return layout_;
    }

  private:
    void destroy();
    Device* device_ = nullptr;
    VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
};

class DescriptorPool {
  public:
    DescriptorPool() = default;
    [[nodiscard]] static core::Result<DescriptorPool> create(Device& device,
                                                             std::vector<VkDescriptorPoolSize> const& sizes,
                                                             std::uint32_t max_sets,
                                                             std::string_view debug_name);
    DescriptorPool(DescriptorPool&& other) noexcept;
    DescriptorPool& operator=(DescriptorPool&& other) noexcept;
    DescriptorPool(DescriptorPool const&) = delete;
    DescriptorPool& operator=(DescriptorPool const&) = delete;
    ~DescriptorPool();

    [[nodiscard]] VkDescriptorPool handle() const noexcept {
        return pool_;
    }

    // Allocates one set (fails when the pool is exhausted).
    [[nodiscard]] core::Result<VkDescriptorSet> allocate(VkDescriptorSetLayout layout) const;

  private:
    void destroy();
    Device* device_ = nullptr;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
};

// Pipeline layout (set layouts + push-constant range).
class PipelineLayout {
  public:
    PipelineLayout() = default;
    [[nodiscard]] static core::Result<PipelineLayout>
    create(Device& device, std::vector<VkDescriptorSetLayout> const& set_layouts,
           std::vector<VkPushConstantRange> const& push_constants, std::string_view debug_name);
    PipelineLayout(PipelineLayout&& other) noexcept;
    PipelineLayout& operator=(PipelineLayout&& other) noexcept;
    PipelineLayout(PipelineLayout const&) = delete;
    PipelineLayout& operator=(PipelineLayout const&) = delete;
    ~PipelineLayout();

    [[nodiscard]] VkPipelineLayout handle() const noexcept {
        return layout_;
    }

  private:
    void destroy();
    Device* device_ = nullptr;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
};

} // namespace helios::vk
