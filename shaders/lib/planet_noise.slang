// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Terrain of the planet labs: a height field on the unit sphere built from
// lattice value noise, evaluated identically on the GPU (here) and on the
// CPU (examples/planet_lod_lab/planet_surface.cpp), so that the camera, the
// level of detail and the drawn surface agree.
//
// Octave k of every layer samples the same lattice at frequency f0 2^k
// (cells per radian of direction).  A position is addressed as an integer
// cell plus a fraction, and a patch's anchor supplies the cell and the
// fraction of octave 0 with 32 fixed-point bits, so the lattice coordinate of
// octave 22 (cells 6 metres across on an Earth-sized body) is still exact to
// a millionth of a cell: float32 alone loses it beyond a few hundred metres.
//
// Height = continents + mountains + detail, floored at the sea level on
// continental crust (+ craters on airless bodies):
//   continents  low octaves of noise, remapped so that heights are bimodal
//               (ocean floor and continental platform), as the hypsometry of
//               a body with plate tectonics and an ocean is;
//   mountains   ridged multifractal (Musgrave 1998) confined to belts on the
//               continents, the way orogens follow plate boundaries;
//   detail      the remaining octaves, rougher in the mountains than on the
//               plains (a stand-in for erosion smoothing low ground);
//   craters     bowls with raised rims from a power-law size distribution
//               (depth 0.2 and rim 0.04 of the diameter for simple craters,
//               Pike 1977; Melosh 1989), on two interleaved lattices per
//               octave so that no grid shows.
// Every term returns its derivative with respect to direction as well, from
// which the vertex normal follows without finite differences.
//
// Not modelled: tectonic histories, river networks, glaciers, complex
// craters with central peaks, and any real body's map.
#pragma once

static const float PLANET_PI = 3.14159265358979f;

// ---------------------------------------------------------------------------
// Parameters of a body (ConstantBuffer<PlanetBodyParams> body)
// ---------------------------------------------------------------------------

struct PlanetBodyParams {
    // x = radius (m), y = sea level (m, relative to the radius), z = lattice
    // frequency of octave 0 (cells per radian), w = highest octave + 1
    float4 shape;
    // x = ocean floor (m), y = continental platform (m), z = bias of the
    // land fraction (-1 all ocean .. 1 all land), w = roughness of the base
    // level (m per unit noise)
    float4 continent;
    // x = amplitude (m), y = belt threshold, z = belt width, w = first ridge
    // octave
    float4 mountain;
    // x = amplitude (m), y = first octave, z = fraction kept on the plains,
    // w = octave 0 of the humidity field
    float4 detail;
    // x = fraction of cells with a crater (0: none), y = depth / diameter,
    // z = rim height / diameter, w = first crater octave
    float4 crater;
    // x = equatorial sea-level temperature (K), y = pole drop (K), z = lapse
    // rate (K/m), w = humidity bias
    float4 climate;
    // Hash seeds of the layers: x continents, y belts, z ridges, w detail
    uint4 seeds;
    // Hash seeds: x humidity, y craters, z bands, w clouds
    uint4 seeds2;
    // Linear sRGB albedos.
    float4 color_vegetation;
    float4 color_desert; // w = beach height (m)
    float4 color_rock;
    float4 color_snow;          // w = ocean depth (m) over which it turns deep
    float4 color_ocean_deep;    // w = ocean present (0/1)
    float4 color_ocean_shallow; // w = ocean specular strength
    // Gas giant: x = enabled, y = bands per hemisphere, z = turbulence,
    // w = storm density
    float4 bands;
    float4 band_color_a;
    float4 band_color_b;
    float4 band_color_c;
};

// ---------------------------------------------------------------------------
// Lattice noise with derivatives
// ---------------------------------------------------------------------------

uint3 planetPcg3d(uint3 v) {
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return v;
}

