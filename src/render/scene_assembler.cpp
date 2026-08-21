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
    if (!input.camera.position.allFinite() || !std::isfinite(input.camera.orientation.w) ||
        !std::isfinite(input.camera.orientation.x) || !std::isfinite(input.camera.orientation.y) ||
        !std::isfinite(input.camera.orientation.z)) {
        return core::makeError(core::ErrorCode::InvalidArgument, "camera pose must be finite");
    }

    AssembledScene scene;
    scene.near_records.reserve(input.near_origins.size());
    scene.distant_records.reserve(input.distant_bodies.size());

    for (auto const& origin : input.near_origins) {
        if (!origin.allFinite()) {
            return core::makeError(core::ErrorCode::InvalidArgument,
                                   "near origin coordinates must be finite");
        }
        // ONE controlled subtraction in double, in a common frame, BEFORE
        // any float conversion: offset = origin - camera ("from camera to
        // object"). The camera orientation does not matter for the offset
        // bound; offsets stay in storage-frame axes and the GPU view
        // transform (rotation only) orients them.
        auto offset = input.camera.position.differenceTo(origin);
        if (offset.failed()) {
            return offset.error();
        }
        const double dist = length(offset.value());
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
        rec.offset_m =
            math::Vec3f{static_cast<float>(offset.value().v[0]), static_cast<float>(offset.value().v[1]),
                        static_cast<float>(offset.value().v[2])};
        for (int i = 0; i < 3; ++i) {
            if (!std::isfinite(rec.offset_m.v[i])) {
                return core::makeError(core::ErrorCode::OutOfRange, "near offset overflowed binary32");
            }
        }
        scene.near_records.push_back(rec);
    }

    for (auto const& body : input.distant_bodies) {
        if (!body.center.allFinite() || !std::isfinite(body.radius_m) || body.radius_m <= 0.0) {
            return core::makeError(core::ErrorCode::InvalidArgument,
                                   "distant body center/radius must be finite and positive");
        }
        auto to_center = input.camera.position.differenceTo(body.center);
        if (to_center.failed()) {
            return to_center.error();
        }
        const double dist = length(to_center.value());
        if (!(dist > body.radius_m)) {
            return core::makeError(core::ErrorCode::OutOfRange,
                                   "distant body must be farther than its own radius");
        }
        DistantBodyRecord rec;
        rec.direction_unit = to_center.value() * (1.0 / dist); // double throughout
        rec.distance_m = dist;
        rec.angular_radius_rad = std::asin(body.radius_m / dist);
        if (!allFinite(rec.direction_unit) || !std::isfinite(rec.distance_m) ||
            !std::isfinite(rec.angular_radius_rad)) {
            return core::makeError(core::ErrorCode::InvalidArgument,
                                   "distant body record computation produced non-finite values");
        }
        scene.distant_records.push_back(rec);
    }
    return scene;
}

} // namespace helios::render
