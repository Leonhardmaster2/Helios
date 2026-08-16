// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Pipeline creation helpers (dynamic rendering; no legacy render passes).
#pragma once

#include "helios/core/error.hpp"
#include "helios/vk/device.hpp"

#include <vector>

#include <vulkan/vulkan.h>

namespace helios::vk {

struct GraphicsPipelineDescription {
    VkShaderModule vertex = VK_NULL_HANDLE;
    VkShaderModule fragment = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkFormat color_format = VK_FORMAT_UNDEFINED;
    VkFormat depth_format = VK_FORMAT_UNDEFINED;
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkCompareOp depth_compare = VK_COMPARE_OP_GREATER; // reversed-Z (D-002)
    bool enable_depth_test = false;
    bool enable_depth_write = false;
    bool enable_cull = true;
    VkCullModeFlags cull_mode = VK_CULL_MODE_BACK_BIT;
    VkFrontFace front_face = VK_FRONT_FACE_CLOCKWISE;
    std::string debug_name;
};

class Pipeline {
public:
    Pipeline() = default;
    [[nodiscard]] static core::Result<Pipeline> create(Device& device, GraphicsPipelineDescription const& desc);
    Pipeline(Pipeline&& other) noexcept;
    Pipeline& operator=(Pipeline&& other) noexcept;
    Pipeline(Pipeline const&) = delete;
    Pipeline& operator=(Pipeline const&) = delete;
    ~Pipeline();

    [[nodiscard]] VkPipeline handle() const noexcept { return pipeline_; }
    [[nodiscard]] bool valid() const noexcept { return pipeline_ != VK_NULL_HANDLE; }

private:
    void destroy();
    Device* device_ = nullptr;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};

class ComputePipeline {
public:
    ComputePipeline() = default;
    [[nodiscard]] static core::Result<ComputePipeline> create(Device& device, VkShaderModule shader,
                                                              VkPipelineLayout layout,
                                                              std::string_view debug_name);
    ComputePipeline(ComputePipeline&& other) noexcept;
    ComputePipeline& operator=(ComputePipeline&& other) noexcept;
    ComputePipeline(ComputePipeline const&) = delete;
    ComputePipeline& operator=(ComputePipeline const&) = delete;
    ~ComputePipeline();

    [[nodiscard]] VkPipeline handle() const noexcept { return pipeline_; }

private:
    void destroy();
    Device* device_ = nullptr;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};

// Persistent pipeline cache (device/driver scoped; invalidated by Vulkan IDs).
class PipelineCache {
public:
    PipelineCache() = default;
    [[nodiscard]] static core::Result<PipelineCache> create(Device& device, std::string_view cache_file);
    PipelineCache(PipelineCache&& other) noexcept;
    PipelineCache& operator=(PipelineCache&& other) noexcept;
    PipelineCache(PipelineCache const&) = delete;
    PipelineCache& operator=(PipelineCache const&) = delete;
    ~PipelineCache();

    [[nodiscard]] VkPipelineCache handle() const noexcept { return cache_; }
    // Persists the cache to disk (best effort; failures are logged, not fatal).
    void save(std::string_view cache_file) const;

private:
    void destroy();
    Device* device_ = nullptr;
    VkPipelineCache cache_ = VK_NULL_HANDLE;
};

} // namespace helios::vk
