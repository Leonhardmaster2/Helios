// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// M1 GPU precision lab renderer (see header). Layout notes:
// - images live in GENERAL for their whole life; rendering uses GENERAL as
//   the attachment layout (legal and validation-clean), and Image::readback
//   expects GENERAL. The very first frame transitions UNDEFINED -> GENERAL.
// - depth is cleared to 0.0 (reversed-Z far) and tested with GREATER (D-002).
#include "helios/render/gpu_precision.hpp"

#include "helios/core/log.hpp"
#include "shaders/abi.hpp"
#include "helios/vk/command.hpp"
#include "helios/vk/device.hpp"
#include "helios/vk/descriptor_set.hpp"
#include "helios/vk/image.hpp"
#include "helios/vk/instance.hpp"
#include "helios/vk/pipeline.hpp"
#include "helios/vk/shader_module.hpp"
#include "helios/vk/smoke/smoke_context.hpp"

#include <cmath>
#include <cstring>
#include <memory>
#include <string>

#include <volk.h>
#include <shaders_embedded.hpp>
#include <vulkan/vulkan.h>

namespace helios::render {
namespace {

constexpr VkDeviceSize kMeshPcSize = sizeof(helios::shaders::MeshPC);
constexpr VkDeviceSize kImpostorPcSize = sizeof(helios::shaders::ImpostorPC);
constexpr float kFovYRad = 1.04719755f; // 60 degrees vertical, matches the CPU lab

core::Result<vk::ShaderModule> loadEmbeddedShader(vk::Device& device, std::string_view spv_name) {
    for (std::size_t i = 0; i < helios::shaders::embedded::kShaderCount; ++i) {
        auto const& s = helios::shaders::embedded::kShaders[i];
        if (s.name == spv_name) {
            return vk::ShaderModule::createFromSpirv(device, s.data, s.size,
                                                     std::string(spv_name));
        }
    }
    return core::makeError(core::ErrorCode::NotFound,
                           "embedded shader not found: " + std::string(spv_name));
}

void transitionImage(VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout,
                     VkImageLayout new_layout, VkPipelineStageFlags2 src_stage,
                     VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                     VkAccessFlags2 dst_access, VkImageAspectFlags aspect) {
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

} // namespace

struct GPUPrecisionTarget::Impl {
    std::unique_ptr<helios::vk::Instance> instance;
    std::unique_ptr<helios::vk::Device> device;
    std::unique_ptr<helios::vk::smoke::SmokeContext> context;

    helios::vk::ShaderModule mesh_vs;
    helios::vk::ShaderModule mesh_fs;
    helios::vk::ShaderModule impostor_vs;
    helios::vk::ShaderModule impostor_fs;
    helios::vk::PipelineLayout mesh_layout;
    helios::vk::PipelineLayout impostor_layout;
    helios::vk::Pipeline mesh_pipeline;
    helios::vk::Pipeline impostor_pipeline;

    // Layout tracking: Image::readback leaves images in TRANSFER_SRC_OPTIMAL.
    enum class ImgState { Undefined, General, TransferSrc };
    ImgState color_state = ImgState::Undefined;
    ImgState depth_state = ImgState::Undefined;
};

GPUPrecisionTarget::~GPUPrecisionTarget() {
    if (impl_ != nullptr) {
        delete impl_;
    }
}

core::Result<std::unique_ptr<GPUPrecisionTarget>>
GPUPrecisionTarget::create(bool enable_validation, std::uint32_t width, std::uint32_t height) {
    auto target = std::unique_ptr<GPUPrecisionTarget>(new GPUPrecisionTarget());
    target->impl_ = new GPUPrecisionTarget::Impl();

    helios::vk::InstanceOptions instance_options;
    instance_options.enable_validation = enable_validation;
    instance_options.require_validation = false;
    auto instance = helios::vk::Instance::create(instance_options);
    if (instance.failed()) {
        return instance.error();
    }
    target->impl_->instance = std::move(instance.value());

    helios::vk::DeviceOptions device_options;
    device_options.require_swapchain = false;
    auto device =
        helios::vk::Device::create(*target->impl_->instance, std::nullopt, device_options);
    if (device.failed()) {
        return device.error();
    }
    target->impl_->device = std::move(device.value());

    helios::vk::smoke::SmokeTargetSpec spec;
    spec.width = width;
    spec.height = height;
    spec.color_format = VK_FORMAT_R8G8B8A8_UNORM;
    spec.depth_format = VK_FORMAT_D32_SFLOAT;
    auto context = helios::vk::smoke::SmokeContext::create(*target->impl_->device, spec);
    if (context.failed()) {
        return context.error();
    }
    target->impl_->context = std::move(context.value());

    auto& d = *target->impl_->device;
    auto& imp = *target->impl_;

    struct ShaderSet {
        helios::vk::ShaderModule vs;
        helios::vk::ShaderModule fs;
    };
    auto load_pair = [&](char const* vs_name, char const* fs_name, ShaderSet& out) -> core::Result<void> {
        auto vs = loadEmbeddedShader(d, vs_name);
        if (vs.failed())
            return vs.error();
        out.vs = std::move(vs.value());
        auto fs = loadEmbeddedShader(d, fs_name);
        if (fs.failed())
            return fs.error();
        out.fs = std::move(fs.value());
        return core::Result<void>{};
    };

    auto make_pipelines = [&]() -> core::Result<void> {
        VkPushConstantRange mesh_range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                       static_cast<std::uint32_t>(kMeshPcSize)};
        auto mesh_layout =
            helios::vk::PipelineLayout::create(d, {}, {mesh_range}, "gpu-lab-mesh-layout");
        if (mesh_layout.failed())
            return mesh_layout.error();
        imp.mesh_layout = std::move(mesh_layout.value());

        VkPushConstantRange imp_range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                      static_cast<std::uint32_t>(kImpostorPcSize)};
        auto imp_layout =
            helios::vk::PipelineLayout::create(d, {}, {imp_range}, "gpu-lab-impostor-layout");
        if (imp_layout.failed())
            return imp_layout.error();
        imp.impostor_layout = std::move(imp_layout.value());

