// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "helios/universe/frame_provider.hpp"

#include <cmath>

namespace helios::universe {

void StaticFrameProvider::addFrame(math::FrameId id, FrameTransform transform) {
    base_.add(id, transform);
}

core::Result<FrameSnapshot> StaticFrameProvider::snapshot(Instant const& /*t*/) const {
    // Copy: snapshots are immutable value snapshots owned by the caller.
    return base_;
}

void RotatingFrameProvider::addBody(RotatingBodyParams params) {
    bodies_.push_back(params);
}

core::Result<FrameSnapshot> RotatingFrameProvider::snapshot(Instant const& t) const {
    const double seconds = static_cast<double>(t.totalSeconds());
    FrameSnapshot out;
    for (auto const& b : bodies_) {
        const double orbit_angle = b.orbit_rate_rad_per_s * seconds;
        const double spin_angle = b.spin_rate_rad_per_s * seconds;
        FrameTransform xf;
        xf.rotation = math::Quatd::fromAxisAngle(math::Vec3d{0.0, 0.0, 1.0}, spin_angle);
        xf.translation = math::Vec3d{b.orbit_radius_m * std::cos(orbit_angle) + b.translation_at_epoch.v[0],
                                     b.orbit_radius_m * std::sin(orbit_angle) + b.translation_at_epoch.v[1],
                                     b.translation_at_epoch.v[2]};
        out.add(b.id, xf);
    }
    return out;
}

} // namespace helios::universe