uint3 planetLatticeHash(int3 cell, uint seed, uint octave) {
    return planetPcg3d(uint3(cell) + uint3(seed, seed * 0x9E3779B9u + octave * 0x85EBCA6Bu,
                                           octave * 0xC2B2AE35u + 0x27D4EB2Fu));
}

// Value in [-1, 1] at a lattice cell.
float planetLatticeValue(int3 cell, uint seed, uint octave) {
    return float(planetLatticeHash(cell, seed, octave).x >> 8u) * (2.0f / 16777216.0f) - 1.0f;
}

// Origin of an octave's lattice coordinate: the anchor's cell and fraction
// at octave 0, scaled by 2^k exactly.
struct LatticeOrigin {
    int3 cell;
    float3 frac;
};

LatticeOrigin latticeOrigin(int3 cell0, uint3 frac_fixed, uint k) {
    LatticeOrigin origin;
    origin.cell = (cell0 << k) + (k == 0u ? int3(0, 0, 0) : int3(frac_fixed >> (32u - k)));
    origin.frac = float3(frac_fixed << k) * (1.0f / 4294967296.0f);
    return origin;
}

// Value noise with quintic interpolation and its gradient (per lattice
// unit) at cell `base` + `p`.
float4 planetValueNoiseD(int3 base, float3 p, uint seed, uint octave) {
    float3 i = floor(p);
    float3 f = p - i;
    int3 c = base + int3(i);
    float3 u = f * f * f * (f * (f * 6.0f - 15.0f) + 10.0f);
    float3 du = 30.0f * f * f * (f * (f - 2.0f) + 1.0f);
    float a = planetLatticeValue(c, seed, octave);
    float b = planetLatticeValue(c + int3(1, 0, 0), seed, octave);
    float cc = planetLatticeValue(c + int3(0, 1, 0), seed, octave);
    float d = planetLatticeValue(c + int3(1, 1, 0), seed, octave);
    float e = planetLatticeValue(c + int3(0, 0, 1), seed, octave);
    float ff = planetLatticeValue(c + int3(1, 0, 1), seed, octave);
    float g = planetLatticeValue(c + int3(0, 1, 1), seed, octave);
    float h = planetLatticeValue(c + int3(1, 1, 1), seed, octave);
    float k0 = a;
    float k1 = b - a;
    float k2 = cc - a;
    float k3 = e - a;
    float k4 = a - b - cc + d;
    float k5 = a - cc - e + g;
    float k6 = a - b - e + ff;
    float k7 = -a + b + cc - d + e - ff - g + h;
    float value = k0 + k1 * u.x + k2 * u.y + k3 * u.z + k4 * u.x * u.y + k5 * u.y * u.z +
                  k6 * u.z * u.x + k7 * u.x * u.y * u.z;
    float3 grad = du * float3(k1 + k4 * u.y + k6 * u.z + k7 * u.y * u.z,
                              k2 + k4 * u.x + k5 * u.z + k7 * u.z * u.x,
                              k3 + k5 * u.y + k6 * u.x + k7 * u.x * u.y);
    return float4(value, grad);
}

// A sample point for all octaves: the patch's lattice anchor and the offset
// of this vertex from it in direction space.  The coarse offset is the
// float32 difference of the directions (exact to 1e-7, enough below octave
// 18); the fine one is the patch-local linearisation of the cube-face
// mapping, which the fine octaves need (and which is accurate only for the
// small patches that evaluate them).
static const uint FINE_OCTAVE = 18u;

struct SurfacePoint {
    int3 cell0;
    uint3 frac0;
    float3 delta_coarse;
    float3 delta_fine;
    float3 direction;
};

float3 surfaceDelta(SurfacePoint point, uint k) {
    return k < FINE_OCTAVE ? point.delta_coarse : point.delta_fine;
}

