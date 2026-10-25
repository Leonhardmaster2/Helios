// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Real-time Schwarzschild renderer for the black-hole lab.  On a
// caller-supplied Diligent/Vulkan device it runs:
//
//   1. trace     - one null geodesic per pixel centre, written as HDR scene
//                  radiance plus filter footprints (shaders/black_hole.slang);
//   2. refine    - extra samples only where one is not enough: the band
//                  around the critical curve and high-contrast disk pixels
//                  (shaders/black_hole_refine.slang);
//   3. display   - glare pyramid, exposure, tone map and dither
//                  (examples/common/hdr_post_chain).
//
// The renderer never creates a window or a swap chain, so the same object
// serves the interactive lab and the windowless capture/benchmark/validation
// routes.
#pragma once

#include "fullscreen_pass.hpp"
#include "hdr_post_chain.hpp"

#include "helios/blackhole/blackbody.hpp"
#include "helios/math/vec.hpp"
#include "shaders/abi.hpp"

#include <cstdint>
#include <string>

namespace helios::examples {

enum class BlackHoleSkyMode : std::uint8_t {
    Stars = 0,
    CelestialGrid = 1,
    Black = 2,
};

enum class BlackHoleOutput : std::uint8_t {
    Radiance = 0,
    // (radius, x, y, 2) for the first disk crossing, (direction, 1) for an
    // escaped ray, zeros for a captured one.  Read back by the validation route.
    GeometryRecord = 1,
    StepHeatMap = 2,
};

// A static observer.  The basis must be orthonormal; lengths are in M = GM/c^2.
struct BlackHoleCamera {
    math::Vec3d position_in_masses{0.0, -24.0, 3.4};
    math::Vec3d forward{0.0, 1.0, 0.0};
    math::Vec3d right{1.0, 0.0, 0.0};
    math::Vec3d up{0.0, 0.0, 1.0};
    double vertical_fov_rad = 0.6981317007977318; // 40 degrees
};

struct BlackHoleSettings {
    BlackHoleCamera camera{};
    double disk_time_in_masses = 0.0;

    bool disk_enabled = true;
    float disk_outer_radius_in_masses = 26.0f;
    // Maximum effective temperature of the Page-Thorne profile, before any
    // frequency shift.  Real disks peak far in the ultraviolet or X-ray band
    // and would look uniformly blue-white; the default is deliberately cool
    // so the radial and Doppler temperature structure is visible.
    float disk_peak_temperature_k = 5200.0f;
    float disk_opacity = 1.0f;
    float disk_turbulence = 0.6f;
    bool limb_darkening = true;

    bool doppler = true;
    bool gravitational_shift = true;
    bool curvature = true;

    BlackHoleSkyMode sky_mode = BlackHoleSkyMode::Stars;
    float star_intensity = 1.0f;
    float galaxy_intensity = 1.0f;

    float exposure_ev = 0.0f;
    float glare = 0.08f;
    bool tone_map = true;
    bool dither = true;

    int steps_per_half_turn = 32;
    int max_image_orders = 4;
    // Samples for a pixel that needs supersampling.  With adaptive sampling
    // only pixels the refinement pass flags pay for them; without it every
    // pixel does.
    int samples_per_pixel = 1;
    int critical_samples = 8;
    bool adaptive_sampling = true;
    // Tone-mapped luminance difference to a neighbour, in [0, 1], above which
    // a disk pixel is supersampled.
    float adaptive_contrast = 0.05f;
    BlackHoleOutput output = BlackHoleOutput::Radiance;
};

struct BlackHoleMetrics {
    std::uint32_t scene_width = 0;
    std::uint32_t scene_height = 0;
    bool gpu_timer_supported = false;
    bool gpu_sample_new = false;
    double gpu_trace_ms = 0.0; // trace + refine
    double gpu_post_ms = 0.0;
};

class BlackHoleRenderer {
  public:
    static constexpr float kDiskInnerRadiusInMasses = 6.0f; // Schwarzschild ISCO
    static constexpr double kMinimumCameraRadiusInMasses = 2.05;
    static constexpr std::uint32_t kBlackbodyTableSize = 512;

