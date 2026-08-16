// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Windowed smoke scene: shared with the headless suite, but rendered into
// swapchain images for interactive/visual validation.
#include "helios/vk/smoke/smoke_suite.hpp"

#include "helios/core/log.hpp"
#include "helios/vk/buffer.hpp"
#include "helios/vk/descriptor_set.hpp"
#include "helios/vk/pipeline.hpp"
#include "helios/vk/shader_module.hpp"
#include "shaders/abi.hpp"
#include "shaders_embedded.hpp"

#include <array>
#include <filesystem>
#include <vector>

#include <volk.h>

namespace fs = std::filesystem;

namespace helios::vk::smoke {

struct WindowedSmokeScene::Impl {
    ShaderModule triangle_vs;
    ShaderModule triangle_fs;
    ShaderModule depth_vs;
    ShaderModule depth_fs;
    DescriptorSetLayout ubo_layout;
    PipelineLayout triangle_layout;
    Pipeline triangle_pipeline;
    PipelineLayout depth_layout;
    Pipeline depth_pipeline;
    DescriptorPool pool;
    Buffer triangle_ubo;
    Buffer depth_ubo;
    VkDescriptorSet triangle_set = VK_NULL_HANDLE;
    VkDescriptorSet depth_set = VK_NULL_HANDLE;
};

core::Result<std::unique_ptr<WindowedSmokeScene>> WindowedSmokeScene::create(Device& device,
                                                                             VkFormat color_format,
                                                                             VkFormat depth_format) {
    auto scene = std::unique_ptr<WindowedSmokeScene>(new WindowedSmokeScene());
    auto impl = std::make_unique<Impl>();
    scene->device_ = &device;

    auto loadEmbedded = [&](std::string_view name, std::string_view dbg) -> core::Result<ShaderModule> {
        for (std::size_t i = 0; i < helios::shaders::embedded::kShaderCount; ++i) {
            auto const& s = helios::shaders::embedded::kShaders[i];
            if (std::string_view(s.name) == name) {
                return ShaderModule::createFromSpirv(device, s.data, s.size, dbg);
            }
        }
        return core::makeError(core::ErrorCode::NotFound, "embedded shader not found: " + std::string(name));
    };

    auto tri_vs = loadEmbedded("triangle.slang.vertex.spv", "win-triangle-vs");
    auto tri_fs = loadEmbedded("triangle.slang.fragment.spv", "win-triangle-fs");
    auto dep_vs = loadEmbedded("depth.slang.vertex.spv", "win-depth-vs");
    auto dep_fs = loadEmbedded("depth.slang.fragment.spv", "win-depth-fs");
    if (tri_vs.failed() || tri_fs.failed() || dep_vs.failed() || dep_fs.failed()) {
        return core::makeError(core::ErrorCode::NotFound, "embedded shader data missing");
    }
    impl->triangle_vs = std::move(tri_vs.value());
    impl->triangle_fs = std::move(tri_fs.value());
    impl->depth_vs = std::move(dep_vs.value());
    impl->depth_fs = std::move(dep_fs.value());

    auto ubo_layout = DescriptorSetLayout::create(
        device, {{{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr}}},
        "win-ubo-layout");
    if (ubo_layout.failed()) return ubo_layout.error();
    impl->ubo_layout = std::move(ubo_layout.value());

    auto tri_layout = PipelineLayout::create(device, {impl->ubo_layout.handle()}, {}, "win-triangle-layout");
    if (tri_layout.failed()) return tri_layout.error();
    impl->triangle_layout = std::move(tri_layout.value());

    GraphicsPipelineDescription tri_desc;
    tri_desc.vertex = impl->triangle_vs.handle();
    tri_desc.fragment = impl->triangle_fs.handle();
    tri_desc.layout = impl->triangle_layout.handle();
    tri_desc.color_format = color_format;
    tri_desc.depth_format = depth_format;
    tri_desc.enable_depth_test = true;
    tri_desc.enable_depth_write = false;
    tri_desc.debug_name = "win-triangle";
    auto tri_pipeline = Pipeline::create(device, tri_desc);
    if (tri_pipeline.failed()) return tri_pipeline.error();
    impl->triangle_pipeline = std::move(tri_pipeline.value());

    auto depth_layout = PipelineLayout::create(device, {impl->ubo_layout.handle()}, {}, "win-depth-layout");
    if (depth_layout.failed()) return depth_layout.error();
    impl->depth_layout = std::move(depth_layout.value());

    GraphicsPipelineDescription depth_desc;
    depth_desc.vertex = impl->depth_vs.handle();
    depth_desc.fragment = impl->depth_fs.handle();
    depth_desc.layout = impl->depth_layout.handle();
    depth_desc.color_format = color_format;
    depth_desc.depth_format = depth_format;
    depth_desc.enable_depth_test = true;
    depth_desc.enable_depth_write = true;
    depth_desc.depth_compare = VK_COMPARE_OP_GREATER;
    depth_desc.cull_mode = VK_CULL_MODE_NONE;
    depth_desc.debug_name = "win-depth";
    auto depth_pipeline = Pipeline::create(device, depth_desc);
    if (depth_pipeline.failed()) return depth_pipeline.error();
    impl->depth_pipeline = std::move(depth_pipeline.value());

    auto pool = DescriptorPool::create(device, {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 8}}, 8, "win-pool");
    if (pool.failed()) return pool.error();
    impl->pool = std::move(pool.value());

