// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Depth smoke path: instanced quads at explicit NDC depths, drawn with a
// reversed-Z GREATER depth test. CPU side verifies depth ordering after
// readback. The VS passes per-instance color to the FS through a user
// varying (SV_Position is a built-in; mixing built-ins and user locations
// must be explicit to keep the SPIR-V interface consistent).
struct DepthParams {
    float4 quads[8];   // xy = center, z = clip z (reversed-Z: near=1, far=0), w = half-size
    float4 colors[8];  // rgb = color, a unused
};

[[vk::binding(0, 0)]] ConstantBuffer<DepthParams> params;

struct VsOut {
    float4 pos : SV_Position;
    float4 color : COLOR0;
};

[shader("vertex")]
VsOut vsMain(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    float2 corners[6] = {
        float2(-1.0f, -1.0f), float2(1.0f, -1.0f), float2(1.0f, 1.0f),
        float2(-1.0f, -1.0f), float2(1.0f, 1.0f),  float2(-1.0f, 1.0f),
    };
    float4 q = params.quads[iid];
    VsOut o;
    o.pos = float4(q.xy + corners[vid] * q.w, q.z, 1.0f);
    o.color = params.colors[iid];
    return o;
}

[shader("fragment")]
float4 fsMain(VsOut i) : SV_Target {
    return float4(i.color.rgb, 1.0f);
}
