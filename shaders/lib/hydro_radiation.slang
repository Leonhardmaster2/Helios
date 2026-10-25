// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Lookup into the radiation field of the nebula's ionising star, which
// hydro_rays.slang integrates outwards along rays on a spherical grid.
//
// Define HYDRO_RAYS_BINDING before including this file.
#pragma once

#include "hydro.slang"

// Spherical grid about the star: uv = direction in the octahedral map (which
// has no poles, so rays are spread evenly over the sky), w = radius / outer
// radius.
//   r = fraction of the star's ionising photons used up by recombinations
//       closer to the star (the Stromgren integral over its maximum),
//   g = gas column from the star, in 1000 cm^-3 pc.
[[vk::binding(HYDRO_RAYS_BINDING, 0)]] Sampler3D rays;

static const float RAY_COLUMN_UNIT = 1000.0f;

// Unit vector -> [0, 1]^2 (Cigolle et al. 2014).
float2 octahedralEncode(float3 direction) {
    float3 n = direction / (abs(direction.x) + abs(direction.y) + abs(direction.z));
    float2 p = n.xy;
    if (n.z < 0.0f) {
        float2 folded = 1.0f - abs(p.yx);
        p = float2(p.x >= 0.0f ? folded.x : -folded.x, p.y >= 0.0f ? folded.y : -folded.y);
    }
    return p * 0.5f + 0.5f;
}

float3 octahedralDecode(float2 uv) {
    float2 f = uv * 2.0f - 1.0f;
    float3 n = float3(f.x, f.y, 1.0f - abs(f.x) - abs(f.y));
    float t = max(-n.z, 0.0f);
    n.x += n.x >= 0.0f ? -t : t;
    n.y += n.y >= 0.0f ? -t : t;
    return normalize(n);
}

struct Radiation {
    float used;     // fraction of the ionising photons already absorbed
    float column;   // gas column from the star, cm^-3 pc
    float distance; // from the star, pc
};

Radiation radiationAt(float3 position) {
    float3 offset = position - hydro.source[0].xyz;
    float distance = max(length(offset), 1.0e-4f);
    float3 uvw = float3(octahedralEncode(offset / distance),
                        unstretchRadius(distance / hydro.source[2].x));
    float4 value = rays.SampleLevel(uvw, 0.0f);
    Radiation result;
    result.used = value.r;
    result.column = value.g * RAY_COLUMN_UNIT;
    result.distance = distance;
    return result;
}

// Fraction of the hydrogen that is ionised, for one value of the used-up
// photon fraction.  In ionisation equilibrium the gas is ionised out to where
// the photons run out; the front is far thinner than a cell, so its position
// inside the cell sets the fraction.  `width` is the number of cells the
// transition is spread over, `cell` the local cell size.
float ionisedFraction(float used, float distance, float density, float width, float cell) {
    float integral = hydro.source[0].w;
    // Share of the photon budget that this cell alone would use up.
    float share = density * density * distance * distance * cell / integral;
    return saturate((1.0f - used) / max(width * share, 1.0e-6f) + 0.5f);
}

// Fraction of the hydrogen that is ionised at `position`.  The four rays
// around the direction to the point are evaluated one by one and their
// answers blended: blending the photon budgets instead would let the ray with
// the nearer front decide for its neighbours, and leave neutral stripes.
float ionisedAt(float3 position, float density, float width) {
    if (hydro.source[0].w <= 0.0f) {
        return 0.0f;
    }
    uint size;
    uint height;
    uint radii;
    rays.GetDimensions(size, height, radii);
    float3 offset = position - hydro.source[0].xyz;
    float distance = max(length(offset), 1.0e-4f);
    float2 texel = octahedralEncode(offset / distance) * float(size) - 0.5f;
    float w = unstretchRadius(distance / hydro.source[2].x);
    float cell = localCell(position);
    float2 base = floor(texel);
    float2 t = texel - base;
    float result = 0.0f;
    for (int corner = 0; corner < 4; ++corner) {
        float2 step = float2(float(corner & 1), float(corner >> 1));
        float2 uv = (clamp(base + step, 0.0f, float(size) - 1.0f) + 0.5f) / float(size);
        float used = rays.SampleLevel(float3(uv, w), 0.0f).r;
        float2 weights = lerp(1.0f - t, t, step);
        result += weights.x * weights.y * ionisedFraction(used, distance, density, width, cell);
    }
    return result;
}