    auto tri_set = impl->pool.allocate(impl->ubo_layout.handle());
    auto dep_set = impl->pool.allocate(impl->ubo_layout.handle());
    if (tri_set.failed() || dep_set.failed()) {
        return core::makeError(core::ErrorCode::Vulkan, "descriptor allocation failed");
    }
    impl->triangle_set = tri_set.value();
    impl->depth_set = dep_set.value();

    auto tri_ubo = Buffer::create(device, sizeof(shaders::TriangleUbo), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true,
                                  "win-triangle-ubo");
    auto dep_ubo = Buffer::create(device, sizeof(shaders::DepthParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true,
                                  "win-depth-ubo");
    if (tri_ubo.failed() || dep_ubo.failed()) {
        return core::makeError(core::ErrorCode::Vulkan, "ubo allocation failed");
    }
    impl->triangle_ubo = std::move(tri_ubo.value());
    impl->depth_ubo = std::move(dep_ubo.value());

    shaders::TriangleUbo tri_data{};
    tri_data.mvp[0] = 1.0f;
    tri_data.mvp[5] = 1.0f;
    tri_data.mvp[10] = 1.0f;
    tri_data.mvp[15] = 1.0f;
    impl->triangle_ubo.upload(&tri_data, sizeof(tri_data));

    shaders::DepthParams dep_data{};
    dep_data.quads[0] = {-0.55f, 0.0f, 0.15f, 0.22f};
    dep_data.quads[1] = {0.0f, 0.0f, 0.5f, 0.22f};
    dep_data.quads[2] = {0.55f, 0.0f, 0.9f, 0.22f};
    dep_data.colors[0] = {1.0f, 0.2f, 0.2f, 1.0f};
    dep_data.colors[1] = {0.2f, 1.0f, 0.2f, 1.0f};
    dep_data.colors[2] = {0.2f, 0.2f, 1.0f, 1.0f};
    impl->depth_ubo.upload(&dep_data, sizeof(dep_data));

    VkDescriptorBufferInfo tri_info{impl->triangle_ubo.handle(), 0, sizeof(shaders::TriangleUbo)};
    VkWriteDescriptorSet tri_write{};
    tri_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    tri_write.dstSet = impl->triangle_set;
    tri_write.dstBinding = 0;
    tri_write.descriptorCount = 1;
    tri_write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    tri_write.pBufferInfo = &tri_info;
    vkUpdateDescriptorSets(device.handle(), 1, &tri_write, 0, nullptr);

    VkDescriptorBufferInfo dep_info{impl->depth_ubo.handle(), 0, sizeof(shaders::DepthParams)};
    VkWriteDescriptorSet dep_write{};
    dep_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    dep_write.dstSet = impl->depth_set;
    dep_write.dstBinding = 0;
    dep_write.descriptorCount = 1;
    dep_write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    dep_write.pBufferInfo = &dep_info;
    vkUpdateDescriptorSets(device.handle(), 1, &dep_write, 0, nullptr);

    scene->impl_ = std::move(impl);
    return scene;
}

void WindowedSmokeScene::record(VkCommandBuffer cmd, VkExtent2D extent, std::uint32_t /*frame_index*/) {
    // NOTE: the caller (helios_render::Renderer) owns the active dynamic
    // rendering pass (color + depth attachments); this only draws.
    auto& impl = *impl_;

    VkViewport viewport{0, 0, static_cast<float>(extent.width), static_cast<float>(extent.height), 0, 1};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{{0, 0}, extent};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Triangle (upper area).
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, impl.triangle_pipeline.handle());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, impl.triangle_layout.handle(), 0, 1,
                            &impl.triangle_set, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);

    // Depth probes (lower area).
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, impl.depth_pipeline.handle());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, impl.depth_layout.handle(), 0, 1,
                            &impl.depth_set, 0, nullptr);
    vkCmdDraw(cmd, 6, 3, 0, 0);
}

void WindowedSmokeScene::destroy() {
    impl_.reset();
    device_ = nullptr;
}

WindowedSmokeScene::~WindowedSmokeScene() {
    destroy();
}

} // namespace helios::vk::smoke
