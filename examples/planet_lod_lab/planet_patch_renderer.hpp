// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#pragma once

#include "planet_surface.hpp"

#include "helios/math/vec.hpp"
#include "helios/planet/quadtree_lod.hpp"

#include "Buffer.h"
#include "DeviceContext.h"
#include "PipelineState.h"
#include "Query.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "SwapChain.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace helios::examples {

enum class PatchDebugColor : std::uint8_t {
    Natural = 0,
    LodLevel = 1,
    CubeFace = 2,
    ScreenSpaceError = 3,
};

struct PlanetPatchView {
    math::Vec3d camera_body_fixed_m{};
    math::Vec3d forward_body_fixed{};
    math::Vec3d right_body_fixed{};
    math::Vec3d up_body_fixed{};
    double vertical_fov_rad = 1.0471975511965976;
    double near_plane_m = 0.25;
    std::uint32_t viewport_width_px = 1;
    std::uint32_t viewport_height_px = 1;
    // Sub-pixel shift of the image (pixels), for antialiasing by
    // accumulation; zero draws the pixel centres.
    std::array<double, 2> jitter_px{0.0, 0.0};
};

struct PlanetPatchSettings {
    double planet_radius_m = 6'371'000.0;
    double max_render_distance_m = 2'000'000'000.0;
    // Largest height the terrain reaches, for the horizon test.
    float relief_amplitude_m = 9'000.0f;
    float skirt_depth_m = 300.0f;
    bool horizon_culling = true;
    bool wire_overlay = false;
    PatchDebugColor debug_color = PatchDebugColor::Natural;
    float max_sse_for_heatmap_px = 8.0f;

    // The surface (terrain, climate, colours; planet_surface.hpp).
    PlanetBody body{};
    // Body-fixed direction to the sun, and the sky's ambient light as a
    // fraction of the sun's.
    math::Vec3d light_direction{0.42, 0.73, -0.54};
    float ambient = 0.18f;
    // Tone-map in the shader (an sRGB swap chain) or output linear HDR
    // radiance for a post chain.
    bool tone_map_output = true;
    double time_s = 0.0;
    // Shadow of a ring in the equatorial plane: radii in metres and a
    // profile of its normal optical depth (a texture, inner to outer edge);
    // off when `ring_outer_m` is zero or the profile is null.
    double ring_inner_m = 0.0;
    double ring_outer_m = 0.0;
    Diligent::ITextureView* ring_profile = nullptr;
    // A body that may shadow this one: centre in this body's frame and
    // radius (m); radius 0 = none.
    math::Vec3d occluder_centre_m{};
    double occluder_radius_m = 0.0;
    // Clouds above the surface: base and top radius (m), cover and the
    // extinction of dense cloud (1/m); extinction 0 = none.
    double cloud_base_m = 0.0;
    double cloud_top_m = 0.0;
    double cloud_cover = 0.0;
    double cloud_extinction = 0.0;
    double cloud_drift_rad = 0.0; // how far the wind has carried the cloud field
    // Colour of the sky's ambient light.
    math::Vec3d ambient_color{1.0, 1.0, 1.0};
};

struct PlanetPatchMetrics {
    std::uint32_t selected_leaf_count = 0;
    std::uint32_t visible_leaf_count = 0;
    std::uint32_t horizon_culled_count = 0;
    std::uint32_t distance_culled_count = 0;
    std::uint32_t draw_calls = 0;
    std::uint64_t triangles = 0;
    bool gpu_timer_supported = false;
    bool gpu_sample_available = false;
    bool gpu_sample_new = false;
    double gpu_planet_pass_ms = 0.0;
};

// Draws one 16x16 procedural grid and four crack-hiding skirts for every
// visible quadtree leaf. Vertex positions are generated from SV_VertexID;
// only camera-relative patch anchors, lattice anchors and topology metadata
// are streamed.  The terrain itself is shaders/lib/planet_noise.slang.
class PlanetPatchRenderer {
  public:
    static constexpr std::uint32_t kPatchCellsPerAxis = 16;
    static constexpr std::uint32_t kSurfaceVerticesPerPatch =
        kPatchCellsPerAxis * kPatchCellsPerAxis * 6;
    static constexpr std::uint32_t kSkirtVerticesPerPatch =
        4 * kPatchCellsPerAxis * 6;
    static constexpr std::uint32_t kVerticesPerPatch =
        kSurfaceVerticesPerPatch + kSkirtVerticesPerPatch;
    static constexpr std::uint32_t kMaxPatchInstances = 16'384;

    [[nodiscard]] bool initialize(Diligent::IRenderDevice* device,
                                  Diligent::IDeviceContext* context,
                                  Diligent::SwapChainDesc const& swap_chain_desc,
                                  std::string& error);

    [[nodiscard]] bool render(std::span<planet::TileLodRecord const> leaves,
                              PlanetPatchView const& view,
                              PlanetPatchSettings const& settings,
                              PlanetPatchMetrics& metrics,
                              std::string& error);

  private:
    using FrameGpu = shaders::PlanetPatchFrame;
    using PatchGpu = shaders::PlanetPatchRecord;
    using BodyGpu = shaders::PlanetBodyParams;

    struct QuerySlot {
        Diligent::RefCntAutoPtr<Diligent::IQuery> query;
        bool pending = false;
    };

    [[nodiscard]] bool createPipeline(Diligent::SwapChainDesc const& swap_chain_desc,
                                      std::string& error);
    void pollAndBeginGpuTimer(QuerySlot*& active_slot, PlanetPatchMetrics& metrics);
    void endGpuTimer(QuerySlot* active_slot);

    Diligent::RefCntAutoPtr<Diligent::IRenderDevice> device_;
    Diligent::RefCntAutoPtr<Diligent::IDeviceContext> context_;
    Diligent::RefCntAutoPtr<Diligent::IPipelineState> pipeline_;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> resources_;
    Diligent::RefCntAutoPtr<Diligent::IBuffer> frame_buffer_;
    Diligent::RefCntAutoPtr<Diligent::IBuffer> body_buffer_;
    Diligent::RefCntAutoPtr<Diligent::IBuffer> patch_buffer_;
    // A one-texel profile for bodies without rings.
    Diligent::RefCntAutoPtr<Diligent::ITexture> empty_profile_;
    Diligent::RefCntAutoPtr<Diligent::ISampler> profile_sampler_;

    std::array<QuerySlot, 4> gpu_queries_{};
    std::uint64_t frame_index_ = 0;
    bool gpu_timer_supported_ = false;
    bool has_gpu_sample_ = false;
    double latest_gpu_ms_ = 0.0;
};

} // namespace helios::examples
