// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Helpers shared by the deep-sky lab's full-screen fragment passes (the gas
// volume, the neutron star and the background sky).  Each returns linear HDR
// radiance; progressive accumulation, glare and the display transform happen
// in later passes.
#pragma once

#include "deep_sky_frame.slang"
#include "sky.slang"

[[vk::binding(1, 0)]] Texture2D<float4> blackbody_table;

struct VsOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 blackbodyEntry(float kelvin) {
    return blackbodyEntry(blackbody_table, frame.radiometry, kelvin);
}

// Colour of a black body scaled to unit luminance.
float3 blackbodyChroma(float kelvin) {
    return blackbodyEntry(blackbody_table, frame.radiometry, kelvin).rgb;
}

float3 backgroundSky(float3 direction) {
    return skyRadiance(blackbody_table, frame.radiometry, frame.sky, direction, pixelAngle(), 1.0f);
}

// Interleaved gradient noise (Jimenez 2014), decorrelated between frames by
// the golden ratio; a low-discrepancy offset for the first ray-march sample.
float marchJitter(float2 pixel) {
    float frame_index = frame.quality.y;
    float2 p = pixel + 5.588238f * fmod(frame_index, 64.0f);
    return frac(52.9829189f * frac(0.06711056f * p.x + 0.00583715f * p.y));
}

// Intersects a ray with a sphere at the origin.  Returns false when missed.
bool intersectSphere(float3 origin, float3 direction, float radius, out float t_near,
                     out float t_far) {
    float b = dot(origin, direction);
    float c = dot(origin, origin) - radius * radius;
    float discriminant = b * b - c;
    t_near = 0.0f;
    t_far = 0.0f;
    if (discriminant <= 0.0f) {
        return false;
    }
    float root = sqrt(discriminant);
    t_near = -b - root;
    t_far = -b + root;
    return t_far > 0.0f;
}

// Intersects a ray with the axis-aligned box [-half_extent, half_extent]^3.
bool intersectBox(float3 origin, float3 direction, float3 half_extent, out float t_near,
                  out float t_far) {
    float3 inverse = 1.0f / direction;
    float3 a = (-half_extent - origin) * inverse;
    float3 b = (half_extent - origin) * inverse;
    float3 lower = min(a, b);
    float3 upper = max(a, b);
    t_near = max(max(lower.x, lower.y), lower.z);
    t_far = min(min(upper.x, upper.y), upper.z);
    return t_far > max(t_near, 0.0f);
}
