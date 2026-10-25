// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "black_hole_renderer.hpp"

#include "helios/blackhole/thin_disk.hpp"

#include "MapHelper.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace helios::examples {
namespace {

// The table spans every temperature the lab can produce: the coolest visible
// emitter up to a strongly blueshifted hot disk.  Above it the shader
// continues along the Rayleigh-Jeans limit.
constexpr double kTableMinimumKelvin = 500.0;
constexpr double kTableMaximumKelvin = 1.0e6;

// Fraction of the outer radius where the disk starts to thin out.
constexpr float kOuterFadeStart = 0.82f;

// Linear scale applied at 0 EV.  The reference luminance (an unshifted black
// body at the disk's peak temperature) is mapped two stops above display
// white: most of the disk is redshifted and cooler than its peak, so this
// keeps the outer and receding parts visible while the tone map rolls the
// Doppler-boosted side off towards white.
constexpr float kBaseExposure = 3.4f;

void store(float destination[4], math::Vec3d const& value, double scale, double w) {
    destination[0] = static_cast<float>(value[0] * scale);
    destination[1] = static_cast<float>(value[1] * scale);
    destination[2] = static_cast<float>(value[2] * scale);
    destination[3] = static_cast<float>(w);
}

[[nodiscard]] bool finiteVector(math::Vec3d const& value) noexcept {
    return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
}

} // namespace

bool BlackHoleRenderer::initialize(Diligent::IRenderDevice* device,
                                   Diligent::IDeviceContext* context, CreateInfo const& info,
                                   std::string& error) {
    if (device == nullptr || context == nullptr) {
        error = "black-hole renderer requires a valid Diligent device and context";
        return false;
    }
    if (device->GetDeviceInfo().Type != Diligent::RENDER_DEVICE_TYPE_VULKAN) {
        error = "black-hole renderer is a Vulkan-only v0.0.1 path";
        return false;
    }
    device_ = device;
    context_ = context;
    scene_format_ = info.float32_scene ? Diligent::TEX_FORMAT_RGBA32_FLOAT
                                       : Diligent::TEX_FORMAT_RGBA16_FLOAT;
    inverse_peak_flux_ = 1.0 / blackhole::novikovThornePeak().flux;

    frame_buffer_ = createDynamicUniformBuffer(device_, "Helios black-hole frame constants",
                                               sizeof(FrameGpu));
    if (frame_buffer_ == nullptr) {
        error = "Diligent failed to create the black-hole frame constant buffer";
        return false;
    }
    vertex_shader_ = createEmbeddedShader(device_, "fullscreen.slang.vertex.spv",
                                          "Helios full-screen vertex shader",
                                          Diligent::SHADER_TYPE_VERTEX, error);
    trace_shader_ = createEmbeddedShader(device_, "black_hole.slang.fragment.spv",
                                         "Helios black-hole geodesic trace shader",
                                         Diligent::SHADER_TYPE_PIXEL, error);
    refine_shader_ = createEmbeddedShader(device_, "black_hole_refine.slang.fragment.spv",
                                          "Helios black-hole adaptive refinement shader",
                                          Diligent::SHADER_TYPE_PIXEL, error);
    if (vertex_shader_ == nullptr || trace_shader_ == nullptr || refine_shader_ == nullptr) {
        return false;
    }
    if (!createBlackbodyTable(error) || !createPipelines(error) ||
        !post_.initialize(device_, context_, vertex_shader_, info.output_format, error)) {
        return false;
    }
    trace_timer_.initialize(device_, "Helios black-hole trace and refine duration");
    post_timer_.initialize(device_, "Helios black-hole display duration");
    return true;
}

