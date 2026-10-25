// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "planet_patch_renderer.hpp"

#include "helios/planet/cube_sphere.hpp"

#include "MapHelper.hpp"
#include "Shader.h"
#include "shaders_embedded.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <string_view>
#include <vector>

namespace helios::examples {
namespace {

struct EmbeddedView {
    unsigned char const* data = nullptr;
    std::size_t size = 0;
};

[[nodiscard]] EmbeddedView findEmbedded(std::string_view name) {
    for (std::size_t i = 0; i < shaders::embedded::kShaderCount; ++i) {
        auto const& shader = shaders::embedded::kShaders[i];
        if (shader.name != nullptr && name == shader.name) {
            return {shader.data, shader.size};
        }
    }
    return {};
}

[[nodiscard]] bool finiteVector(math::Vec3d const& value) noexcept {
    return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
}

void storeScaled(float row[4], math::Vec3d const& direction, double scale, float w = 0.0f);
void storeScaled(float row[4], math::Vec3d const& direction, double scale, float w) {
    row[0] = static_cast<float>(direction[0] * scale);
    row[1] = static_cast<float>(direction[1] * scale);
    row[2] = static_cast<float>(direction[2] * scale);
    row[3] = w;
}

[[nodiscard]] bool isAboveHorizon(planet::TileLodRecord const& leaf,
                                  math::Vec3d const& camera,
                                  PlanetPatchSettings const& settings) {
    double const camera_distance = math::length(camera);
    double const effective_radius =
        settings.planet_radius_m + std::abs(static_cast<double>(settings.relief_amplitude_m));
    if (camera_distance <= effective_radius || camera_distance <= 0.0) {
        return true;
    }

    math::Vec3d const camera_direction = camera / camera_distance;
    double const horizon_angle =
        std::acos(std::clamp(effective_radius / camera_distance, 0.0, 1.0));
    double patch_angle = 0.0;
    auto corners = planet::tileCornerDirections(leaf.key);
    if (corners.ok()) {
        for (math::Vec3d const& corner : corners.value()) {
            patch_angle = std::max(
                patch_angle,
                std::acos(std::clamp(math::dot(leaf.center_direction, corner), -1.0, 1.0)));
        }
    }
    // A small safety margin avoids popping from relief and floating-point
    // noise at the horizon. Culling is only a workload optimization.
    patch_angle = std::min(std::numbers::pi, patch_angle * 1.08 + 0.002);
    double const center_angle = std::acos(
        std::clamp(math::dot(camera_direction, leaf.center_direction), -1.0, 1.0));
    return center_angle <= horizon_angle + patch_angle;
}

} // namespace

bool PlanetPatchRenderer::initialize(Diligent::IRenderDevice* device,
                                     Diligent::IDeviceContext* context,
                                     Diligent::SwapChainDesc const& swap_chain_desc,
                                     std::string& error) {
    if (device == nullptr || context == nullptr) {
        error = "planet patch renderer requires a valid Diligent device and context";
        return false;
    }
    if (device->GetDeviceInfo().Type != Diligent::RENDER_DEVICE_TYPE_VULKAN) {
        error = "planet patch renderer is a Vulkan-only v0.0.1 path";
        return false;
    }
    device_ = device;
    context_ = context;

    Diligent::BufferDesc frame_desc;
    frame_desc.Name = "Helios LOD lab frame constants";
    frame_desc.Size = sizeof(FrameGpu);
    frame_desc.BindFlags = Diligent::BIND_UNIFORM_BUFFER;
    frame_desc.Usage = Diligent::USAGE_DYNAMIC;
    frame_desc.CPUAccessFlags = Diligent::CPU_ACCESS_WRITE;
    device_->CreateBuffer(frame_desc, nullptr, &frame_buffer_);

    Diligent::BufferDesc body_desc;
    body_desc.Name = "Helios planet body constants";
    body_desc.Size = sizeof(BodyGpu);
    body_desc.BindFlags = Diligent::BIND_UNIFORM_BUFFER;
    body_desc.Usage = Diligent::USAGE_DYNAMIC;
    body_desc.CPUAccessFlags = Diligent::CPU_ACCESS_WRITE;
    device_->CreateBuffer(body_desc, nullptr, &body_buffer_);

    Diligent::BufferDesc patch_desc;
    patch_desc.Name = "Helios LOD lab patch instances";
    patch_desc.Size = static_cast<Diligent::Uint64>(sizeof(PatchGpu)) * kMaxPatchInstances;
    patch_desc.BindFlags = Diligent::BIND_SHADER_RESOURCE;
    patch_desc.Usage = Diligent::USAGE_DYNAMIC;
    patch_desc.CPUAccessFlags = Diligent::CPU_ACCESS_WRITE;
    patch_desc.Mode = Diligent::BUFFER_MODE_STRUCTURED;
    patch_desc.ElementByteStride = sizeof(PatchGpu);
    device_->CreateBuffer(patch_desc, nullptr, &patch_buffer_);
    if (frame_buffer_ == nullptr || patch_buffer_ == nullptr || body_buffer_ == nullptr) {
        error = "Diligent failed to create LOD lab dynamic GPU buffers";
        return false;
    }
    Diligent::SamplerDesc sampler;
    sampler.Name = "Helios ring profile sampler";
    sampler.MinFilter = sampler.MagFilter = Diligent::FILTER_TYPE_LINEAR;
    sampler.MipFilter = Diligent::FILTER_TYPE_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = Diligent::TEXTURE_ADDRESS_CLAMP;
    device_->CreateSampler(sampler, &profile_sampler_);
    {
        float const zero[4]{0.0f, 0.0f, 0.0f, 0.0f};
        Diligent::TextureDesc desc;
        desc.Name = "Helios empty ring profile";
        desc.Type = Diligent::RESOURCE_DIM_TEX_2D;
        desc.Width = 1;
        desc.Height = 1;
        desc.MipLevels = 1;
        desc.Format = Diligent::TEX_FORMAT_R32_FLOAT;
        desc.Usage = Diligent::USAGE_IMMUTABLE;
        desc.BindFlags = Diligent::BIND_SHADER_RESOURCE;
        Diligent::TextureSubResData level;
        level.pData = zero;
        level.Stride = sizeof(float);
        Diligent::TextureData data;
        data.pSubResources = &level;
        data.NumSubresources = 1;
        device_->CreateTexture(desc, &data, &empty_profile_);
    }
    if (profile_sampler_ == nullptr || empty_profile_ == nullptr) {
        error = "Diligent failed to create the ring profile sampler";
        return false;
    }
    empty_profile_->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE)->SetSampler(profile_sampler_);

