// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Stars drawn over a gas volume of the deep-sky lab, and the dust between
// each of them and the camera.  Include after lib/deep_sky.slang.
#pragma once

static const int MAX_SOURCES = 96;

struct PointSources {
    // Per source: [3i]   xy = screen position (px), z = core sigma (px),
    //                    w = spike length (px)
    //             [3i+1] rgb = peak radiance before dust
    //             [3i+2] xyz = position
    float4 data[3 * MAX_SOURCES];
};

// rgb = light emitted per unit length, a = dust (see hydro_shade.slang).
[[vk::binding(3, 0)]] Sampler3D emission_volume;
[[vk::binding(4, 0)]] ConstantBuffer<PointSources> sources;

// The gas grid is stretched (lib/hydro.slang): frame.object[1].y is its
// stretch parameter a (0: uniform), frame.object[0].x the half extent.
struct GasGrid {
    float a;
    float sinh_a;
    float half_extent;
};

GasGrid gasGrid() {
    GasGrid g;
    g.a = frame.object[1].y;
    g.sinh_a = g.a > 0.0f ? sinh(g.a) : 1.0f;
    g.half_extent = frame.object[0].x;
    return g;
}

// Texture coordinate of a position, and the size of the cell there relative
// to the mean cell, per axis (cosh(a xi) = sqrt(1 + sinh^2(a xi)), so one
// square root and one logarithm per axis).
float3 gasGridCoordinate(GasGrid g, float3 position, out float3 cell_scale) {
    float3 x = position / g.half_extent;
    if (g.a <= 0.0f) {
        cell_scale = float3(1.0f, 1.0f, 1.0f);
        return x * 0.5f + 0.5f;
    }
    float3 s = g.sinh_a * x;
    float3 q = sqrt(s * s + 1.0f);
    float3 xi = log(s + q) / g.a;
    cell_scale = (g.a / g.sinh_a) * q;
    return xi * 0.5f + 0.5f;
}

// How far a march step along `direction` may go relative to the mean cell:
// the cell's extent along the ray.
float gasStepScale(float3 cell_scale, float3 direction) {
    return 1.0f / max(length(direction / cell_scale), 1.0e-3f);
}

// Fraction of a source's light that reaches the camera through the dust.
// frame.object[0]: x = half extent of the volume, z = visual optical depth
// per unit dust per unit length.
float3 sourceTransmittance(float3 position, float3 camera) {
    static const int SAMPLES = 64;
    float half_extent = frame.object[0].x;
    float3 direction = camera - position;
    float distance = length(direction);
    direction /= distance;
    float t_near;
    float t_far;
    if (!intersectBox(position, direction, float3(half_extent, half_extent, half_extent), t_near,
                      t_far)) {
        return float3(1.0f, 1.0f, 1.0f);
    }
    float begin = max(t_near, 0.0f);
    float end = min(t_far, distance);
    if (end <= begin) {
        return float3(1.0f, 1.0f, 1.0f);
    }
    float step = (end - begin) / float(SAMPLES);
    float column = 0.0f;
    GasGrid grid = gasGrid();
    float3 cell_scale;
    for (int index = 0; index < SAMPLES; ++index) {
        float3 p = position + direction * (begin + (float(index) + 0.5f) * step);
        column += emission_volume.SampleLevel(gasGridCoordinate(grid, p, cell_scale), 0.0f).a;
    }
    return exp(-DUST_EXTINCTION * (frame.object[0].z * column * step));
}
