// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// GPU precision lab frame plan: the pure-CPU bridge between the
// SceneAssembler output (NearRenderRecord / DistantBodyRecord) and the
// Vulkan draw lists. Deliberately Vulkan-free so the data/contract boundary
// is unit-testable; gpu_precision.cpp consumes this plan verbatim.
//
// Camera convention (documented, applied deliberately everywhere):
//   - the camera orientation quaternion rotates camera-local axes into the
//     storage frame;
//   - camera-local axes are right = +x, up = +y, forward = -z (Vulkan view
//     convention);
//   - the view transform is ROTATION ONLY: all geometry reaches the GPU as
//     camera-relative float32 offsets or view-axis directions, so no
//     astronomical coordinate ever enters float32 arithmetic.
#pragma once

#include "helios/core/error.hpp"
#include "helios/math/frames.hpp"
#include "helios/render/scene_assembler.hpp"

#include <array>
#include <vector>

namespace helios::render {

// One near object: the NearRenderRecord offset (float32) plus draw metadata.
struct MeshDraw {
    math::Vec3f offset_m{}; // straight from NearRenderRecord
    float radius_m = 0.0f;  // world radius (float32 by contract: bounded)
    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
};

// One distant body: view-axis direction + analytic depth (float32 at the
// boundary; computed in double on the CPU).
struct ImpostorDraw {
    math::Vec3f direction_view{}; // unit, view axes
    float ndc_depth = 0.0f;       // analytic reversed-Z depth of record distance
    float angular_radius_rad = 0.0f;
    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
};

struct GpuFramePlan {
    std::vector<MeshDraw> meshes;
    std::vector<ImpostorDraw> impostors;
    // Full projection * view-rotation matrix (column-major float[16]),
    // computed CPU-side from the documented rotation-only view. The mesh
    // shader consumes this verbatim.
    std::array<float, 16> pv{};
};

struct CameraView {
    math::Vec3d right{};  // storage-frame axes of the camera basis
    math::Vec3d up{};
    math::Vec3d forward{}; // direction the camera looks (storage axes)
};

// Basis from the camera orientation quaternion (local right/up/forward(-z)).
[[nodiscard]] CameraView cameraViewFromOrientation(math::Quatd const& orientation);

// Rotation-only view matrix (storage -> view) as column-major float[16].
[[nodiscard]] std::array<float, 16> viewRotationMatrix(CameraView const& view);

[[nodiscard]] std::array<float, 16> multiplyMat4(std::array<float, 16> const& a,
                                                 std::array<float, 16> const& b);

// Analytic double-precision projection of a storage-frame point to pixel
// coordinates (the CPU reference the GPU output is compared against).
struct PixelProjection {
    double width_px = 0.0;
    double height_px = 0.0;
    double tan_half_fov_y = 1.0;
    CameraView view{};
    math::Vec3d camera_position{}; // storage-frame
};

// Returns pixel coords, or {-1,-1} when the point is behind the camera.
[[nodiscard]] std::pair<double, double> projectToPixels(PixelProjection const& proj,
                                                        math::Vec3d const& storage_point);

// Builds the draw plan from an assembled scene. Classification metadata is
// supplied by the caller (which records were body vs landmarks etc.); this
// function applies the camera orientation to distant directions and the
// analytic depth conversion, and never invents geometry.
struct PlanOptions {
    float fov_y_rad = 1.04719755f; // 60 degrees
    float near_plane_m = 1.0f;
    std::vector<float> mesh_colors;      // rgba per mesh draw, flattened
    std::vector<float> impostor_colors;  // rgba per impostor draw, flattened
    std::vector<float> mesh_radii;       // per mesh draw
    double near_plane_md = 1.0;          // double twin of near_plane_m
};

[[nodiscard]] core::Result<GpuFramePlan> buildFramePlan(AssembledScene const& scene,
                                                        math::Quatd const& camera_orientation,
                                                        PixelProjection const& proj,
                                                        PlanOptions const& options);

} // namespace helios::render
