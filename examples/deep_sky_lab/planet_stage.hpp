// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// The planet of the deep-sky lab's object menu: a planetary system
// (planet_system.hpp) drawn with the planet LOD lab's cube-sphere quadtree
// and patch renderer (examples/planet_lod_lab), one body at a time into
// shared colour and depth targets, followed by the sky pass
// (shaders/planet_sky.slang): atmosphere, clouds, rings, sun and stars.
//
// Unlike the other objects the planet is not a simulation: its relief is a
// fixed, physically scaled height field, and its moons move on Kepler
// orbits.  What it demonstrates is rendering bodies from a billion metres
// away down to eye level without precision loss, with their atmospheres.
#pragma once

#include "planet_patch_renderer.hpp"
#include "planet_scene.hpp"
#include "planet_system.hpp"

#include "helios/blackhole/blackbody.hpp"
#include "helios/planet/quadtree_lod.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace helios::examples {

struct PlanetStageSettings {
    PlanetCamera camera{13'629'000.0, 18.8, -20.0, 90.0};
    // Rendering options of the patches (level of detail, debug colours,
    // culling); the body drawn is `system.body`.
    PlanetPatchSettings patches{};
    PlanetSystem system = planetSystemPreset(PlanetSystemKind::EarthLike, 7);
    float vertical_fov_deg = 60.0f;
    // Screen-space error (pixels) at which a tile splits; it merges again at
    // two thirds of that.
    float lod_error_px = 3.0f;
    // Where the sun stands over the body at time zero: latitude and
    // longitude of the point it is overhead.  The planet's rotation carries
    // the sun westwards with time.
    double sun_latitude_deg = 15.0;
    double sun_longitude_deg = 20.0;
    // Planet time (s): moves the moons, the sun and the clouds.
    double time_s = 0.0;
    int atmosphere_samples = 16;
    int cloud_samples = 24;
};

struct PlanetStageMetrics {
    PlanetPatchMetrics patches{}; // of the planet itself
    std::uint32_t leaves = 0;      // quadtree leaves selected, all bodies
    double max_error_px = 0.0;     // largest screen-space error among them
    double selection_ms = 0.0;     // CPU time of the LOD selection
    std::uint32_t moons_drawn = 0;
};

class PlanetStage {
  public:
    // `output_format` is the format of the colour targets it will draw into;
    // the black-body table is the lab's, for the star background.
    [[nodiscard]] bool initialize(Diligent::IRenderDevice* device, Diligent::IDeviceContext* context,
                                  Diligent::TEXTURE_FORMAT output_format,
                                  Diligent::ITextureView* blackbody_table,
                                  blackhole::BlackbodyTable const& table, std::string& error);

    // Selects the level of detail for the view, draws the planet and its
    // moons and then the sky over them into `output`.  With `settle` the
    // selection is repeated until a still camera causes no more splits or
    // merges (a capture); otherwise it advances once, as in an animation.
    // `frame_index` is the index of the accumulated frame; it sets the
    // sub-pixel jitter of the projection and of the sky's ray march.
    [[nodiscard]] bool render(PlanetStageSettings const& settings, bool settle, std::uint32_t frame_index,
                              float star_intensity, float band_intensity, Diligent::ITextureView* output,
                              std::uint32_t width, std::uint32_t height, PlanetStageMetrics& metrics,
                              std::string& error);

  private:
    struct Body {
        std::optional<planet::QuadtreeLodSelector> selector{};
        planet::LodSelection selection{};
        double radius_m = 0.0; // the selector's radius
        float lod_error_px = 0.0f;
    };

    [[nodiscard]] bool ensureTargets(std::uint32_t width, std::uint32_t height, std::string& error);
    [[nodiscard]] bool ensureAtmosphereTables(PlanetBody const& body, std::string& error);
    [[nodiscard]] bool ensureRingProfile(PlanetSystem const& system, std::string& error);
    [[nodiscard]] bool ensureSelector(Body& body, double radius_m, float lod_error_px, std::string& error);
    [[nodiscard]] bool createSkyPass(Diligent::TEXTURE_FORMAT output_format, std::string& error);
    [[nodiscard]] bool drawBody(Body& body, PlanetPatchView const& view, PlanetPatchSettings const& patches,
                                bool settle, PlanetPatchMetrics& metrics, std::string& error);

    Diligent::RefCntAutoPtr<Diligent::IRenderDevice> device_;
    Diligent::RefCntAutoPtr<Diligent::IDeviceContext> context_;
    PlanetPatchRenderer renderer_{};
    Body planet_{};
    std::array<Body, kMaxPlanetMoons> moons_{};
    double selection_ms_ = 0.0;

    // The terrain pass's targets.
    Diligent::RefCntAutoPtr<Diligent::ITexture> color_;
    Diligent::RefCntAutoPtr<Diligent::ITexture> depth_;
    std::uint32_t target_width_ = 0;
    std::uint32_t target_height_ = 0;

    // The sky pass.
    Diligent::RefCntAutoPtr<Diligent::IShader> fullscreen_vertex_;
    Diligent::RefCntAutoPtr<Diligent::IPipelineState> sky_pipeline_;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> sky_resources_;
    Diligent::RefCntAutoPtr<Diligent::IBuffer> frame_buffer_;
    Diligent::RefCntAutoPtr<Diligent::IBuffer> body_buffer_;
    Diligent::RefCntAutoPtr<Diligent::IBuffer> extra_buffer_;
    Diligent::RefCntAutoPtr<Diligent::ISampler> linear_sampler_;
    Diligent::RefCntAutoPtr<Diligent::ITextureView> blackbody_view_;
    blackhole::BlackbodyTable blackbody_{};
    // Optical depth to space over height and sun elevation, and the
    // multiply scattered light per unit scattering coefficient (Hillaire
    // 2020), for the body they were built for.
    Diligent::RefCntAutoPtr<Diligent::ITexture> transmittance_;
    Diligent::RefCntAutoPtr<Diligent::ITexture> multiscatter_;
    std::optional<PlanetBody> transmittance_body_{};
    // The rings' normal optical depth over radius, and what it was built from.
    Diligent::RefCntAutoPtr<Diligent::ITexture> ring_profile_;
    std::vector<float> ring_profile_values_{};
};

} // namespace helios::examples
