// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Glare pyramid shared by the HDR labs.  One fragment module serves three
// passes selected by `pyramid.params.z`:
//   0 - first downsample of the HDR scene, with a luminance-weighted (Karis)
//       average so single very bright pixels cannot flicker;
//   1 - 13-tap downsample to the next coarser level;
//   2 - 9-tap tent upsample, drawn with additive blending into the next finer
//       level so each level ends up holding itself plus every coarser one.
// There is no brightness threshold: glare in a lens or an eye is linear in
// the incoming light, so the pyramid redistributes a small fraction of all of
// it.  It is rendered with the full-screen vertex module, fullscreen.slang.

struct HdrGlareParams {
    // xy = 1 / source size (px), z = pass mode, w = upsample radius (source texels)
    float4 params;
};

[[vk::binding(0, 0)]] ConstantBuffer<HdrGlareParams> pyramid;
[[vk::binding(1, 0)]] Sampler2D source;

struct VsOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

float luminance(float3 color) {
    return dot(color, float3(0.2126729f, 0.7151522f, 0.0721750f));
}

float3 tap(float2 uv, float2 offset) {
    return source.SampleLevel(uv + offset * pyramid.params.xy, 0.0f).rgb;
}

float3 downsample(float2 uv, bool weighted) {
    float3 a = tap(uv, float2(-2.0f, -2.0f));
    float3 b = tap(uv, float2(0.0f, -2.0f));
    float3 c = tap(uv, float2(2.0f, -2.0f));
    float3 d = tap(uv, float2(-2.0f, 0.0f));
    float3 e = tap(uv, float2(0.0f, 0.0f));
    float3 f = tap(uv, float2(2.0f, 0.0f));
    float3 g = tap(uv, float2(-2.0f, 2.0f));
    float3 h = tap(uv, float2(0.0f, 2.0f));
    float3 i = tap(uv, float2(2.0f, 2.0f));
    float3 j = tap(uv, float2(-1.0f, -1.0f));
    float3 k = tap(uv, float2(1.0f, -1.0f));
    float3 l = tap(uv, float2(-1.0f, 1.0f));
    float3 m = tap(uv, float2(1.0f, 1.0f));

    // Five overlapping 2x2 boxes: the centre box carries half the weight.
    float3 centre = (j + k + l + m) * 0.25f;
    float3 top_left = (a + b + d + e) * 0.25f;
    float3 top_right = (b + c + e + f) * 0.25f;
    float3 bottom_left = (d + e + g + h) * 0.25f;
    float3 bottom_right = (e + f + h + i) * 0.25f;
    if (!weighted) {
        return centre * 0.5f + (top_left + top_right + bottom_left + bottom_right) * 0.125f;
    }
    float w0 = 0.5f / (1.0f + luminance(centre));
    float w1 = 0.125f / (1.0f + luminance(top_left));
    float w2 = 0.125f / (1.0f + luminance(top_right));
    float w3 = 0.125f / (1.0f + luminance(bottom_left));
    float w4 = 0.125f / (1.0f + luminance(bottom_right));
    return (centre * w0 + top_left * w1 + top_right * w2 + bottom_left * w3 + bottom_right * w4) /
           (w0 + w1 + w2 + w3 + w4);
}

float3 upsample(float2 uv) {
    float r = pyramid.params.w;
    float3 sum = tap(uv, float2(0.0f, 0.0f)) * 4.0f;
    sum += (tap(uv, float2(-r, 0.0f)) + tap(uv, float2(r, 0.0f)) + tap(uv, float2(0.0f, -r)) +
            tap(uv, float2(0.0f, r))) * 2.0f;
    sum += tap(uv, float2(-r, -r)) + tap(uv, float2(r, -r)) + tap(uv, float2(-r, r)) +
           tap(uv, float2(r, r));
    return sum * (1.0f / 16.0f);
}

[shader("fragment")]
float4 fsMain(VsOut input) : SV_Target {
    float mode = pyramid.params.z;
    float3 color;
    if (mode < 0.5f) {
        color = downsample(input.uv, true);
    } else if (mode < 1.5f) {
        color = downsample(input.uv, false);
    } else {
        color = upsample(input.uv);
    }
    // A NaN or infinity here would spread through the whole pyramid.
    color = min(max(color, float3(0.0f, 0.0f, 0.0f)), float3(6.0e4f, 6.0e4f, 6.0e4f));
    return float4(color, 1.0f);
}
