// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// The terrain of a planet or moon: a 16x16-cell patch per quadtree leaf,
// generated from the vertex index and one StructuredBuffer element selected
// by the instance index (SV_Vulkan* semantics map straight to
// gl_VertexIndex / gl_InstanceIndex).  The height, its gradient and the
// material come from lib/planet_noise.slang, so the camera on the CPU and
// the surface on the GPU agree to millimetres.
//
// Vertex positions are camera-relative: the patch anchor arrives already
// relative to the camera, in metres, and every vertex is the anchor plus a
// float32 offset, so a six-million-metre body keeps millimetre precision at
// eye level.  Crack-hiding skirts hang from the patch edges.
//
// Shading: a Lambert surface under one sun with an ambient term from the
// sky, materials by height, slope, temperature and humidity (snow where it
// is cold, rock where it is steep, vegetation where it is warm and humid,
// desert otherwise, beaches at the shore), an ocean at sea level with a
// Fresnel specular highlight.  The output is linear HDR radiance for the
// labs' post chain, or tone-mapped directly when `frame.planet.w` asks.
//
// Not modelled: multiple scattering in the atmosphere (the sky pass adds
// aerial perspective), shadows cast by relief, waves, vegetation structure.

#include "lib/planet_noise.slang"

struct FrameData {
    float4 row0;
    float4 row1;
    float4 row2;
    float4 row3;
    // x = nominal radius (m), y = wire overlay, z = debug-color mode,
    // w = tone-map here (1) or output linear HDR radiance (0)
    float4 planet;
    // xyz = body-fixed direction to the light; w = ambient fraction
    float4 light;
    // xyz = camera position in the body frame (m), w = time (s)
    float4 camera;
    // Shadow of a ring: x = inner radius (m), y = outer radius (m),
    // z = unused, w = on (0/1); the optical depth comes from `ring_profile`
    float4 ring;
    // A body that may shadow this one (its planet, for a moon): xyz = centre
    // in this body's frame (m), w = radius (m); 0 = none
    float4 occluder;
    // Clouds above this body: x = base radius (m), y = top radius (m),
    // z = cover, w = extinction of dense cloud (1/m); zero = none
    float4 clouds;
    // Colour of the sky's ambient light (rgb), w = unused
    float4 ambient_color; // w = drift of the clouds (rad)
};

struct PatchData {
    // x = u minimum, y = v minimum, z = uv extent, w = cube face [0,5]
    float4 uv_face;
    // xyz = nominal patch anchor relative to camera (m), w = quadtree level
    float4 anchor_level;
    // x/y = reserved, z = skirt depth (m), w = normalized SSE heat
    float4 debug;
    // xyz = the float32 anchor direction the lattice anchor refers to,
    // w = octaves to evaluate
    float4 anchor_direction;
    // Lattice cell and 32-bit fraction of octave 0 at the anchor.
    int4 lattice_cell;
    uint4 lattice_frac;
    // d direction / d(u, v) at the anchor, per unit of face coordinate.
    float4 tangent_u;
    float4 tangent_v;
};

[[vk::binding(0, 0)]] ConstantBuffer<FrameData> frame;
[[vk::binding(1, 0)]] StructuredBuffer<PatchData> patches;
[[vk::binding(2, 0)]] ConstantBuffer<PlanetBodyParams> body;
// Normal optical depth of the ring from its inner to its outer edge.
[[vk::binding(3, 0)]] Sampler2D<float4> ring_profile;

float3 cubeFaceVector(uint face, float u, float v) {
    switch (face) {
    case 0: return float3(1.0f, v, -u);   // +X
    case 1: return float3(-1.0f, v, u);   // -X
    case 2: return float3(u, 1.0f, -v);   // +Y
    case 3: return float3(u, -1.0f, v);   // -Y
    case 4: return float3(u, v, 1.0f);    // +Z
    default: return float3(-u, v, -1.0f); // -Z
    }
}