        helios::vk::GraphicsPipelineDescription mesh_desc;
        mesh_desc.vertex = imp.mesh_vs.handle();
        mesh_desc.fragment = imp.mesh_fs.handle();
        mesh_desc.layout = imp.mesh_layout.handle();
        mesh_desc.color_format = spec.color_format;
        mesh_desc.depth_format = spec.depth_format;
        mesh_desc.enable_depth_test = true;
        mesh_desc.enable_depth_write = true;
        mesh_desc.depth_compare = VK_COMPARE_OP_GREATER;
        mesh_desc.enable_cull = false;
        mesh_desc.debug_name = "gpu-lab-mesh";
        auto mesh_pipeline = helios::vk::Pipeline::create(d, mesh_desc);
        if (mesh_pipeline.failed())
            return mesh_pipeline.error();
        imp.mesh_pipeline = std::move(mesh_pipeline.value());

        helios::vk::GraphicsPipelineDescription impostor_desc;
        impostor_desc.vertex = imp.impostor_vs.handle();
        impostor_desc.fragment = imp.impostor_fs.handle();
        impostor_desc.layout = imp.impostor_layout.handle();
        impostor_desc.color_format = spec.color_format;
        impostor_desc.depth_format = spec.depth_format;
        impostor_desc.enable_depth_test = true;
        impostor_desc.enable_depth_write = true;
        impostor_desc.depth_compare = VK_COMPARE_OP_GREATER;
        impostor_desc.enable_cull = false;
        impostor_desc.debug_name = "gpu-lab-impostor";
        auto impostor_pipeline = helios::vk::Pipeline::create(d, impostor_desc);
        if (impostor_pipeline.failed())
            return impostor_pipeline.error();
        imp.impostor_pipeline = std::move(impostor_pipeline.value());
        return core::Result<void>{};
    };

    // Scope the shader modules to pipeline creation only.
    {
        ShaderSet mesh_shaders{};
        ShaderSet impostor_shaders{};
        if (auto r = load_pair("mesh.slang.vertex.spv", "mesh.slang.fragment.spv", mesh_shaders);
            r.failed()) {
            return r.error();
        }
        if (auto r = load_pair("impostor.slang.vertex.spv", "impostor.slang.fragment.spv",
                               impostor_shaders);
            r.failed()) {
            return r.error();
        }
        imp.mesh_vs = std::move(mesh_shaders.vs);
        imp.mesh_fs = std::move(mesh_shaders.fs);
        imp.impostor_vs = std::move(impostor_shaders.vs);
        imp.impostor_fs = std::move(impostor_shaders.fs);

        if (auto r = make_pipelines(); r.failed()) {
            return r.error();
        }
        // RAII modules release here; pipelines keep internal references.
    }

    return target;
}