// Noise of octave k at the point: value and gradient per radian.
float4 octaveNoise(SurfacePoint point, uint k, uint seed) {
    float frequency = body.shape.z * float(1u << k);
    LatticeOrigin origin = latticeOrigin(point.cell0, point.frac0, k);
    float4 n = planetValueNoiseD(origin.cell, origin.frac + surfaceDelta(point, k) * frequency, seed, k);
    return float4(n.x, n.yzw * frequency);
}

// ---------------------------------------------------------------------------
// Layers
// ---------------------------------------------------------------------------

struct Field {
    float value;
    float3 grad; // per radian of direction
};

Field fieldZero() {
    Field f;
    f.value = 0.0f;
    f.grad = float3(0.0f, 0.0f, 0.0f);
    return f;
}

// Fractal sum of octaves [first, last) whose amplitude falls by `gain` per
// octave: 0.5 is Brownian relief, 0.68 (Hurst exponent 0.55) the roughness
// of real terrain at the scales below a few kilometres.
Field fbmField(SurfacePoint point, uint first, uint last, uint seed, float gain) {
    Field f = fieldZero();
    float amplitude = 1.0f;
    for (uint k = first; k < last; ++k) {
        float4 n = octaveNoise(point, k, seed);
        f.value += amplitude * n.x;
        f.grad += amplitude * n.yzw;
        amplitude *= gain;
    }
    return f;
}

// Ridged multifractal: each octave is (1 - |n|)^2, weighted by the octave
// below so that ridges carry the detail and valleys stay smooth; the sum
// is normalised to [0, 1] by the total amplitude.
Field ridgedField(SurfacePoint point, uint first, uint last, uint seed) {
    Field f = fieldZero();
    float amplitude = 0.5f; // the octave amplitudes sum to 1
    float weight = 1.0f;
    float3 weight_grad = float3(0.0f, 0.0f, 0.0f);
    for (uint k = first; k < last; ++k) {
        float4 n = octaveNoise(point, k, seed);
        float sign_n = n.x < 0.0f ? -1.0f : 1.0f;
        float r = 1.0f - abs(n.x);
        float3 r_grad = -sign_n * n.yzw;
        float ridge = r * r;
        float3 ridge_grad = 2.0f * r * r_grad;
        f.value += amplitude * weight * ridge;
        f.grad += amplitude * (weight * ridge_grad + ridge * weight_grad);
        // The next octave's weight follows this one's ridge value.
        float next = ridge * 1.6f;
        if (next < 1.0f) {
            weight = next;
            weight_grad = ridge_grad * 1.6f;
        } else {
            weight = 1.0f;
            weight_grad = float3(0.0f, 0.0f, 0.0f);
        }
        amplitude *= 0.5f;
    }
    return f;
}

// Fractal sum evaluated straight from a direction (no lattice anchor): for
// the low octaves, where float32 is exact enough, and for warped inputs.
Field fbmDirect(float3 direction, uint first, uint last, uint seed, float gain) {
    Field f = fieldZero();
    float amplitude = 1.0f;
    for (uint k = first; k < last; ++k) {
        float frequency = body.shape.z * float(1u << k);
        float4 n = planetValueNoiseD(int3(0, 0, 0), direction * frequency, seed, k);
        f.value += amplitude * n.x;
        f.grad += amplitude * n.yzw * frequency;
        amplitude *= gain;
    }
    return f;
}

// The continent field: low octaves of noise on a direction warped by three
// more noise fields (domain warping), which bends the lattice's blobs into
// the irregular, elongated shapes of real landmasses.
Field continentField(float3 direction, uint cap) {
    static const float WARP = 0.18f;
    Field wx = fbmDirect(direction, 1u, min(3u, cap), body.seeds.x ^ 0x1B873593u, 0.5f);
    Field wy = fbmDirect(direction, 1u, min(3u, cap), body.seeds.x ^ 0xCC9E2D51u, 0.5f);
    Field wz = fbmDirect(direction, 1u, min(3u, cap), body.seeds.x ^ 0xE6546B64u, 0.5f);
    float3 warped = direction + WARP * float3(wx.value, wy.value, wz.value);
    Field c = fbmDirect(warped, 0u, min(7u, cap), body.seeds.x, 0.6f);
    // Chain rule through the warp: grad = (I + J^T) grad_c.
    float3 g = c.grad;
    c.grad = g + WARP * (wx.grad * g.x + wy.grad * g.y + wz.grad * g.z);
    return c;
}