float3 palette(float level) {
    float t = frac(level * 0.137f);
    return 0.55f + 0.45f * cos(6.2831853f * (t + float3(0.00f, 0.33f, 0.67f)));
}

struct VsOut {
    float4 pos : SV_Position;
    float3 direction : TEXCOORD0; // unit direction from the body's centre
    float3 normal : TEXCOORD1;    // body frame
    float3 view : TEXCOORD2;      // position relative to the camera (m)
    // x = height above the radius (m), y = humidity, z = mountain belt,
    // w = skirt flag
    float4 surface : TEXCOORD3;
    float texture_factor : TEXCOORD5;
    float3 debug_color : COLOR0;
    float3 barycentric : TEXCOORD4;
};

[shader("vertex")]
VsOut vsMain(uint vertex_id : SV_VulkanVertexID, uint instance_id : SV_VulkanInstanceID) {
    const uint CELLS = 16u;
    const uint SURFACE_VERTICES = CELLS * CELLS * 6u;
    PatchData patch = patches[instance_id];

    uint triangle_vertex = vertex_id % 6u;
    float2 local;
    bool skirt_vertex = vertex_id >= SURFACE_VERTICES;
    bool skirt_bottom = false;
    if (!skirt_vertex) {
        uint cell = vertex_id / 6u;
        uint cell_x = cell % CELLS;
        uint cell_y = cell / CELLS;
        uint dx = (triangle_vertex == 1u || triangle_vertex == 2u || triangle_vertex == 4u) ? 1u : 0u;
        uint dy = (triangle_vertex == 2u || triangle_vertex == 4u || triangle_vertex == 5u) ? 1u : 0u;
        local = float2(float(cell_x + dx), float(cell_y + dy)) / float(CELLS);
    } else {
        uint skirt_id = vertex_id - SURFACE_VERTICES;
        uint segment = (skirt_id / 6u) % CELLS;
        uint edge = skirt_id / (CELLS * 6u);
        bool endpoint = triangle_vertex == 1u || triangle_vertex == 2u || triangle_vertex == 4u;
        skirt_bottom = triangle_vertex == 2u || triangle_vertex == 4u || triangle_vertex == 5u;
        float t = (float(segment) + (endpoint ? 1.0f : 0.0f)) / float(CELLS);
        if (edge == 0u) local = float2(0.0f, t);
        else if (edge == 1u) local = float2(1.0f, t);
        else if (edge == 2u) local = float2(t, 0.0f);
        else local = float2(t, 1.0f);
    }

    float2 uv = patch.uv_face.xy + local * patch.uv_face.z;
    uint face = uint(patch.uv_face.w + 0.5f);
    float3 direction = normalize(cubeFaceVector(face, uv.x, uv.y));
    float3 anchor = patch.anchor_direction.xyz;
    float2 duv = (local - 0.5f) * patch.uv_face.z;

    SurfacePoint point;
    point.cell0 = patch.lattice_cell.xyz;
    point.frac0 = patch.lattice_frac.xyz;
    point.delta_coarse = direction - anchor;
    point.delta_fine = patch.tangent_u.xyz * duv.x + patch.tangent_v.xyz * duv.y;
    point.direction = direction;
    uint octaves = uint(patch.anchor_direction.w);
    Surface s = planetSurface(point, octaves);
    float radius = frame.planet.x;
    // Where the ground lies under water the mesh is the sea surface.
    bool ocean = body.color_ocean_deep.w > 0.5f && s.height < body.shape.y;
    float height = ocean ? body.shape.y : s.height;
    if (skirt_bottom) {
        height -= patch.debug.z;
    }

    float3 relative_position = patch.anchor_level.xyz + (direction - anchor) * radius + direction * height;
    float4 point4 = float4(relative_position, 1.0f);

    VsOut output;
    output.pos = float4(dot(frame.row0, point4), dot(frame.row1, point4), dot(frame.row2, point4),
                        dot(frame.row3, point4));
    output.direction = direction;
    output.normal = ocean ? direction : planetNormal(direction, radius, s);
    output.view = relative_position;
    output.surface = float4(s.height, planetHumidity(point), s.belt,
                            skirt_vertex ? 1.0f : 0.0f);
    output.texture_factor = planetAlbedoTexture(point, octaves);

    float3 debug_color = float3(0.0f, 0.0f, 0.0f);
    if (frame.planet.z > 0.5f && frame.planet.z < 1.5f) {
        debug_color = palette(patch.anchor_level.w);
    } else if (frame.planet.z >= 1.5f && frame.planet.z < 2.5f) {
        debug_color = palette(float(face) * 1.7f + 0.5f);
    } else if (frame.planet.z >= 2.5f) {
        float heat = saturate(patch.debug.w);
        debug_color = lerp(float3(0.08f, 0.30f, 0.95f), float3(1.0f, 0.08f, 0.01f), heat);
    }
    output.debug_color = debug_color;
    uint tri_corner = triangle_vertex < 3u ? triangle_vertex : triangle_vertex - 3u;
    output.barycentric = tri_corner == 0u ? float3(1.0f, 0.0f, 0.0f) :
                           tri_corner == 1u ? float3(0.0f, 1.0f, 0.0f) :
                                              float3(0.0f, 0.0f, 1.0f);
    return output;
}

