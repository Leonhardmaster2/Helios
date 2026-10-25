// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Deep-sky lab: the star field behind the galaxy, plus the smooth light of
// the galaxy's old stars, which shaders/galaxy_stars.slang draws into a
// half-resolution target.  Its young stars and ionised gas are drawn over
// this pass at full resolution.

#include "lib/deep_sky.slang"

[[vk::binding(3, 0)]] Sampler2D galaxy_light;

[shader("fragment")]
float4 fsMain(VsOut input) : SV_Target {
    return float4(backgroundSky(cameraRay(input.pos.xy)) + galaxy_light.SampleLevel(input.uv, 0.0f).rgb,
                  1.0f);
}
