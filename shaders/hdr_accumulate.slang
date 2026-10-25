// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Progressive accumulation for the HDR labs.  It copies one frame's radiance
// unchanged; the pipeline blends it into the running average with a constant
// blend factor of 1 / (frames + 1), so a still view converges while every
// frame uses a different ray-march jitter.  Rendered with fullscreen.slang.

[[vk::binding(0, 0)]] Texture2D<float4> source;

struct VsOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("fragment")]
float4 fsMain(VsOut input) : SV_Target {
    return source.Load(int3(int2(input.pos.xy), 0));
}
