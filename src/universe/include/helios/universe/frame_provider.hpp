// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Synthetic frame providers behind the final abstract FrameSnapshot shape
// (docs/ARCHITECTURE.md: "M1 uses synthetic static/rotating frame providers
// ... M7 selects real time scales, astronomical conventions, analytic models,
// and rotation providers without replacing the M1 consumer API").
#pragma once

#include "helios/core/error.hpp"
#include "helios/math/frames.hpp"
#include "helios/universe/frame_snapshot.hpp"
#include "helios/universe/instant.hpp"

#include <vector>

namespace helios::universe {

// Source of immutable per-instant frame snapshots.
class FrameProvider {
  public:
    virtual ~FrameProvider() = default;
    FrameProvider() = default;
    FrameProvider(FrameProvider const&) = delete;
    FrameProvider& operator=(FrameProvider const&) = delete;

    // Returns a validated, immutable snapshot for `t`. Implementations must
    // reject invalid instants (non-finite values, unsupported time scales)
    // with Result errors rather than computing garbage.
    [[nodiscard]] virtual core::Result<FrameSnapshot> snapshot(Instant const& t) const = 0;
};

// Frames with fixed transforms, identical at every instant.
class StaticFrameProvider final : public FrameProvider {
  public:
    // Same validation as FrameSnapshotBuilder::add (parent defaults to root).
    [[nodiscard]] core::Result<void> addFrame(math::FrameId id, FrameTransform transform);
    [[nodiscard]] core::Result<FrameSnapshot> snapshot(Instant const& t) const override;

  private:
    FrameSnapshotBuilder builder_;
};

// One synthetic rotating body: its body-fixed frame origin orbits the
// barycentric root in the XY plane at a constant angular rate while the
// frame spins about its own Z axis. Deliberately simple; exercises the same
// consumer API that M7's real rotation models will use.
struct RotatingBodyParams {
    math::FrameId id{};
    double orbit_radius_m = 0.0; // metres from the barycentric root
    double orbit_rate_rad_per_s = 0.0;
    double spin_rate_rad_per_s = 0.0;
    math::Vec3d translation_at_epoch{}; // static offset added to the orbit position

    [[nodiscard]] bool isValid() const noexcept;
};

class RotatingFrameProvider final : public FrameProvider {
  public:
    [[nodiscard]] core::Result<void> addBody(RotatingBodyParams params);
    [[nodiscard]] core::Result<FrameSnapshot> snapshot(Instant const& t) const override;

    // Precision-safe phase: rate * t reduced through the period FIRST so the
    // large day-count term never collapses sub-day increments.
    //
    //   period = 2*pi / rate                       (one rounding, constant)
    //   red    = fmod(days*86400, period)          (exact: IEEE fmod)
    //   red    = fmod(red + seconds_of_day, period)(exact + one rounding ~ulp(period))
    //   phase  = rate * red                        (one rounding, |phase| < 2*pi)
    //
    // whole_days * 86400 is exact in binary64 while |whole_days| <
    // 2^53 / 86400 (~1.04e10 days), far beyond any v1 date range. The
    // result preserves increments of rate*delta down to well below a
    // microsecond at arbitrary day counts, which a naive
    // `(double)totalSeconds() * rate` does NOT (binary64 spacing at
    // 8.64e11 s is already ~1.2e-4 s).
    [[nodiscard]] static double phaseFor(double rate_rad_per_s, Instant const& t);

  private:
    std::vector<RotatingBodyParams> bodies_;
};

} // namespace helios::universe
