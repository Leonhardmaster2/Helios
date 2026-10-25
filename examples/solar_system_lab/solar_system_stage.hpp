// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// The solar-system scene of helios_solar_system: the Sun and the eight
// planets, all drawn through the planet LOD lab's cube-sphere patch renderer.
// A star is a PlanetBody with `star = true` (emissive photosphere in
// shaders/planet_patch.slang); a planet is the same body with terrain or a
// gas-giant band deck.  Bodies keep camera-relative binary64 positions and
// their own quadtree selectors, so the camera can leave a whole-system view
// and end up at eye level on one world without a scene load.
#pragma once

#include "planet_patch_renderer.hpp"
#include "planet_scene.hpp"
#include "solar_system.hpp"
#include "solar_system_data.hpp"

#include "fullscreen_pass.hpp"
#include "hdr_post_chain.hpp"

#include "helios/math/vec.hpp"
#include "helios/planet/quadtree_lod.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace helios::examples {

struct SolarSystemCamera {
    double distance_m = 2.0 * kAstronomicalUnitM;
    double elevation_deg = 50.0;
    double azimuth_deg = -25.0;
    double vertical_fov_deg = 55.0;
};

struct SolarSystemSettings {
    double days_since_j2000 = 0.0;
    int focus = 0; // 0 = Sun, 1 = Mercury ... 8 = Neptune
    SolarSystemCamera camera{};
    // Display exaggeration of the planets' radii (1 = true scale).  The Sun
    // has its own factor so the disk does not swallow the inner planets.
    double planet_radius_scale = 500.0;
    double sun_radius_scale = 12.0;
    // Render the 3-D scene at this multiple of the output resolution and let
    // the HDR resolve downsample it: 2 = four samples per output pixel.  The
    // Sun and the planet limbs are the main beneficiaries.
    int supersample = 2;
    float lod_error_px = 3.0f;
    float exposure_ev = 0.0f;
    float glare = 0.18f;
    bool tone_map = true;
    bool show_orbits = true; // drawn by the application as an overlay
};

struct SolarSystemStageMetrics {
    std::uint32_t bodies_drawn = 0;
    std::uint32_t leaves = 0;
    std::uint32_t draw_calls = 0;
    std::uint64_t triangles = 0;
    double max_error_px = 0.0;
    double selection_ms = 0.0;
    double post_ms = 0.0;
    bool gpu_sample_new = false;
    double gpu_scene_ms = 0.0;
    std::uint32_t render_width = 0; // before the supersample resolve
    std::uint32_t render_height = 0;
    std::uint32_t scene_texture_width = 0; // what the device actually allocated
    std::uint32_t scene_texture_height = 0;
};

// The camera basis shared by the stage and the application's orbit overlay.
struct SolarSystemView {
    math::Vec3d camera_position_m{};
    math::Vec3d forward{0.0, 1.0, 0.0};
    math::Vec3d right{1.0, 0.0, 0.0};
    math::Vec3d up{0.0, 0.0, 1.0};
    double vertical_fov_rad = 0.7853981633974483;
    double aspect = 1.0;
};

[[nodiscard]] SolarSystemView solarSystemView(SolarSystemSettings const& settings,
                                              std::uint32_t width, std::uint32_t height) noexcept;

// Projects a heliocentric point to pixels.  Returns false when the point is
// behind the camera or outside the image.
[[nodiscard]] bool projectSolarSystemPoint(SolarSystemView const& view, math::Vec3d const& point_m,
                                           std::uint32_t width, std::uint32_t height, double& x,
                                           double& y) noexcept;

class SolarSystemStage {
  public:
    [[nodiscard]] bool initialize(Diligent::IRenderDevice* device,
                                  Diligent::IDeviceContext* context,
                                  Diligent::TEXTURE_FORMAT output_format, std::string& error);

    // Draws the system into `output` (covered in full).  With `settle` the
    // LOD selection is repeated until a still camera causes no more splits or
    // merges, which is what a capture wants.
    [[nodiscard]] bool render(SolarSystemSettings const& settings, std::uint32_t width,
                              std::uint32_t height, bool settle, Diligent::ITextureView* output,
                              SolarSystemStageMetrics& metrics, std::string& error);

  private:
    struct Body {
        std::optional<planet::QuadtreeLodSelector> selector{};
        planet::LodSelection selection{};
        double radius_m = 0.0; // display radius the selector was built for
        float lod_error_px = 0.0f;
    };

    [[nodiscard]] bool ensureTargets(std::uint32_t width, std::uint32_t height, std::string& error);
    [[nodiscard]] bool createResolvePass(std::string& error);
    [[nodiscard]] bool ensureResolveTarget(std::uint32_t width, std::uint32_t height,
                                           std::string& error);
    [[nodiscard]] bool resolveSupersample(Diligent::ITexture* source, std::uint32_t factor,
                                          std::uint32_t width, std::uint32_t height,
                                          std::string& error);
    [[nodiscard]] bool ensureSelector(Body& body, double radius_m, float lod_error_px,
                                      std::string& error);
    [[nodiscard]] bool drawBody(Body& body, PlanetPatchView const& view,
                                PlanetPatchSettings const& patches, bool settle,
                                SolarSystemStageMetrics& metrics, std::string& error);

    Diligent::RefCntAutoPtr<Diligent::IRenderDevice> device_;
    Diligent::RefCntAutoPtr<Diligent::IDeviceContext> context_;
    PlanetPatchRenderer renderer_{};

    Diligent::RefCntAutoPtr<Diligent::ITexture> scene_;
    Diligent::RefCntAutoPtr<Diligent::ITexture> depth_;
    std::uint32_t target_width_ = 0;
    std::uint32_t target_height_ = 0;

    Diligent::RefCntAutoPtr<Diligent::IShader> fullscreen_vertex_;
    // Exact box resolve of the supersampled scene, before the display chain.
    Diligent::RefCntAutoPtr<Diligent::IShader> resolve_shader_;
    Diligent::RefCntAutoPtr<Diligent::IPipelineState> resolve_pipeline_;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> resolve_resources_;
    Diligent::RefCntAutoPtr<Diligent::IShader> resolve4_shader_;
    Diligent::RefCntAutoPtr<Diligent::IPipelineState> resolve4_pipeline_;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> resolve4_resources_;
    Diligent::RefCntAutoPtr<Diligent::ITexture> resolved_;
    std::uint32_t resolved_width_ = 0;
    std::uint32_t resolved_height_ = 0;

    HdrPostChain post_{};
    std::array<Body, kSolarSystemBodyCount> bodies_{};
};

} // namespace helios::examples