// smoothstep and its derivative with respect to x.
float2 smoothstepD(float edge0, float edge1, float x) {
    float t = saturate((x - edge0) / (edge1 - edge0));
    float value = t * t * (3.0f - 2.0f * t);
    float slope = (t > 0.0f && t < 1.0f) ? 6.0f * t * (1.0f - t) / (edge1 - edge0) : 0.0f;
    return float2(value, slope);
}

// Craters of one lattice at octave k, offset by `shift` (two lattices per
// octave, half a cell apart).  Returns height (m) and gradient per radian.
Field craterField(SurfacePoint point, uint k, float3 shift, uint seed) {
    Field f = fieldZero();
    float frequency = body.shape.z * float(1u << k);
    LatticeOrigin origin = latticeOrigin(point.cell0, point.frac0, k);
    float3 p = origin.frac + surfaceDelta(point, k) * frequency + shift;
    float3 i = floor(p);
    int3 cell = origin.cell + int3(i);
    uint3 h = planetLatticeHash(cell, seed, k);
    float u0 = float(h.x >> 8u) * (1.0f / 16777216.0f);
    if (u0 >= body.crater.x) {
        return f;
    }
    float u1 = float(h.y >> 8u) * (1.0f / 16777216.0f);
    float u2 = float(h.z >> 8u) * (1.0f / 16777216.0f);
    // Centre within the middle half of the cell, radius from a power law
    // N(> D) ~ D^-2 between an eighth and a quarter of a cell.
    float3 centre = float3(0.25f, 0.25f, 0.25f) +
                    0.5f * float3(u1, u2, float(planetPcg3d(h).x >> 8u) * (1.0f / 16777216.0f));
    float radius = 0.125f / sqrt(max(1.0f - 0.75f * u0 / max(body.crater.x, 1.0e-6f), 0.25f));
    float3 offset = (p - i) - centre;
    float distance = length(offset);
    float rho = distance / radius;
    if (rho >= 2.0f) {
        return f;
    }
    // Sizes in metres: the cell is 1 / frequency radians.
    float diameter_m = 2.0f * radius / frequency * body.shape.x;
    float depth = body.crater.y * diameter_m;
    float rim = body.crater.z * diameter_m;
    float height;
    float slope; // d height / d rho
    if (rho < 1.0f) {
        // Bowl, rising to the rim crest.
        float rho2 = rho * rho;
        height = depth * (rho2 - 1.0f) + rim * rho2 * rho2;
        slope = 2.0f * depth * rho + 4.0f * rim * rho2 * rho;
    } else {
        // Ejecta blanket falling off outside the rim.
        float fall = exp(-3.0f * (rho - 1.0f));
        height = rim * fall;
        slope = -3.0f * rim * fall;
    }
    f.value = height;
    // d rho / d direction = offset / (distance * radius) * frequency
    f.grad = distance > 1.0e-6f ? slope * offset / (distance * radius) * frequency
                                : float3(0.0f, 0.0f, 0.0f);
    return f;
}

// ---------------------------------------------------------------------------
// The surface
// ---------------------------------------------------------------------------

struct Surface {
    float height;    // m above the radius
    float3 grad;     // d height / d direction, per radian
    float continent; // the continent field (land > 0)
    float belt;      // mountain belt mask
};