bool BlackHoleRenderer::createBlackbodyTable(std::string& error) {
    blackbody_table_ = blackhole::makeBlackbodyTable(kTableMinimumKelvin, kTableMaximumKelvin,
                                                     kBlackbodyTableSize);
    if (!blackbody_table_.valid()) {
        error = "failed to build the black-body colour table";
        return false;
    }

    Diligent::TextureDesc desc;
    desc.Name = "Helios black-body colour table";
    desc.Type = Diligent::RESOURCE_DIM_TEX_2D;
    desc.Width = blackbody_table_.count;
    desc.Height = 1;
    desc.MipLevels = 1;
    desc.Format = Diligent::TEX_FORMAT_RGBA32_FLOAT;
    desc.Usage = Diligent::USAGE_IMMUTABLE;
    desc.BindFlags = Diligent::BIND_SHADER_RESOURCE;

    Diligent::TextureSubResData level;
    level.pData = blackbody_table_.rgba.data();
    level.Stride = static_cast<Diligent::Uint64>(blackbody_table_.count) * 4U * sizeof(float);
    Diligent::TextureData data;
    data.pSubResources = &level;
    data.NumSubresources = 1;
    device_->CreateTexture(desc, &data, &blackbody_texture_);
    if (blackbody_texture_ == nullptr) {
        error = "Diligent failed to create the black-body colour table texture";
        return false;
    }
    return true;
}

bool BlackHoleRenderer::createPipelines(std::string& error) {
    std::array const trace_formats{scene_format_, Diligent::TEX_FORMAT_RGBA16_FLOAT};
    FullscreenPipelineInfo trace_info;
    trace_info.name = "Helios black-hole geodesic trace pipeline";
    trace_info.vertex_shader = vertex_shader_;
    trace_info.pixel_shader = trace_shader_;
    trace_info.target_formats = trace_formats;
    trace_pipeline_ = createFullscreenPipeline(device_, trace_info);

    std::array const refine_formats{scene_format_};
    Diligent::ShaderResourceVariableDesc const refine_variables[] = {
        {Diligent::SHADER_TYPE_PIXEL, "scene", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE},
        {Diligent::SHADER_TYPE_PIXEL, "footprint_map",
         Diligent::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE}};
    FullscreenPipelineInfo refine_info;
    refine_info.name = "Helios black-hole adaptive refinement pipeline";
    refine_info.vertex_shader = vertex_shader_;
    refine_info.pixel_shader = refine_shader_;
    refine_info.target_formats = refine_formats;
    refine_info.variables = refine_variables;
    refine_pipeline_ = createFullscreenPipeline(device_, refine_info);

    if (trace_pipeline_ == nullptr || refine_pipeline_ == nullptr) {
        error = "Diligent failed to create a black-hole graphics pipeline";
        return false;
    }
    auto* const table_view =
        blackbody_texture_->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
    if (!bindStaticVariable(trace_pipeline_, "frame", frame_buffer_, error) ||
        !bindStaticVariable(trace_pipeline_, "blackbody_table", table_view, error) ||
        !bindStaticVariable(refine_pipeline_, "frame", frame_buffer_, error) ||
        !bindStaticVariable(refine_pipeline_, "blackbody_table", table_view, error)) {
        return false;
    }
    trace_pipeline_->CreateShaderResourceBinding(&trace_resources_, true);
    if (trace_resources_ == nullptr) {
        error = "Diligent failed to create the black-hole trace resource binding";
        return false;
    }
    return true;
}

bool BlackHoleRenderer::ensureTargets(std::uint32_t width, std::uint32_t height,
                                      std::string& error) {
    if (scene_ != nullptr && width == scene_width_ && height == scene_height_) {
        return true;
    }
    // The refine binding holds references to the old textures; drop it first.
    refine_resources_.Release();
    scene_ = createRenderTexture(device_, "Helios black-hole HDR scene", width, height,
                                 scene_format_, post_.linearSampler());
    footprints_ = createRenderTexture(device_, "Helios black-hole filter footprints", width,
                                      height, Diligent::TEX_FORMAT_RGBA16_FLOAT, nullptr);
    refined_ = createRenderTexture(device_, "Helios black-hole refined HDR scene", width, height,
                                   scene_format_, post_.linearSampler());
    if (scene_ == nullptr || footprints_ == nullptr || refined_ == nullptr) {
        error = "Diligent failed to create the black-hole scene targets";
        return false;
    }
    refine_pipeline_->CreateShaderResourceBinding(&refine_resources_, true);
    if (!bindVariable(refine_resources_, "scene",
                      scene_->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE), error) ||
        !bindVariable(refine_resources_, "footprint_map",
                      footprints_->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE),
                      error)) {
        return false;
    }
    scene_width_ = width;
    scene_height_ = height;
    return true;
}

