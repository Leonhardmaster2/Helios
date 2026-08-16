// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Depth smoke path: instanced quads at explicit NDC depths, drawn with a
// reversed-Z GREATER depth test. CPU side verifies depth ordering after
// readback.
struct DepthParams {
    float4 quads[8];   // xy = center, z = clip z (reversed-Z: near=1, far=0), w = half-size
    float4 colors[8];  // rgb = color, a unused
};

[[vk::binding(0, 0)]] ConstantBuffer<DepthParams> params;

[shader("vertex")]
float4 vsMain(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    float2 corners[6] = {
        float2(-1.0f, -1.0f), float2(1.0f, -1.0f), float2(1.0f, 1.0f),
        float2(-1.0f, -1.0f), float2(1.0f, 1.0f),  float2(-1.0f, 1.0f),
    };
    float4 q = params.quads[iid];
    float2 p = q.xy + corners[vid] * q.w;
    return float4(p, q.z, 1.0f);
}

[shader("fragment")]
float4 fsMain(uint iid : SV_InstanceID) : SV_Target {
    return float4(params.colors[iid].rgb, 1.0f);
}
