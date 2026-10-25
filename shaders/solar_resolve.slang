// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Exact 2x2 box downsample of a 2x supersampled HDR scene.  The solar-system
// stage renders at twice the output resolution; this pass averages the four
// source texels under each output pixel with integer Loads before exposure,
// glare and tone mapping.  Integer loads avoid any sampler/filter ambiguity
// in the display chain.

[[vk::binding(0, 0)]] Texture2D<float4> source;

struct VsOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("fragment")]
float4 fsMain(VsOut input) : SV_Target {
    uint width = 1u;
    uint height = 1u;
    source.GetDimensions(width, height);
    uint2 output_size = uint2(max(width / 2u, 1u), max(height / 2u, 1u));
    uint2 pixel = min(uint2(input.uv * float2(output_size)), output_size - 1u);
    int2 first = int2(pixel * 2u);
    float4 sum = source.Load(int3(first, 0));
    sum += source.Load(int3(first + int2(1, 0), 0));
    sum += source.Load(int3(first + int2(0, 1), 0));
    sum += source.Load(int3(first + int2(1, 1), 0));
    return sum * 0.25f;
}