// ---------------------------------------------------------------------------
// Materials
// ---------------------------------------------------------------------------

// Gas giant: the cloud deck.  Zonal jets stretch the cloud pattern along
// latitude circles; the bands alternate with latitude, storms sit as ovals
// between them.  A stand-in for a circulation model: the structure is a
// pattern, not a simulation.
float3 gasGiantAlbedo(float3 direction) {
    float3 squashed = normalize(direction * float3(0.12f, 0.12f, 1.0f));
    float turbulence = 0.0f;
    float amplitude = 0.5f;
    for (uint k = 0u; k < 4u; ++k) {
        float frequency = 6.0f * float(1u << k);
        turbulence += amplitude * planetValueNoiseD(int3(0, 0, 0), squashed * frequency, body.seeds2.z, k).x;
        amplitude *= 0.5f;
    }
    float latitude = asin(clamp(direction.z, -1.0f, 1.0f));
    float phase = sin(latitude) * body.bands.y * PLANET_PI + body.bands.z * 2.0f * turbulence;
    float band = 0.5f + 0.5f * sin(phase);
    float zone = 0.5f + 0.5f * sin(2.0f * phase + 1.3f);
    float3 color = lerp(body.band_color_a.rgb, body.band_color_b.rgb, band);
    color = lerp(color, body.band_color_c.rgb, 0.35f * zone * (1.0f - band));
    // Storms: ovals where a low-frequency field peaks, stretched zonally.
    float storm_field = planetValueNoiseD(int3(0, 0, 0), normalize(direction * float3(0.5f, 0.5f, 1.0f)) * 9.0f,
                                          body.seeds2.z ^ 0x68E31DA4u, 7u).x;
    float storm = smoothstep(1.0f - 0.9f * body.bands.w, 1.0f - 0.6f * body.bands.w, storm_field);
    color = lerp(color, body.band_color_b.rgb * 1.08f, storm);
    // Polar regions are darker and bluer (less sunlight, deeper haze).
    color *= lerp(1.0f, 0.75f, smoothstep(0.75f, 1.0f, abs(direction.z)));
    return color;
}

// Fresnel reflectance of water (Schlick, F0 = 0.02).
float waterFresnel(float cos_theta) {
    float f = pow(1.0f - saturate(cos_theta), 5.0f);
    return 0.02f + 0.98f * f;
}

float3 toneMap(float3 x) {
    // Fitted ACES curve (Narkowicz), for the stand-alone planet lab's
    // sRGB swap chain.
    return saturate((x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f));
}

