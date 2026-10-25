// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// M1 GPU precision lab: near-geometry pipeline. Consumes NearRenderRecord
// offsets (bounded camera-relative float32) directly as push-constant data.
// The sphere mesh is generated procedurally from SV_VertexID (no vertex
// buffers): a SEGS x RINGS lat-long grid emitted as 6 vertices per cell.
// Vertex positions double as normals (unit sphere).
//
// Precision contract: `offset_radius.xyz` is the SceneAssembler-emitted
// camera-relative float32 record; the view transform is ROTATION ONLY, so no
// astronomical coordinate ever enters float32 arithmetic here.
//
// The PV matrix arrives as four explicit clip-space ROW vectors (vector
// members carry no matrix-layout ambiguity across shading languages):
//   clip = float4(dot(row0,w), dot(row1,w), dot(row2,w), dot(row3,w));
struct MeshPC {
    float4 row0;
    float4 row1;
    float4 row2;
    float4 row3;
    float4 offset_radius; // xyz = NearRenderRecord offset (m, float32), w = radius (m)
    float4 color;         // rgba
};

[[vk::push_constant]] ConstantBuffer<MeshPC> pc;

struct VsOut {
    float4 pos : SV_Position;
    float3 nrm : NORMAL;
    float3 color : COLOR0;
};

[shader("vertex")]
VsOut vsMain(uint vid : SV_VertexID) {
    const float PI = 3.14159265358979323846;
    const uint SEGS = 48;
    const uint RINGS = 24;
    const uint cell = vid / 6u;
    const uint corner = vid % 6u;
    const uint i = cell % SEGS;
    const uint j = cell / SEGS;
    // Two triangles per cell: (0,0)(1,0)(1,1) and (0,0)(1,1)(0,1).
    // corner: 0=(0,0) 1=(1,0) 2=(1,1) 3=(0,0) 4=(1,1) 5=(0,1)
    uint dx = (corner == 1u || corner == 2u || corner == 4u) ? 1u : 0u;
    uint dy = (corner == 2u || corner == 4u || corner == 5u) ? 1u : 0u;
    const float lon = (float(i + dx) / float(SEGS)) * 2.0f * PI;
    const float lat = ((float(j + dy) + 1.0f) / float(RINGS + 1)) * PI - PI * 0.5f;
    float3 n = float3(cos(lat) * cos(lon), sin(lat), cos(lat) * sin(lon));

    VsOut o;
    float4 world = float4(n * pc.offset_radius.w + pc.offset_radius.xyz, 1.0f);
    o.pos = float4(dot(pc.row0, world), dot(pc.row1, world), dot(pc.row2, world),
                   dot(pc.row3, world));
    o.nrm = n;
    // Simple fixed-light lambert for readable geometry.
    float3 l = normalize(float3(0.4f, 0.5f, -0.75f));
    float diff = max(dot(n, l), 0.0f);
    o.color = pc.color.rgb * (0.35f + 0.65f * diff);
    return o;
}

[shader("fragment")]
float4 fsMain(VsOut i) : SV_Target {
    return float4(i.color, pc.color.a);
}
