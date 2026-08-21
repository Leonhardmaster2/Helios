// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "helios/universe/frame_provider.hpp"

#include <cmath>
#include <limits>

namespace helios::universe {
namespace {
constexpr double kTwoPi = 6.283185307179586476925286766559;
} // namespace

bool RotatingBodyParams::isValid() const noexcept {
    return std::isfinite(orbit_radius_m) && std::isfinite(orbit_rate_rad_per_s) &&
           std::isfinite(spin_rate_rad_per_s) && orbit_radius_m >= 0.0 &&
           std::isfinite(translation_at_epoch.v[0]) && std::isfinite(translation_at_epoch.v[1]) &&
           std::isfinite(translation_at_epoch.v[2]);
}

core::Result<void> StaticFrameProvider::addFrame(math::FrameId id, FrameTransform transform) {
    return builder_.add(id, rootFrameId(), transform);
}

core::Result<FrameSnapshot> StaticFrameProvider::snapshot(Instant const& t) const {
    if (!t.isValid()) {
        return core::makeError(core::ErrorCode::InvalidArgument,
                               "static frame provider received an invalid instant");
    }
    if (t.scale != TimeScale::Synthetic) {
        return core::makeError(core::ErrorCode::Unsupported,
                               "synthetic providers only support TimeScale::Synthetic");
    }
    // Static transforms are instant-independent; build a fresh immutable
    // snapshot per call so callers can never alias provider internals.
    return builder_.build();
}

double RotatingFrameProvider::phaseFor(double rate_rad_per_s, Instant const& t) {
    if (rate_rad_per_s == 0.0 || !t.isValid() || !std::isfinite(rate_rad_per_s)) {
        return 0.0;
    }
    const double period = kTwoPi / rate_rad_per_s;
    double reduced = std::fmod(static_cast<double>(t.whole_days) * kSecondsPerDay, period);
    reduced = std::fmod(reduced + t.seconds_of_day, period);
    return rate_rad_per_s * reduced; // in (-2*pi, 2*pi)
}

core::Result<void> RotatingFrameProvider::addBody(RotatingBodyParams params) {
    if (!params.isValid()) {
        return core::makeError(core::ErrorCode::InvalidArgument,
                               "rotating body params must be finite and non-negative");
    }
    if (params.id == rootFrameId()) {
        return core::makeError(core::ErrorCode::InvalidArgument,
                               "the barycentric root cannot be a rotating body");
    }
    bodies_.push_back(params);
    return core::Result<void>{};
}

core::Result<FrameSnapshot> RotatingFrameProvider::snapshot(Instant const& t) const {
    if (!t.isValid()) {
        return core::makeError(core::ErrorCode::InvalidArgument,
                               "rotating frame provider received an invalid instant");
    }
    if (t.scale != TimeScale::Synthetic) {
        return core::makeError(core::ErrorCode::Unsupported,
                               "synthetic providers only support TimeScale::Synthetic");
    }
    FrameSnapshotBuilder builder;
    for (auto const& b : bodies_) {
        const double orbit_phase = phaseFor(b.orbit_rate_rad_per_s, t);
        const double spin_phase = phaseFor(b.spin_rate_rad_per_s, t);
        FrameTransform xf;
        xf.rotation = math::Quatd::fromAxisAngle(math::Vec3d{0.0, 0.0, 1.0}, spin_phase);
        xf.translation = math::Vec3d{b.orbit_radius_m * std::cos(orbit_phase) + b.translation_at_epoch.v[0],
                                     b.orbit_radius_m * std::sin(orbit_phase) + b.translation_at_epoch.v[1],
                                     b.translation_at_epoch.v[2]};
        if (auto r = builder.add(b.id, rootFrameId(), xf); r.failed()) {
            return r.error();
        }
    }
    return builder.build();
}

} // namespace helios::universe
