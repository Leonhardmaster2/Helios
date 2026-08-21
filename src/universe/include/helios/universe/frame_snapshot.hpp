// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// FrameSnapshot: immutable per-instant transforms of named frames
// (docs/ARCHITECTURE.md). Consumers receive a finalized snapshot and cannot
// mutate it; providers construct snapshots through FrameSnapshotBuilder,
// which validates finiteness, unit rotations, parent existence, and acyclicity.
//
// Hierarchy: every frame stores its transform RELATIVE TO ITS PARENT frame.
// M1 synthetic providers use the barycentric root (FrameId{0}) as the parent
// of all frames; M7 adds real hierarchies without changing this shape.
#pragma once

#include "helios/core/error.hpp"
#include "helios/math/frames.hpp"
#include "helios/math/quat.hpp"
#include "helios/math/vec.hpp"

#include <cstdint>
#include <vector>

namespace helios::universe {

// The barycentric root frame id. Reserved: builders reject adding a frame
// with this id.
inline constexpr std::uint32_t kRootFrameIdValue = 0;
[[nodiscard]] constexpr math::FrameId rootFrameId() noexcept {
    return math::FrameId{kRootFrameIdValue};
}

// Transform from a child frame to its parent frame:
//   p_parent = rotation * p_child + translation
struct FrameTransform {
    math::Quatd rotation = math::Quatd::identity();
    math::Vec3d translation{};
};

[[nodiscard]] FrameTransform compose(FrameTransform const& outer, FrameTransform const& inner);
[[nodiscard]] FrameTransform invert(FrameTransform const& t);

// Immutable set of frame transforms for one Instant.
class FrameSnapshot {
  public:
    [[nodiscard]] bool contains(math::FrameId id) const noexcept;
    [[nodiscard]] std::size_t frameCount() const noexcept {
        return entries_.size();
    }

    // Full transform from `id` coordinates to ROOT coordinates, composed
    // along the parent chain. NotFound for unknown frames.
    [[nodiscard]] core::Result<FrameTransform> transformToRoot(math::FrameId id) const;

    // Full transform from `from` coordinates to `to` coordinates.
    [[nodiscard]] core::Result<FrameTransform> transformBetween(math::FrameId from, math::FrameId to) const;

    // Transform a point between any two frames in the snapshot. Unknown
    // frames are NotFound; non-finite input points are InvalidArgument.
    [[nodiscard]] core::Result<math::Vec3d> pointInFrame(math::FrameId from, math::FrameId to,
                                                         math::Vec3d const& p) const;

  private:
    friend class FrameSnapshotBuilder;
    struct Entry {
        math::FrameId id{};
        math::FrameId parent{};
        FrameTransform local_to_parent{};
    };
    explicit FrameSnapshot(std::vector<Entry> entries) : entries_(std::move(entries)) {}

    std::vector<Entry> entries_; // sorted by id.value for lookup
};

// Validated construction path; the only way to create a FrameSnapshot.
class FrameSnapshotBuilder {
  public:
    // Registers `id` with a transform relative to `parent`. Rejects:
    // the reserved root id, duplicate ids, unknown parents (parents must be
    // added before children), non-finite translations, non-unit rotations
    // (within 1e-9), and non-finite quaternion components.
    [[nodiscard]] core::Result<void> add(math::FrameId id, math::FrameId parent,
                                         FrameTransform local_to_parent);

    // Finalizes: verifies every parent chain terminates at the root (no
    // cycles are possible given add-ordering, but this also bounds chain
    // depth defensively) and returns the immutable snapshot.
    [[nodiscard]] core::Result<FrameSnapshot> build() const;

  private:
    FrameSnapshot::Entry entryFor(math::FrameId id) const;
    std::vector<FrameSnapshot::Entry> entries_;
};

} // namespace helios::universe
