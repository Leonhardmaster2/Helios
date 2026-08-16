// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// M0 smoke suite implementation. Each path:
//   1. records deterministic work (graphics/compute/transfer),
//   2. submits and waits (serialized baseline — no async overlap yet),
//   3. reads back results,
//   4. verifies against CPU-computed expectations,
//   5. reports pass/fail with measurements.
#include "helios/vk/smoke/smoke_suite.hpp"

#include "helios/core/log.hpp"
#include "helios/core/timer.hpp"
#include "helios/vk/buffer.hpp"
#include "helios/vk/command.hpp"
#include "helios/vk/descriptor_set.hpp"
#include "helios/vk/pipeline.hpp"
#include "helios/vk/shader_module.hpp"
#include "helios/vk/sync.hpp"
#include "shaders/abi.hpp"
#include "shaders_embedded.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>

#include <volk.h>

namespace fs = std::filesystem;

namespace helios::vk::smoke {

namespace {

// Shader directory: <exe dir>/shaders or HELIOS_SHADER_DIR (configure-time).
std::string shaderDir() {
    static const std::string dir = [] {
        std::error_code ec;
        const fs::path exe_dir = fs::absolute(fs::path(".")).lexically_normal();
        // Try executable-adjacent first, then compile-time baked path.
        for (auto const& candidate : {exe_dir / "shaders", fs::path(HELIOS_SHADER_DIR)}) {
            if (fs::exists(candidate / "triangle.spv", ec)) {
                return candidate.string();
            }
        }
        return std::string(HELIOS_SHADER_DIR);
    }();
    return dir;
}

core::Result<ShaderModule> loadShader(Device& device, std::string_view name) {
    for (std::size_t i = 0; i < helios::shaders::embedded::kShaderCount; ++i) {
        auto const& s = helios::shaders::embedded::kShaders[i];
        if (std::string_view(s.name).size() == std::string_view(name).size() + 4 &&
            std::string_view(s.name).substr(0, std::string_view(name).size()) == name) {
            return ShaderModule::createFromSpirv(device, s.data, s.size, name);
        }
    }
    return core::makeError(core::ErrorCode::NotFound, "embedded shader not found: " + std::string(name));
}

// Records an image transition to/from a renderable layout.
void transitionImage(VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout, VkImageLayout new_layout,
                     VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                     VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access, VkImageAspectFlags aspect) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = src_stage;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {aspect, 0, 1, 0, 1};
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
}

// Renders the depth probes into the given color+depth attachments.
void recordDepthProbes(Device& device, VkCommandBuffer cmd, VkPipeline pipeline, VkPipelineLayout layout,
                       VkDescriptorSet set, VkExtent2D extent, VkImageView color_view, VkImageView depth_view) {
    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = color_view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{0.1f, 0.1f, 0.15f, 1.0f}};

    VkRenderingAttachmentInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth.imageView = depth_view;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.clearValue.depthStencil = {0.0f, 0}; // reversed-Z: clear = far

    VkRenderingInfo render{};
    render.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    render.renderArea = {{0, 0}, extent};
    render.layerCount = 1;
    render.colorAttachmentCount = 1;
    render.pColorAttachments = &color;
    render.pDepthAttachment = &depth;

    vkCmdBeginRendering(cmd, &render);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height),
                        0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{{0, 0}, extent};
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
    vkCmdDraw(cmd, 6, 4, 0, 0); // 4 depth probes (instances 0-3)
    vkCmdEndRendering(cmd);
}

} // namespace

// ---------------------------------------------------------------------------
// HeadlessSmoke
// ---------------------------------------------------------------------------

struct HeadlessSmoke::Shared {
    // triangle path
    ShaderModule triangle_vs;
    ShaderModule triangle_fs;
    DescriptorSetLayout ubo_layout;
    PipelineLayout triangle_layout;
    Pipeline triangle_pipeline;
    // depth path
    ShaderModule depth_vs;
    ShaderModule depth_fs;
    PipelineLayout depth_layout;
    Pipeline depth_pipeline;
    // compute path
    ShaderModule compute_cs;
    DescriptorSetLayout ssbo_layout;
    PipelineLayout compute_layout;
    ComputePipeline compute_pipeline;
    // pools
    DescriptorPool pool;

    ~Shared();
};

HeadlessSmoke::Shared::~Shared() = default;