// Height and gradient at the point with octaves below `octaves`.
Surface planetSurface(SurfacePoint point, uint octaves) {
    Surface s;
    uint cap = min(octaves, uint(body.shape.w));

    // Continents: the lowest seven octaves, falling off slowly so that the
    // coastlines are fractal down to tens of kilometres.
    Field c = continentField(point.direction, cap);
    float2 land = smoothstepD(-0.12f, 0.12f, c.value + body.continent.z);
    float base = lerp(body.continent.x, body.continent.y, land.x) + body.continent.w * c.value;
    float3 base_grad = (body.continent.y - body.continent.x) * land.y * c.grad + body.continent.w * c.grad;

    // Mountain belts on land.
    Field b = fbmField(point, 1u, min(4u, cap), body.seeds.y, 0.5f);
    float2 belt_mask = smoothstepD(body.mountain.y - body.mountain.z, body.mountain.y + body.mountain.z, b.value);
    float belt = belt_mask.x * land.x;
    float3 belt_grad = belt_mask.y * b.grad * land.x + belt_mask.x * land.y * c.grad;

    float height = base;
    float3 grad = base_grad;
    uint ridge_first = uint(body.mountain.w);
    if (body.mountain.x > 0.0f && cap > ridge_first) {
        Field r = ridgedField(point, ridge_first, min(ridge_first + 8u, cap), body.seeds.z);
        // ridge^2 gives sharp crests and broad valleys.
        float ridge2 = r.value * r.value;
        float3 ridge2_grad = 2.0f * r.value * r.grad;
        height += body.mountain.x * ridge2 * belt;
        grad += body.mountain.x * (ridge2_grad * belt + ridge2 * belt_grad);
    }

    uint detail_first = uint(body.detail.y);
    if (body.detail.x > 0.0f && cap > detail_first) {
        Field d = fbmField(point, detail_first, cap, body.seeds.w, 0.62f);
        float keep = body.detail.z + (1.0f - body.detail.z) * belt;
        height += body.detail.x * d.value * keep;
        grad += body.detail.x * (d.grad * keep + d.value * (1.0f - body.detail.z) * belt_grad);
    }

    // Base level: on continental crust rivers cannot cut below the sea
    // level and basins fill with sediment, so the land's depressions are
    // floored softly at the sea (a smooth maximum 60 m wide).  The floor
    // fades out where the continent field reaches the coast, so coasts,
    // shelves and estuaries keep their relief.  Without it the additive
    // octaves pock every lowland with lakes.
    if (body.color_ocean_deep.w > 0.5f) {
        float sea = body.shape.y;
        float2 crust = smoothstepD(-0.05f, 0.1f, c.value + body.continent.z);
        float excess = height - sea;
        float width = 60.0f;
        float root = sqrt(excess * excess + width * width);
        float floored = sea + 0.5f * (excess + root);
        float d_floored = 0.5f * (1.0f + excess / root);
        grad = grad + ((d_floored - 1.0f) * grad) * crust.x + (floored - height) * crust.y * c.grad;
        height = height + (floored - height) * crust.x;
    }

    uint crater_first = uint(body.crater.w);
    if (body.crater.x > 0.0f && cap > crater_first) {
        for (uint k = crater_first; k < min(crater_first + 6u, cap); ++k) {
            Field c0 = craterField(point, k, float3(0.0f, 0.0f, 0.0f), body.seeds2.y);
            Field c1 = craterField(point, k, float3(0.5f, 0.5f, 0.5f), body.seeds2.y ^ 0x5bd1e995u);
            height += c0.value + c1.value;
            grad += c0.grad + c1.grad;
        }
    }

    s.height = height;
    s.grad = grad;
    s.continent = c.value + body.continent.z;
    s.belt = belt;
    return s;
}

// Humidity in [-1, 1]: three low octaves of their own field.
float planetHumidity(SurfacePoint point) {
    uint first = uint(body.detail.w);
    return fbmField(point, first, first + 3u, body.seeds2.x, 0.5f).value;
}

