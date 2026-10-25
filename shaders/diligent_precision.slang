// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Headless Diligent/Vulkan precision contract. World-space subtraction is
// deliberately absent from this shader: authoritative binary64 world
// positions are rebased on the CPU, and only bounded camera-relative float32
// coordinates cross the GPU boundary.

struct PrecisionFrameData {
    float4 view_row_0;
    float4 view_row_1;
    float4 view_row_2;
    uint4 metadata; // x = valid sample count
};

[[vk::binding(0, 0)]] ConstantBuffer<PrecisionFrameData> precision_frame;
[[vk::binding(1, 0)]] StructuredBuffer<float4> camera_relative_positions;
[[vk::binding(2, 0)]] RWStructuredBuffer<float4> view_space_positions;

[shader("compute")]
[numthreads(8, 1, 1)]
void csMain(uint3 dispatch_thread_id : SV_DispatchThreadID) {
    uint index = dispatch_thread_id.x;
    if (index >= precision_frame.metadata.x) {
        return;
    }

    float4 point = camera_relative_positions[index];
    view_space_positions[index] = float4(
        dot(precision_frame.view_row_0, point),
        dot(precision_frame.view_row_1, point),
        dot(precision_frame.view_row_2, point),
        point.w);
}
