// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Deep-sky lab gas dynamics: the radiation field of the nebula's star.
//
// A star emitting Q ionising photons per second keeps hydrogen ionised along
// a ray until recombinations have used the photons up:
//
//     Q / (4 pi) = alpha_B * integral of n^2 r^2 dr
//
// (the Stromgren condition, per unit solid angle).  This pass walks outwards
// from the star along every direction of a spherical grid and records how far
// that integral has got, together with the gas column, at every radius.
// Directions are laid out on an octahedral map, which has no poles.
// Dense clumps exhaust their rays early and cast shadows; behind them the gas
// stays neutral and cold, which is how pillars and bright rims arise.
//
// Not modelled: the diffuse ionising photons that recombinations themselves
// emit (they soften the shadows), and absorption of ionising photons by dust.

#include "lib/hydro.slang"

[[vk::binding(1, 0)]] Sampler3D density_field; // state a; r = density
// See lib/hydro_radiation.slang for the layout.
[[vk::binding(2, 0)]] [[vk::image_format("rg16f")]] RWTexture3D<float2> rays_out;

static const float RAY_COLUMN_UNIT = 1000.0f;

// Inverse of the octahedral map used by lib/hydro_radiation.slang.
float3 octahedralDecode(float2 uv) {
    float2 f = uv * 2.0f - 1.0f;
    float3 n = float3(f.x, f.y, 1.0f - abs(f.x) - abs(f.y));
    float t = max(-n.z, 0.0f);
    n.x += n.x >= 0.0f ? -t : t;
    n.y += n.y >= 0.0f ? -t : t;
    return normalize(n);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint3 id : SV_DispatchThreadID) {
    uint width;
    uint height;
    uint depth;
    rays_out.GetDimensions(width, height, depth);
    if (id.x >= width || id.y >= height) {
        return;
    }
    float3 direction = octahedralDecode((float2(id.xy) + 0.5f) / float2(float(width), float(height)));
    float3 star = hydro.source[0].xyz;
    float integral = max(hydro.source[0].w, 1.0e-20f);
    float half_extent = hydro.scenario.y;
    float outer = hydro.source[2].x;

    float used = 0.0f;
    float column = 0.0f;
    for (uint index = 0; index < depth; ++index) {
        // Radii are stretched like the grid: fine near the star, coarse far out.
        float w = (float(index) + 0.5f) / float(depth);
        float radius = outer * stretchRadius(w);
        float step = outer * stretchRadiusSlope(w) / float(depth);
        float3 position = star + direction * radius;
        float density = 0.0f;
        if (all(abs(position) < half_extent)) {
            density = density_field.SampleLevel(gridCoordinate(position), 0.0f).r;
        }
        float used_here = density * density * radius * radius * step / integral;
        float column_here = density * step / RAY_COLUMN_UNIT;
        // Values at the middle of the radial cell.
        rays_out[uint3(id.xy, index)] =
            float2(min(used + 0.5f * used_here, 8.0f), column + 0.5f * column_here);
        used += used_here;
        column += column_here;
    }
}
