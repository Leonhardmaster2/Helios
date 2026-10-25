// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "helios/render/gpu_precision_plan.hpp"

#include <cmath>

namespace helios::render {
namespace {
constexpr double kMinDistance = 1e-30;
}

CameraView cameraViewFromOrientation(math::Quatd const& orientation) {
    // Camera-local: right=+x, up=+y, forward=-z (Vulkan view convention).
    return CameraView{orientation.rotate(math::Vec3d{1.0, 0.0, 0.0}),
                      orientation.rotate(math::Vec3d{0.0, 1.0, 0.0}),
                      orientation.rotate(math::Vec3d{0.0, 0.0, -1.0})};
}

std::array<float, 16> viewRotationMatrix(CameraView const& view) {
    // View rotation maps storage -> camera axes; rows are the basis vectors
    // (right, up, and camera +z = -forward). Column-major storage for Vulkan.
    const double m[16] = {view.right.v[0], view.up.v[0], -view.forward.v[0], 0.0,
                          view.right.v[1], view.up.v[1], -view.forward.v[1], 0.0,
                          view.right.v[2], view.up.v[2], -view.forward.v[2], 0.0,
                          0.0,             0.0,          0.0,                1.0};
    std::array<float, 16> out{};
    for (int i = 0; i < 16; ++i) {
        out[i] = static_cast<float>(m[i]);
    }
    return out;
}

std::array<float, 16> multiplyMat4(std::array<float, 16> const& a, std::array<float, 16> const& b) {
    std::array<float, 16> out{};
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            double s = 0.0;
            for (int k = 0; k < 4; ++k) {
                s += static_cast<double>(a[k * 4 + r]) * static_cast<double>(b[c * 4 + k]);
            }
            out[c * 4 + r] = static_cast<float>(s);
        }
    }
    return out;
}

std::pair<double, double> projectToPixels(PixelProjection const& proj,
                                          math::Vec3d const& storage_point) {
    const math::Vec3d d = storage_point - proj.camera_position;
    const double x_c = dot(d, proj.view.right);
    const double y_c = dot(d, proj.view.up);
    const double z_fwd = dot(d, proj.view.forward); // >0 means in front
    if (z_fwd <= 0.0) {
        return {-1.0, -1.0};
    }
    const double f = 1.0 / proj.tan_half_fov_y;
    const double aspect = proj.width_px / proj.height_px;
    const double ndc_x = (x_c / z_fwd) * f / aspect;
    const double ndc_y = (y_c / z_fwd) * f;
    return {proj.width_px * (0.5 + 0.5 * ndc_x), proj.height_px * (0.5 - 0.5 * ndc_y)};
}

core::Result<GpuFramePlan> buildFramePlan(AssembledScene const& scene,
                                          math::Quatd const& camera_orientation,
                                          PixelProjection const& proj, PlanOptions const& options) {
    GpuFramePlan plan;

    // PV = projection * rotation-only view (column-major), computed here so
    // the Vulkan layer consumes the plan verbatim.
    const CameraView view = cameraViewFromOrientation(camera_orientation);
    std::array<float, 16> p{};
    {
        const float f = 1.0f / static_cast<float>(proj.tan_half_fov_y);
        const float aspect =
            static_cast<float>(proj.width_px / proj.height_px);
        p[0] = f / aspect;
        p[5] = f;
        p[10] = 0.0f;
        p[11] = -1.0f;
        p[14] = options.near_plane_m;
        p[15] = 0.0f;
    }
    plan.pv = multiplyMat4(p, viewRotationMatrix(view));

    const std::size_t mesh_count = scene.near_records.size();
    if (!options.mesh_colors.empty() && options.mesh_colors.size() != mesh_count * 4) {
        return core::makeError(core::ErrorCode::InvalidArgument, "mesh_colors size mismatch");
    }
    if (!options.mesh_radii.empty() && options.mesh_radii.size() != mesh_count) {
        return core::makeError(core::ErrorCode::InvalidArgument, "mesh_radii size mismatch");
    }
    plan.meshes.reserve(mesh_count);
    for (std::size_t i = 0; i < mesh_count; ++i) {
        MeshDraw draw;
        draw.offset_m = scene.near_records[i].offset_m;
        draw.radius_m = options.mesh_radii.empty() ? 1.0f : options.mesh_radii[i];
        if (!options.mesh_colors.empty()) {
            for (int c = 0; c < 4; ++c) {
                draw.color[c] = options.mesh_colors[i * 4 + c];
            }
        }
        plan.meshes.push_back(draw);
    }

    // Distant directions are rotated into VIEW axes by the inverse camera
    // orientation (deliberate, documented transform; see header).
    const math::Quatd inv = camera_orientation.conjugate();    const std::size_t impostor_count = scene.distant_records.size();
    if (!options.impostor_colors.empty() && options.impostor_colors.size() != impostor_count * 4) {
        return core::makeError(core::ErrorCode::InvalidArgument, "impostor_colors size mismatch");
    }
    plan.impostors.reserve(impostor_count);
    for (std::size_t i = 0; i < impostor_count; ++i) {
        auto const& rec = scene.distant_records[i];
        ImpostorDraw draw;
        const math::Vec3d dir_local = inv.rotate(rec.direction_unit);
        draw.direction_view = math::Vec3f{static_cast<float>(dir_local.v[0]),
                                          static_cast<float>(dir_local.v[1]),
                                          static_cast<float>(dir_local.v[2])};
        // Analytic reversed-Z depth of the record distance: ndc = near/d.
        const double depth = options.near_plane_m / std::max(rec.distance_m, kMinDistance);
        if (!std::isfinite(depth)) {
            return core::makeError(core::ErrorCode::InvalidArgument,
                                   "impostor depth computation produced a non-finite value");
        }
        draw.ndc_depth = static_cast<float>(depth);
        draw.angular_radius_rad = static_cast<float>(rec.angular_radius_rad);
        if (!options.impostor_colors.empty()) {
            for (int c = 0; c < 4; ++c) {
                draw.color[c] = options.impostor_colors[i * 4 + c];
            }
        }
        plan.impostors.push_back(draw);
    }
    return plan;
}

} // namespace helios::render
