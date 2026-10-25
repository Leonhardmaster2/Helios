// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Frame constants shared by every deep-sky lab pass, and the camera model
// derived from them.  Lengths are in the scene's own unit (parsec,
// kiloparsec, or M = GM/c^2); each scene documents its use of `frame.object`.
#pragma once

struct DeepSkyFrame {
    // xyz = camera position (scene units); w = scene time (scene time units)
    float4 camera_position;
    // xyz = camera right * tan(fov_y / 2) * aspect
    float4 camera_right;
    // xyz = camera up * tan(fov_y / 2)
    float4 camera_up;
    // xyz = camera forward (unit)
    float4 camera_forward;
    // x = width (px), y = height (px), z = 1 / width, w = 1 / height
    float4 viewport;
    // black-body table: x = ln(reference luminance), y = ln(minimum K),
    // z = 1 / (ln(maximum K) - y), w = texel count
    float4 radiometry;
    // x = star intensity, y = galactic band intensity,
    // z = mode (0 stars, 1 celestial grid, 2 black), w = unused
    float4 sky;
    // x = ray-march step count, y = frame index of the progressive
    // accumulation (decorrelates the per-pixel jitter), z = scale of the
    // neutron star's geodesic steps, w = unused
    float4 quality;
    // Per-scene parameters.
    float4 object[8];
};

[[vk::binding(0, 0)]] ConstantBuffer<DeepSkyFrame> frame;

float3 cameraRay(float2 pixel) {
    float2 ndc = float2(pixel.x * 2.0f * frame.viewport.z - 1.0f,
                        1.0f - pixel.y * 2.0f * frame.viewport.w);
    return normalize(frame.camera_forward.xyz + ndc.x * frame.camera_right.xyz +
                     ndc.y * frame.camera_up.xyz);
}

// Angle subtended by one pixel at the image centre, in radians.
float pixelAngle() {
    return 2.0f * length(frame.camera_up.xyz) * frame.viewport.w;
}

// Standard interstellar reddening: A_R : A_V : A_B = 0.75 : 1 : 1.32.
static const float3 DUST_EXTINCTION = float3(0.75f, 1.0f, 1.32f);
