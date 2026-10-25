// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Shared building blocks for the lab shaders: integer hashing, band-limited
// value noise, the black-body colour table lookup, and a procedural sky
// (unresolved stars, a galactic band, and a celestial inspection grid).
//
// Nothing here reads a global: the table texture and its parameters are
// passed in, so any lab can include this file whatever it calls its uniforms.
// `radiometry` is x = ln(reference luminance), y = ln(table minimum K),
// z = 1 / (ln(table maximum K) - y), w = table texel count.
// `sky` is x = star intensity, y = galactic band intensity,
// z = mode (0 stars, 1 celestial grid, 2 black).
#pragma once

static const float PI = 3.14159265358979f;

// ---------------------------------------------------------------------------
// Hashing and band-limited noise
// ---------------------------------------------------------------------------

uint3 pcg3d(uint3 v) {
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

float3 hash3(int3 cell, uint seed) {
    uint3 h = pcg3d(uint3(cell) + uint3(seed, seed * 0x9E3779B9u, seed ^ 0x85EBCA6Bu));
    return float3(h >> 8u) * (1.0f / 16777216.0f);
}

float hash1(int3 cell, uint seed) {
    return hash3(cell, seed).x;
}

// Value noise in [-1, 1] with quintic interpolation.
float valueNoise(float3 p, uint seed) {
    float3 cell = floor(p);
    float3 f = p - cell;
    float3 w = f * f * f * (f * (f * 6.0f - 15.0f) + 10.0f);
    int3 c = int3(cell);
    float n000 = hash1(c + int3(0, 0, 0), seed);
    float n100 = hash1(c + int3(1, 0, 0), seed);
    float n010 = hash1(c + int3(0, 1, 0), seed);
    float n110 = hash1(c + int3(1, 1, 0), seed);
    float n001 = hash1(c + int3(0, 0, 1), seed);
    float n101 = hash1(c + int3(1, 0, 1), seed);
    float n011 = hash1(c + int3(0, 1, 1), seed);
    float n111 = hash1(c + int3(1, 1, 1), seed);
    float low = lerp(lerp(n000, n100, w.x), lerp(n010, n110, w.x), w.y);
    float high = lerp(lerp(n001, n101, w.x), lerp(n011, n111, w.x), w.y);
    return lerp(low, high, w.z) * 2.0f - 1.0f;
}

// Fractal noise whose octaves fade out once a pixel covers more than about
// one lattice cell, so minified regions converge to the mean instead of
// aliasing.  `footprint` is the pixel footprint in the units of `p`.
float filteredFbm(float3 p, float footprint, int octaves, uint seed) {
    float sum = 0.0f;
    float amplitude = 0.5f;
    float frequency = 1.0f;
    for (int octave = 0; octave < octaves; ++octave) {
        float fade = 1.0f - smoothstep(0.3f, 1.0f, footprint * frequency);
        if (fade <= 0.0f) {
            break;
        }
        sum += amplitude * fade * valueNoise(p * frequency, seed + uint(octave) * 101u);
        amplitude *= 0.5f;
        frequency *= 2.0f;
    }
    return sum;
}

// ---------------------------------------------------------------------------
// Black-body table
// ---------------------------------------------------------------------------

// rgb = linear sRGB colour of unit luminance, a = ln(luminance).
float4 blackbodyEntry(Texture2D<float4> table, float4 radiometry, float kelvin) {
    float unit = (log(max(kelvin, 1.0f)) - radiometry.y) * radiometry.z;
    float count = radiometry.w;
    float scaled = saturate(unit) * (count - 1.0f);
    float base = min(floor(scaled), count - 2.0f);
    float4 low = table.Load(int3(int(base), 0, 0));
    float4 high = table.Load(int3(int(base) + 1, 0, 0));
    float4 entry = lerp(low, high, scaled - base);
    // Rayleigh-Jeans continuation above the table: luminance grows like T.
    entry.a += max(unit - 1.0f, 0.0f) / radiometry.z;
    return entry;
}

float3 blackbodyRadiance(Texture2D<float4> table, float4 radiometry, float kelvin) {
    if (log(max(kelvin, 1.0f)) < radiometry.y) {
        return float3(0.0f, 0.0f, 0.0f);
    }
    float4 entry = blackbodyEntry(table, radiometry, kelvin);
    return entry.rgb * exp(entry.a - radiometry.x);
}

// ---------------------------------------------------------------------------
// Sky
// ---------------------------------------------------------------------------

// The pixel's footprint on the sky: screen-space derivatives of the sky
// direction, i.e. the two columns of the Jacobian from pixel offsets to
// direction offsets.  Where light is bent they differ in length and are not
// perpendicular; that is the lensing magnification and shear.
struct SkyFootprint {
    float3 dx;
    float3 dy;
};

// An unlensed footprint: `footprint` radians per pixel in every direction.
SkyFootprint isotropicFootprint(float3 direction, float footprint) {
    float3 helper = abs(direction.x) < 0.9f ? float3(1.0f, 0.0f, 0.0f) : float3(0.0f, 1.0f, 0.0f);
    float3 tangent = normalize(cross(direction, helper));
    SkyFootprint result;
    result.dx = tangent * footprint;
    result.dy = cross(direction, tangent) * footprint;
    return result;
}

// Equi-angular coordinate in [-1, 1]^2 of a direction on the cube face whose
// major axis is `axis` (0 x, 1 y, 2 z).
float2 equiAngular(float3 direction, int axis) {
    float2 uv;
    if (axis == 0) {
        uv = direction.yz / abs(direction.x);
    } else if (axis == 1) {
        uv = direction.zx / abs(direction.y);
    } else {
        uv = direction.xy / abs(direction.z);
    }
    return atan(uv) * (4.0f / PI);
}

// One layer of unresolved stars on an equi-angular cube map.
//
// A star is a point source.  Its image is a Gaussian of fixed width in
// *pixels*; the pixel's footprint on the sky (the Jacobian `uv_dx`, `uv_dy`,
// in face coordinates per pixel) maps that Gaussian onto the sky, so under
// lensing the star stays a dot while its flux is multiplied by the
// magnification, the inverse of the sky area the pixel covers.  Surface
// brightness is conserved, so once stars are too dense to resolve the layer
// becomes its mean radiance, which lensing leaves unchanged.
//
// `face_uv` is the equi-angular cube-map coordinate in [-1, 1]^2 on `face`,
// and `mean_color` the colour of the layer's unresolved glow; both are the
// same for every layer, so the caller computes them once.
float3 starLayer(Texture2D<float4> table, float4 radiometry, float2 face_uv, float2 uv_dx,
                 float2 uv_dy, int face, float3 mean_color, float cells, float density,
                 float base_flux, uint seed, float shift) {
    static const float SIGMA_PX = 0.55f; // image width of a star
    static const float MEAN_FLUX_FACTOR = 0.62f;
    // Axis ratio beyond which a pixel's sky footprint is treated as this
    // thin: bounds the magnification a point source can show (a real star's
    // finite size does the same).
    static const float MAX_AXIS_RATIO = 50.0f;

    float2 grid = (face_uv * 0.5f + 0.5f) * cells;
    float2 jx = uv_dx * (0.5f * cells); // cells per pixel along screen x
    float2 jy = uv_dy * (0.5f * cells); // ... and along screen y
    float cell_angle = 0.5f * PI / cells;
    float det = jx.x * jy.y - jx.y * jy.x;
    // Longest stretch of the pixel on the sky (largest singular value), in
    // cells: whether a star in it can still be resolved.
    float frobenius = dot(jx, jx) + dot(jy, jy);
    float stretch_sq =
        0.5f * (frobenius + sqrt(max(frobenius * frobenius - 4.0f * det * det, 0.0f)));
    float sigma = SIGMA_PX * sqrt(stretch_sq);

    // Mean radiance of the layer, used once stars are smaller than the footprint.
    float3 mean = mean_color * (density * base_flux * MEAN_FLUX_FACTOR / (cell_angle * cell_angle));
    float average = smoothstep(0.2f, 0.45f, sigma);
    if (average >= 1.0f) {
        return mean;
    }

    // Sky area of the pixel, in cells^2, with the axis-ratio bound.
    float area = max(abs(det), stretch_sq / MAX_AXIS_RATIO);
    float inverse_det = (det < 0.0f ? -1.0f : 1.0f) / area;
    // Radiance of unit flux at the image centre: the pixel-space Gaussian
    // over the sky area the pixel covers.
    float peak = 1.0f / (2.0f * PI * SIGMA_PX * SIGMA_PX * area * cell_angle * cell_angle);
    float inverse_variance = 1.0f / (SIGMA_PX * SIGMA_PX);

    float3 sum = float3(0.0f, 0.0f, 0.0f);
    float2 base = floor(grid - 0.5f);
    for (int j = 0; j < 2; ++j) {
        for (int i = 0; i < 2; ++i) {
            float2 cell = base + float2(float(i), float(j));
            if (cell.x < 0.0f || cell.y < 0.0f || cell.x >= cells || cell.y >= cells) {
                continue;
            }
            int3 key = int3(int(cell.x), int(cell.y), face);
            float3 h = hash3(key, seed);
            if (h.z > density) {
                continue;
            }
            // Keep centres half a cell inside the face so no star is cut by a cube edge.
            float2 centre = clamp(cell + h.xy, float2(0.5f, 0.5f), float2(cells - 0.5f, cells - 0.5f));
            float2 offset = grid - centre;
            // The star's offset in pixels: the inverse Jacobian.
            float2 pixels = float2(jy.y * offset.x - jy.x * offset.y,
                                   jx.x * offset.y - jx.y * offset.x) * inverse_det;
            float distance_sq = dot(pixels, pixels) * inverse_variance;
            if (distance_sq > 16.0f) {
                continue;
            }
            float3 k = hash3(key, seed ^ 0x51ED270Bu);
            float flux = base_flux * (0.2f + 1.6f * k.x * k.x * k.x + (k.y > 0.965f ? 2.5f : 0.0f));
            float kelvin = 2900.0f * exp(2.35f * pow(k.z, 1.7f)); // 2900 K .. 30000 K
            float3 color = blackbodyEntry(table, radiometry, kelvin * shift).rgb;
            sum += color * (flux * peak * exp(-0.5f * distance_sq));
        }
    }
    return lerp(sum, mean, average);
}

float3 galacticBand(Texture2D<float4> table, float4 radiometry, float3 direction,
                    float footprint, float shift) {
    // The band is inclined 60 degrees to the disk plane and crosses the +y
    // direction, so the default camera sees it pass behind the hole; the
    // bulge sits 25 degrees along the band from there.
    static const float3 POLE = float3(0.8660254f, 0.0f, 0.5f);
    static const float3 CENTRE = float3(-0.2113091f, 0.9063078f, 0.3659982f);
    float sin_latitude = dot(direction, POLE);
    float toward_centre = dot(direction, CENTRE);

    float disk = exp(-sin_latitude * sin_latitude * 55.0f);
    float bulge = exp(-(sin_latitude * sin_latitude * 16.0f + (1.0f - toward_centre) * 3.0f));
    float envelope = disk * (0.22f + 0.5f * smoothstep(-1.0f, 1.0f, toward_centre)) + 1.1f * bulge;
    // Away from the band there is nothing to structure; skip the noise.
    if (envelope < 4.0e-3f) {
        return float3(0.0f, 0.0f, 0.0f);
    }

    float clouds = filteredFbm(direction * 5.0f + 11.0f, footprint * 5.0f, 5, 911u);
    float lanes = filteredFbm(direction * 11.0f - 5.0f, footprint * 11.0f, 5, 1777u);
    float structure = saturate(0.72f + 0.75f * clouds);
    // Dust lanes hug the mid-plane and remove light; they redden what is left.
    float dust = saturate((lanes + 0.18f) * 2.6f) * exp(-sin_latitude * sin_latitude * 220.0f);
    float density = envelope * structure * (1.0f - 0.82f * dust);

    // Unresolved starlight: young blue-white populations in the disk, older
    // and yellower ones in the bulge, reddened where dust removes light.
    float3 warm = blackbodyEntry(table, radiometry, 4700.0f * shift).rgb;
    float3 cool = blackbodyEntry(table, radiometry, 8200.0f * shift).rgb;
    float3 color = lerp(cool, warm, saturate(0.12f + 0.75f * bulge + 0.7f * dust));
    return color * density;
}

float3 celestialGrid(float3 direction, float footprint) {
    static const float SPACING = PI / 12.0f; // 15 degrees
    float longitude = atan2(direction.y, direction.x);
    float latitude = asin(clamp(direction.z, -1.0f, 1.0f));
    float2 cell = float2(longitude, latitude) / SPACING;
    float2 to_line = abs(frac(cell + 0.5f) - 0.5f) * SPACING;
    to_line.x *= max(cos(latitude), 1.0e-3f);
    float width = 0.0035f;
    float blur = max(footprint, 1.0e-5f);
    float nearest = min(to_line.x, to_line.y);
    float line_coverage = 1.0f - smoothstep(width, width + blur * 1.5f, nearest);
    // Once a pixel spans several cells the lines average to their area fraction.
    float average = saturate(4.0f * width / SPACING);
    line_coverage = lerp(line_coverage, average, smoothstep(0.25f, 1.0f, blur / SPACING));

    int quadrant = (int(floor(longitude / (0.5f * PI))) & 3) + (direction.z < 0.0f ? 4 : 0);
    static const float3 TINTS[8] = {
        float3(0.90f, 0.30f, 0.25f), float3(0.95f, 0.75f, 0.25f),
        float3(0.30f, 0.80f, 0.40f), float3(0.25f, 0.55f, 0.95f),
        float3(0.60f, 0.25f, 0.50f), float3(0.60f, 0.50f, 0.25f),
        float3(0.20f, 0.50f, 0.45f), float3(0.25f, 0.30f, 0.65f)};
    float3 tint = TINTS[quadrant];
    return tint * (0.035f + 0.55f * line_coverage);
}

// Radiance of the sky in `direction` for a pixel with the given footprint.
// `lapse` is sqrt(1 - 2M/r) of a static camera: it sees the sky blueshifted.
float3 skyRadiance(Texture2D<float4> table, float4 radiometry, float4 sky, float3 direction,
                   SkyFootprint footprint, float lapse) {
    float mode = sky.z;
    if (mode > 1.5f) {
        return float3(0.0f, 0.0f, 0.0f);
    }
    float extent = max(length(footprint.dx), length(footprint.dy));
    if (mode > 0.5f) {
        return celestialGrid(direction, extent) * sky.x;
    }

    // A static observer at radius r sees the sky blueshifted by 1 / lapse.
    float shift = 1.0f / lapse;
    float gain = exp(blackbodyEntry(table, radiometry, 5800.0f * shift).a -
                     blackbodyEntry(table, radiometry, 5800.0f).a);

    float3 radiance = float3(0.0f, 0.0f, 0.0f);
    if (sky.x > 0.0f) {
        // Cells per cube-face edge, occupancy and flux fall together so the
        // counts rise roughly as flux^-1, like a magnitude-limited sky.
        static const float STAR_SCALE = 2.6e-6f;

        // Equi-angular cube map: cells of nearly equal solid angle and no poles.
        float3 a = abs(direction);
        int axis;
        int face;
        if (a.x >= a.y && a.x >= a.z) {
            axis = 0;
            face = direction.x > 0.0f ? 0 : 1;
        } else if (a.y >= a.z) {
            axis = 1;
            face = direction.y > 0.0f ? 2 : 3;
        } else {
            axis = 2;
            face = direction.z > 0.0f ? 4 : 5;
        }
        float2 uv = equiAngular(direction, axis);
        // The footprint in face coordinates, by finite differences on the
        // same face (the derivatives are far below a pixel of the face).
        float2 uv_dx = equiAngular(direction + footprint.dx, axis) - uv;
        float2 uv_dy = equiAngular(direction + footprint.dy, axis) - uv;
        float3 mean_color = blackbodyEntry(table, radiometry, 6200.0f * shift).rgb;

        float3 stars = starLayer(table, radiometry, uv, uv_dx, uv_dy, face, mean_color, 9.0f, 0.55f,
                                 1.0f, 11u, shift);
        stars += starLayer(table, radiometry, uv, uv_dx, uv_dy, face, mean_color, 31.0f, 0.60f,
                           0.20f, 23u, shift);
        stars += starLayer(table, radiometry, uv, uv_dx, uv_dy, face, mean_color, 97.0f, 0.65f,
                           0.045f, 37u, shift);
        stars += starLayer(table, radiometry, uv, uv_dx, uv_dy, face, mean_color, 263.0f, 0.70f,
                           0.011f, 53u, shift);
        stars += starLayer(table, radiometry, uv, uv_dx, uv_dy, face, mean_color, 613.0f, 0.75f,
                           0.0030f, 71u, shift);
        radiance += stars * (STAR_SCALE * sky.x);
    }
    if (sky.y > 0.0f) {
        radiance += galacticBand(table, radiometry, direction, extent, shift) * (0.016f * sky.y);
    }
    return radiance * gain;
}

// The same for an unlensed view: `footprint` radians per pixel.
float3 skyRadiance(Texture2D<float4> table, float4 radiometry, float4 sky, float3 direction,
                   float footprint, float lapse) {
    return skyRadiance(table, radiometry, sky, direction, isotropicFootprint(direction, footprint),
                       lapse);
}
