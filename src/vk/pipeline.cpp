// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "helios/vk/pipeline.hpp"

#include "helios/core/io.hpp"
#include "helios/core/log.hpp"

#include <volk.h>

namespace helios::vk {

core::Result<Pipeline> Pipeline::create(Device& device, GraphicsPipelineDescription const& desc) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = desc.vertex;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = desc.fragment;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = desc.topology;

    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.depthClampEnable = VK_FALSE;
    raster.rasterizerDiscardEnable = VK_FALSE;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = desc.cull_mode;
    raster.frontFace = desc.front_face;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = desc.enable_depth_test ? VK_TRUE : VK_FALSE;
    depth.depthWriteEnable = desc.enable_depth_write ? VK_TRUE : VK_FALSE;
    depth.depthCompareOp = desc.depth_compare;
    depth.depthBoundsTestEnable = VK_FALSE;
    depth.stencilTestEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = 0xF;
    blend_attachment.blendEnable = VK_FALSE;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;

    VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;

    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &desc.color_format;
    rendering.depthAttachmentFormat = desc.depth_format;

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext = &rendering;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &input_assembly;
    info.pViewportState = &viewport_state;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = desc.layout;

    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult r = vkCreateGraphicsPipelines(device.handle(), VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
    if (r != VK_SUCCESS) {
        return core::makeError(core::ErrorCode::Vulkan,
                               "vkCreateGraphicsPipelines failed: " + std::to_string(static_cast<int>(r)));
    }
    device.setDebugName(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<std::uint64_t>(pipeline), desc.debug_name);

    Pipeline out;
    out.device_ = &device;
    out.pipeline_ = pipeline;
    return out;
}

Pipeline::Pipeline(Pipeline&& other) noexcept {
    *this = std::move(other);
}

Pipeline& Pipeline::operator=(Pipeline&& other) noexcept {
    if (this != &other) {
        destroy();
        device_ = other.device_;
        pipeline_ = other.pipeline_;
        other.device_ = nullptr;
        other.pipeline_ = VK_NULL_HANDLE;
    }
    return *this;
}

void Pipeline::destroy() {
    if (pipeline_ != VK_NULL_HANDLE && device_ != nullptr) {
        vkDestroyPipeline(device_->handle(), pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
}

Pipeline::~Pipeline() {
    destroy();
}

core::Result<ComputePipeline> ComputePipeline::create(Device& device, VkShaderModule shader,
                                                      VkPipelineLayout layout, std::string_view debug_name) {
    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = shader;
    stage.pName = "main";

    VkComputePipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    info.stage = stage;
    info.layout = layout;

    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult r = vkCreateComputePipelines(device.handle(), VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
    if (r != VK_SUCCESS) {
        return core::makeError(core::ErrorCode::Vulkan,
                               "vkCreateComputePipelines failed: " + std::to_string(static_cast<int>(r)));
    }
    device.setDebugName(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<std::uint64_t>(pipeline), debug_name);

    ComputePipeline out;
    out.device_ = &device;
    out.pipeline_ = pipeline;
    return out;
}

ComputePipeline::ComputePipeline(ComputePipeline&& other) noexcept {
    *this = std::move(other);
}

ComputePipeline& ComputePipeline::operator=(ComputePipeline&& other) noexcept {
    if (this != &other) {
        destroy();
        device_ = other.device_;
        pipeline_ = other.pipeline_;
        other.device_ = nullptr;
        other.pipeline_ = VK_NULL_HANDLE;
    }
    return *this;
}

void ComputePipeline::destroy() {
    if (pipeline_ != VK_NULL_HANDLE && device_ != nullptr) {
        vkDestroyPipeline(device_->handle(), pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
}

ComputePipeline::~ComputePipeline() {
    destroy();
}

core::Result<PipelineCache> PipelineCache::create(Device& device, std::string_view cache_file) {
    PipelineCache out;
    out.device_ = &device;

    std::vector<std::uint8_t> initial;
    if (!cache_file.empty() && core::fileExists(cache_file)) {
        auto data = core::readBinaryFile(cache_file, 256U * 1024U * 1024U);
        if (data.ok() && data.value().size() >= 16) {
            // Validate the cache header magic; discard otherwise.
            std::uint32_t header[4];
            std::memcpy(header, data.value().data(), 16);
            const bool valid = header[0] == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
                               header[1] == device.report().vendor_id && header[2] == device.report().device_id;
            if (valid) {
                initial = std::move(data.value());
            } else {
                core::logWarn("pipeline cache file rejected (wrong device/driver): {}", cache_file);
            }
        }
    }

    VkPipelineCacheCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    info.initialDataSize = initial.size();
    info.pInitialData = initial.empty() ? nullptr : initial.data();
    const VkResult r = vkCreatePipelineCache(device.handle(), &info, nullptr, &out.cache_);
    if (r != VK_SUCCESS) {
        return core::makeError(core::ErrorCode::Vulkan,
                               "vkCreatePipelineCache failed: " + std::to_string(static_cast<int>(r)));
    }
    device.setDebugName(VK_OBJECT_TYPE_PIPELINE_CACHE, reinterpret_cast<std::uint64_t>(out.cache_),
                        "pipeline-cache");
    return out;
}

PipelineCache::PipelineCache(PipelineCache&& other) noexcept {
    *this = std::move(other);
}

PipelineCache& PipelineCache::operator=(PipelineCache&& other) noexcept {
    if (this != &other) {
        destroy();
        device_ = other.device_;
        cache_ = other.cache_;
        other.device_ = nullptr;
        other.cache_ = VK_NULL_HANDLE;
    }
    return *this;
}

void PipelineCache::destroy() {
    if (cache_ != VK_NULL_HANDLE && device_ != nullptr) {
        vkDestroyPipelineCache(device_->handle(), cache_, nullptr);
        cache_ = VK_NULL_HANDLE;
    }
}

PipelineCache::~PipelineCache() {
    destroy();
}

void PipelineCache::save(std::string_view cache_file) const {
    if (cache_ == VK_NULL_HANDLE || cache_file.empty()) {
        return;
    }
    std::size_t size = 0;
    vkGetPipelineCacheData(device_->handle(), cache_, &size, nullptr);
    if (size == 0) {
        return;
    }
    std::vector<std::uint8_t> data(size);
    if (vkGetPipelineCacheData(device_->handle(), cache_, &size, data.data()) == VK_SUCCESS) {
        if (core::writeFileAtomic(cache_file, data.data(), data.size()).failed()) {
            core::logWarn("failed to persist pipeline cache to {}", cache_file);
        }
    }
}

} // namespace helios::vk