// Fine albedo texture: the surface's small-scale variation of colour (soil,
// stones, vegetation patches), as a factor about 1.
float planetAlbedoTexture(SurfacePoint point, uint octaves) {
    uint cap = min(octaves, uint(body.shape.w));
    uint first = 9u;
    if (cap <= first) {
        return 1.0f;
    }
    Field t = fbmField(point, first, min(first + 9u, cap), body.seeds2.x ^ 0x7F4A7C15u, 0.7f);
    return 1.0f + 0.22f * t.value;
}

// Cloud cover at a direction, 0..1: a field of its own on the sphere,
// carried in longitude by the wind (`drift`, rad) and thresholded by the
// cover fraction so that about `cover` of the sky is cloudy.  Shared by
// the sky pass (the clouds) and the terrain pass (their shadows).
float cloudCoverage(float3 direction, float drift, float cover) {
    float c = cos(drift);
    float s = sin(drift);
    float3 d = float3(direction.x * c + direction.y * s, direction.y * c - direction.x * s, direction.z);
    uint seed = body.seeds2.w ^ 0x2545F491u;
    // Weather systems are not lattice-aligned: the domain is warped by a
    // low-frequency vector field, as the continents' is.
    float3 warp = float3(planetValueNoiseD(int3(0, 0, 0), d * 2.0f, seed, 11u).x,
                         planetValueNoiseD(int3(0, 0, 0), d * 2.0f, seed, 12u).x,
                         planetValueNoiseD(int3(0, 0, 0), d * 2.0f, seed, 13u).x) -
                  0.5f;
    float3 q = d + warp * 0.3f;
    float field = 0.0f;
    float amplitude = 0.5f;
    for (uint k = 0u; k < 8u; ++k) {
        float frequency = 3.0f * float(1u << k);
        field += amplitude * planetValueNoiseD(int3(0, 0, 0), q * frequency, seed, k).x;
        amplitude *= 0.5f;
    }
    // Less cloud at the subtropical latitudes of the descending Hadley
    // branch, more at the equator and the mid-latitude storm tracks.
    float latitude = asin(clamp(direction.z, -1.0f, 1.0f));
    float belt = 0.06f * cos(6.0f * latitude);
    // The field is close to Gaussian with a standard deviation of 0.33 (the
    // lattice values are uniform in -1..1); in units of it the threshold
    // that leaves the fraction `cover` above it is the normal quantile,
    // here the logistic approximation ln((1 - c) / c) / 1.7.
    float x = (field + belt) * (1.0f / 0.33f);
    float fraction = clamp(cover, 0.005f, 0.995f);
    float threshold = log((1.0f - fraction) / fraction) / 1.7f;
    // A gradual ramp: the interior of a cloud field keeps the field's
    // structure as variations of its cover, as real fields do.
    return smoothstep(threshold - 0.6f, threshold + 0.6f, x);
}

// The fraction of the sky under a pixel that billows fill at a coverage
// value, once they are too small to resolve: a billow exists where its
// shape (0.5 +- 0.15) exceeds 1 - coverage, so the fraction is the normal
// distribution's tail, here a smoothstep.  Seen from afar a cloud field is
// this fraction of opaque billows, not a uniform haze of their mean
// density: the mean of the transmittance, not the transmittance of the
// mean.  The clouds' shadows use it too.
float cloudOpacity(float coverage) {
    return smoothstep(0.2f, 0.8f, coverage);
}

// The optical depth over the layer that gives a far pixel the mean
// transmittance of its billows.
float cloudFarOpticalDepth(float coverage) {
    return -log(max(1.0f - 0.999f * cloudOpacity(coverage), 1.0e-3f));
}

// Normal of the displaced sphere from the height gradient: the tangential
// part of the gradient tilts the normal away from the radial direction.
float3 planetNormal(float3 direction, float radius, Surface s) {
    float3 tangential = s.grad - dot(s.grad, direction) * direction;
    return normalize(direction - tangential / max(radius + s.height, 1.0f));
}
