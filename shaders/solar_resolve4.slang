// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Exact 4x4 box downsample of a 4x supersampled HDR scene, the 4x variant of
// solar_resolve.slang.  Integer Loads avoid any sampler/filter ambiguity in
// the display chain.

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
    uint2 output_size = uint2(max(width / 4u, 1u), max(height / 4u, 1u));
    uint2 pixel = min(uint2(input.uv * float2(output_size)), output_size - 1u);
    int2 first = int2(pixel * 4u);
    float4 sum = float4(0.0f, 0.0f, 0.0f, 0.0f);
    for (uint y = 0u; y < 4u; ++y) {
        for (uint x = 0u; x < 4u; ++x) {
            sum += source.Load(int3(first + int2(int(x), int(y)), 0));
        }
    }
    return sum * (1.0f / 16.0f);
}
