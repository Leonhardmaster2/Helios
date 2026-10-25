// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Display transform shared by the HDR labs: HDR scene + glare pyramid ->
// exposure -> tone map -> dithered output.  The render target is an sRGB
// format, so this shader returns linear values and the hardware encodes them.
// It is rendered with the full-screen vertex module, fullscreen.slang.

struct HdrCompositeParams {
    // x = linear exposure multiplier, y = glare mix in [0, 1],
    // z = 1 / pyramid level count, w = tone map (0 ACES fit, 1 clamp only)
    float4 exposure;
    // x = dither seed, y = dither amplitude in 8-bit steps (0 disables), zw = unused
    float4 dither;
};

[[vk::binding(0, 0)]] ConstantBuffer<HdrCompositeParams> composite;
[[vk::binding(1, 0)]] Sampler2D scene;
[[vk::binding(2, 0)]] Sampler2D glare;

struct VsOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

// Stephen Hill's fit of the ACES reference rendering and sRGB output
// transforms (input and output matrices include the sRGB <-> AP1 conversion).
float3 acesFitted(float3 color) {
    static const float3x3 INPUT = {
        0.59719f, 0.35458f, 0.04823f,
        0.07600f, 0.90834f, 0.01566f,
        0.02840f, 0.13383f, 0.83777f};
    static const float3x3 OUTPUT = {
        1.60475f, -0.53108f, -0.07367f,
        -0.10208f, 1.10813f, -0.00605f,
        -0.00327f, -0.07276f, 1.07602f};
    float3 v = mul(INPUT, color);
    float3 a = v * (v + 0.0245786f) - 0.000090537f;
    float3 b = v * (0.983729f * v + 0.4329510f) + 0.238081f;
    return saturate(mul(OUTPUT, a / b));
}

float3 linearToSrgb(float3 c) {
    float3 low = c * 12.92f;
    float3 high = 1.055f * pow(max(c, float3(0.0f, 0.0f, 0.0f)), float3(1.0f / 2.4f)) - 0.055f;
    return float3(c.x <= 0.0031308f ? low.x : high.x, c.y <= 0.0031308f ? low.y : high.y,
                  c.z <= 0.0031308f ? low.z : high.z);
}

float3 srgbToLinear(float3 c) {
    float3 low = c / 12.92f;
    float3 high = pow((max(c, float3(0.0f, 0.0f, 0.0f)) + 0.055f) / 1.055f, float3(2.4f));
    return float3(c.x <= 0.04045f ? low.x : high.x, c.y <= 0.04045f ? low.y : high.y,
                  c.z <= 0.04045f ? low.z : high.z);
}

float hashPixel(float2 p, float seed) {
    float3 q = frac(float3(p.xyx) * float3(0.1031f, 0.1030f, 0.0973f) + seed * 0.61803399f);
    q += dot(q, q.yzx + 33.33f);
    return frac((q.x + q.y) * q.z);
}

[shader("fragment")]
float4 fsMain(VsOut input) : SV_Target {
    float3 hdr = scene.SampleLevel(input.uv, 0.0f).rgb;
    float3 halo = glare.SampleLevel(input.uv, 0.0f).rgb * composite.exposure.z;
    float3 color = lerp(hdr, halo, composite.exposure.y) * composite.exposure.x;

    color = composite.exposure.w < 0.5f ? acesFitted(color) : saturate(color);

    if (composite.dither.y > 0.0f) {
        // Triangular-PDF dither applied in the encoded domain, where one
        // quantisation step is perceptually uniform, then decoded again
        // because the sRGB target re-encodes on write.
        float n = hashPixel(input.pos.xy, composite.dither.x) +
                  hashPixel(input.pos.xy + 17.0f, composite.dither.x + 1.0f) - 1.0f;
        float3 encoded = linearToSrgb(color) + n * composite.dither.y * (1.0f / 255.0f);
        color = srgbToLinear(saturate(encoded));
    }
    return float4(color, 1.0f);
}