    if (!createPipeline(swap_chain_desc, error)) {
        return false;
    }

    gpu_timer_supported_ = device_->GetDeviceInfo().Features.DurationQueries !=
                           Diligent::DEVICE_FEATURE_STATE_DISABLED;
    if (gpu_timer_supported_) {
        for (QuerySlot& slot : gpu_queries_) {
            Diligent::QueryDesc query_desc{Diligent::QUERY_TYPE_DURATION};
            query_desc.Name = "Helios LOD lab planet-pass duration";
            device_->CreateQuery(query_desc, &slot.query);
            if (slot.query == nullptr) {
                gpu_timer_supported_ = false;
                break;
            }
        }
    }
    return true;
}

bool PlanetPatchRenderer::createPipeline(Diligent::SwapChainDesc const& swap_chain_desc,
                                         std::string& error) {
    EmbeddedView const vertex_bytecode = findEmbedded("planet_patch.slang.vertex.spv");
    EmbeddedView const fragment_bytecode = findEmbedded("planet_patch.slang.fragment.spv");
    if (vertex_bytecode.data == nullptr || fragment_bytecode.data == nullptr) {
        error = "embedded planet_patch Slang SPIR-V is missing";
        return false;
    }
    if ((vertex_bytecode.size & 3U) != 0 || (fragment_bytecode.size & 3U) != 0) {
        error = "embedded planet_patch SPIR-V byte counts are not 32-bit aligned";
        return false;
    }

    Diligent::RefCntAutoPtr<Diligent::IShader> vertex_shader;
    Diligent::ShaderCreateInfo vertex_info;
    vertex_info.Desc.Name = "Helios cube-sphere patch vertex shader";
    vertex_info.Desc.ShaderType = Diligent::SHADER_TYPE_VERTEX;
    vertex_info.EntryPoint = "main";
    vertex_info.ByteCode = vertex_bytecode.data;
    vertex_info.ByteCodeSize = vertex_bytecode.size;
    device_->CreateShader(vertex_info, &vertex_shader);

    Diligent::RefCntAutoPtr<Diligent::IShader> fragment_shader;
    Diligent::ShaderCreateInfo fragment_info;
    fragment_info.Desc.Name = "Helios cube-sphere patch fragment shader";
    fragment_info.Desc.ShaderType = Diligent::SHADER_TYPE_PIXEL;
    fragment_info.EntryPoint = "main";
    fragment_info.ByteCode = fragment_bytecode.data;
    fragment_info.ByteCodeSize = fragment_bytecode.size;
    device_->CreateShader(fragment_info, &fragment_shader);
    if (vertex_shader == nullptr || fragment_shader == nullptr) {
        error = "Diligent rejected the embedded planet_patch SPIR-V";
        return false;
    }

    Diligent::GraphicsPipelineStateCreateInfo pipeline_info;
    pipeline_info.PSODesc.Name = "Helios cube-sphere LOD inspection pipeline";
    pipeline_info.PSODesc.PipelineType = Diligent::PIPELINE_TYPE_GRAPHICS;
    pipeline_info.PSODesc.ResourceLayout.DefaultVariableType =
        Diligent::SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
    Diligent::ShaderResourceVariableDesc const variables[] = {
        {Diligent::SHADER_TYPE_PIXEL, "ring_profile", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC}};
    pipeline_info.PSODesc.ResourceLayout.Variables = variables;
    pipeline_info.PSODesc.ResourceLayout.NumVariables = 1;
    pipeline_info.GraphicsPipeline.NumRenderTargets = 1;
    pipeline_info.GraphicsPipeline.RTVFormats[0] = swap_chain_desc.ColorBufferFormat;
    pipeline_info.GraphicsPipeline.DSVFormat = swap_chain_desc.DepthBufferFormat;
    pipeline_info.GraphicsPipeline.PrimitiveTopology =
        Diligent::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    pipeline_info.GraphicsPipeline.RasterizerDesc.CullMode = Diligent::CULL_MODE_NONE;
    pipeline_info.GraphicsPipeline.DepthStencilDesc.DepthEnable = true;
    pipeline_info.GraphicsPipeline.DepthStencilDesc.DepthWriteEnable = true;
    pipeline_info.GraphicsPipeline.DepthStencilDesc.DepthFunc =
        Diligent::COMPARISON_FUNC_GREATER_EQUAL; // reversed Z: near=1, infinity=0
    pipeline_info.pVS = vertex_shader;
    pipeline_info.pPS = fragment_shader;
    device_->CreateGraphicsPipelineState(pipeline_info, &pipeline_);
    if (pipeline_ == nullptr) {
        error = "Diligent failed to create the cube-sphere graphics pipeline";
        return false;
    }

    auto* frame_variable =
        pipeline_->GetStaticVariableByName(Diligent::SHADER_TYPE_VERTEX, "frame");
    auto* patch_variable =
        pipeline_->GetStaticVariableByName(Diligent::SHADER_TYPE_VERTEX, "patches");
    auto* body_variable = pipeline_->GetStaticVariableByName(Diligent::SHADER_TYPE_VERTEX, "body");
    auto* patch_view = patch_buffer_->GetDefaultView(Diligent::BUFFER_VIEW_SHADER_RESOURCE);
    if (frame_variable == nullptr || patch_variable == nullptr || body_variable == nullptr ||
        patch_view == nullptr) {
        error = "Diligent could not reflect frame/patches/body from planet_patch SPIR-V";
        return false;
    }
    frame_variable->Set(frame_buffer_);
    patch_variable->Set(patch_view);
    body_variable->Set(body_buffer_);
    // The fragment stage declares the same buffers; a shared variable is
    // bound once per stage that sees it.
    if (auto* fragment_frame = pipeline_->GetStaticVariableByName(Diligent::SHADER_TYPE_PIXEL, "frame")) {
        fragment_frame->Set(frame_buffer_);
    }
    if (auto* fragment_body = pipeline_->GetStaticVariableByName(Diligent::SHADER_TYPE_PIXEL, "body")) {
        fragment_body->Set(body_buffer_);
    }
    pipeline_->CreateShaderResourceBinding(&resources_, true);
    if (resources_ == nullptr) {
        error = "Diligent failed to create cube-sphere shader resource bindings";
        return false;
    }
    return true;
}

