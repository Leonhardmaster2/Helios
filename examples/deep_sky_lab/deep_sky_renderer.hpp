// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Real-time renderer for the deep-sky lab, on a caller-supplied
// Diligent/Vulkan device.  Three of the four objects are simulations whose
// current state is drawn; none of them is a picture of the object.
//
//   Nebula            a gas-dynamics run: a turbulent cloud, then a star that
//                     ionises it (gas_scene.hpp)
//   NeutronStar       rotating, magnetised neutron star in one of several
//                     states (neutron_star_model.hpp): exact light paths,
//                     a surface heated through an anisotropic crust, beamed
//                     outflow, wind current sheet, accretion disk and streams
//   Galaxy            the particles of an N-body run (galaxy_scene.hpp)
//   SupernovaRemnant  a gas-dynamics run: supernova ejecta swept up by a
//                     pulsar's wind (gas_scene.hpp)
//
// Two objects of the other labs are in the same menu, drawn by their own
// renderers:
//
//   BlackHole         the Schwarzschild black hole of the black-hole lab
//                     (examples/black_hole_lab/black_hole_renderer.hpp)
//   Planet            the cube-sphere planet of the planet LOD lab
//                     (planet_stage.hpp)
//   Pulsar            the neutron star again, in one of its rotation-powered
//                     states, with its own settings and clock
//
// A frame is: simulation steps (if any are due) -> scene pass(es) ->
// progressive accumulation (a still view averages frames with different
// ray-march jitter) -> glare, exposure and tone map
// (examples/common/hdr_post_chain).  No window or swap chain is created here.
#pragma once

#include "black_hole_renderer.hpp"
#include "fullscreen_pass.hpp"
#include "galaxy_scene.hpp"
#include "gas_scene.hpp"
#include "hdr_post_chain.hpp"
#include "neutron_star_model.hpp"
#include "planet_stage.hpp"

#include "helios/blackhole/blackbody.hpp"
#include "helios/math/vec.hpp"
#include "shaders/abi.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace helios::examples {

enum class DeepSkyObject : std::uint8_t {
    Nebula = 0,
    NeutronStar = 1,
    Galaxy = 2,
    SupernovaRemnant = 3,
    BlackHole = 4,
    Planet = 5,
    Pulsar = 6,
};
inline constexpr std::size_t kDeepSkyObjectCount = 7;

// Orthonormal basis; lengths in the object's unit (parsec for the nebula and
// the remnant, M = GM/c^2 for the neutron star and the black hole, kiloparsec
// for the galaxy).  The planet has its own camera (PlanetStageSettings).
struct DeepSkyCamera {
    math::Vec3d position{0.0, -30.0, 6.0};
    math::Vec3d forward{0.0, 1.0, 0.0};
    math::Vec3d right{1.0, 0.0, 0.0};
    math::Vec3d up{0.0, 0.0, 1.0};
    double vertical_fov_rad = 0.6981317007977318;
};

struct DeepSkySettings {
    DeepSkyObject object = DeepSkyObject::Nebula;
    DeepSkyCamera camera{};
    // Scene time of the neutron star and of the black hole's disk, M/c.  The
    // simulated objects keep their own clocks in their settings.
    double time = 0.0;
    // Block in render() until a simulated object has reached its target time
    // (captures); otherwise the newest available state is drawn.
    bool wait_for_simulation = false;
    // GPU time per frame that a gas-dynamics run may use while it is behind
    // its target time; as many time steps as fit are taken (at least one).
    float simulation_budget_ms = 5.0f;

    NebulaSettings nebula{};
    NeutronStarSettings neutron_star{};
    GalaxySettings galaxy{};
    RemnantSettings remnant{};
    // Camera, time, sky intensities and display controls of the black hole
    // are taken from the shared fields of this struct, not from here.
    BlackHoleSettings black_hole{};
    PlanetStageSettings planet{};
    // The pulsar entry: the neutron-star model in a rotation-powered state.
    NeutronStarSettings pulsar{};

    float star_intensity = 1.0f;
    float band_intensity = 1.0f;

    float exposure_ev = 0.0f;
    float glare = 0.08f;
    bool tone_map = true;
    bool dither = true;

    int steps = 96; // ray-march samples (volumetric objects)
    // Scale of the neutron star's geodesic steps (smaller: finer, slower).
    float step_scale = 1.0f;
    // Frames a still view may average.  1 disables accumulation.
    int max_accumulation = 64;
};

struct DeepSkyMetrics {
    std::uint32_t scene_width = 0;
    std::uint32_t scene_height = 0;
    int accumulated_frames = 0;
    bool gpu_timer_supported = false;
    bool gpu_sample_new = false;
    double gpu_scene_ms = 0.0;
    double gpu_post_ms = 0.0;
    double gpu_simulation_ms = 0.0;  // of the most recent measured frame
    double gpu_step_ms = 0.0;        // per gas time step, smoothed
    std::uint32_t simulation_steps = 0; // gas time steps taken this frame
    GalaxyStatus galaxy{};
    GasStatus gas{}; // of the nebula or the remnant, whichever is shown
    PlanetStageMetrics planet{};
};