double BlackHoleRenderer::logReferenceLuminance(BlackHoleSettings const& settings) const noexcept {
    return blackhole::sampleBlackbodyLogLuminance(
        blackbody_table_, static_cast<double>(settings.disk_peak_temperature_k));
}

BlackHoleRenderer::FrameGpu BlackHoleRenderer::makeFrame(BlackHoleSettings const& settings,
                                                         std::uint32_t width,
                                                         std::uint32_t height, bool refine) const {
    FrameGpu frame{};
    BlackHoleCamera const& camera = settings.camera;
    double const tangent = std::tan(camera.vertical_fov_rad * 0.5);
    double const aspect = static_cast<double>(width) / static_cast<double>(height);
    store(frame.camera_position, camera.position_in_masses, 1.0, settings.disk_time_in_masses);
    store(frame.camera_right, camera.right, tangent * aspect, 0.0);
    store(frame.camera_up, camera.up, tangent, 0.0);
    store(frame.camera_forward, camera.forward, 1.0, 0.0);

    frame.viewport[0] = static_cast<float>(width);
    frame.viewport[1] = static_cast<float>(height);
    frame.viewport[2] = 1.0f / static_cast<float>(width);
    frame.viewport[3] = 1.0f / static_cast<float>(height);

    float const outer_radius = std::max(settings.disk_outer_radius_in_masses,
                                        kDiskInnerRadiusInMasses + 1.0f);
    frame.disk[0] = kDiskInnerRadiusInMasses;
    frame.disk[1] = outer_radius;
    frame.disk[2] = settings.disk_peak_temperature_k;
    frame.disk[3] = settings.disk_enabled ? std::clamp(settings.disk_opacity, 0.0f, 1.0f) : 0.0f;

    frame.disk_detail[0] = std::clamp(settings.disk_turbulence, 0.0f, 0.95f);
    frame.disk_detail[1] = settings.limb_darkening ? 1.0f : 0.0f;
    frame.disk_detail[2] = static_cast<float>(inverse_peak_flux_);
    frame.disk_detail[3] = kOuterFadeStart;

    frame.sky[0] = std::max(settings.star_intensity, 0.0f);
    frame.sky[1] = std::max(settings.galaxy_intensity, 0.0f);
    frame.sky[2] = static_cast<float>(settings.sky_mode);

    double const log_minimum = std::log(blackbody_table_.minKelvin);
    double const log_maximum = std::log(blackbody_table_.maxKelvin);
    frame.radiometry[0] = static_cast<float>(logReferenceLuminance(settings));
    frame.radiometry[1] = static_cast<float>(log_minimum);
    frame.radiometry[2] = static_cast<float>(1.0 / (log_maximum - log_minimum));
    frame.radiometry[3] = static_cast<float>(blackbody_table_.count);

    frame.integration[0] = static_cast<float>(
        std::numbers::pi / static_cast<double>(std::clamp(settings.steps_per_half_turn, 8, 128)));
    frame.integration[1] = static_cast<float>(std::clamp(settings.max_image_orders, 1, 8));
    frame.integration[2] = static_cast<float>(std::clamp(settings.samples_per_pixel, 1, 16));
    frame.integration[3] = static_cast<float>(std::clamp(settings.critical_samples, 0, 32));

    frame.effects[0] = settings.doppler ? 1.0f : 0.0f;
    frame.effects[1] = settings.gravitational_shift ? 1.0f : 0.0f;
    frame.effects[2] = settings.curvature ? 1.0f : 0.0f;
    frame.effects[3] = static_cast<float>(settings.output);

    frame.refine[0] = kBaseExposure * std::exp2(settings.exposure_ev);
    frame.refine[1] = std::clamp(settings.adaptive_contrast, 0.001f, 1.0f);
    frame.refine[2] = refine ? 1.0f : 0.0f;
    return frame;
}

