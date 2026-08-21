// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "helios/universe/camera_reparent.hpp"

#include <cmath>
#include <string>

namespace helios::universe {
namespace {

bool allFinite(math::Vec3d const& v) {
    return std::isfinite(v.v[0]) && std::isfinite(v.v[1]) && std::isfinite(v.v[2]);
}

core::Result<FrameTransform> bodyToRootTransform(FrameSnapshot const& snapshot, math::FrameId body_frame) {
    return snapshot.transformToRoot(body_frame);
}

// Pose-only derivation barycentric -> body-fixed (no ownership change).
core::Result<void> deriveBodyFixedPose(FrameSnapshot const& snapshot, math::BarycentricCameraPose const& from,
                                       math::FrameId body_frame, math::BodyFixedCameraPose& out) {
    auto xf = bodyToRootTransform(snapshot, body_frame);
    if (xf.failed()) {
        return xf.error();
    }
    const math::Quatd q_inv = xf.value().rotation.conjugate();
    const math::Vec3d rel = from.position.coords() - xf.value().translation;
    out.position = math::BodyFixedPosition{body_frame, q_inv.rotate(rel)};
    out.orientation = (q_inv * from.orientation).normalized();
    return core::Result<void>{};
}

// Pose-only derivation body-fixed -> barycentric (no ownership change).
core::Result<void> deriveBarycentricPose(FrameSnapshot const& snapshot, math::BodyFixedCameraPose const& from,
                                         math::FrameId body_frame, math::BarycentricCameraPose& out) {
    auto xf = bodyToRootTransform(snapshot, body_frame);
    if (xf.failed()) {
        return xf.error();
    }
    out.position = math::BarycentricPosition{math::FrameId{kRootFrameIdValue},
                                             xf.value().rotation.rotate(from.position.coords()) +
                                                 xf.value().translation};
    out.orientation = (xf.value().rotation * from.orientation).normalized();
    return core::Result<void>{};
}

} // namespace

bool ReparentThresholds::isValid() const noexcept {
    // Hysteresis requires a strictly positive band: enter strictly below
    // exit. Non-finite or degenerate thresholds are invalid.
    return std::isfinite(enter_radius_m) && std::isfinite(exit_radius_m) && enter_radius_m > 0.0 &&
           exit_radius_m > enter_radius_m;
}

CameraReparenter::CameraReparenter(math::FrameId body_frame, ReparentThresholds thresholds)
    : body_frame_(body_frame), thresholds_(thresholds) {}

core::Result<double> CameraReparenter::distanceToBody(FrameSnapshot const& snapshot,
                                                      ReparentableCamera const& camera) const {
    auto body_to_root = bodyToRootTransform(snapshot, body_frame_);
    if (body_to_root.failed()) {
        return body_to_root.error();
    }
    math::Vec3d p_root{};
    if (camera.owner == OwningFrame::Barycentric) {
        if (!camera.barycentric.position.allFinite()) {
            return core::makeError(core::ErrorCode::InvalidArgument,
                                   "camera barycentric position must be finite");
        }
        p_root = camera.barycentric.position.coords();
    } else {
        if (!camera.body_fixed.position.allFinite()) {
            return core::makeError(core::ErrorCode::InvalidArgument,
                                   "camera body-fixed position must be finite");
        }
        p_root = body_to_root.value().rotation.rotate(camera.body_fixed.position.coords()) +
                 body_to_root.value().translation;
    }
    return length(p_root - body_to_root.value().translation);
}

core::Result<OwningFrame> CameraReparenter::update(FrameSnapshot const& snapshot,
                                                   FrameKinematics const& kinematics,
                                                   ReparentableCamera& camera) const {
    if (!valid()) {
        return core::makeError(core::ErrorCode::InvalidArgument, "reparenter thresholds invalid");
    }
    if (!(camera.body_frame == body_frame_)) {
        return core::makeError(core::ErrorCode::InvalidArgument,
                               "camera body frame " + std::to_string(camera.body_frame.value) +
                                   " does not match reparenter body frame " +
                                   std::to_string(body_frame_.value));
    }
    if (!allFinite(kinematics.origin_velocity_m_per_s) || !allFinite(kinematics.angular_velocity_rad_per_s)) {
        return core::makeError(core::ErrorCode::InvalidArgument, "frame kinematics must be finite");
    }

    auto body_to_root = bodyToRootTransform(snapshot, body_frame_);
    if (body_to_root.failed()) {
        return body_to_root.error();
    }
    auto distance = distanceToBody(snapshot, camera);
    if (distance.failed()) {
        return distance.error();
    }

    bool transitioned = false;
    // Hysteresis: enter only at/below the inner radius, exit only beyond the
    // outer radius. Inside the band the previous ownership is kept, so a
    // camera hovering at the boundary cannot oscillate. Exact boundary
    // values are deterministic: d == enter enters, d == exit stays.
    if (camera.owner == OwningFrame::Barycentric && distance.value() <= thresholds_.enter_radius_m) {
        // Barycentric -> BodyFixed. The pose and velocity are DERIVED from
        // the authoritative barycentric state through the current snapshot
        // transform (never integrated), so repeated transitions do not
        // accumulate drift.
        if (!camera.barycentric.position.allFinite() || !allFinite(camera.linear_velocity) ||
            !allFinite(camera.angular_velocity)) {
            return core::makeError(core::ErrorCode::InvalidArgument,
                                   "camera state must be finite before reparenting");
        }
        const math::Quatd q_inv = body_to_root.value().rotation.conjugate();
        const math::Vec3d p_root = camera.barycentric.position.coords();
        const math::Vec3d rel = p_root - body_to_root.value().translation;
        const math::Vec3d v_rel_root = camera.linear_velocity - kinematics.origin_velocity_m_per_s -
                                       cross(kinematics.angular_velocity_rad_per_s, rel);
        camera.body_fixed.position = math::BodyFixedPosition{body_frame_, q_inv.rotate(rel)};
        camera.body_fixed.orientation = (q_inv * camera.barycentric.orientation).normalized();
        camera.linear_velocity = q_inv.rotate(v_rel_root);
        camera.angular_velocity = q_inv.rotate(camera.angular_velocity);
        camera.owner = OwningFrame::BodyFixed;
        transitioned = true;
    } else if (camera.owner == OwningFrame::BodyFixed && distance.value() > thresholds_.exit_radius_m) {
        // BodyFixed -> Barycentric: re-add the frame motion.
        if (!camera.body_fixed.position.allFinite() || !allFinite(camera.linear_velocity) ||
            !allFinite(camera.angular_velocity)) {
            return core::makeError(core::ErrorCode::InvalidArgument,
                                   "camera state must be finite before reparenting");
        }
        const math::Vec3d p_body = camera.body_fixed.position.coords();
        const math::Vec3d rel = body_to_root.value().rotation.rotate(p_body);
        const math::Vec3d v_frame_root =
            kinematics.origin_velocity_m_per_s + cross(kinematics.angular_velocity_rad_per_s, rel);
        camera.barycentric.position = math::BarycentricPosition{math::FrameId{kRootFrameIdValue},
                                                                rel + body_to_root.value().translation};
        camera.barycentric.orientation =
            (body_to_root.value().rotation * camera.body_fixed.orientation).normalized();
        camera.linear_velocity = body_to_root.value().rotation.rotate(camera.linear_velocity) + v_frame_root;
        camera.angular_velocity = body_to_root.value().rotation.rotate(camera.angular_velocity);
        camera.owner = OwningFrame::Barycentric;
        transitioned = true;
    }

    // Refresh the non-authoritative POSE cache so both representations are
    // always readable and consistent with the current snapshot. Right after
    // a transition both poses are already consistent (one is the abandoned
    // authoritative pose), so the extra transform is skipped. Velocity state
    // is expressed in the owning frame's axes only (single copy, no cache).
    if (!transitioned) {
        if (camera.owner == OwningFrame::Barycentric) {
            if (auto r = deriveBodyFixedPose(snapshot, camera.barycentric, body_frame_, camera.body_fixed);
                r.failed()) {
                return r.error();
            }
        } else {
            if (auto r = deriveBarycentricPose(snapshot, camera.body_fixed, body_frame_, camera.barycentric);
                r.failed()) {
                return r.error();
            }
        }
    }
    return camera.owner;
}

} // namespace helios::universe