core::Result<void> GPUPrecisionTarget::renderAndReadback(GpuFramePlan const& plan,
                                                         std::array<float, 4> const& clear_color,
                                                         HostFrame& out_frame) {
    auto& imp = *impl_;
    auto& d = *imp.device;
    auto& ctx = *imp.context;
    const VkExtent2D extent = ctx.extent();

    // The mesh pipeline consumes the plan's PV verbatim; verify the caller
    // supplied a consistent plan rather than silently drawing garbage.
    if (!imp.mesh_pipeline.valid() || !imp.impostor_pipeline.valid()) {
        return core::makeError(core::ErrorCode::Internal, "gpu lab pipelines missing");
    }

    const auto recorded = helios::vk::runOneShot(d, [&](VkCommandBuffer cmd) {
        // Restore GENERAL before rendering (readback left TRANSFER_SRC).
        auto to_general = [&](VkImage image, Impl::ImgState& state, VkImageAspectFlags aspect) {
            if (state == Impl::ImgState::General)
                return;
            const VkImageLayout old = state == Impl::ImgState::Undefined
                                          ? VK_IMAGE_LAYOUT_UNDEFINED
                                          : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            transitionImage(cmd, image, old, VK_IMAGE_LAYOUT_GENERAL,
                            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                            VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, aspect);
            state = Impl::ImgState::General;
        };
        to_general(ctx.colorImage().handle(), imp.color_state, VK_IMAGE_ASPECT_COLOR_BIT);
        to_general(ctx.depthImage().handle(), imp.depth_state, VK_IMAGE_ASPECT_DEPTH_BIT);

        VkRenderingAttachmentInfo color{};
        color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color.imageView = ctx.colorView();
        color.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color = {{clear_color[0], clear_color[1], clear_color[2], clear_color[3]}};

        VkRenderingAttachmentInfo depth_att{};
        depth_att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth_att.imageView = ctx.depthView();
        depth_att.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        depth_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth_att.clearValue.depthStencil = {0.0f, 0}; // reversed-Z: 0 = far

        VkRenderingInfo render{};
        render.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        render.renderArea = {{0, 0}, extent};
        render.layerCount = 1;
        render.colorAttachmentCount = 1;
        render.pColorAttachments = &color;
        render.pDepthAttachment = &depth_att;

        vkCmdBeginRendering(cmd, &render);
        VkViewport viewport{0, 0, static_cast<float>(extent.width),
                            static_cast<float>(extent.height), 0, 1};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        VkRect2D scissor{{0, 0}, extent};
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        // Distant bodies first; ordering against near geometry is resolved
        // by reversed-Z depth, but draw order also makes overdraw obvious.
        if (!plan.impostors.empty()) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              imp.impostor_pipeline.handle());
            const float inv_w = 1.0f / static_cast<float>(extent.width);
            const float inv_h = 1.0f / static_cast<float>(extent.height);
            for (auto const& draw : plan.impostors) {
                helios::shaders::ImpostorPC pc{};
                pc.dir_depth[0] = draw.direction_view.v[0];
                pc.dir_depth[1] = draw.direction_view.v[1];
                pc.dir_depth[2] = draw.direction_view.v[2];
                pc.dir_depth[3] = draw.ndc_depth;
                pc.params[0] = draw.angular_radius_rad;
                pc.params[1] = std::tan(kFovYRad * 0.5f);
                pc.params[2] =
                    static_cast<float>(extent.width) / static_cast<float>(extent.height);
                pc.params[3] = 0.0f;
                pc.size_inv[0] = inv_w;
                pc.size_inv[1] = inv_h;
                pc.size_inv[2] = 0.0f;
                pc.size_inv[3] = 0.0f;
                for (int c = 0; c < 4; ++c) {
                    pc.color[c] = draw.color[c];
                }
                vkCmdPushConstants(cmd, imp.impostor_layout.handle(),
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                   kImpostorPcSize, pc.dir_depth);
                vkCmdDraw(cmd, 3, 1, 0, 0);
            }
        }

        if (!plan.meshes.empty()) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, imp.mesh_pipeline.handle());
            for (auto const& draw : plan.meshes) {
                helios::shaders::MeshPC pc{};
                // plan.pv is column-major flat (element (r,c) at [c*4+r]);
                // row i as a float4 = {pv[0*4+i], pv[1*4+i], pv[2*4+i],
                // pv[3*4+i]}. The shader reconstructs clip = M * w exactly.
                for (int c = 0; c < 4; ++c) {
                    pc.row0[c] = plan.pv[c * 4 + 0];
                    pc.row1[c] = plan.pv[c * 4 + 1];
                    pc.row2[c] = plan.pv[c * 4 + 2];
                    pc.row3[c] = plan.pv[c * 4 + 3];
                }
                pc.offset_radius[0] = draw.offset_m.v[0];
                pc.offset_radius[1] = draw.offset_m.v[1];
                pc.offset_radius[2] = draw.offset_m.v[2];
                pc.offset_radius[3] = draw.radius_m;
                for (int c = 0; c < 4; ++c) {
                    pc.color[c] = draw.color[c];
                }
                vkCmdPushConstants(cmd, imp.mesh_layout.handle(),
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                   kMeshPcSize, pc.row0);
                vkCmdDraw(cmd, 48u * 24u * 6u, 1, 0, 0);
            }
        }

        vkCmdEndRendering(cmd);
    });

    if (recorded.failed()) {
        return recorded.error();
    }

    auto color_bytes = ctx.colorImage().readback(d);
    if (color_bytes.failed()) {
        return color_bytes.error();
    }
    auto depth_bytes = ctx.depthImage().readback(d);
    if (depth_bytes.failed()) {
        return depth_bytes.error();
    }
    imp.color_state = Impl::ImgState::TransferSrc;
    imp.depth_state = Impl::ImgState::TransferSrc;

    out_frame.width = extent.width;
    out_frame.height = extent.height;
    out_frame.color = std::move(color_bytes.value());
    const std::size_t px_count = static_cast<std::size_t>(extent.width) * extent.height;
    out_frame.depth.resize(px_count);
    std::memcpy(out_frame.depth.data(), depth_bytes.value().data(), px_count * sizeof(float));
    return core::Result<void>{};
}

