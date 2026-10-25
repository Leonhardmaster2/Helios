// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Black-hole lab, adaptive refinement pass.  The trace pass took one sample
// per pixel; this pass adds samples only where one is not enough:
//
//   - in the thin band around the critical curve, found analytically from
//     the ray's impact parameter, where the photon ring and the shadow edge
//     are narrower than a pixel and a contrast test could miss them;
//   - where the disk is involved and the tone-mapped luminance differs from
//     a neighbour by more than a threshold (rim streamers, image-order
//     boundaries, sharp structure).
//
// Sky-only pixels never qualify: stars and the galactic band are filtered
// analytically by their footprint.  Everything else is copied through, so the
// cost of supersampling is paid by a few percent of the frame instead of all
// of it.  The extra samples reuse the footprints the trace pass measured.

#include "lib/black_hole_trace.slang"

[[vk::binding(2, 0)]] Texture2D<float4> scene;
[[vk::binding(3, 0)]] Texture2D<float4> footprint_map;

// Luminance after exposure, compressed like a display would show it, so the
// contrast test is uniform across dark and bright parts of the image.
float displayLuminance(float3 radiance) {
    float luminance = dot(radiance, float3(0.2126729f, 0.7151522f, 0.0721750f)) * frame.refine.x;
    return luminance / (1.0f + luminance);
}

[shader("fragment")]
float4 fsMain(VsOut input) : SV_Target {
    int2 pixel = int2(input.pos.xy);
    float4 centre = scene.Load(int3(pixel, 0));

    // The impact parameter is a closed-form function of the ray direction, so
    // its derivative is defined here without tracing anything.
    float impact = rayImpact(cameraRay(input.pos.xy));
    float impact_footprint = abs(ddx(impact)) + abs(ddy(impact));

    int extra = 0;
    if (abs(impact - criticalImpact()) < max(0.05f, 2.5f * impact_footprint)) {
        extra = int(frame.integration.w);
    } else if (frame.integration.z > 1.5f) {
        int2 limit = int2(frame.viewport.xy) - 1;
        float4 left = scene.Load(int3(clamp(pixel + int2(-1, 0), int2(0, 0), limit), 0));
        float4 right = scene.Load(int3(clamp(pixel + int2(1, 0), int2(0, 0), limit), 0));
        float4 up = scene.Load(int3(clamp(pixel + int2(0, -1), int2(0, 0), limit), 0));
        float4 down = scene.Load(int3(clamp(pixel + int2(0, 1), int2(0, 0), limit), 0));
        float disk_class = max(max(centre.a, left.a), max(max(right.a, up.a), down.a));
        if (disk_class > 1.5f) {
            float here = displayLuminance(centre.rgb);
            float contrast = max(max(abs(displayLuminance(left.rgb) - here),
                                     abs(displayLuminance(right.rgb) - here)),
                                 max(abs(displayLuminance(up.rgb) - here),
                                     abs(displayLuminance(down.rgb) - here)));
            if (contrast > frame.refine.y) {
                extra = int(frame.integration.z) - 1;
            }
        }
    }
    if (extra <= 0) {
        return centre;
    }

    float4 packed = footprint_map.Load(int3(pixel, 0));
    Footprints footprints;
    footprints.sky.dx = float3(0.0f, 0.0f, 0.0f);
    footprints.sky.dy = float3(0.0f, 0.0f, 0.0f);
    footprints.sky_isotropic = max(packed.x, 1.0e-7f);
    footprints.hit[0] = packed.y;
    footprints.hit[1] = packed.z;
    footprints.hit[2] = packed.w;

    float3 color = centre.rgb;
    for (int sample = 1; sample <= extra; ++sample) {
        color += shadeTrace(traceRay(cameraRay(input.pos.xy + sampleJitter(sample))), footprints);
    }
    return float4(color / float(extra + 1), centre.a);
}
