// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Deep-sky lab: how much of each star's light reaches the camera through the
// dust of the gas volume.  One texel per star of a MAX_SOURCES x 1 target, so
// the dust is integrated once per star and frame instead of once per pixel of
// the star's image.

#include "lib/deep_sky.slang"
#include "lib/point_sources.slang"

[shader("fragment")]
float4 fsMain(VsOut input) : SV_Target {
    int index = int(input.pos.x);
    if (index >= min(int(frame.object[0].w), MAX_SOURCES)) {
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }
    return float4(sourceTransmittance(sources.data[3 * index + 2].xyz, frame.camera_position.xyz),
                  1.0f);
}
