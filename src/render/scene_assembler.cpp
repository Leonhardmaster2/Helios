// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "helios/render/scene_assembler.hpp"

#include <cmath>
#include <limits>
#include <string>

namespace helios::render {
namespace {

bool allFinite(math::Vec3d const& v) {
    return std::isfinite(v.v[0]) && std::isfinite(v.v[1]) && std::isfinite(v.v[2]);
}

// Transform a frame-tagged point into the storage frame through the
// snapshot (root storage resolves to identity; body-fixed chains compose).
core::Result<math::Vec3d> toStorageFrame(universe::FrameSnapshot const& snapshot, math::FrameId storage_frame,
                                         math::FrameId origin_frame, math::Vec3d const& coords) {
    return snapshot.pointInFrame(origin_frame, storage_frame, coords);
}

} // namespace

SceneAssembler::SceneAssembler(double max_near_distance_m) : max_near_distance_m_(max_near_distance_m) {}

core::Result<SceneAssembler> SceneAssembler::create(double max_near_distance_m) {
    if (!std::isfinite(max_near_distance_m) || max_near_distance_m <= 0.0 ||
        max_near_distance_m > static_cast<double>(FLT_MAX)) {
        return core::makeError(core::ErrorCode::InvalidArgument,
                               "scene assembler bound must be finite and positive");
    }
    // The bound itself must survive the float32 cast it is checked against.
    if (static_cast<float>(max_near_distance_m) == std::numeric_limits<float>::infinity()) {
        return core::makeError(core::ErrorCode::InvalidArgument,
                               "scene assembler bound must be representable in binary32");
    }
    return SceneAssembler(max_near_distance_m);
}

core::Result<AssembledScene> SceneAssembler::assemble(universe::FrameSnapshot const& snapshot,
                                                      SceneAssemblerInput const& input) const {
    if (!allFinite(input.camera_position) || !std::isfinite(input.camera_orientation.w) ||
        !std::isfinite(input.camera_orientation.x) || !std::isfinite(input.camera_orientation.y) ||
        !std::isfinite(input.camera_orientation.z)) {
        return core::makeError(core::ErrorCode::InvalidArgument, "camera pose must be finite");
    }
    // The storage frame must be meaningful: root, or known to the snapshot.
    if (input.storage_frame != universe::rootFrameId() && !snapshot.contains(input.storage_frame)) {
        return core::makeError(core::ErrorCode::NotFound, "camera storage frame " +
                                                              std::to_string(input.storage_frame.value) +
                                                              " is not present in the snapshot");
    }

    AssembledScene scene;
    scene.near_records.reserve(input.near_barycentric.size() + input.near_body_fixed.size());
    scene.distant_records.reserve(input.distant_bodies.size() + input.distant_body_fixed.size());

    auto emitNear = [&](math::Vec3d const& origin_in_storage) -> core::Result<void> {
        // ONE controlled subtraction in double, in the camera's storage
        // frame, BEFORE any float conversion: offset = origin - camera.
        const math::Vec3d offset = origin_in_storage - input.camera_position;
        const double dist = length(offset);
        if (!std::isfinite(dist)) {
            return core::makeError(core::ErrorCode::InvalidArgument, "near offset distance must be finite");
        }
        if (dist > max_near_distance_m_) {
            return core::makeError(core::ErrorCode::OutOfRange,
                                   "near origin beyond float32 representation bound (" +
                                       std::to_string(dist) + " m > " + std::to_string(max_near_distance_m_) +
                                       " m); reclassify as distant");
        }
        NearRenderRecord rec;
        rec.offset_m = math::Vec3f{static_cast<float>(offset.v[0]), static_cast<float>(offset.v[1]),
                                   static_cast<float>(offset.v[2])};
        for (int i = 0; i < 3; ++i) {
            if (!std::isfinite(rec.offset_m.v[i])) {
                return core::makeError(core::ErrorCode::OutOfRange, "near offset overflowed binary32");
            }
        }
        scene.near_records.push_back(rec);
        return core::Result<void>{};
    };

    for (auto const& origin : input.near_barycentric) {
        if (!origin.allFinite()) {
            return core::makeError(core::ErrorCode::InvalidArgument,
                                   "near origin coordinates must be finite");
        }
        auto p = toStorageFrame(snapshot, input.storage_frame, origin.frame(), origin.coords());
        if (p.failed()) {
            return p.error();
        }
        if (auto r = emitNear(p.value()); r.failed()) {
            return r.error();
        }
    }
    for (auto const& origin : input.near_body_fixed) {
        if (!origin.allFinite()) {
            return core::makeError(core::ErrorCode::InvalidArgument,
                                   "near origin coordinates must be finite");
        }
        auto p = toStorageFrame(snapshot, input.storage_frame, origin.frame(), origin.coords());
        if (p.failed()) {
            return p.error();
        }
        if (auto r = emitNear(p.value()); r.failed()) {
            return r.error();
        }
    }

    auto emitDistant = [&](math::Vec3d const& centre_in_storage, double radius_m) -> core::Result<void> {
        if (!allFinite(centre_in_storage) || !std::isfinite(radius_m) || radius_m <= 0.0) {
            return core::makeError(core::ErrorCode::InvalidArgument,
                                   "distant body center/radius must be finite and positive");
        }
        const math::Vec3d to_centre = centre_in_storage - input.camera_position;
        const double dist = length(to_centre);
        if (!(dist > radius_m)) {
            return core::makeError(core::ErrorCode::OutOfRange,
                                   "distant body must be farther than its own radius");
        }
        DistantBodyRecord rec;
        rec.direction_unit = to_centre * (1.0 / dist); // double throughout
        rec.distance_m = dist;
        rec.angular_radius_rad = std::asin(radius_m / dist);
        if (!allFinite(rec.direction_unit) || !std::isfinite(rec.distance_m) ||
            !std::isfinite(rec.angular_radius_rad)) {
            return core::makeError(core::ErrorCode::InvalidArgument,
                                   "distant body record computation produced non-finite values");
        }
        scene.distant_records.push_back(rec);
        return core::Result<void>{};
    };

    for (auto const& body : input.distant_bodies) {
        if (!body.center.allFinite()) {
            return core::makeError(core::ErrorCode::InvalidArgument,
                                   "distant body center coordinates must be finite");
        }
        auto p = toStorageFrame(snapshot, input.storage_frame, body.center.frame(), body.center.coords());
        if (p.failed()) {
            return p.error();
        }
        if (auto r = emitDistant(p.value(), body.radius_m); r.failed()) {
            return r.error();
        }
    }
    for (auto const& body : input.distant_body_fixed) {
        if (!body.center.allFinite()) {
            return core::makeError(core::ErrorCode::InvalidArgument,
                                   "distant body center coordinates must be finite");
        }
        auto p = toStorageFrame(snapshot, input.storage_frame, body.center.frame(), body.center.coords());
        if (p.failed()) {
            return p.error();
        }
        if (auto r = emitDistant(p.value(), body.radius_m); r.failed()) {
            return r.error();
        }
    }
    return scene;
}

} // namespace helios::render