bool BlackHoleRenderer::render(BlackHoleSettings const& settings, std::uint32_t scene_width,
                               std::uint32_t scene_height, Diligent::ITextureView* output,
                               std::uint32_t output_width, std::uint32_t output_height,
                               BlackHoleMetrics& metrics, std::string& error) {
    metrics = {};
    metrics.gpu_timer_supported = trace_timer_.supported();
    if (trace_pipeline_ == nullptr) {
        error = "black-hole renderer was not initialized";
        return false;
    }
    BlackHoleCamera const& camera = settings.camera;
    bool const camera_valid =
        finiteVector(camera.position_in_masses) && finiteVector(camera.forward) &&
        finiteVector(camera.right) && finiteVector(camera.up) &&
        math::length(camera.position_in_masses) >= kMinimumCameraRadiusInMasses &&
        std::isfinite(camera.vertical_fov_rad) && camera.vertical_fov_rad > 0.0 &&
        camera.vertical_fov_rad < std::numbers::pi;
    if (!camera_valid || scene_width == 0 || scene_height == 0 || scene_width > 16384 ||
        scene_height > 16384 || !std::isfinite(settings.disk_time_in_masses) ||
        !std::isfinite(settings.disk_peak_temperature_k) ||
        settings.disk_peak_temperature_k < static_cast<float>(kTableMinimumKelvin)) {
        error = "black-hole renderer received an invalid camera, resolution or disk temperature";
        return false;
    }
    if (output != nullptr && (output_width == 0 || output_height == 0)) {
        error = "black-hole renderer received an empty output target";
        return false;
    }
    if (!ensureTargets(scene_width, scene_height, error)) {
        return false;
    }
    metrics.scene_width = scene_width;
    metrics.scene_height = scene_height;

    // The diagnostic outputs are per-pixel-centre records, never refined.
    bool const refine =
        settings.adaptive_sampling && settings.output == BlackHoleOutput::Radiance &&
        (settings.samples_per_pixel > 1 || settings.critical_samples > 0);
    {
        Diligent::MapHelper<FrameGpu> mapped{context_, frame_buffer_, Diligent::MAP_WRITE,
                                             Diligent::MAP_FLAG_DISCARD};
        if (static_cast<FrameGpu*>(mapped) == nullptr) {
            error = "failed to map the black-hole frame constant buffer";
            return false;
        }
        *mapped = makeFrame(settings, scene_width, scene_height, refine);
    }

    bool const trace_sample = trace_timer_.begin(context_, frame_index_);
    std::array<Diligent::ITextureView*, 2> const trace_targets{
        scene_->GetDefaultView(Diligent::TEXTURE_VIEW_RENDER_TARGET),
        footprints_->GetDefaultView(Diligent::TEXTURE_VIEW_RENDER_TARGET)};
    drawFullscreen(context_, trace_targets, scene_width, scene_height, trace_pipeline_,
                   trace_resources_);
    if (refine) {
        Diligent::ITextureView* const refined_target =
            refined_->GetDefaultView(Diligent::TEXTURE_VIEW_RENDER_TARGET);
        drawFullscreen(context_, {&refined_target, 1}, scene_width, scene_height,
                       refine_pipeline_, refine_resources_);
    }
    trace_timer_.end(context_);

    bool post_sample = false;
    if (output != nullptr) {
        post_sample = post_timer_.begin(context_, frame_index_);
        HdrDisplaySettings display;
        display.base_exposure = kBaseExposure;
        display.exposure_ev = settings.exposure_ev;
        display.glare = settings.glare;
        display.tone_map = settings.tone_map;
        display.dither = settings.dither;
        bool const resolved = post_.resolve(refine ? refined_ : scene_, output, output_width,
                                            output_height, display, error);
        post_timer_.end(context_);
        if (!resolved) {
            return false;
        }
    }

    metrics.gpu_sample_new = trace_sample || post_sample;
    metrics.gpu_trace_ms = trace_timer_.latestMs();
    metrics.gpu_post_ms = post_timer_.latestMs();
    ++frame_index_;
    return true;
}

} // namespace helios::examples