[shader("fragment")]
float4 fsMain(VsOut input) : SV_Target {
    float3 direction = normalize(input.direction);
    float3 normal = normalize(input.normal);
    float3 to_light = normalize(frame.light.xyz);
    float3 to_camera = normalize(-input.view);

    // A star: the patch geometry is the photosphere, so there is no surface
    // to light.  Emissive with Eddington limb darkening
    // I(mu) = I(0) * (1 - u * (1 - mu)), u = 0.6, and two octaves of
    // granulation that drift slowly.  This keeps a star on the same LOD and
    // camera-relative precision path as a planet.
    if (body.bands.x > 1.5f) {
        // mu is the cosine of the view angle; at the limb it reaches zero.
        // The screen-space footprint of mu gives a one-to-two-pixel soft
        // edge, so a small solar disk is not a staircase even at 1x.
        float mu_raw = dot(normal, to_camera);
        float footprint = max(fwidth(mu_raw), 1.0e-4f);
        float coverage = smoothstep(0.0f, footprint * 1.5f, mu_raw);
        float mu = saturate(mu_raw);
        float limb = 1.0f - 0.6f * (1.0f - mu);
        float drift = frame.camera.w * 0.02f;
        float3 q = direction * 45.0f + float3(drift, 0.0f, -drift);
        float g0 = planetValueNoiseD(int3(0, 0, 0), q, body.seeds[0], 3u).x;
        float g1 = planetValueNoiseD(int3(0, 0, 0), q * 3.1f, body.seeds[1], 4u).x;
        float granulation = 0.86f + 0.22f * (0.7f * g0 + 0.3f * g1);
        float3 radiance = float3(1.0f, 0.91f, 0.74f) * (limb * granulation * coverage) * 2.5f;
        if (frame.planet.w > 0.5f) {
            return float4(toneMap(radiance * 3.0f), 1.0f);
        }
        return float4(radiance, 1.0f);
    }

    float height = input.surface.x;
    float sea = body.shape.y;
    bool ocean = body.color_ocean_deep.w > 0.5f && height < sea;

    float3 albedo;
    float3 shading_normal = normal;
    float specular = 0.0f;
    if (body.bands.x > 0.5f) {
        albedo = gasGiantAlbedo(direction);
        shading_normal = direction;
    } else if (ocean) {
        // Water: colour from depth (shallow shelves turquoise, deep blue),
        // a mirror-like Fresnel highlight of the sun.
        float depth = sea - height;
        albedo = lerp(body.color_ocean_shallow.rgb, body.color_ocean_deep.rgb,
                      saturate(depth / max(body.color_snow.w, 1.0f)));
        shading_normal = direction;
        float3 half_vector = normalize(to_light + to_camera);
        float cos_half = saturate(dot(shading_normal, half_vector));
        // A narrow lobe for the sun's half-degree disc.
        float lobe = pow(cos_half, 1500.0f) * (1502.0f / (2.0f * PLANET_PI));
        specular = body.color_ocean_shallow.w * waterFresnel(dot(shading_normal, to_camera)) * lobe * 0.25f;
    } else {
        // Climate: colder towards the poles and with altitude.
        float latitude = asin(clamp(direction.z, -1.0f, 1.0f));
        float temperature = body.climate.x - body.climate.y * (1.0f - cos(latitude)) -
                            body.climate.z * max(height - sea, 0.0f);
        float humidity = saturate(0.5f + 0.5f * input.surface.y + body.climate.w);
        float warmth = smoothstep(270.0f, 290.0f, temperature) * (1.0f - smoothstep(305.0f, 325.0f, temperature));
        float3 ground = lerp(body.color_desert.rgb, body.color_vegetation.rgb,
                             smoothstep(0.35f, 0.65f, humidity) * warmth);
        // Beaches just above the shore.
        float beach = body.color_ocean_deep.w > 0.5f
                          ? 1.0f - smoothstep(0.0f, max(body.color_desert.w, 1.0f), height - sea)
                          : 0.0f;
        ground = lerp(ground, body.color_desert.rgb * 1.35f, beach);
        // Bare rock on steep slopes.
        float steep = 1.0f - smoothstep(0.80f, 0.92f, dot(normal, direction));
        albedo = lerp(ground, body.color_rock.rgb, steep);
        // Snow where it is cold, less on steep slopes.
        // Snow where it is cold and there is moisture to fall; a dry world
        // frosts over only where it is cold enough for its air to freeze
        // out (polar caps).
        float moisture = saturate(humidity * 1.5f);
        float frost = smoothstep(200.0f, 170.0f, temperature);
        float snow = smoothstep(277.0f, 268.0f, temperature) * lerp(frost, 1.0f, moisture) *
                     (1.0f - 0.7f * steep);
        albedo = lerp(albedo, body.color_snow.rgb, snow);
        albedo *= lerp(input.texture_factor, 1.0f, 0.6f * snow);
    }

    float cos_sun = dot(shading_normal, to_light);
    float lit = saturate(cos_sun);
    float3 position = direction * (frame.planet.x + height);
    // Shadow of the ring on the surface: where the sun ray crosses the ring plane.
    if (frame.ring.w > 0.5f && abs(to_light.z) > 1.0e-4f) {
        float t = -position.z / to_light.z;
        if (t > 0.0f) {
            float3 crossing = position + to_light * t;
            float r = length(crossing.xy);
            if (r > frame.ring.x && r < frame.ring.y) {
                float tau = ring_profile.Sample(float2((r - frame.ring.x) / (frame.ring.y - frame.ring.x), 0.5f)).r;
                lit *= exp(-tau / abs(to_light.z));
            }
        }
    }
    // Shadow of another body (a moon in its planet's shadow).
    if (frame.occluder.w > 0.0f) {
        float3 to_occluder = frame.occluder.xyz - position;
        float along = dot(to_occluder, to_light);
        if (along > 0.0f) {
            float3 across = to_occluder - to_light * along;
            float miss = length(across) - frame.occluder.w;
            // Soft edge over the sun's angular size.
            lit *= smoothstep(-0.00465f * along, 0.00465f * along, miss);
        }
    }
    // Shadow of the clouds: the cover where the sun ray crosses the layer.
    if (frame.clouds.w > 0.0f) {
        float r = length(position);
        float base = frame.clouds.x;
        float cos_zenith = dot(direction, to_light);
        if (cos_zenith > 0.05f && r < base) {
            float t = (base - r) / cos_zenith;
            float3 crossing = normalize(position + to_light * t);
            float cover = cloudCoverage(crossing, frame.ambient_color.w, frame.clouds.z);
            // Billows are opaque to the direct sun; the fraction of them is
            // the shadow.
            lit *= 1.0f - 0.97f * cloudOpacity(cover);
        }
    }
    // Ambient: the sky's light, strongest facing up, fading on the night side.
    float ambient = frame.light.w * (0.5f + 0.5f * dot(shading_normal, direction)) *
                    smoothstep(-0.25f, 0.1f, dot(direction, to_light));
    float3 radiance = albedo * (lit + ambient * frame.ambient_color.rgb) / PLANET_PI +
                      float3(specular, specular, specular) * lit;

    if (frame.planet.z > 0.5f) {
        radiance = input.debug_color * (0.3f + 0.7f * lit);
    }
    // Skirts shade like the surface above them; the inspection modes darken them.
    bool mark_skirt = input.surface.w > 0.5f && (frame.planet.z > 0.5f || frame.planet.y > 0.5f);
    if (mark_skirt) {
        radiance *= 0.52f;
    }
    float nearest_edge = min(input.barycentric.x, min(input.barycentric.y, input.barycentric.z));
    float edge_width = max(fwidth(nearest_edge) * 1.25f, 0.0005f);
    float edge = 1.0f - smoothstep(0.0f, edge_width, nearest_edge);
    radiance = lerp(radiance, float3(0.005f, 0.008f, 0.012f), edge * saturate(frame.planet.y) * 0.86f);

    if (frame.planet.w > 0.5f) {
        return float4(toneMap(radiance * 3.0f), 1.0f);
    }
    return float4(radiance, 1.0f);
}
