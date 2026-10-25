// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Deep-sky lab: the stars of the galaxy near the camera, one by one.
//
// A star particle of the N-body run stands for tens of thousands of stars
// born together (examples/deep_sky_lab/stellar_systems.hpp).  For the
// particles near the camera the CPU lists how many of their stars are bright
// enough to see from where it is; each instance here is one of them: its
// initial mass from its rank in the Kroupa mass function, its state from its
// age (lib/stellar.slang, the same functions the CPU uses), its place a
// Gaussian offset from the particle's centre.  Its light is a black body of
// its radius and temperature, drawn as the image of a point (a Gaussian of
// frame.object[6].w pixels), dimmed and reddened by the dust between it and
// the camera.  The particle's own image keeps the light of the stars that are
// not drawn (galaxy_stars.slang).
//
// Units: parsec relative to the camera, megayear, solar units.  frame.object
// as for galaxy_stars.slang, and [6] x = particle of the visited star (not
// drawn here; star_system.slang draws it), y = its rank, z = ln(luminance of
// the solar surface) on the black-body table's scale, w = image sigma (px).

#include "lib/deep_sky_frame.slang"
#include "lib/sky.slang"
#include "lib/stellar.slang"

[[vk::binding(1, 0)]] Texture2D<float4> blackbody_table;

struct NearParticle {
    float4 position_age; // xyz = centre relative to the camera (pc), w = age (Myr)
    float4 shape;        // x = spread (pc), y = thickness (pc), z = fade, w = V-band optical depth
    uint4 stars;         // x = particle, y = stars born, z = remnant ranks, w = slots drawn
};
[[vk::binding(2, 0)]] StructuredBuffer<NearParticle> near_particles;
[[vk::binding(3, 0)]] StructuredBuffer<uint2> star_chunks;

static const uint CHUNK = 16u;
static const float GAUSSIAN_EXTENT = 2.5f;
static const float GAUSSIAN_EDGE = 0.0439369f; // exp(-GAUSSIAN_EXTENT^2 / 2)
static const float GAUSSIAN_SCALE = 1.2212f;
// Half floats end at 65504; brighter is clipped (and shows as glare).
static const float MAX_RADIANCE = 6.0e4f;

struct StarVsOut {
    float4 pos : SV_Position;
    float2 offset : TEXCOORD0;
    nointerpolation float3 light : TEXCOORD1;
};

[shader("vertex")]
StarVsOut vsMain(uint vertex_id : SV_VulkanVertexID, uint instance_id : SV_VulkanInstanceID) {
    StarVsOut output;
    output.pos = float4(2.0f, 2.0f, 0.0f, 1.0f); // outside the clip volume
    output.offset = float2(0.0f, 0.0f);
    output.light = float3(0.0f, 0.0f, 0.0f);

    uint2 chunk = star_chunks[instance_id / CHUNK];
    NearParticle record = near_particles[chunk.x];
    uint slot = chunk.y + instance_id % CHUNK;
    if (slot >= record.stars.w) {
        return output;
    }
    uint particle = record.stars.x;
    uint count = record.stars.y;
    uint dead = record.stars.z;
    uint living = count - dead;
    // Slots run over the living stars from the brightest, then the remnants.
    uint rank = slot < living ? dead + slot : slot - living;
    if (float(particle) == frame.object[6].x && float(rank) == frame.object[6].y) {
        return output;
    }
    float mass = imfTailMass((float(rank) + starUnit(particle, rank, 0u)) / float(count));
    StarLight star = evolveStar(mass, record.position_age.w);
    if (star.kelvin <= 0.0f) {
        return output;
    }
    float3 position = record.position_age.xyz + starOffset(particle, rank, record.shape.x, record.shape.y);
    float depth = dot(position, frame.camera_forward.xyz);
    if (depth <= 1.0e-9f) {
        return output;
    }
    // Light in solar photopic luminosities: R^2 Y(T) / Y(T_sun).
    float4 entry = blackbodyEntry(blackbody_table, frame.radiometry, star.kelvin);
    float luminosity = star.radius * star.radius * exp(entry.a - frame.object[6].z);
    float sigma_px = frame.object[6].w;
    float sigma_pc = sigma_px * depth * pixelAngle();
    float3 transmittance = exp(-DUST_EXTINCTION * record.shape.w);
    float3 radiance = entry.rgb * (luminosity * record.shape.z / (6.2831853f * sigma_pc * sigma_pc)) *
                      frame.object[1].w * transmittance;
    radiance = min(radiance, float3(MAX_RADIANCE, MAX_RADIANCE, MAX_RADIANCE));

    float2 corner = float2(float(vertex_id & 1u), float((vertex_id >> 1u) & 1u)) * 2.0f - 1.0f;
    float2 right_scaled = float2(dot(frame.camera_right.xyz, frame.camera_right.xyz),
                                 dot(frame.camera_up.xyz, frame.camera_up.xyz));
    float2 centre = float2(dot(position, frame.camera_right.xyz), dot(position, frame.camera_up.xyz)) /
                    (depth * right_scaled);
    float2 half_size = GAUSSIAN_EXTENT * sigma_px * 2.0f * frame.viewport.zw;
    output.pos = float4(centre + corner * half_size, 0.0f, 1.0f);
    output.offset = corner * GAUSSIAN_EXTENT;
    output.light = radiance;
    return output;
}

[shader("fragment")]
float4 fsMain(StarVsOut input) : SV_Target {
    float r2 = dot(input.offset, input.offset);
    float profile = GAUSSIAN_SCALE * max(exp(-0.5f * r2) - GAUSSIAN_EDGE, 0.0f);
    return float4(input.light * profile, 0.0f);
}
