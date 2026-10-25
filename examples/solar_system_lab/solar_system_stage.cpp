// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors

#include "solar_system_stage.hpp"

#include "solar_system.hpp"

#include "helios/math/vec.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <string>

namespace helios::examples {
namespace {

constexpr double kRadians = std::numbers::pi / 180.0;

// Rotation about the ecliptic pole: the v0.0.1 solar-system lab keeps every
// spin axis perpendicular to the ecliptic (axial tilts are a later addition).
[[nodiscard]] math::Vec3d rotateZ(math::Vec3d const& value, double angle) noexcept {
    double const c = std::cos(angle);
    double const s = std::sin(angle);
    return {c * value[0] - s * value[1], s * value[0] + c * value[1], value[2]};
}

[[nodiscard]] double bodyRotationRadians(int index, double days_since_j2000) noexcept {
    double const hours = index <= 0 ? kSun.rotation_period_hours
                                    : kSolarSystemPlanets[static_cast<std::size_t>(index - 1)]
                                          .rotation_period_hours;
    if (hours == 0.0) {
        return 0.0;
    }
    double const days_per_rotation = hours / 24.0;
    return 2.0 * std::numbers::pi * days_since_j2000 / days_per_rotation;
}

[[nodiscard]] float reliefAmplitude(PlanetBody const& body) noexcept {
    double const relief = std::max(body.platform_m + body.base_roughness_m * 1.5 +
                                       body.mountain_amplitude_m + body.detail_amplitude_m,
                                   1000.0);
    return static_cast<float>(std::min(relief, body.radius_m * 0.05));
}

} // namespace
SolarSystemView solarSystemView(SolarSystemSettings const& settings, std::uint32_t width,
                                std::uint32_t height) noexcept {
    SolarSystemView view;
    math::Vec3d const focus = solarSystemBodyPosition(settings.focus, settings.days_since_j2000);
    // Azimuth is measured around the sunward direction of the focus body:
    // a planet's default view then shows its lit hemisphere, not its night
    // side.  For the Sun the reference is the vernal equinox.
    math::Vec3d const world_up{0.0, 0.0, 1.0};
    math::Vec3d reference{1.0, 0.0, 0.0};
    if (settings.focus > 0 && math::length(focus) > 1.0) {
        reference = math::normalized(focus * -1.0);
    }
    math::Vec3d east = math::cross(world_up, reference);
    if (math::length(east) < 1.0e-9) {
        east = {0.0, 1.0, 0.0};
    }
    east = math::normalized(east);
    double const elevation = settings.camera.elevation_deg * kRadians;
    double const azimuth = settings.camera.azimuth_deg * kRadians;
    math::Vec3d const offset =
        reference * (std::cos(elevation) * std::cos(azimuth)) +
        east * (std::cos(elevation) * std::sin(azimuth)) + world_up * std::sin(elevation);
    double const distance = std::max(settings.camera.distance_m, 1.0);
    view.camera_position_m = focus + offset * distance;
    view.forward = math::normalized(focus - view.camera_position_m);
    math::Vec3d right = math::cross(view.forward, world_up);
    if (math::length(right) < 1.0e-9) {
        right = {1.0, 0.0, 0.0};
    }
    view.right = math::normalized(right);
    view.up = math::normalized(math::cross(view.right, view.forward));
    view.vertical_fov_rad = std::clamp(settings.camera.vertical_fov_deg, 5.0, 120.0) * kRadians;
    view.aspect = height > 0 ? static_cast<double>(width) / static_cast<double>(height) : 1.0;
    return view;
}

bool projectSolarSystemPoint(SolarSystemView const& view, math::Vec3d const& point_m,
                             std::uint32_t width, std::uint32_t height, double& x,
                             double& y) noexcept {
    math::Vec3d const relative = point_m - view.camera_position_m;
    double const depth = math::dot(relative, view.forward);
    if (depth <= 1.0e-6) {
        return false;
    }
    double const tangent = std::tan(view.vertical_fov_rad * 0.5);
    double const nx = math::dot(relative, view.right) / (depth * tangent * view.aspect);
    double const ny = math::dot(relative, view.up) / (depth * tangent);
    x = (nx * 0.5 + 0.5) * static_cast<double>(width);
    y = (0.5 - ny * 0.5) * static_cast<double>(height);
    return x >= 0.0 && x <= static_cast<double>(width) && y >= 0.0 &&
           y <= static_cast<double>(height);
}

bool SolarSystemStage::initialize(Diligent::IRenderDevice* device,
                                  Diligent::IDeviceContext* context,
                                  Diligent::TEXTURE_FORMAT output_format, std::string& error) {
    if (device == nullptr || context == nullptr) {
        error = "the solar-system stage needs a Diligent device and context";
        return false;
    }
    device_ = device;
    context_ = context;
    Diligent::SwapChainDesc formats;
    formats.ColorBufferFormat = Diligent::TEX_FORMAT_RGBA16_FLOAT;
    formats.DepthBufferFormat = Diligent::TEX_FORMAT_D32_FLOAT;
    if (!renderer_.initialize(device_, context_, formats, error)) {
        return false;
    }
    fullscreen_vertex_ = createEmbeddedShader(device_, "fullscreen.slang.vertex.spv",
                                              "Helios solar-system vertex shader",
                                              Diligent::SHADER_TYPE_VERTEX, error);
    if (fullscreen_vertex_ == nullptr) {
        return false;
    }
    if (!post_.initialize(device_, context_, fullscreen_vertex_, output_format, error)) {
        return false;
    }
    return createResolvePass(error);
}

bool SolarSystemStage::createResolvePass(std::string& error) {
    resolve_shader_ = createEmbeddedShader(device_, "solar_resolve.slang.fragment.spv",
                                           "Helios solar-system resolve shader",
                                           Diligent::SHADER_TYPE_PIXEL, error);
    if (resolve_shader_ == nullptr) {
        return false;
    }
    Diligent::ShaderResourceVariableDesc const variables[] = {
        {Diligent::SHADER_TYPE_PIXEL, "source", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE},
    };
    std::array const formats{Diligent::TEX_FORMAT_RGBA16_FLOAT};
    FullscreenPipelineInfo info;
    info.name = "Helios solar-system resolve pipeline";
    info.vertex_shader = fullscreen_vertex_;
    info.pixel_shader = resolve_shader_;
    info.target_formats = formats;
    info.variables = variables;
    resolve_pipeline_ = createFullscreenPipeline(device_, info);
    if (resolve_pipeline_ == nullptr) {
        error = "Diligent failed to create the solar-system resolve pipeline";
        return false;
    }
    resolve_pipeline_->CreateShaderResourceBinding(&resolve_resources_, true);
    if (resolve_resources_ == nullptr) {
        error = "Diligent failed to create the solar-system resolve binding";
        return false;
    }

    resolve4_shader_ = createEmbeddedShader(device_, "solar_resolve4.slang.fragment.spv",
                                            "Helios solar-system 4x resolve shader",
                                            Diligent::SHADER_TYPE_PIXEL, error);
    if (resolve4_shader_ == nullptr) {
        return false;
    }
    FullscreenPipelineInfo info4;
    info4.name = "Helios solar-system 4x resolve pipeline";
    info4.vertex_shader = fullscreen_vertex_;
    info4.pixel_shader = resolve4_shader_;
    info4.target_formats = formats;
    info4.variables = variables;
    resolve4_pipeline_ = createFullscreenPipeline(device_, info4);
    if (resolve4_pipeline_ == nullptr) {
        error = "Diligent failed to create the solar-system 4x resolve pipeline";
        return false;
    }
    resolve4_pipeline_->CreateShaderResourceBinding(&resolve4_resources_, true);
    if (resolve4_resources_ == nullptr) {
        error = "Diligent failed to create the solar-system 4x resolve binding";
        return false;
    }
    return true;
}

bool SolarSystemStage::ensureResolveTarget(std::uint32_t width, std::uint32_t height,
                                           std::string& error) {
    if (resolved_ != nullptr && width == resolved_width_ && height == resolved_height_) {
        return true;
    }
    resolved_ = createRenderTexture(device_, "Helios solar-system resolved", width, height,
                                    Diligent::TEX_FORMAT_RGBA16_FLOAT, post_.linearSampler());
    if (resolved_ == nullptr) {
        error = "Diligent failed to create the solar-system resolve target";
        return false;
    }
    resolved_width_ = width;
    resolved_height_ = height;
    return true;
}

bool SolarSystemStage::resolveSupersample(Diligent::ITexture* source, std::uint32_t factor,
                                          std::uint32_t width, std::uint32_t height,
                                          std::string& error) {
    if (source == nullptr || resolved_ == nullptr) {
        error = "the solar-system resolve pass has no source or target";
        return false;
    }
    bool const four = factor >= 4;
    Diligent::IPipelineState* pipeline = four ? resolve4_pipeline_ : resolve_pipeline_;
    Diligent::IShaderResourceBinding* resources = four ? resolve4_resources_ : resolve_resources_;
    if (pipeline == nullptr || resources == nullptr) {
        error = "the solar-system resolve pipeline is not initialized";
        return false;
    }
    Diligent::ITextureView* source_view = source->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
    if (source_view == nullptr) {
        error = "the solar-system resolve source has no shader-resource view";
        return false;
    }
    if (!bindVariable(resources, "source", source_view, error)) {
        return false;
    }
    Diligent::ITextureView* target = resolved_->GetDefaultView(Diligent::TEXTURE_VIEW_RENDER_TARGET);
    drawFullscreen(context_, {&target, 1}, width, height, pipeline, resources);
    return true;
}

bool SolarSystemStage::ensureTargets(std::uint32_t width, std::uint32_t height,
                                     std::string& error) {
    if (scene_ != nullptr && width == target_width_ && height == target_height_) {
        return true;
    }
    scene_.Release();
    depth_.Release();
    scene_ = createRenderTexture(device_, "Helios solar-system scene", width, height,
                                 Diligent::TEX_FORMAT_RGBA16_FLOAT, post_.linearSampler());
    Diligent::TextureDesc depth_desc;
    depth_desc.Name = "Helios solar-system depth";
    depth_desc.Type = Diligent::RESOURCE_DIM_TEX_2D;
    depth_desc.Width = width;
    depth_desc.Height = height;
    depth_desc.Format = Diligent::TEX_FORMAT_D32_FLOAT;
    depth_desc.BindFlags = Diligent::BIND_DEPTH_STENCIL;
    device_->CreateTexture(depth_desc, nullptr, &depth_);
    if (scene_ == nullptr || depth_ == nullptr) {
        error = "Diligent failed to create the solar-system scene targets";
        return false;
    }
    target_width_ = width;
    target_height_ = height;
    return true;
}

bool SolarSystemStage::ensureSelector(Body& body, double radius_m, float lod_error_px,
                                      std::string& error) {
    if (body.selector.has_value() && body.radius_m == radius_m &&
        body.lod_error_px == lod_error_px) {
        return true;
    }
    planet::QuadtreeLodConfig config = defaultPlanetLod(radius_m);
    config.split_threshold_px = std::clamp(static_cast<double>(lod_error_px), 0.5, 16.0);
    config.merge_threshold_px = config.split_threshold_px * (2.0 / 3.0);
    config.root_geometric_error_m = std::max(radius_m * 0.03, 10.0);
    config.max_level = 18;
    config.leaf_budget = 8192;
    if (body.selector.has_value()) {
        auto changed = body.selector->setConfig(config, true);
        if (changed.failed()) {
            error = "solar-system LOD policy: " + changed.error().to_string();
            return false;
        }
    } else {
        auto created = planet::QuadtreeLodSelector::create(config);
        if (created.failed()) {
            error = "solar-system LOD selector: " + created.error().to_string();
            return false;
        }
        body.selector.emplace(std::move(created.value()));
    }
    body.radius_m = radius_m;
    body.lod_error_px = lod_error_px;
    return true;
}

bool SolarSystemStage::drawBody(Body& body, PlanetPatchView const& view,
                                PlanetPatchSettings const& patches, bool settle,
                                SolarSystemStageMetrics& metrics, std::string& error) {
    planet::LodViewInput lod_view;
    lod_view.camera_position_body_fixed_m = view.camera_body_fixed_m;
    lod_view.viewport_height_px = view.viewport_height_px;
    lod_view.vertical_fov_rad = view.vertical_fov_rad;
    if (settle) {
        for (int pass = 0; pass < 8; ++pass) {
            auto selected = body.selector->select(lod_view);
            if (selected.failed()) {
                error = "solar-system LOD selection: " + selected.error().to_string();
                return false;
            }
            body.selection = std::move(selected.value());
            if (pass > 0 && body.selection.splits_from_previous == 0 &&
                body.selection.merges_from_previous == 0) {
                break;
            }
        }
    } else {
        auto selected = body.selector->select(lod_view);
        if (selected.failed()) {
            error = "solar-system LOD selection: " + selected.error().to_string();
            return false;
        }
        body.selection = std::move(selected.value());
    }
    PlanetPatchMetrics patch_metrics;
    if (!renderer_.render(body.selection.leaves, view, patches, patch_metrics, error)) {
        return false;
    }
    metrics.leaves += static_cast<std::uint32_t>(body.selection.leaves.size());
    metrics.draw_calls += patch_metrics.draw_calls;
    metrics.triangles += patch_metrics.triangles;
    metrics.max_error_px = std::max(metrics.max_error_px, body.selection.max_leaf_sse_px);
    metrics.gpu_sample_new = metrics.gpu_sample_new || patch_metrics.gpu_sample_new;
    if (patch_metrics.gpu_sample_new) {
        metrics.gpu_scene_ms += patch_metrics.gpu_planet_pass_ms;
    }
    return true;
}

bool SolarSystemStage::render(SolarSystemSettings const& settings, std::uint32_t width,
                              std::uint32_t height, bool settle, Diligent::ITextureView* output,
                              SolarSystemStageMetrics& metrics, std::string& error) {
    metrics = {};
    if (output == nullptr || width == 0 || height == 0) {
        error = "the solar-system stage has no output or a zero-sized image";
        return false;
    }
    int const requested = std::clamp(settings.supersample, 1, 4);
    int const supersample = requested >= 4 ? 4 : (requested >= 2 ? 2 : 1);
    std::uint32_t const render_width = width * static_cast<std::uint32_t>(supersample);
    std::uint32_t const render_height = height * static_cast<std::uint32_t>(supersample);
    if (!ensureTargets(render_width, render_height, error)) {
        return false;
    }
    metrics.render_width = render_width;
    metrics.render_height = render_height;
    metrics.scene_texture_width = scene_->GetDesc().Width;
    metrics.scene_texture_height = scene_->GetDesc().Height;

    SolarSystemView const view = solarSystemView(settings, render_width, render_height);

    Diligent::ITextureView* color = scene_->GetDefaultView(Diligent::TEXTURE_VIEW_RENDER_TARGET);
    Diligent::ITextureView* depth = depth_->GetDefaultView(Diligent::TEXTURE_VIEW_DEPTH_STENCIL);
    context_->SetRenderTargets(1, &color, depth, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    float const clear[4]{0.0015f, 0.0025f, 0.006f, 1.0f};
    context_->ClearRenderTarget(color, clear, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    context_->ClearDepthStencil(depth, Diligent::CLEAR_DEPTH_FLAG, 0.0f, 0,
                                Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    Diligent::Viewport const viewport{0.0f, 0.0f, static_cast<float>(render_width),
                                      static_cast<float>(render_height), 0.0f, 1.0f};
    context_->SetViewports(1, &viewport, render_width, render_height);

    auto const selection_begin = std::chrono::steady_clock::now();
    for (int index = 0; index < static_cast<int>(kSolarSystemBodyCount); ++index) {
        double const radius = solarSystemDisplayRadius(index, settings.planet_radius_scale,
                                                       settings.sun_radius_scale);
        if (radius <= 0.0) {
            continue;
        }
        math::Vec3d const position = solarSystemBodyPosition(index, settings.days_since_j2000);
        PlanetBody const body = solarSystemBody(index, 11u + static_cast<std::uint32_t>(index));
        if (!ensureSelector(bodies_[static_cast<std::size_t>(index)], radius,
                            settings.lod_error_px, error)) {
            return false;
        }

        PlanetPatchSettings patches{};
        patches.body = body;
        patches.planet_radius_m = radius;
        // The lab's default 2e9 m is a planet-local limit; a solar-system
        // view is up to ~30 AU, so the distance cull must cover the system.
        patches.max_render_distance_m = 2.0e14;
        patches.relief_amplitude_m = (body.star || body.gas_giant) ? 0.0f : reliefAmplitude(body);
        patches.skirt_depth_m = (body.star || body.gas_giant) ? 0.0f : 300.0f;
        patches.tone_map_output = false;
        patches.time_s = settings.days_since_j2000 * 86400.0;
        patches.ambient = body.atmosphere ? 0.10f : 0.02f;
        patches.ambient_color = {1.0f, 1.0f, 1.0f};
        // The body-fixed frame turns with the body's sidereal rotation.
        double const rotation = bodyRotationRadians(index, settings.days_since_j2000);
        if (body.star) {
            patches.light_direction = {0.0, 0.0, 1.0};
        } else {
            patches.light_direction =
                rotateZ(math::normalized(position * -1.0), -rotation);
        }
        if (index == 3) {
            // Earth's cloud deck: the terrain pass draws its shadows, so it
            // needs the layer even though the sky pass is not used here.  The
            // cloud heights are exaggerated with the body, otherwise they sit
            // inside a display radius hundreds of times larger.
            double const scale = std::max(settings.planet_radius_scale, 0.01);
            patches.cloud_base_m = radius + 3'000.0 * scale;
            patches.cloud_top_m = radius + 7'500.0 * scale;
            patches.cloud_cover = 0.55;
            patches.cloud_extinction = 0.03;
            patches.cloud_drift_rad = settings.days_since_j2000 * 0.02;
        }

        PlanetPatchView patch_view;
        patch_view.camera_body_fixed_m = rotateZ(view.camera_position_m - position, -rotation);
        patch_view.forward_body_fixed = rotateZ(view.forward, -rotation);
        patch_view.right_body_fixed = rotateZ(view.right, -rotation);
        patch_view.up_body_fixed = rotateZ(view.up, -rotation);
        patch_view.vertical_fov_rad = view.vertical_fov_rad;
        patch_view.near_plane_m = 0.25;
        patch_view.viewport_width_px = render_width;
        patch_view.viewport_height_px = render_height;
        if (!drawBody(bodies_[static_cast<std::size_t>(index)], patch_view, patches, settle,
                      metrics, error)) {
            return false;
        }
        ++metrics.bodies_drawn;
    }
    metrics.selection_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - selection_begin)
            .count();

    HdrDisplaySettings display;
    // Sunlit planet surfaces sit near albedo / pi; the Sun itself is much
    // brighter.  This scale puts the planets in the middle of the tone curve
    // and lets the glare carry the Sun.
    display.base_exposure = 2.6f;
    display.exposure_ev = settings.exposure_ev;
    display.glare = settings.glare;
    display.tone_map = settings.tone_map;

    Diligent::ITexture* display_source = scene_;
    if (supersample > 1) {
        if (!ensureResolveTarget(width, height, error)) {
            return false;
        }
        if (!resolveSupersample(scene_, static_cast<std::uint32_t>(supersample), width, height,
                                error)) {
            return false;
        }
        display_source = resolved_;
    }
    if (!post_.resolve(display_source, output, width, height, display, error)) {
        return false;
    }
    metrics.post_ms = 0.0; // reported by the caller when it has a pass timer
    return true;
}

} // namespace helios::examples
