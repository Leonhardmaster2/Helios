// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Compute smoke path: deterministic kernel writing into an SSBO.
// out[i] = (i * 0x9E3779B1u + seed) ^ (i >> 1)  — CPU-verifiable exactly.
[[vk::binding(0, 0)]] RWStructuredBuffer<uint> out_buf;

struct ComputeParams {
    uint count;
    uint seed;
};
[[vk::push_constant]] ComputeParams params;

[shader("compute")]
[numthreads(64, 1, 1)]
void csMain(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= params.count) {
        return;
    }
    uint i = tid.x;
    out_buf[i] = (i * 0x9E3779B1u + params.seed) ^ (i >> 1u);
}
