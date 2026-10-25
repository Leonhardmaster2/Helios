// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Deep-sky lab: display of a simulated gas volume (the emission nebula and
// the supernova remnant).
//
// `emission_volume` is written by shaders/hydro_shade.slang from the state
// of the gas-dynamics simulation: rgb is the light emitted per unit length,
// a is dust.  This pass integrates emission and wavelength-dependent dust
// extinction along each camera ray and adds the scene's stars as point
// sources, each dimmed and reddened by the dust between it and the camera
// (shaders/point_source_dust.slang).  It adds no structure of its own.
//
// frame.object:
//   [0] x = half extent of the volume, y = emission scale,
//       z = visual optical depth per unit dust per unit length,
//       w = point source count
//   [1] x = rotation of the diffraction spikes (rad), y = stretch parameter
//       of the gas grid (lib/hydro.slang; 0 = uniform)
//   [2] rgb = light the medium outside the box scatters per unit length at
//       one parsec from the star, w = 1 when the medium continues beyond the
//       box (the nebula; the remnant's surroundings are dark)
//   [3] xyz = position of the star whose light it scatters
//
// The box is finite but the medium around the cloud is not: the thin gas
// beyond the faces scatters the star's light too, falling off with the
// square of the distance from the star.  Beyond the box the display adds
// that light analytically, continuing from what the volume holds at the
// face it leaves (so shadows the cloud casts carry on), and for rays that
// miss the box altogether it adds the medium's own light.  Without this
// the faces of the box show as a faint rectangle against the sky.

#include "lib/deep_sky.slang"
#include "lib/point_sources.slang"

// One texel per source: rgb = fraction of its light that reaches the camera.
[[vk::binding(5, 0)]] Texture2D<float4> source_light;

// A star's image extends this many spike lengths from its centre.
static const float SPIKE_REACH = 7.0f;

// Image of a point source through a telescope: a Gaussian core and the four
// diffraction spikes of the secondary mirror's supports.  The wide glow
// around bright stars is added later by the glare pass.  The image fades to
// exactly zero at `reach`, so its bounding circle cannot be seen.
float starProfile(float2 offset, float sigma, float spike_length, float reach, float rotation) {
    float r2 = dot(offset, offset);
    float window = saturate(1.0f - r2 / (reach * reach));
    float core = exp(-0.5f * r2 / (sigma * sigma));
    float s = sin(rotation);
    float c = cos(rotation);
    float2 q = float2(offset.x * c + offset.y * s, offset.y * c - offset.x * s);
    float width = 0.55f * sigma;
    float2 across = exp(-0.5f * (q.yx * q.yx) / (width * width));
    float2 along = exp(-abs(q) / spike_length);
    float spikes = 0.20f * (across.x * along.x + across.y * along.y);
    return (core + spikes) * window * window;
}

// The grid's faces are where the simulation stops, not where the gas does:
// the display fades the volume out over the outermost tenth of the grid
// coordinate (the coarse outer cells of the stretched grid), so that no
// face shows as an edge from any distance.  The medium beyond continues
// analytically from just inside the fade.
float boxWindow(float3 uvw) {
    float3 edge = min(uvw, 1.0f - uvw);
    float3 w = smoothstep(0.0f, 0.1f, edge);
    return w.x * w.y * w.z;
}

// Integral along the ray from t_a to t_b of 1 / |p(t) - star|^2, which is
// (t - t_c)^2 + b^2 with t_c the closest approach and b the miss distance.
float inverseSquareIntegral(float3 origin, float3 direction, float3 star, float t_a, float t_b) {
    float3 to_star = star - origin;
    float t_c = dot(to_star, direction);
    float b = sqrt(max(dot(to_star, to_star) - t_c * t_c, 1.0e-4f));
    float upper = t_b > 1.0e29f ? 0.5f * PI : atan((t_b - t_c) / b);
    return (upper - atan((t_a - t_c) / b)) / b;
}