class DeepSkyRenderer {
  public:
    [[nodiscard]] bool initialize(Diligent::IRenderDevice* device,
                                  Diligent::IDeviceContext* context,
                                  Diligent::TEXTURE_FORMAT output_format, std::uint32_t threads,
                                  std::string& error);

    // Renders the selected object at scene_width x scene_height and resolves
    // it into `output` (covered in full).  Leaves `output` bound.
    [[nodiscard]] bool render(DeepSkySettings const& settings, std::uint32_t scene_width,
                              std::uint32_t scene_height, Diligent::ITextureView* output,
                              std::uint32_t output_width, std::uint32_t output_height,
                              DeepSkyMetrics& metrics, std::string& error);

    // Forgets the running average, e.g. after something outside the settings
    // changed what the next frame shows.
    void resetAccumulation() noexcept { accumulated_frames_ = 0; }

    // The gas run behind the nebula or the remnant, for diagnostics.
    [[nodiscard]] GasScene& gas(DeepSkyObject object) noexcept {
        return object == DeepSkyObject::SupernovaRemnant ? remnant_ : nebula_;
    }
    // The galaxy: its view, the visited system, picking.
    [[nodiscard]] GalaxyScene& galaxy() noexcept { return galaxy_; }
    [[nodiscard]] GalaxyScene const& galaxy() const noexcept { return galaxy_; }

  private:
    using FrameGpu = shaders::DeepSkyFrame;

    struct Pass {
        Diligent::RefCntAutoPtr<Diligent::IPipelineState> pipeline;
        Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> resources;
    };

    [[nodiscard]] bool createTextures(std::string& error);
    [[nodiscard]] bool createPipelines(std::string& error);
    [[nodiscard]] bool createScenePass(Pass& pass, char const* shader, char const* name,
                                       char const* dynamic_texture, bool sources,
                                       Diligent::ITexture* source_light, std::string& error);
    [[nodiscard]] bool ensureTargets(std::uint32_t width, std::uint32_t height, std::string& error);
    [[nodiscard]] FrameGpu makeFrame(DeepSkySettings const& settings, std::uint32_t width,
                                     std::uint32_t height) const;
    [[nodiscard]] GasScene const& gasScene(DeepSkyObject object) const;
    void writeSources(DeepSkySettings const& settings, std::uint32_t width, std::uint32_t height);

    Diligent::RefCntAutoPtr<Diligent::IRenderDevice> device_;
    Diligent::RefCntAutoPtr<Diligent::IDeviceContext> context_;
    std::uint32_t threads_ = 1;

    Diligent::RefCntAutoPtr<Diligent::IShader> vertex_shader_;
    Pass neutron_star_{};
    Pass background_{};
    Pass volume_{};
    Pass source_dust_{};
    Diligent::RefCntAutoPtr<Diligent::IPipelineState> accumulate_pipeline_;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> accumulate_resources_;

    Diligent::RefCntAutoPtr<Diligent::IBuffer> frame_buffer_;
    Diligent::RefCntAutoPtr<Diligent::IBuffer> sources_buffer_;
    Diligent::RefCntAutoPtr<Diligent::ITexture> blackbody_texture_;
    // One texel per star: the fraction of its light that gets through the dust.
    Diligent::RefCntAutoPtr<Diligent::ITexture> source_light_;
    blackhole::BlackbodyTable blackbody_table_{};
    GalaxyScene galaxy_{};
    GasScene nebula_{};
    GasScene remnant_{};
    BlackHoleRenderer black_hole_{};
    PlanetStage planet_{};

    Diligent::RefCntAutoPtr<Diligent::ITexture> scene_;       // one frame
    Diligent::RefCntAutoPtr<Diligent::ITexture> galaxy_light_; // old stars, half resolution
    Diligent::RefCntAutoPtr<Diligent::ITexture> accumulated_; // running average
    std::uint32_t scene_width_ = 0;
    std::uint32_t scene_height_ = 0;

    // What the running average was built from.
    DeepSkySettings accumulated_settings_{};
    int accumulated_frames_ = 0;

    HdrPostChain post_{};
    GpuPassTimer scene_timer_{};
    GpuPassTimer post_timer_{};
    GpuPassTimer simulation_timer_{};
    // Gas time steps taken in each of the frames the timer still has pending,
    // and the smoothed GPU cost of one step.
    std::array<std::uint32_t, 4> simulation_steps_{};
    double step_ms_ = 0.0;
    std::uint64_t frame_index_ = 0;
};

} // namespace helios::examples
