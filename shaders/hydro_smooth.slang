// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Deep-sky lab gas dynamics: reconstruction filter of the emission volume.
//
// hydro_shade.slang gives every cell one value, and the brightest structures
// (ionisation fronts, shocked shells) are one or two cells thick.  Drawn as
// they are, such cells show the mesh as blocks and combs.  This pass applies
// the binomial filter (1 2 1) / 4 along each axis, which removes exactly the
// cell-to-cell alternation while keeping the total light; the display pass
// then interpolates a band-limited field.
//
// Each of the eight samples sits at a corner shared by eight cells, where the
// trilinear sampler returns their mean, so the eight together weigh the 27
// surrounding cells by (1 2 1)^3 / 64.

#include "lib/hydro.slang"

[[vk::binding(1, 0)]] Sampler3D emission_raw;
[[vk::binding(2, 0)]] [[vk::image_format("rgba16f")]] RWTexture3D<float4> emission_out;

[shader("compute")]
[numthreads(8, 8, 8)]
void csMain(uint3 id : SV_DispatchThreadID) {
    int n = cellsPerEdge();
    if (any(id >= uint3(n, n, n))) {
        return;
    }
    float inverse = 1.0f / float(n);
    float3 centre = (float3(id) + 0.5f) * inverse;
    float4 sum = float4(0.0f, 0.0f, 0.0f, 0.0f);
    for (int corner = 0; corner < 8; ++corner) {
        float3 offset = float3(float(corner & 1), float((corner >> 1) & 1), float(corner >> 2)) - 0.5f;
        sum += emission_raw.SampleLevel(centre + offset * inverse, 0.0f);
    }
    emission_out[id] = sum * 0.125f;
}
