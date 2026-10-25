// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Black-hole lab, trace pass: one geodesic per pixel centre, written as HDR
// radiance plus the data the refinement pass needs to add samples only where
// the image requires them (see black_hole_refine.slang).  The model lives in
// lib/black_hole_trace.slang; the vertex stage is fullscreen.slang.

#include "lib/black_hole_trace.slang"

struct TraceOutput {
    // rgb = radiance, a = pixel class (0 captured, 1 sky only, 2 disk involved)
    float4 color : SV_Target0;
    // x = sky footprint (rad / px, isotropic), yzw = disk footprint (M / px) of image orders 0-2
    float4 footprints : SV_Target1;
};

[shader("fragment")]
TraceOutput fsMain(VsOut input) {
    float2 pixel = input.pos.xy;

    // The centre sample is traced in uniform control flow so its screen-space
    // derivatives are defined; they provide the filter footprints reused by
    // every additional sample of this pixel, here or in the refinement pass.
    Trace centre = traceRay(cameraRay(pixel));

    Footprints footprints;
    footprints.sky.dx = ddx(centre.sky_dir);
    footprints.sky.dy = ddy(centre.sky_dir);
    footprints.sky_isotropic = 0.0f;
    float sky_footprint = max(length(footprints.sky.dx), length(footprints.sky.dy));
    for (int order = 0; order < MAX_HITS; ++order) {
        footprints.hit[order] = max(length(ddx(centre.hit_xy[order])),
                                    length(ddy(centre.hit_xy[order])));
    }
    float impact_footprint = abs(ddx(centre.impact)) + abs(ddy(centre.impact));

    TraceOutput output;
    output.footprints =
        float4(sky_footprint, footprints.hit[0], footprints.hit[1], footprints.hit[2]);
    float pixel_class =
        centre.hit_count > 0 ? 2.0f : (centre.termination == TERMINATION_ESCAPED ? 1.0f : 0.0f);

    float output_mode = frame.effects.w;
    if (output_mode > 0.5f && output_mode < 1.5f) {
        // Geometry record for CPU-reference validation: w = class.  Only
        // crossings inside the nominal disk count; the rim margin does not.
        output.color = float4(0.0f, 0.0f, 0.0f, 0.0f);
        if (centre.termination == TERMINATION_ESCAPED) {
            output.color = float4(centre.sky_dir, 1.0f);
        }
        for (int order = MAX_HITS - 1; order >= 0; --order) {
            if (order < centre.hit_count && centre.hit_radius[order] < frame.disk.y) {
                output.color = float4(centre.hit_radius[order], centre.hit_xy[order], 2.0f);
            }
        }
        return output;
    }
    if (output_mode >= 1.5f) {
        float load = centre.steps / 160.0f;
        output.color = float4(saturate(load * 2.0f),
                              saturate(2.0f - load * 2.0f) * saturate(load * 4.0f),
                              saturate(1.0f - load * 4.0f), pixel_class);
        return output;
    }

    float3 color = shadeTrace(centre, footprints);

    if (frame.refine.z < 0.5f) {
        // No refinement pass: supersample here, everywhere if requested, and
        // always in the thin band around the critical curve where the photon
        // ring and the shadow edge are sub-pixel features.
        bool near_critical =
            abs(centre.impact - criticalImpact()) < max(0.05f, 2.5f * impact_footprint);
        int samples = max(int(frame.integration.z), 1) +
                      (near_critical ? int(frame.integration.w) : 0);
        for (int sample = 1; sample < samples; ++sample) {
            color += shadeTrace(traceRay(cameraRay(pixel + sampleJitter(sample))), footprints);
        }
        color /= float(samples);
    }
    output.color = float4(color, pixel_class);
    return output;
}