[shader("fragment")]
float4 fsMain(VsOut input) : SV_Target {
    float2 pixel = input.pos.xy;
    float3 origin = frame.camera_position.xyz;
    float3 direction = cameraRay(pixel);
    float half_extent = frame.object[0].x;
    bool continues = frame.object[2].w > 0.5f;
    float3 star = frame.object[3].xyz;

    float3 color = float3(0.0f, 0.0f, 0.0f);
    float3 transmittance = float3(1.0f, 1.0f, 1.0f);

    float t_near;
    float t_far;
    if (intersectBox(origin, direction, float3(half_extent, half_extent, half_extent), t_near,
                     t_far)) {
        t_near = max(t_near, 0.0f);
        if (continues && t_near > 0.0f) {
            // The medium between the camera and the box: what the volume
            // holds just inside the face, continued outwards by 1 / r^2.
            float3 entry = origin + direction * (t_near + 0.15f * half_extent);
            float3 unused_scale;
            float3 face_light =
                emission_volume.SampleLevel(gasGridCoordinate(gasGrid(), entry, unused_scale), 0.0f).rgb;
            float r2 = dot(entry - star, entry - star);
            // Only the thin medium continues: the face sample is capped at the
            // medium's own light there, so dense gas at a face does not streak
            // outwards.
            float3 continued = min(face_light * (frame.object[0].y * r2), frame.object[2].rgb);
            color += continued * inverseSquareIntegral(origin, direction, star, 0.0f, t_near);
        }
        int steps = max(int(frame.quality.x), 8);
        // The ray is cut into segments; the stretched grid's coordinate is
        // computed exactly at their ends and interpolated between (over a
        // thirty-second of the path the error is a fraction of a cell).
        // Within a segment the steps follow the cell's extent along the
        // ray: fine through the grid's centre, long through its coarse
        // outskirts.
        static const int SEGMENTS = 32;
        GasGrid grid = gasGrid();
        float path = t_far - t_near;
        float segment_length = path / float(SEGMENTS);
        float base = path / float(steps);
        float jitter = marchJitter(pixel);
        float3 opacity = DUST_EXTINCTION * frame.object[0].z;
        float3 scale_end;
        float3 uvw_end = gasGridCoordinate(grid, origin + direction * t_near, scale_end);
        bool opaque = false;
        for (int segment = 0; segment < SEGMENTS && !opaque; ++segment) {
            float t_end = t_near + segment_length * float(segment + 1);
            float3 uvw0 = uvw_end;
            float3 scale0 = scale_end;
            uvw_end = gasGridCoordinate(grid, origin + direction * t_end, scale_end);
            float ds = base * gasStepScale(0.5f * (scale0 + scale_end), direction);
            int count = clamp(int(ceil(segment_length / ds)), 1, 64);
            ds = segment_length / float(count);
            float offset = frac(jitter + 0.618034f * float(segment));
            for (int index = 0; index < count; ++index) {
                float3 uvw = lerp(uvw0, uvw_end, (float(index) + offset) / float(count));
                float4 voxel = emission_volume.SampleLevel(uvw, 0.0f) * boxWindow(uvw);
                // Emission and absorption are uniform across the step.
                float3 depth = opacity * (voxel.a * ds);
                float3 through = exp(-depth);
                float3 weight = select(depth > 1.0e-4f, (1.0f - through) / max(depth, 1.0e-4f), 1.0f);
                color += transmittance * voxel.rgb * (frame.object[0].y * ds) * weight;
                transmittance *= through;
                if (max(transmittance.r, max(transmittance.g, transmittance.b)) < 0.003f) {
                    opaque = true;
                    break;
                }
            }
        }
        if (continues && !opaque) {
            // The medium beyond the far face.
            float3 exit = origin + direction * (t_far - 0.15f * half_extent);
            float3 unused_scale;
            float3 face_light =
                emission_volume.SampleLevel(gasGridCoordinate(gasGrid(), exit, unused_scale), 0.0f).rgb;
            float r2 = dot(exit - star, exit - star);
            float3 continued = min(face_light * (frame.object[0].y * r2), frame.object[2].rgb);
            color += transmittance * continued *
                     inverseSquareIntegral(origin, direction, star, t_far, 1.0e30f);
        }
    } else if (continues) {
        // The ray misses the box: the medium's own light along all of it.
        color += frame.object[2].rgb * inverseSquareIntegral(origin, direction, star, 0.0f, 1.0e30f);
    }

    color += transmittance * backgroundSky(direction);

    int count = min(int(frame.object[0].w), MAX_SOURCES);
    for (int index = 0; index < count; ++index) {
        float4 placement = sources.data[3 * index];
        float2 offset = pixel - placement.xy;
        float reach = SPIKE_REACH * placement.w;
        if (dot(offset, offset) >= reach * reach) {
            continue;
        }
        color += sources.data[3 * index + 1].rgb * source_light.Load(int3(index, 0, 0)).rgb *
                 starProfile(offset, placement.z, placement.w, reach, frame.object[1].x);
    }
    return float4(color, 1.0f);
}
