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

    [[nodiscard]] virtual core::Result<FrameSnapshot> snapshot(Instant const& t) const = 0;
};

// Frames with fixed transforms, identical at every instant.
class StaticFrameProvider final : public FrameProvider {
  public:
    void addFrame(math::FrameId id, FrameTransform transform);
    [[nodiscard]] core::Result<FrameSnapshot> snapshot(Instant const& t) const override;

  private:
    FrameSnapshot base_;
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
};

class RotatingFrameProvider final : public FrameProvider {
  public:
    void addBody(RotatingBodyParams params);
    [[nodiscard]] core::Result<FrameSnapshot> snapshot(Instant const& t) const override;

  private:
    std::vector<RotatingBodyParams> bodies_;
};

} // namespace helios::universe
