// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// M1 GPU precision laboratory: renders the synthetic body route through the
// REAL Vulkan path. The frame path is exactly:
//
//   FrameSnapshot -> camera pose / CameraReparenter -> SceneAssembler ->
//   NearRenderRecord / DistantBodyRecord -> GpuFramePlan -> Vulkan push-
//   constant pipelines -> readback -> measurements + PNG captures.
//
// There is no second coordinate path: every rendered offset comes from an
// AssembledScene produced by SceneAssembler, and every astronomical
// subtraction happened in binary64 before any float32 conversion.
//
// Evidence gate: pass_fail contains one entry per required category plus an
// overall boolean; the process exits nonzero when any category fails.
// Missing measurements are failures in their category, never zeros.
#pragma once

#include "helios/app/cli.hpp"

namespace helios::app {

// Runs the GPU lab. Returns 0 when all thresholds pass, 3 on threshold
// failure, 2 on errors. Evidence goes under options.report_dir
// (default evidence/m1/gpu): precision_gpu.json + captures/*.png.
int runPrecisionLabGpu(CliOptions const& options);

} // namespace helios::app
