// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Full-screen triangle shared by every full-screen lab pass (geodesic trace,
// volumetric scenes, glare pyramid, composite).

struct VsOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

// SV_VulkanVertexID maps to gl_VertexIndex without the DrawParameters
// capability that SV_VertexID would require.
//
// Diligent gives its Vulkan backend Direct3D-style clip space (+y up, through
// a negative viewport height), so the first texel row, uv.y = 0, belongs at
// clip-space y = +1.  Getting this wrong mirrors every pass that samples by uv.
[shader("vertex")]
VsOut vsMain(uint vertex_id : SV_VulkanVertexID) {
    VsOut output;
    float2 corner = float2(float((vertex_id << 1u) & 2u), float(vertex_id & 2u));
    output.uv = corner;
    output.pos = float4(corner.x * 2.0f - 1.0f, 1.0f - corner.y * 2.0f, 0.0f, 1.0f);
    return output;
}