bool PlanetPatchRenderer::render(std::span<planet::TileLodRecord const> leaves,
                                 PlanetPatchView const& view,
                                 PlanetPatchSettings const& settings,
                                 PlanetPatchMetrics& metrics,
                                 std::string& error) {
    metrics = {};
    metrics.selected_leaf_count = static_cast<std::uint32_t>(leaves.size());
    metrics.gpu_timer_supported = gpu_timer_supported_;
    if (leaves.size() > kMaxPatchInstances) {
        error = "LOD selection exceeds the renderer's 16,384-patch instance capacity";
        return false;
    }
    bool const view_valid = finiteVector(view.camera_body_fixed_m) &&
                            finiteVector(view.forward_body_fixed) &&
                            finiteVector(view.right_body_fixed) && finiteVector(view.up_body_fixed) &&
                            std::isfinite(view.vertical_fov_rad) && view.vertical_fov_rad > 0.0 &&
                            view.vertical_fov_rad < std::numbers::pi &&
                            std::isfinite(view.near_plane_m) && view.near_plane_m > 0.0 &&
                            view.viewport_width_px > 0 && view.viewport_height_px > 0 &&
                            std::isfinite(settings.planet_radius_m) && settings.planet_radius_m > 0.0 &&
                            std::isfinite(settings.max_render_distance_m) &&
                            settings.max_render_distance_m > 0.0 &&
                            std::isfinite(settings.relief_amplitude_m) &&
                            std::isfinite(settings.skirt_depth_m) && settings.skirt_depth_m >= 0.0f;
    if (!view_valid) {
        error = "planet patch renderer received an invalid camera/projection";
        return false;
    }

    FrameGpu frame{};
    double const focal = 1.0 / std::tan(view.vertical_fov_rad * 0.5);
    double const aspect = static_cast<double>(view.viewport_width_px) /
                          static_cast<double>(view.viewport_height_px);
    storeScaled(frame.row0, view.right_body_fixed, focal / aspect);
    storeScaled(frame.row1, view.up_body_fixed, focal);
    frame.row2[3] = static_cast<float>(view.near_plane_m);
    storeScaled(frame.row3, view.forward_body_fixed, 1.0);
    // The sub-pixel jitter: a shift of the image by jitter_px pixels is a
    // shift of the clip-space x and y by that fraction of w (the forward
    // distance).
    double const shift_x = -2.0 * view.jitter_px[0] / static_cast<double>(view.viewport_width_px);
    double const shift_y = 2.0 * view.jitter_px[1] / static_cast<double>(view.viewport_height_px);
    for (int axis = 0; axis < 3; ++axis) {
        frame.row0[axis] += static_cast<float>(view.forward_body_fixed[axis] * shift_x);
        frame.row1[axis] += static_cast<float>(view.forward_body_fixed[axis] * shift_y);
    }
    frame.planet[0] = static_cast<float>(settings.planet_radius_m);
    frame.planet[1] = settings.wire_overlay ? 1.0f : 0.0f;
    frame.planet[2] = static_cast<float>(settings.debug_color);
    frame.planet[3] = settings.tone_map_output ? 1.0f : 0.0f;
    math::Vec3d const light = math::lengthSq(settings.light_direction) > 1.0e-12
                                  ? math::normalized(settings.light_direction)
                                  : math::Vec3d{0.0, 0.0, 1.0};
    storeScaled(frame.light, light, 1.0, settings.ambient);
    storeScaled(frame.camera, view.camera_body_fixed_m, 1.0, static_cast<float>(settings.time_s));
    bool const rings = settings.ring_outer_m > settings.ring_inner_m && settings.ring_profile != nullptr;
    frame.ring[0] = static_cast<float>(settings.ring_inner_m);
    frame.ring[1] = static_cast<float>(settings.ring_outer_m);
    frame.ring[2] = 0.0f;
    frame.ring[3] = rings ? 1.0f : 0.0f;
    storeScaled(frame.occluder, settings.occluder_centre_m, 1.0, static_cast<float>(settings.occluder_radius_m));
    frame.clouds[0] = static_cast<float>(settings.cloud_base_m);
    frame.clouds[1] = static_cast<float>(settings.cloud_top_m);
    frame.clouds[2] = static_cast<float>(settings.cloud_cover);
    frame.clouds[3] = static_cast<float>(settings.cloud_extinction);
    storeScaled(frame.ambient_color, settings.ambient_color, 1.0,
                static_cast<float>(settings.cloud_drift_rad));
    BodyGpu const body = planetBodyParams(settings.body);

    std::vector<PatchGpu> patch_data;
    patch_data.reserve(leaves.size());
    for (planet::TileLodRecord const& leaf : leaves) {
        planet::TileKey const& key = leaf.key;
        if (!planet::isValid(key)) {
            error = "LOD selection contains an invalid tile key";
            return false;
        }
        if (leaf.conservative_distance_m > settings.max_render_distance_m) {
            ++metrics.distance_culled_count;
            continue;
        }
        if (settings.horizon_culling && !isAboveHorizon(leaf, view.camera_body_fixed_m, settings)) {
            ++metrics.horizon_culled_count;
            continue;
        }

        std::uint64_t const tiles_per_axis = std::uint64_t{1} << key.level;
        double const uv_size = 2.0 / static_cast<double>(tiles_per_axis);
        double const u_min = -1.0 + uv_size * static_cast<double>(key.x);
        double const v_min = -1.0 + uv_size * static_cast<double>(key.y);

        // The anchor direction by the shader's own face mapping, rounded to
        // float32 and used in that form on both sides, so that the vertex
        // offsets from it and the lattice anchor agree exactly.
        double const u_centre = u_min + 0.5 * uv_size;
        double const v_centre = v_min + 0.5 * uv_size;
        math::Vec3d face_vector;
        math::Vec3d du;
        math::Vec3d dv;
        switch (static_cast<std::uint8_t>(key.face)) {
        case 0:
            face_vector = {1.0, v_centre, -u_centre};
            du = {0.0, 0.0, -1.0};
            dv = {0.0, 1.0, 0.0};
            break;
        case 1:
            face_vector = {-1.0, v_centre, u_centre};
            du = {0.0, 0.0, 1.0};
            dv = {0.0, 1.0, 0.0};
            break;
        case 2:
            face_vector = {u_centre, 1.0, -v_centre};
            du = {1.0, 0.0, 0.0};
            dv = {0.0, 0.0, -1.0};
            break;
        case 3:
            face_vector = {u_centre, -1.0, v_centre};
            du = {1.0, 0.0, 0.0};
            dv = {0.0, 0.0, 1.0};
            break;
        case 4:
            face_vector = {u_centre, v_centre, 1.0};
            du = {1.0, 0.0, 0.0};
            dv = {0.0, 1.0, 0.0};
            break;
        default:
            face_vector = {-u_centre, v_centre, -1.0};
            du = {-1.0, 0.0, 0.0};
            dv = {0.0, 1.0, 0.0};
            break;
        }
        double const face_length = math::length(face_vector);
        math::Vec3d const exact_anchor = face_vector / face_length;
        std::array<float, 3> const anchor_float{static_cast<float>(exact_anchor[0]),
                                                static_cast<float>(exact_anchor[1]),
                                                static_cast<float>(exact_anchor[2])};
        math::Vec3d const anchor{static_cast<double>(anchor_float[0]), static_cast<double>(anchor_float[1]),
                                 static_cast<double>(anchor_float[2])};
        math::Vec3d const anchor_relative = anchor * settings.planet_radius_m - view.camera_body_fixed_m;
        // d normalize(V) / du = (e_u - d (d . e_u)) / |V|.
        math::Vec3d const tangent_u = (du - exact_anchor * math::dot(exact_anchor, du)) / face_length;
        math::Vec3d const tangent_v = (dv - exact_anchor * math::dot(exact_anchor, dv)) / face_length;
        PlanetLatticeAnchor const lattice = planetLatticeAnchor(settings.body, anchor_float);

        PatchGpu patch{};
        patch.uv_face[0] = static_cast<float>(u_min);
        patch.uv_face[1] = static_cast<float>(v_min);
        patch.uv_face[2] = static_cast<float>(uv_size);
        patch.uv_face[3] = static_cast<float>(key.face);
        patch.anchor_level[0] = static_cast<float>(anchor_relative[0]);
        patch.anchor_level[1] = static_cast<float>(anchor_relative[1]);
        patch.anchor_level[2] = static_cast<float>(anchor_relative[2]);
        patch.anchor_level[3] = static_cast<float>(key.level);
        patch.debug[2] = settings.skirt_depth_m;
        patch.debug[3] = std::clamp(
            static_cast<float>(leaf.screen_space_error_px) /
                std::max(0.01f, settings.max_sse_for_heatmap_px),
            0.0f, 1.0f);
        patch.anchor_direction[0] = anchor_float[0];
        patch.anchor_direction[1] = anchor_float[1];
        patch.anchor_direction[2] = anchor_float[2];
        patch.anchor_direction[3] = static_cast<float>(planetOctavesForLevel(settings.body, key.level));
        for (std::size_t axis = 0; axis < 3; ++axis) {
            patch.lattice_cell[axis] = lattice.cell[axis];
            patch.lattice_frac[axis] = lattice.frac[axis];
            patch.tangent_u[axis] = static_cast<float>(tangent_u[axis]);
            patch.tangent_v[axis] = static_cast<float>(tangent_v[axis]);
        }
        patch_data.push_back(patch);
    }

    metrics.visible_leaf_count = static_cast<std::uint32_t>(patch_data.size());
    metrics.triangles = static_cast<std::uint64_t>(patch_data.size()) *
                        (kVerticesPerPatch / 3U);
    if (patch_data.empty()) {
        return true;
    }

    {
        Diligent::MapHelper<FrameGpu> mapped{context_, frame_buffer_, Diligent::MAP_WRITE,
                                             Diligent::MAP_FLAG_DISCARD};
        if (static_cast<FrameGpu*>(mapped) == nullptr) {
            error = "failed to map the LOD frame constant buffer";
            return false;
        }
        *mapped = frame;
    }
    {
        Diligent::MapHelper<BodyGpu> mapped{context_, body_buffer_, Diligent::MAP_WRITE,
                                            Diligent::MAP_FLAG_DISCARD};
        if (static_cast<BodyGpu*>(mapped) == nullptr) {
            error = "failed to map the planet body constant buffer";
            return false;
        }
        *mapped = body;
    }
    {
        Diligent::MapHelper<PatchGpu> mapped{context_, patch_buffer_, Diligent::MAP_WRITE,
                                             Diligent::MAP_FLAG_DISCARD};
        PatchGpu* destination = mapped;
        if (destination == nullptr) {
            error = "failed to map the LOD patch instance buffer";
            return false;
        }
        std::memcpy(destination, patch_data.data(), patch_data.size() * sizeof(PatchGpu));
    }

    if (auto* profile = resources_->GetVariableByName(Diligent::SHADER_TYPE_PIXEL, "ring_profile")) {
        profile->Set(rings ? settings.ring_profile
                           : empty_profile_->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE));
    }
    QuerySlot* active_query = nullptr;
    pollAndBeginGpuTimer(active_query, metrics);
    context_->SetPipelineState(pipeline_);
    context_->CommitShaderResources(resources_,
                                    Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    Diligent::DrawAttribs draw;
    draw.NumVertices = kVerticesPerPatch;
    draw.NumInstances = static_cast<Diligent::Uint32>(patch_data.size());
    draw.Flags = Diligent::DRAW_FLAG_VERIFY_ALL;
    context_->Draw(draw);
    endGpuTimer(active_query);

    metrics.draw_calls = 1;
    metrics.gpu_sample_available = has_gpu_sample_;
    metrics.gpu_planet_pass_ms = latest_gpu_ms_;
    ++frame_index_;
    return true;
}

void PlanetPatchRenderer::pollAndBeginGpuTimer(QuerySlot*& active_slot,
                                               PlanetPatchMetrics& metrics) {
    metrics.gpu_timer_supported = gpu_timer_supported_;
    if (!gpu_timer_supported_) {
        return;
    }
    QuerySlot& slot = gpu_queries_[frame_index_ % gpu_queries_.size()];
    if (slot.pending) {
        Diligent::QueryDataDuration data;
        if (slot.query->GetData(&data, sizeof(data), true)) {
            slot.pending = false;
            if (data.Frequency > 0) {
                latest_gpu_ms_ = static_cast<double>(data.Duration) * 1000.0 /
                                 static_cast<double>(data.Frequency);
                has_gpu_sample_ = true;
                metrics.gpu_sample_new = true;
            }
        }
    }
    if (!slot.pending) {
        context_->BeginQuery(slot.query);
        active_slot = &slot;
    }
}

void PlanetPatchRenderer::endGpuTimer(QuerySlot* active_slot) {
    if (active_slot != nullptr) {
        context_->EndQuery(active_slot->query);
        active_slot->pending = true;
    }
}

} // namespace helios::examples