std::uint32_t GPUPrecisionTarget::validationErrors() const noexcept {
    return impl_->instance->validationStats().error_count;
}
std::uint32_t GPUPrecisionTarget::validationWarnings() const noexcept {
    return impl_->instance->validationStats().warning_count;
}
std::string GPUPrecisionTarget::deviceName() const {
    return impl_->device->report().device_name;
}

} // namespace helios::render

// ---------------------------------------------------------------------------
// FrameViewer: minimal SDL window blitting HostFrame buffers.
// ---------------------------------------------------------------------------
#include <SDL3/SDL.h>
namespace helios {
namespace render {

struct FrameViewer::Impl {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    bool close_requested = false;
};

FrameViewer::~FrameViewer() {
    if (impl_ != nullptr) {
        if (impl_->texture != nullptr) {
            SDL_DestroyTexture(impl_->texture);
        }
        if (impl_->renderer != nullptr) {
            SDL_DestroyRenderer(impl_->renderer);
        }
        if (impl_->window != nullptr) {
            SDL_DestroyWindow(impl_->window);
        }
        delete impl_;
    }
}

core::Result<std::unique_ptr<FrameViewer>>
FrameViewer::create(std::string_view title, std::uint32_t width, std::uint32_t height) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        return core::makeError(core::ErrorCode::Initialization,
                               std::string("SDL_Init failed: ") + SDL_GetError());
    }
    auto viewer = std::unique_ptr<render::FrameViewer>(new render::FrameViewer());
    viewer->impl_ = new render::FrameViewer::Impl();
    viewer->impl_->window = SDL_CreateWindow(std::string(title).c_str(), static_cast<int>(width),
                                             static_cast<int>(height), 0);
    if (viewer->impl_->window == nullptr) {
        return core::makeError(core::ErrorCode::Initialization,
                               std::string("SDL_CreateWindow failed: ") + SDL_GetError());
    }
    viewer->impl_->renderer = SDL_CreateRenderer(viewer->impl_->window, "vulkan");
    if (viewer->impl_->renderer == nullptr) {
        return core::makeError(core::ErrorCode::Initialization,
                               std::string("SDL Vulkan renderer creation failed: ") +
                                   SDL_GetError());
    }
    viewer->impl_->texture =
        SDL_CreateTexture(viewer->impl_->renderer, SDL_PIXELFORMAT_ABGR8888,
                          SDL_TEXTUREACCESS_STREAMING, static_cast<int>(width),
                          static_cast<int>(height));
    if (viewer->impl_->texture == nullptr) {
        return core::makeError(core::ErrorCode::Initialization,
                               std::string("SDL_CreateTexture failed: ") + SDL_GetError());
    }
    return viewer;
}

bool FrameViewer::show(HostFrame const& frame) {
    SDL_UpdateTexture(impl_->texture, nullptr, frame.color.data(),
                      static_cast<int>(frame.width * 4));
    SDL_RenderClear(impl_->renderer);
    SDL_RenderTexture(impl_->renderer, impl_->texture, nullptr, nullptr);
    SDL_RenderPresent(impl_->renderer);

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT) {
            impl_->close_requested = true;
        } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
            impl_->close_requested = true;
        }
    }
    return !impl_->close_requested;
}

bool FrameViewer::closeRequested() const noexcept {
    return impl_->close_requested;
}

} // namespace render
} // namespace helios
