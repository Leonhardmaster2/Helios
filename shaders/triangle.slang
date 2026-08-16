// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Native triangle smoke path: vertex-generated triangle with per-vertex
// color, transformed by a UBO matrix (exercises the descriptor path and
// the CPU/GPU ABI contract).
struct TriangleUbo {
    float4x4 mvp;
};

[[vk::binding(0, 0)]] ConstantBuffer<TriangleUbo> ubo;

struct VsOut {
    float4 pos : SV_Position;
    float3 color : COLOR0;
};

[shader("vertex")]
VsOut vsMain(uint vid : SV_VertexID) {
    float2 corners[3] = { float2(-0.75f, -0.75f), float2(0.75f, -0.75f), float2(0.0f, 0.75f) };
    float3 colors[3] = { float3(1.0f, 0.0f, 0.0f), float3(0.0f, 1.0f, 0.0f), float3(0.0f, 0.0f, 1.0f) };
    VsOut o;
    float4 p = float4(corners[vid], 0.5f, 1.0f);
    o.pos = mul(ubo.mvp, p);
    o.color = colors[vid];
    return o;
}

[shader("fragment")]
float4 fsMain(VsOut i) : SV_Target {
    return float4(i.color, 1.0f);
}
