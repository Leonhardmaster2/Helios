// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// FrameSnapshot: immutable per-instant transforms of named frames relative
// to the Solar-System barycentric root (docs/ARCHITECTURE.md). Published by
// a FrameProvider for one Instant and never mutated afterwards; scene
// assembly reads it without synchronization concerns.
#pragma once

#include "helios/core/error.hpp"
#include "helios/math/frames.hpp"
#include "helios/math/quat.hpp"
#include "helios/math/vec.hpp"

#include <vector>

namespace helios::universe {

// Transform from a child frame to its parent:
//   p_parent = rotation * p_child + translation
struct FrameTransform {
    math::Quatd rotation = math::Quatd::identity();
    math::Vec3d translation{};
};

[[nodiscard]] FrameTransform compose(FrameTransform const& outer, FrameTransform const& inner);
[[nodiscard]] FrameTransform invert(FrameTransform const& t);

// Immutable set of frame transforms for one Instant. M1: all frames are
// direct children of the barycentric root (synthetic providers); M7 adds
// real hierarchies, so lookups are by FrameId against a flat table now.
class FrameSnapshot {
  public:
    void add(math::FrameId id, FrameTransform transform);

    [[nodiscard]] core::Result<FrameTransform> transformOf(math::FrameId id) const;

    // Transform a point from one frame to another through the barycentric
    // root. Cross-frame use is checked; unknown frames are NotFound.
    [[nodiscard]] core::Result<math::Vec3d> pointInFrame(math::FrameId from, math::FrameId to,
                                                         math::Vec3d const& p) const;

    [[nodiscard]] std::size_t frameCount() const noexcept {
        return entries_.size();
    }

  private:
    struct Entry {
        math::FrameId id{};
        FrameTransform transform{};
    };
    std::vector<Entry> entries_;
};

} // namespace helios::universe