core::Result<std::unique_ptr<HeadlessSmoke>> HeadlessSmoke::create(Instance& /*instance*/, Device& device,
                                                                   SmokeOptions const& options) {
    auto smoke = std::unique_ptr<HeadlessSmoke>(new HeadlessSmoke());
    smoke->device_ = &device;
    smoke->options_ = options;

    core::logInfo("smoke: create context");
    SmokeTargetSpec spec;
    spec.width = options.width;
    spec.height = options.height;
    spec.depth_format = device.report().depth_choice == DepthFormatChoice::D32Sfloat ? VK_FORMAT_D32_SFLOAT
                        : device.report().depth_choice == DepthFormatChoice::D32SfloatS8
                            ? VK_FORMAT_D32_SFLOAT_S8_UINT
                            : VK_FORMAT_D24_UNORM_S8_UINT;
    auto context = SmokeContext::create(device, spec);
    if (context.failed()) {
        return context.error();
    }
    smoke->context_ = std::move(context.value());

    auto shared = std::make_unique<Shared>();
    core::logInfo("smoke: load shaders");

    // --- triangle path ---
    auto tri_vs = loadShader(device, "triangle.slang.vertex");
    if (tri_vs.failed())
        return tri_vs.error();
    shared->triangle_vs = std::move(tri_vs.value());
    auto tri_fs = loadShader(device, "triangle.slang.fragment");
    if (tri_fs.failed())
        return tri_fs.error();
    shared->triangle_fs = std::move(tri_fs.value());

    // Used by vertex AND fragment stages (triangle fs, depth fs read params).
    auto ubo_layout = DescriptorSetLayout::create(
        device, {{{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}}},
        "smoke-ubo-layout");
    if (ubo_layout.failed())
        return ubo_layout.error();
    shared->ubo_layout = std::move(ubo_layout.value());

    auto tri_layout =
        PipelineLayout::create(device, {shared->ubo_layout.handle()}, {}, "smoke-triangle-layout");
    if (tri_layout.failed())
        return tri_layout.error();
    shared->triangle_layout = std::move(tri_layout.value());

    GraphicsPipelineDescription tri_desc;
    tri_desc.vertex = shared->triangle_vs.handle();
    tri_desc.fragment = shared->triangle_fs.handle();
    tri_desc.layout = shared->triangle_layout.handle();
    tri_desc.color_format = smoke->context_->colorFormat();
    tri_desc.depth_format = VK_FORMAT_UNDEFINED; // no depth attachment in this pass
    tri_desc.enable_depth_test = false;
    tri_desc.enable_depth_write = false;
    tri_desc.cull_mode = VK_CULL_MODE_NONE; // winding-independent for the smoke
    tri_desc.debug_name = "smoke-triangle";
    auto tri_pipeline = Pipeline::create(device, tri_desc);
    if (tri_pipeline.failed())
        return tri_pipeline.error();
    shared->triangle_pipeline = std::move(tri_pipeline.value());

    // --- depth path ---
    auto depth_vs = loadShader(device, "depth.slang.vertex");
    if (depth_vs.failed())
        return depth_vs.error();
    shared->depth_vs = std::move(depth_vs.value());
    auto depth_fs = loadShader(device, "depth.slang.fragment");
    if (depth_fs.failed())
        return depth_fs.error();
    shared->depth_fs = std::move(depth_fs.value());

    auto depth_layout =
        PipelineLayout::create(device, {shared->ubo_layout.handle()}, {}, "smoke-depth-layout");
    if (depth_layout.failed())
        return depth_layout.error();
    shared->depth_layout = std::move(depth_layout.value());

    GraphicsPipelineDescription depth_desc;
    depth_desc.vertex = shared->depth_vs.handle();
    depth_desc.fragment = shared->depth_fs.handle();
    depth_desc.layout = shared->depth_layout.handle();
    depth_desc.color_format = smoke->context_->colorFormat();
    depth_desc.depth_format = smoke->context_->depthFormat();
    depth_desc.enable_depth_test = true;
    depth_desc.enable_depth_write = true;
    depth_desc.depth_compare = VK_COMPARE_OP_GREATER; // reversed-Z
    depth_desc.cull_mode = VK_CULL_MODE_NONE;
    depth_desc.debug_name = "smoke-depth";
    auto depth_pipeline = Pipeline::create(device, depth_desc);
    if (depth_pipeline.failed())
        return depth_pipeline.error();
    shared->depth_pipeline = std::move(depth_pipeline.value());

    // --- compute path ---
    auto compute_cs = loadShader(device, "compute.slang.compute");
    if (compute_cs.failed())
        return compute_cs.error();
    shared->compute_cs = std::move(compute_cs.value());

    auto ssbo_layout = DescriptorSetLayout::create(
        device, {{{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}}},
        "smoke-ssbo-layout");
    if (ssbo_layout.failed())
        return ssbo_layout.error();
    shared->ssbo_layout = std::move(ssbo_layout.value());

    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pc.offset = 0;
    pc.size = 8;
    auto compute_layout =
        PipelineLayout::create(device, {shared->ssbo_layout.handle()}, {pc}, "smoke-compute-layout");
    if (compute_layout.failed())
        return compute_layout.error();
    shared->compute_layout = std::move(compute_layout.value());

    auto compute_pipeline = ComputePipeline::create(device, shared->compute_cs.handle(),
                                                    shared->compute_layout.handle(), "smoke-compute");
    if (compute_pipeline.failed())
        return compute_pipeline.error();
    shared->compute_pipeline = std::move(compute_pipeline.value());

    // --- descriptor pool ---
    auto pool = DescriptorPool::create(
        device, {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 8}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4}}, 16,
        "smoke-pool");
    if (pool.failed())
        return pool.error();
    shared->pool = std::move(pool.value());

    smoke->shared_ = std::move(shared);
    core::logInfo("smoke: suite ready");
    return smoke;
}

