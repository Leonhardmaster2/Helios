// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Deep-sky lab gas dynamics: fastest signal speed, |v| + c, in each block of
// 8^3 cells.  The host reads the result back and takes the largest value to
// choose the next time steps (the Courant condition).  It also collects the
// block's mass and total energy, so a run can be checked for conservation.

#include "lib/hydro.slang"

[[vk::binding(1, 0)]] Texture3D<float4> state_a;
[[vk::binding(2, 0)]] Texture3D<float4> state_b;
// One texel per block at (bx + by * blocks, bz):
// r = fastest signal speed, g = mass, b = energy, a = mass of scalar 1.
[[vk::binding(3, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> block_summary;

[shader("compute")]
[numthreads(4, 4, 4)]
void csMain(uint3 id : SV_DispatchThreadID) {
    int n = cellsPerEdge();
    int blocks = n / 8;
    if (any(id >= uint3(blocks, blocks, blocks))) {
        return;
    }
    float fastest = 0.0f;
    float mass = 0.0f;
    float energy = 0.0f;
    float scalar = 0.0f;
    for (int z = 0; z < 8; ++z) {
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                int3 cell = int3(id) * 8 + int3(x, y, z);
                Pair u;
                u.a = state_a.Load(int4(cell, 0));
                u.b = state_b.Load(int4(cell, 0));
                Pair w = toPrimitive(u);
                fastest = max(fastest, length(w.a.yzw) + soundSpeed(w));
                float volume = cellVolumeFactor(cell);
                mass += u.a.x * volume;
                energy += u.b.x * volume;
                scalar += u.b.y * volume;
            }
        }
    }
    block_summary[uint2(id.x + id.y * uint(blocks), id.z)] = float4(fastest, mass, energy, scalar);
}
