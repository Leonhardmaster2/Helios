// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// CPU-side shader ABI contract. The reflection test
// (tests/test_shader_abi.cpp) verifies these layouts against the
// offline-compiled SPIR-V; no undocumented layout assumptions are allowed.
// This header must stay dependency-free (helios_vk may not depend on
// helios_math — docs/ARCHITECTURE.md).
#pragma once

#include <cstddef>
#include <cstdint>

namespace helios::shaders {

// triangle.slang: set 0, binding 0 (std140)
struct TriangleUbo {
    float mvp[16]; // column-major mat4
};
static_assert(sizeof(TriangleUbo) == 64, "TriangleUbo must be 64 bytes (std140 mat4)");

// depth.slang: set 0, binding 0 (std140)
struct DepthParams {
    struct Quad {
        float x, y, z, w; // center.xy, clip z, half-size
    };
    Quad quads[8];
    struct Color {
        float r, g, b, a;
    };
    Color colors[8];
};
static_assert(sizeof(DepthParams) == 8 * 16 + 8 * 16, "DepthParams size (std140 vec4 arrays)");
static_assert(offsetof(DepthParams, quads) == 0);
static_assert(offsetof(DepthParams, colors) == 128);

// compute.slang: push constants (8 bytes)
struct ComputeParams {
    std::uint32_t count;
    std::uint32_t seed;
};
static_assert(sizeof(ComputeParams) == 8);
static_assert(offsetof(ComputeParams, count) == 0);
static_assert(offsetof(ComputeParams, seed) == 4);

// mesh.slang: push constants. The PV matrix arrives as four explicit
// clip-space ROW vectors (no matrix memory-layout ambiguity); the vertex
// shader computes clip = (dot(r0,w), dot(r1,w), dot(r2,w), dot(r3,w)).
struct MeshPC {
    float row0[4];
    float row1[4];
    float row2[4];
    float row3[4];
    float offset_radius[4]; // xyz = NearRenderRecord offset (m), w = radius (m)
    float color[4];         // rgba
};
static_assert(sizeof(MeshPC) == 96);
static_assert(offsetof(MeshPC, offset_radius) == 64);
static_assert(offsetof(MeshPC, color) == 80);

// impostor.slang: push constants.
struct ImpostorPC {
    float dir_depth[4]; // xyz = unit direction in VIEW axes, w = ndc depth at centre
    float params[4];    // x = angular radius (rad), y = tan(fovY/2), z = aspect, w = pad
    float size_inv[4];  // x = 1/width, y = 1/height, zw = pad
    float color[4];     // rgba
};
static_assert(sizeof(ImpostorPC) == 64);
static_assert(offsetof(ImpostorPC, params) == 16);
static_assert(offsetof(ImpostorPC, size_inv) == 32);
static_assert(offsetof(ImpostorPC, color) == 48);

// planet_patch.slang: set 0 binding 0 constant buffer and set 0 binding 1
// structured patch records. These mirror the private Diligent lab upload
// structures and are checked against SPIR-V reflection.
struct PlanetPatchFrame {
    float row0[4];
    float row1[4];
    float row2[4];
    float row3[4];
    float planet[4];
    float light[4];
    float camera[4];
    float ring[4];
    float occluder[4];
    float clouds[4];
    float ambient_color[4];
};
static_assert(sizeof(PlanetPatchFrame) == 176);
static_assert(offsetof(PlanetPatchFrame, planet) == 64);
static_assert(offsetof(PlanetPatchFrame, light) == 80);
static_assert(offsetof(PlanetPatchFrame, ring) == 112);
static_assert(offsetof(PlanetPatchFrame, ambient_color) == 160);

// planet_sky.slang: set 0 binding 7.
struct PlanetSkyExtra {
    float ring_tint[4];
};
static_assert(sizeof(PlanetSkyExtra) == 16);

struct PlanetPatchRecord {
    float uv_face[4];
    float anchor_level[4];
    float debug[4];
    float anchor_direction[4];
    std::int32_t lattice_cell[4];
    std::uint32_t lattice_frac[4];
    float tangent_u[4];
    float tangent_v[4];
};
static_assert(sizeof(PlanetPatchRecord) == 128);
static_assert(offsetof(PlanetPatchRecord, anchor_level) == 16);
static_assert(offsetof(PlanetPatchRecord, debug) == 32);
static_assert(offsetof(PlanetPatchRecord, lattice_cell) == 64);

// planet_patch.slang and planet_sky.slang: set 0 binding 2 constant buffer
// (shaders/lib/planet_noise.slang).
struct PlanetBodyParams {
    float shape[4];
    float continent[4];
    float mountain[4];
    float detail[4];
    float crater[4];
    float climate[4];
    std::uint32_t seeds[4];
    std::uint32_t seeds2[4];
    float color_vegetation[4];
    float color_desert[4];
    float color_rock[4];
    float color_snow[4];
    float color_ocean_deep[4];
    float color_ocean_shallow[4];
    float bands[4];
    float band_color_a[4];
    float band_color_b[4];
    float band_color_c[4];
};
static_assert(sizeof(PlanetBodyParams) == 288);
static_assert(offsetof(PlanetBodyParams, seeds) == 96);
static_assert(offsetof(PlanetBodyParams, bands) == 224);

// diligent_precision.slang: set 0 binding 0 constant buffer.
struct DiligentPrecisionFrame {
    float view_row_0[4];
    float view_row_1[4];
    float view_row_2[4];
    std::uint32_t metadata[4];
};
static_assert(sizeof(DiligentPrecisionFrame) == 64);
static_assert(offsetof(DiligentPrecisionFrame, metadata) == 48);

// lib/black_hole_trace.slang (black_hole.slang and black_hole_refine.slang):
// set 0 binding 0 constant buffer (fragment stage). Twelve float4 rows; the
// shader source documents each component.
struct BlackHoleFrame {
    float camera_position[4];
    float camera_right[4];
    float camera_up[4];
    float camera_forward[4];
    float viewport[4];
    float disk[4];
    float disk_detail[4];
    float sky[4];
    float radiometry[4];
    float integration[4];
    float effects[4];
    float refine[4];
};
static_assert(sizeof(BlackHoleFrame) == 192);
static_assert(offsetof(BlackHoleFrame, viewport) == 64);
static_assert(offsetof(BlackHoleFrame, disk) == 80);
static_assert(offsetof(BlackHoleFrame, radiometry) == 128);
static_assert(offsetof(BlackHoleFrame, effects) == 160);
static_assert(offsetof(BlackHoleFrame, refine) == 176);

// hdr_glare.slang: set 0 binding 0 constant buffer.
struct HdrGlareParams {
    float params[4]; // xy = 1 / source size, z = pass mode, w = upsample radius
};
static_assert(sizeof(HdrGlareParams) == 16);

// hdr_composite.slang: set 0 binding 0 constant buffer.
struct HdrCompositeParams {
    float exposure[4]; // x = exposure, y = glare mix, z = 1 / levels, w = tone map
    float dither[4];   // x = seed, y = amplitude in 8-bit steps, zw = unused
};
static_assert(sizeof(HdrCompositeParams) == 32);
static_assert(offsetof(HdrCompositeParams, dither) == 16);

// lib/deep_sky_frame.slang (emission_volume, neutron_star,
// deep_sky_background, galaxy_stars): set 0 binding 0 constant buffer. Sixteen float4 rows; the
// shader sources document each component and each scene's use of `object`.
struct DeepSkyFrame {
    float camera_position[4];
    float camera_right[4];
    float camera_up[4];
    float camera_forward[4];
    float viewport[4];
    float radiometry[4];
    float sky[4];
    float quality[4];
    float object[8][4];
};
static_assert(sizeof(DeepSkyFrame) == 256);
static_assert(offsetof(DeepSkyFrame, radiometry) == 80);
static_assert(offsetof(DeepSkyFrame, quality) == 112);
static_assert(offsetof(DeepSkyFrame, object) == 128);

// galaxy_stars.slang: one record per instance, read as two vertex attributes
// (four normalised uint16, then four normalised int16).
struct GalaxyParticle {
    std::uint16_t position[3]; // -kGalaxyPositionKpc .. +kGalaxyPositionKpc
    std::uint16_t birth;       // birth-time code, see below
    std::int16_t velocity[3];  // +-kGalaxyVelocityKmS
    std::int16_t reserved;
};
static_assert(sizeof(GalaxyParticle) == 16);
static_assert(offsetof(GalaxyParticle, velocity) == 8);
inline constexpr float kGalaxyPositionKpc = 32.0f;
inline constexpr float kGalaxyVelocityKmS = 800.0f;
// Birth times in Myr: codes 0 .. kGalaxyBirthSplit span the old stars
// (oldest .. recent), the following codes up to 65534 span recent .. latest
// at 0.33 Myr per code (runs to 16 Gyr); 65535 marks a particle that is
// still gas.
inline constexpr float kGalaxyBirthOldestMyr = -13000.0f;
inline constexpr float kGalaxyBirthRecentMyr = -100.0f;
inline constexpr float kGalaxyBirthLatestMyr = 16000.0f;
inline constexpr std::uint16_t kGalaxyBirthSplit = 16383;
inline constexpr std::uint16_t kGalaxyBirthGas = 65535;

// galaxy_resolved_stars.slang: set 0 binding 2, one record per star particle
// near the camera whose stars are drawn one by one (std430).  Binding 3 holds
// uint2 chunks (record, first slot) of kGalaxyStarChunk stars each, one draw
// instance per star.
struct GalaxyNearParticle {
    float position_age[4]; // xyz = centre relative to the camera (pc), w = age (Myr)
    float shape[4];        // x = spread in the plane (pc), y = thickness (pc), z = fade, w = V-band optical depth
    std::uint32_t stars[4]; // x = particle index, y = stars born, z = remnant ranks, w = slots drawn
};
static_assert(sizeof(GalaxyNearParticle) == 48);
inline constexpr std::uint32_t kGalaxyStarChunk = 16;

// star_system.slang: set 0 binding 2 constant buffer.  The visited star and
// up to kMaximumBodies - 1 planets and moons, positions relative to the camera
// in km, radiances pre-multiplied by the display scale (see the shader).
struct StarSystemBlock {
    static constexpr std::size_t kMaximumBodies = 24;
    float star[4][4];
    float bodies[4 * kMaximumBodies][4];
};
static_assert(sizeof(StarSystemBlock) == 64 + 4 * 24 * 16);

// emission_volume.slang: set 0 binding 4 constant buffer, three float4 rows
// per point source (screen placement, radiance, position).
struct PointSourcesBlock {
    static constexpr std::size_t kMaximumSources = 96;
    float data[3 * kMaximumSources][4];
};
static_assert(sizeof(PointSourcesBlock) == 4608);

// lib/hydro.slang: set 0 binding 0 constant buffer of every hydro_* compute
// pass. Fourteen float4 rows, documented in the shader.
struct HydroParams {
    float grid[4];
    float gas[4];
    float scenario[4];
    float source[5][4];
    float shade[6][4];
    float mesh[4];
};
static_assert(sizeof(HydroParams) == 240);
static_assert(offsetof(HydroParams, source) == 48);
static_assert(offsetof(HydroParams, shade) == 128);

// Expected SPIR-V entry points (reflection test). Slang renames every
// entry to "main" in the generated SPIR-V module, which is what the
// reflection test (tests/test_shader_abi.cpp) and the pipeline bindings use.
inline constexpr char const* kTriangleVertexEntry = "main";
inline constexpr char const* kTriangleFragmentEntry = "main";
inline constexpr char const* kDepthVertexEntry = "main";
inline constexpr char const* kDepthFragmentEntry = "main";
inline constexpr char const* kComputeEntry = "main";
inline constexpr char const* kMeshVertexEntry = "main";
inline constexpr char const* kMeshFragmentEntry = "main";
inline constexpr char const* kImpostorVertexEntry = "main";
inline constexpr char const* kImpostorFragmentEntry = "main";
inline constexpr char const* kPlanetPatchVertexEntry = "main";
inline constexpr char const* kPlanetPatchFragmentEntry = "main";
inline constexpr char const* kDiligentPrecisionComputeEntry = "main";
inline constexpr char const* kFullscreenVertexEntry = "main";
inline constexpr char const* kBlackHoleFragmentEntry = "main";
inline constexpr char const* kBlackHoleRefineFragmentEntry = "main";
inline constexpr char const* kHdrGlareFragmentEntry = "main";
inline constexpr char const* kHdrCompositeFragmentEntry = "main";

} // namespace helios::shaders