void HeadlessSmoke::destroy() {
    shared_.reset();
    context_.reset();
    device_ = nullptr;
}

HeadlessSmoke::~HeadlessSmoke() {
    destroy();
}

std::vector<SmokePathResult> HeadlessSmoke::run() {
    results_.clear();
    if (options_.clear)
        results_.push_back(runClear());
    if (options_.triangle)
        results_.push_back(runTriangle());
    if (options_.depth)
        results_.push_back(runDepth());
    if (options_.compute)
        results_.push_back(runCompute());
    if (options_.transfer)
        results_.push_back(runTransfer());
    if (options_.timestamp)
        results_.push_back(runTimestamp());
    return results_;
}

// --- clear path ---
SmokePathResult HeadlessSmoke::runClear() {
    core::logInfo("smoke: run clear");
    SmokePathResult result;
    result.name = "clear";
    constexpr std::array<float, 4> kClear = {0.05f, 0.1f, 0.2f, 1.0f};

    const core::Clock::time_point t0 = core::Clock::now();
    auto clear_result = context_->clearColor(kClear);
    if (clear_result.failed()) {
        result.pass = false;
        result.detail = clear_result.error().to_string();
        return result;
    }
    auto pixels = context_->colorImage().readback(*device_);
    if (pixels.failed()) {
        result.pass = false;
        result.detail = pixels.error().to_string();
        return result;
    }
    const double elapsed_ms = std::chrono::duration<double, std::milli>(core::Clock::now() - t0).count();

    // Quantization tolerance: GPU unorm conversion may differ by one LSB
    // from the CPU's round-half-up (e.g. 0.1f*255 = 25.5 -> 25 on GPU).
    auto const near_u8 = [](std::uint8_t a, int expected) {
        return std::abs(static_cast<int>(a) - expected) <= 1;
    };
    bool all_match = true;
    std::size_t mismatches = 0;
    auto const& data = pixels.value();
    for (std::size_t i = 0; i < data.size(); i += 4) {
        const int expect_r = static_cast<int>(kClear[0] * 255.0f + 0.5f);
        const int expect_g = static_cast<int>(kClear[1] * 255.0f + 0.5f);
        const int expect_b = static_cast<int>(kClear[2] * 255.0f + 0.5f);
        const int expect_a = static_cast<int>(kClear[3] * 255.0f + 0.5f);
        if (!near_u8(data[i], expect_r) || !near_u8(data[i + 1], expect_g) || !near_u8(data[i + 2], expect_b) ||
            !near_u8(data[i + 3], expect_a)) {
            all_match = false;
            if (++mismatches > 8)
                break;
        }
    }
    result.pass = all_match;
    result.detail = all_match ? "all " + std::to_string(data.size() / 4) + " pixels match the clear color"
                              : std::to_string(mismatches) + "+ pixel mismatches";
    result.metrics = {{"width", options_.width},
                      {"height", options_.height},
                      {"pixels", data.size() / 4},
                      {"elapsed_ms", elapsed_ms}};
    return result;
}