    struct CreateInfo {
        Diligent::TEXTURE_FORMAT output_format = Diligent::TEX_FORMAT_RGBA8_UNORM_SRGB;
        // RGBA32F scene target for geometry-record readback; RGBA16F otherwise.
        bool float32_scene = false;
    };

    [[nodiscard]] bool initialize(Diligent::IRenderDevice* device,
                                  Diligent::IDeviceContext* context, CreateInfo const& info,
                                  std::string& error);

    // Traces at scene_width x scene_height and, when `output` is not null,
    // resolves the result into it (a view of `output_format`, covered in
    // full).  A scene larger than the output is supersampling; a smaller one
    // is upscaling.  Leaves the output bound as the only render target.
    [[nodiscard]] bool render(BlackHoleSettings const& settings, std::uint32_t scene_width,
                              std::uint32_t scene_height, Diligent::ITextureView* output,
                              std::uint32_t output_width, std::uint32_t output_height,
                              BlackHoleMetrics& metrics, std::string& error);

    // The trace pass's first target (before refinement).
    [[nodiscard]] Diligent::ITexture* sceneTexture() const noexcept { return scene_; }
    [[nodiscard]] blackhole::BlackbodyTable const& blackbodyTable() const noexcept {
        return blackbody_table_;
    }

    // ln(luminance) that the display pipeline maps to 1 before exposure: an
    // unshifted black body at the disk's peak temperature.
    [[nodiscard]] double logReferenceLuminance(BlackHoleSettings const& settings) const noexcept;

  private:
    // Upload layout from shaders/abi.hpp, which the shader ABI test checks
    // against SPIR-V reflection.
    using FrameGpu = shaders::BlackHoleFrame;

    [[nodiscard]] bool createPipelines(std::string& error);
    [[nodiscard]] bool createBlackbodyTable(std::string& error);
    [[nodiscard]] bool ensureTargets(std::uint32_t width, std::uint32_t height, std::string& error);
    [[nodiscard]] FrameGpu makeFrame(BlackHoleSettings const& settings, std::uint32_t width,
                                     std::uint32_t height, bool refine) const;

    Diligent::RefCntAutoPtr<Diligent::IRenderDevice> device_;
    Diligent::RefCntAutoPtr<Diligent::IDeviceContext> context_;

    Diligent::RefCntAutoPtr<Diligent::IShader> vertex_shader_;
    Diligent::RefCntAutoPtr<Diligent::IShader> trace_shader_;
    Diligent::RefCntAutoPtr<Diligent::IShader> refine_shader_;
    Diligent::RefCntAutoPtr<Diligent::IPipelineState> trace_pipeline_;
    Diligent::RefCntAutoPtr<Diligent::IPipelineState> refine_pipeline_;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> trace_resources_;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> refine_resources_;

    Diligent::RefCntAutoPtr<Diligent::IBuffer> frame_buffer_;
    Diligent::RefCntAutoPtr<Diligent::ITexture> blackbody_texture_;
    blackhole::BlackbodyTable blackbody_table_{};
    double inverse_peak_flux_ = 0.0;

    Diligent::TEXTURE_FORMAT scene_format_ = Diligent::TEX_FORMAT_RGBA16_FLOAT;
    Diligent::RefCntAutoPtr<Diligent::ITexture> scene_;      // trace output
    Diligent::RefCntAutoPtr<Diligent::ITexture> footprints_; // trace output
    Diligent::RefCntAutoPtr<Diligent::ITexture> refined_;    // refine output
    std::uint32_t scene_width_ = 0;
    std::uint32_t scene_height_ = 0;

    HdrPostChain post_{};
    GpuPassTimer trace_timer_{};
    GpuPassTimer post_timer_{};
    std::uint64_t frame_index_ = 0;
};

} // namespace helios::examples
