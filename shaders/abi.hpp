// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// CPU-side shader ABI contract. The reflection test
// (tests/test_shader_abi.cpp) verifies these layouts against the
// offline-compiled SPIR-V; no undocumented layout assumptions are allowed.
#pragma once

#include "helios/math/mat4.hpp"

#include <cstddef>
#include <cstdint>

namespace helios::shaders {

// triangle.slang: set 0, binding 0 (std140)
struct TriangleUbo {
    math::Mat4f mvp;
};
static_assert(sizeof(TriangleUbo) == 64, "TriangleUbo must be 64 bytes (std140 mat4)");

// depth.slang: set 0, binding 0 (std140)
struct DepthParams {
    struct Quad {
        float x, y, z, w; // center.xy, clip z, half-size
    };
    Quad quads[8];
    struct Color {
        float r, g, b, a;
    };
    Color colors[8];
};
static_assert(sizeof(DepthParams) == 8 * 16 + 8 * 16, "DepthParams size (std140 vec4 arrays)");
static_assert(offsetof(DepthParams, quads) == 0);
static_assert(offsetof(DepthParams, colors) == 128);

// compute.slang: push constants (8 bytes)
struct ComputeParams {
    std::uint32_t count;
    std::uint32_t seed;
};
static_assert(sizeof(ComputeParams) == 8);
static_assert(offsetof(ComputeParams, count) == 0);
static_assert(offsetof(ComputeParams, seed) == 4);

// Expected SPIR-V entry points (reflection test).
inline constexpr char const* kTriangleVertexEntry = "vsMain";
inline constexpr char const* kTriangleFragmentEntry = "fsMain";
inline constexpr char const* kDepthVertexEntry = "vsMain";
inline constexpr char const* kDepthFragmentEntry = "fsMain";
inline constexpr char const* kComputeEntry = "csMain";

} // namespace helios::shaders