// --- triangle path ---
SmokePathResult HeadlessSmoke::runTriangle() {
    core::logInfo("smoke: run triangle");
    SmokePathResult result;
    result.name = "triangle";

    auto set = shared_->pool.allocate(shared_->ubo_layout.handle());
    if (set.failed()) {
        result.pass = false;
        result.detail = set.error().to_string();
        return result;
    }

    auto ubo = Buffer::create(*device_, sizeof(shaders::TriangleUbo), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                              true, "smoke-triangle-ubo");
    if (ubo.failed()) {
        result.pass = false;
        result.detail = ubo.error().to_string();
        return result;
    }

    shaders::TriangleUbo data{};
    // Identity matrix (NDC-space triangle; depth disabled).
    data.mvp[0] = 1.0f;
    data.mvp[5] = 1.0f;
    data.mvp[10] = 1.0f;
    data.mvp[15] = 1.0f;
    if (ubo.value().upload(&data, sizeof(data)).failed()) {
        result.pass = false;
        result.detail = "ubo upload failed";
        return result;
    }

    VkDescriptorBufferInfo buffer_info{ubo.value().handle(), 0, sizeof(shaders::TriangleUbo)};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set.value();
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.pBufferInfo = &buffer_info;
    vkUpdateDescriptorSets(device_->handle(), 1, &write, 0, nullptr);

    const core::Clock::time_point t0 = core::Clock::now();
    auto r = runOneShot(*device_, [&](VkCommandBuffer cmd) {
        auto const extent = context_->extent();
        // color: undefined -> transfer dst, clear, -> color attachment
        transitionImage(cmd, context_->colorImage().handle(), VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT);
        VkClearColorValue clear{{0.05f, 0.05f, 0.08f, 1.0f}};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, context_->colorImage().handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &clear, 1, &range);
        transitionImage(cmd, context_->colorImage().handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                        VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);

        VkRenderingAttachmentInfo color{};
        color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color.imageView = context_->colorView();
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

        VkRenderingInfo render{};
        render.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        render.renderArea = {{0, 0}, extent};
        render.layerCount = 1;
        render.colorAttachmentCount = 1;
        render.pColorAttachments = &color;
        vkCmdBeginRendering(cmd, &render);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shared_->triangle_pipeline.handle());
        VkViewport viewport{0, 0, static_cast<float>(extent.width), static_cast<float>(extent.height), 0, 1};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        VkRect2D scissor{{0, 0}, extent};
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shared_->triangle_layout.handle(), 0, 1,
                                &set.value(), 0, nullptr);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);

        transitionImage(cmd, context_->colorImage().handle(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                        VK_ACCESS_2_MEMORY_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    });
    if (r.failed()) {
        result.pass = false;
        result.detail = r.error().to_string();
        return result;
    }
    const double elapsed_ms = std::chrono::duration<double, std::milli>(core::Clock::now() - t0).count();

    auto pixels = context_->colorImage().readback(*device_);
    if (pixels.failed()) {
        result.pass = false;
        result.detail = pixels.error().to_string();
        return result;
    }

    // DEBUG: scan for non-background pixels
    {
        auto const& px = pixels.value();
        const std::uint32_t w = options_.width, h = options_.height;
        int non_bg = 0;
        int first_x = -1, first_y = -1;
        for (std::uint32_t y = 0; y < h; y += 16) {
            for (std::uint32_t x = 0; x < w; x += 16) {
                const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
                const bool is_bg = std::abs(static_cast<int>(px[i]) - 13) <= 3 &&
                                   std::abs(static_cast<int>(px[i + 1]) - 13) <= 3 &&
                                   std::abs(static_cast<int>(px[i + 2]) - 20) <= 3;
                if (!is_bg) {
                    ++non_bg;
                    if (first_x < 0) { first_x = static_cast<int>(x); first_y = static_cast<int>(y); }
                }
            }
        }
        std::printf("DEBUG triangle non-bg samples: %d (first at %d,%d)\n", non_bg, first_x, first_y);
    }
    // Verification: center pixel must be within the triangle (non-background),
    // corners must be background.
    auto const& px = pixels.value();
    const std::uint32_t w = options_.width, h = options_.height;
    auto pixelAt = [&](std::uint32_t x, std::uint32_t y) -> std::array<std::uint8_t, 4> {
        const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
        return {px[i], px[i + 1], px[i + 2], px[i + 3]};
    };
    auto isBackground = [](std::array<std::uint8_t, 4> const& p) {
        return std::abs(static_cast<int>(p[0]) - 13) <= 3 && std::abs(static_cast<int>(p[1]) - 13) <= 3 &&
               std::abs(static_cast<int>(p[2]) - 20) <= 3;
    };
    const auto center = pixelAt(w / 2, h / 2);
    const auto corner_tl = pixelAt(2, 2);
    const auto corner_br = pixelAt(w - 3, h - 3);

    bool ok = !isBackground(center) && isBackground(corner_tl) && isBackground(corner_br);
    result.pass = ok;
    result.detail =
        ok ? "triangle rendered; center=" +
                 std::format("#{:02x}{:02x}{:02x}", center[0], center[1], center[2])
           : "center=" + std::format("#{:02x}{:02x}{:02x}", center[0], center[1], center[2]) +
                 " tl=" + std::format("#{:02x}{:02x}{:02x}", corner_tl[0], corner_tl[1], corner_tl[2]);
    result.metrics = {{"elapsed_ms", elapsed_ms}, {"width", w}, {"height", h}};
    return result;
}

