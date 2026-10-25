// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// M1 GPU precision lab: distant-body impostor pipeline. Renders a screen-
// space disc from a DistantBodyRecord (direction + angular radius + analytic
// distance), never from a float32 astronomical position. The fragment depth
// is the analytic reversed-Z value of the record distance, so impostors
// depth-order correctly against near geometry.
struct ImpostorPC {
    float4 dir_depth; // xyz = unit direction to centre in VIEW axes, w = ndc depth at centre
    float4 params;    // x = angular radius (rad), y = tan(fovY/2), z = aspect, w = pad
    float4 size_inv;  // x = 1/framebuffer width, y = 1/framebuffer height, zw = pad
    float4 color;     // rgba
};

[[vk::push_constant]] ConstantBuffer<ImpostorPC> pc;

struct VsOut {
    float4 pos : SV_Position;
};

[shader("vertex")]
VsOut vsMain(uint vid : SV_VertexID) {
    // Fullscreen triangle covering the viewport.
    VsOut o;
    float2 p = float2(-1.0f, -1.0f);
    if (vid == 1u) {
        p = float2(3.0f, -1.0f);
    } else if (vid == 2u) {
        p = float2(-1.0f, 3.0f);
    }
    o.pos = float4(p, 0.0f, 1.0f);
    return o;
}

struct FsOut {
    float4 color : SV_Target;
    float depth : SV_Depth;
};

[shader("fragment")]
FsOut fsMain(VsOut i) {
    // Pixel -> NDC -> view ray (Vulkan framebuffer y points down; camera
    // looks down -z in view space).
    FsOut o;
    float2 ndc;
    ndc.x = i.pos.x * 2.0f * pc.size_inv.x - 1.0f;
    ndc.y = 1.0f - i.pos.y * 2.0f * pc.size_inv.y;
    float3 ray = normalize(float3(ndc.x * pc.params.y * pc.params.z, ndc.y * pc.params.y, -1.0f));
    float cos_angle = dot(ray, pc.dir_depth.xyz);
    if (cos_angle < cos(pc.params.x)) {
        discard;
    }
    // Analytic reversed-Z depth at the record distance (near/d for the
    // infinite-far reversed-Z projection).
    o.color = float4(pc.color.rgb, pc.color.a);
    o.depth = pc.dir_depth.w;
    return o;
}
