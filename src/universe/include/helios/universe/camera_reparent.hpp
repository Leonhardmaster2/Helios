// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Camera frame reparenting (docs/ARCHITECTURE.md "Camera frame changes"):
// a camera is OWNED either by the barycentric root or by one body-fixed
// frame. Ownership changes use separate enter/exit radii (hysteresis band),
// so a camera hovering near the boundary cannot oscillate.
//
// Correctness contract:
//  - pose and velocity are always DERIVED FROM THE CURRENT SNAPSHOT through
//    an exact transform — never integrated incrementally — so thousands of
//    repeated transitions accumulate no drift beyond transform round-off;
//  - orientation transforms through the frame rotation (quaternions);
//  - velocity state (linear + angular), once introduced, transforms too:
//    entering a rotating frame subtracts the frame motion (v_origin + w x r);
//    exiting re-adds it;
//  - unknown frames / invalid inputs are Result errors (no exceptions).
#pragma once

#include "helios/core/error.hpp"
#include "helios/math/frames.hpp"
#include "helios/universe/frame_snapshot.hpp"

namespace helios::universe {

enum class OwningFrame : std::uint8_t {
    Barycentric, // storage in Solar-System barycentric coordinates
    BodyFixed,   // storage in the body's rotating frame
};

struct ReparentThresholds {
    double enter_radius_m = 0.0; // adopt body-fixed ownership when distance <= enter
    double exit_radius_m = 0.0;  // return to barycentric ownership when distance > exit

    [[nodiscard]] bool isValid() const noexcept;
};

// Motion of the body-fixed frame relative to the barycentric root at the
// current instant (root-frame axes). For M1 synthetic frames these are
// computed analytically by the caller from the provider parameters; M7 will
// publish them alongside real rotation models.
struct FrameKinematics {
    math::Vec3d origin_velocity_m_per_s{};
    math::Vec3d angular_velocity_rad_per_s{};
};

// Full camera state across ownership changes. Exactly one of the two poses
// is authoritative (matching `owner`); the other is a cache that update()
// keeps refreshed so consumers can read either without a transition.
struct ReparentableCamera {
    OwningFrame owner = OwningFrame::Barycentric;
    math::FrameId body_frame{};
    math::BarycentricCameraPose barycentric{}; // position tagged barycentric-root id (0)
    math::BodyFixedCameraPose body_fixed{};    // position tagged body_frame
    math::Vec3d linear_velocity{};             // owning-frame axes, m/s
    math::Vec3d angular_velocity{};            // owning-frame axes, rad/s
};

class CameraReparenter {
  public:
    CameraReparenter(math::FrameId body_frame, ReparentThresholds thresholds);

    [[nodiscard]] bool valid() const noexcept {
        return thresholds_.isValid();
    }

    // Distance (metres) between the camera and the body-frame origin,
    // measured in root coordinates from the current authoritative pose.
    [[nodiscard]] core::Result<double> distanceToBody(FrameSnapshot const& snapshot,
                                                      ReparentableCamera const& camera) const;

    // Applies the hysteresis rule for this instant and returns the resulting
    // ownership. Never switches twice in one call; never switches when the
    // distance stays inside the hysteresis band.
    [[nodiscard]] core::Result<OwningFrame> update(FrameSnapshot const& snapshot,
                                                   FrameKinematics const& kinematics,
                                                   ReparentableCamera& camera) const;

  private:
    [[nodiscard]] core::Result<void> enterBodyFrame(FrameSnapshot const& snapshot,
                                                    FrameKinematics const& kinematics,
                                                    ReparentableCamera& camera) const;
    [[nodiscard]] core::Result<void> exitToBarycentric(FrameSnapshot const& snapshot,
                                                       FrameKinematics const& kinematics,
                                                       ReparentableCamera& camera) const;

    math::FrameId body_frame_;
    ReparentThresholds thresholds_;
};

} // namespace helios::universe