// --- depth path ---
SmokePathResult HeadlessSmoke::runDepth() {
    core::logInfo("smoke: run depth");
    SmokePathResult result;
    result.name = "depth";

    auto set = shared_->pool.allocate(shared_->ubo_layout.handle());
    if (set.failed()) {
        result.pass = false;
        result.detail = set.error().to_string();
        return result;
    }
    auto ubo = Buffer::create(*device_, sizeof(shaders::DepthParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                              true, "smoke-depth-ubo");
    if (ubo.failed()) {
        result.pass = false;
        result.detail = ubo.error().to_string();
        return result;
    }

    shaders::DepthParams params{};
    // Probes 0-2: three depth values side by side (far/mid/near).
    // Probe 3: a near quad overlapping probe 1 at the center to verify the
    // reversed-Z GREATER test actually resolves overlapping primitives.
    params.quads[0] = {-0.40f, 0.0f, 0.15f, 0.15f}; // far
    params.quads[1] = {0.0f, 0.0f, 0.5f, 0.15f};    // mid
    params.quads[2] = {0.40f, 0.0f, 0.9f, 0.15f};   // near
    params.quads[3] = {0.0f, 0.0f, 0.9f, 0.05f};    // overlap-near (over mid at center)
    params.colors[3] = {0.0f, 0.0f, 1.0f, 1.0f};
    params.colors[0] = {1.0f, 0.0f, 0.0f, 1.0f};
    params.colors[1] = {0.0f, 1.0f, 0.0f, 1.0f};
    params.colors[2] = {0.0f, 0.0f, 1.0f, 1.0f};
    params.colors[3] = {0.0f, 0.0f, 1.0f, 1.0f};

    if (ubo.value().upload(&params, sizeof(params)).failed()) {
        result.pass = false;
        result.detail = "ubo upload failed";
        return result;
    }

    VkDescriptorBufferInfo buffer_info{ubo.value().handle(), 0, sizeof(shaders::DepthParams)};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set.value();
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.pBufferInfo = &buffer_info;
    vkUpdateDescriptorSets(device_->handle(), 1, &write, 0, nullptr);

    const core::Clock::time_point t0 = core::Clock::now();
    auto r = runOneShot(*device_, [&](VkCommandBuffer cmd) {
        auto const extent = context_->extent();
        transitionImage(cmd, context_->colorImage().handle(), VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        transitionImage(cmd, context_->depthImage().handle(), VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                        0, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
        // Draw far -> mid -> near with GREATER test: near wins where overlapping.
        recordDepthProbes(*device_, cmd, shared_->depth_pipeline.handle(), shared_->depth_layout.handle(),
                          set.value(), extent, context_->colorView(), context_->depthView());
        transitionImage(cmd, context_->colorImage().handle(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                        VK_ACCESS_2_MEMORY_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        transitionImage(cmd, context_->depthImage().handle(),
                        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                        VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                        VK_ACCESS_2_MEMORY_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    });
    if (r.failed()) {
        result.pass = false;
        result.detail = r.error().to_string();
        return result;
    }
    const double elapsed_ms = std::chrono::duration<double, std::milli>(core::Clock::now() - t0).count();

    auto color_pixels = context_->colorImage().readback(*device_);
    auto depth_pixels = context_->depthImage().readback(*device_);
    if (color_pixels.failed() || depth_pixels.failed()) {
        result.pass = false;
        result.detail = "readback failed";
        return result;
    }

    const std::uint32_t w = options_.width, h = options_.height;
    auto const& cp = color_pixels.value();
    auto const& dp = depth_pixels.value();

    // Sample depth at the center of each probe (x offsets -0.40/0.08/0.40;
    // x=0.08 is inside the mid probe but outside the overlap-near quad).
    auto depthAt = [&](float ndc_x) {
        const std::uint32_t x = static_cast<std::uint32_t>((ndc_x * 0.5f + 0.5f) * static_cast<float>(w));
        const std::uint32_t y = h / 2;
        const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
        float d = 0.0f;
        std::memcpy(&d, &dp[i], sizeof(float));
        return d;
    };
    // DEBUG dumps
    {
        auto px2 = [&](float ndc_x, float ndc_y) {
            const std::uint32_t x = static_cast<std::uint32_t>((ndc_x * 0.5f + 0.5f) * static_cast<float>(w));
            const std::uint32_t y = static_cast<std::uint32_t>((ndc_y * 0.5f + 0.5f) * static_cast<float>(h));
            const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
            return std::array<std::uint8_t, 4>{cp[i], cp[i + 1], cp[i + 2], cp[i + 3]};
        };
    }
    // Color at center (all three overlap there; near must win -> blue).
    auto colorAt = [&](std::uint32_t x, std::uint32_t y) {
        const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
        return std::array<std::uint8_t, 4>{cp[i], cp[i + 1], cp[i + 2], cp[i + 3]};
    };

    const float d_far = depthAt(-0.40f);
    const float d_mid = depthAt(0.08f);
    const float d_near = depthAt(0.40f);
    const auto center_color = colorAt(w / 2, h / 2);

    // Reversed-Z GREATER: near (0.9) > mid (0.5) > far (0.15) > clear (0.0).
    const bool ordered = d_near > d_mid && d_mid > d_far && d_far > 0.0f;
    const bool near_wins = center_color[2] > 200 && center_color[0] < 60 && center_color[1] < 60;
    const bool near_value_ok = std::abs(d_near - 0.9f) < 0.01f;
    const bool mid_value_ok = std::abs(d_mid - 0.5f) < 0.01f;
    const bool far_value_ok = std::abs(d_far - 0.15f) < 0.01f;

    result.pass = ordered && near_wins && near_value_ok && mid_value_ok && far_value_ok;
    result.detail = std::format(
        "depth values far={:.3f} mid={:.3f} near={:.3f}, near-wins={}, center=#{:02x}{:02x}{:02x}", d_far,
        d_mid, d_near, near_wins, center_color[0], center_color[1], center_color[2]);
    result.metrics = {{"elapsed_ms", elapsed_ms},
                      {"depth_far", d_far},
                      {"depth_mid", d_mid},
                      {"depth_near", d_near},
                      {"reversed_z_ordered", ordered}};
    return result;
}

// --- compute path ---
SmokePathResult HeadlessSmoke::runCompute() {
    core::logInfo("smoke: run compute");
    SmokePathResult result;
    result.name = "compute";

    constexpr std::uint32_t kCount = 4096;
    constexpr std::uint32_t kSeed = 0x5EED1234U;

    auto set = shared_->pool.allocate(shared_->ssbo_layout.handle());
    if (set.failed()) {
        result.pass = false;
        result.detail = set.error().to_string();
        return result;
    }
    auto ssbo = Buffer::create(*device_, kCount * sizeof(std::uint32_t),
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true,
                               "smoke-compute-ssbo");
    if (ssbo.failed()) {
        result.pass = false;
        result.detail = ssbo.error().to_string();
        return result;
    }

    VkDescriptorBufferInfo buffer_info{ssbo.value().handle(), 0, kCount * sizeof(std::uint32_t)};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set.value();
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &buffer_info;
    vkUpdateDescriptorSets(device_->handle(), 1, &write, 0, nullptr);

    shaders::ComputeParams params{kCount, kSeed};

    const core::Clock::time_point t0 = core::Clock::now();
    auto r = runOneShot(*device_, [&](VkCommandBuffer cmd) {
        // SSBO: host write -> compute write.
        VkBufferMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        barrier.buffer = ssbo.value().handle();
        barrier.offset = 0;
        barrier.size = kCount * sizeof(std::uint32_t);
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.bufferMemoryBarrierCount = 1;
        dep.pBufferMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dep);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, shared_->compute_pipeline.handle());
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, shared_->compute_layout.handle(), 0, 1,
                                &set.value(), 0, nullptr);
        vkCmdPushConstants(cmd, shared_->compute_layout.handle(), VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(params), &params);
        vkCmdDispatch(cmd, (kCount + 63) / 64, 1, 1);

        VkBufferMemoryBarrier2 barrier2{};
        barrier2.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        barrier2.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier2.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        barrier2.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        barrier2.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        barrier2.buffer = ssbo.value().handle();
        barrier2.offset = 0;
        barrier2.size = kCount * sizeof(std::uint32_t);
        VkDependencyInfo dep2{};
        dep2.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep2.bufferMemoryBarrierCount = 1;
        dep2.pBufferMemoryBarriers = &barrier2;
        vkCmdPipelineBarrier2(cmd, &dep2);
    });
    if (r.failed()) {
        result.pass = false;
        result.detail = r.error().to_string();
        return result;
    }
    const double elapsed_ms = std::chrono::duration<double, std::milli>(core::Clock::now() - t0).count();

    auto mapped = ssbo.value().map();
    if (mapped.failed()) {
        result.pass = false;
        result.detail = mapped.error().to_string();
        return result;
    }
    auto const* out = static_cast<std::uint32_t const*>(mapped.value());
    std::size_t mismatches = 0;
    for (std::uint32_t i = 0; i < kCount; ++i) {
        const std::uint32_t expected = (i * 0x9E3779B1U + kSeed) ^ (i >> 1U);
        if (out[i] != expected) {
            ++mismatches;
            if (mismatches <= 3) {
                core::logError("compute mismatch at {}: got {:08x} expected {:08x}", i, out[i], expected);
            }
        }
    }
    ssbo.value().unmap();

    result.pass = mismatches == 0;
    result.detail = mismatches == 0
                        ? std::format("{} elements verified bit-exact against CPU reference", kCount)
                        : std::format("{} mismatches", mismatches);
    result.metrics = {{"elapsed_ms", elapsed_ms}, {"elements", kCount}, {"mismatches", mismatches}};
    return result;
}

// --- transfer/upload path ---
SmokePathResult HeadlessSmoke::runTransfer() {
    core::logInfo("smoke: run transfer");
    SmokePathResult result;
    result.name = "transfer-upload";

    constexpr std::uint32_t kCount = 8192;
    std::vector<std::uint32_t> src(kCount);
    for (std::uint32_t i = 0; i < kCount; ++i) {
        src[i] = i * 2654435761U; // deterministic pattern
    }

    auto src_buf = Buffer::create(*device_, kCount * sizeof(std::uint32_t), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                  true, "smoke-transfer-src");
    auto dst_buf = Buffer::create(*device_, kCount * sizeof(std::uint32_t), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  true, "smoke-transfer-dst");
    if (src_buf.failed() || dst_buf.failed()) {
        result.pass = false;
        result.detail = "buffer allocation failed";
        return result;
    }
    if (src_buf.value().upload(src.data(), src.size() * sizeof(std::uint32_t)).failed()) {
        result.pass = false;
        result.detail = "src upload failed";
        return result;
    }

    const core::Clock::time_point t0 = core::Clock::now();
    auto r = runOneShot(*device_, [&](VkCommandBuffer cmd) {
        VkBufferMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        barrier.buffer = src_buf.value().handle();
        barrier.offset = 0;
        barrier.size = kCount * sizeof(std::uint32_t);
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.bufferMemoryBarrierCount = 1;
        dep.pBufferMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dep);

        VkBufferCopy region{0, 0, kCount * sizeof(std::uint32_t)};
        vkCmdCopyBuffer(cmd, src_buf.value().handle(), dst_buf.value().handle(), 1, &region);

        VkBufferMemoryBarrier2 barrier2{};
        barrier2.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        barrier2.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barrier2.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier2.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        barrier2.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        barrier2.buffer = dst_buf.value().handle();
        barrier2.offset = 0;
        barrier2.size = kCount * sizeof(std::uint32_t);
        VkDependencyInfo dep2{};
        dep2.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep2.bufferMemoryBarrierCount = 1;
        dep2.pBufferMemoryBarriers = &barrier2;
        vkCmdPipelineBarrier2(cmd, &dep2);
    });
    if (r.failed()) {
        result.pass = false;
        result.detail = r.error().to_string();
        return result;
    }
    const double buffer_copy_ms = std::chrono::duration<double, std::milli>(core::Clock::now() - t0).count();

    auto dst = dst_buf.value().map();
    if (dst.failed()) {
        result.pass = false;
        result.detail = dst.error().to_string();
        return result;
    }
    std::size_t mismatches = 0;
    for (std::uint32_t i = 0; i < kCount; ++i) {
        if (static_cast<std::uint32_t const*>(dst.value())[i] != src[i]) {
            ++mismatches;
        }
    }
    dst_buf.value().unmap();

    // Host -> image upload path (2D pattern).
    constexpr std::uint32_t kImgW = 64, kImgH = 64;
    std::vector<std::uint8_t> pattern(kImgW * kImgH * 4);
    for (std::uint32_t y = 0; y < kImgH; ++y) {
        for (std::uint32_t x = 0; x < kImgW; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * kImgW + x) * 4;
            pattern[i] = static_cast<std::uint8_t>(x * 4);
            pattern[i + 1] = static_cast<std::uint8_t>(y * 4);
            pattern[i + 2] = 0x7f;
            pattern[i + 3] = 0xff;
        }
    }
    auto tex = Image::create(*device_, VkExtent2D{kImgW, kImgH}, VK_FORMAT_R8G8B8A8_UNORM,
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, false,
                             "smoke-upload-image");
    if (tex.failed()) {
        result.pass = false;
        result.detail = tex.error().to_string();
        return result;
    }
    const core::Clock::time_point t1 = core::Clock::now();
    auto upload = tex.value().uploadFromHost(*device_, pattern.data(), pattern.size());
    if (upload.failed()) {
        result.pass = false;
        result.detail = upload.error().to_string();
        return result;
    }
    const double image_upload_ms = std::chrono::duration<double, std::milli>(core::Clock::now() - t1).count();
    auto back = tex.value().readback(*device_);
    if (back.failed()) {
        result.pass = false;
        result.detail = back.error().to_string();
        return result;
    }
    bool image_ok = back.value() == pattern;

    result.pass = mismatches == 0 && image_ok;
    result.detail = std::format("buffer copy: {} elements, {} mismatches; image upload: {} bytes, match={}",
                                kCount, mismatches, pattern.size(), image_ok);
    result.metrics = {{"buffer_copy_ms", buffer_copy_ms},
                      {"image_upload_ms", image_upload_ms},
                      {"elements", kCount},
                      {"image_bytes", pattern.size()},
                      {"image_match", image_ok}};
    return result;
}

// --- timestamp path ---
SmokePathResult HeadlessSmoke::runTimestamp() {
    core::logInfo("smoke: run timestamp");
    SmokePathResult result;
    result.name = "timestamp";

    // A tiny compute dispatch between two timestamps.
    auto set = shared_->pool.allocate(shared_->ssbo_layout.handle());
    if (set.failed()) {
        result.pass = false;
        result.detail = set.error().to_string();
        return result;
    }
    auto ssbo = Buffer::create(*device_, 64 * sizeof(std::uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true,
                               "smoke-timestamp-ssbo");
    if (ssbo.failed()) {
        result.pass = false;
        result.detail = ssbo.error().to_string();
        return result;
    }
    VkDescriptorBufferInfo buffer_info{ssbo.value().handle(), 0, 64 * sizeof(std::uint32_t)};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set.value();
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &buffer_info;
    vkUpdateDescriptorSets(device_->handle(), 1, &write, 0, nullptr);

    auto pool = QueryPool::create(*device_, VK_QUERY_TYPE_TIMESTAMP, 8, "smoke-timestamp-pool");
    if (pool.failed()) {
        result.pass = false;
        result.detail = pool.error().to_string();
        return result;
    }

    shaders::ComputeParams params{64, 1};

    const core::Clock::time_point t0 = core::Clock::now();
    auto r = runOneShot(*device_, [&](VkCommandBuffer cmd) {
        if (pool.value().reset(0, 8).failed()) {
            core::logError("timestamp pool reset failed");
        }
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, pool.value().handle(), 0);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, shared_->compute_pipeline.handle());
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, shared_->compute_layout.handle(), 0, 1,
                                &set.value(), 0, nullptr);
        vkCmdPushConstants(cmd, shared_->compute_layout.handle(), VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(params), &params);
        vkCmdDispatch(cmd, 1, 1, 1);

        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, pool.value().handle(), 1);
    });
    if (r.failed()) {
        result.pass = false;
        result.detail = r.error().to_string();
        return result;
    }
    const double elapsed_ms = std::chrono::duration<double, std::milli>(core::Clock::now() - t0).count();

    std::vector<std::uint64_t> timestamps;
    if (pool.value().getResults(0, 2, timestamps).failed()) {
        result.pass = false;
        result.detail = "timestamp results unavailable (VK_NOT_READY)";
        return result;
    }
    const float period_ns = device_->timestampPeriodNanos();
    const double begin_ns = static_cast<double>(timestamps[0]) * period_ns;
    const double end_ns = static_cast<double>(timestamps[1]) * period_ns;
    const double gpu_dispatch_ms = (end_ns - begin_ns) / 1e6;

    const bool valid = timestamps[0] != 0 && timestamps[1] >= timestamps[0];
    const bool has_period = period_ns > 0.0f;
    result.pass = valid && has_period && gpu_dispatch_ms >= 0.0;
    result.detail =
        std::format("GPU dispatch: {:.4f} ms (timestamp period {} ns)", gpu_dispatch_ms, period_ns);
    result.metrics = {{"timestamp_period_ns", period_ns},
                      {"begin_ticks", timestamps[0]},
                      {"end_ticks", timestamps[1]},
                      {"gpu_dispatch_ms", gpu_dispatch_ms},
                      {"elapsed_ms", elapsed_ms}};
    return result;
}

} // namespace helios::vk::smoke
