// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "helios/universe/frame_snapshot.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace helios::universe {
namespace {

bool allFinite(math::Vec3d const& v) {
    return std::isfinite(v.v[0]) && std::isfinite(v.v[1]) && std::isfinite(v.v[2]);
}

bool isUnit(math::Quatd const& q) {
    return std::isfinite(q.w) && std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) &&
           std::abs(q.lengthSq() - 1.0) <= 1e-9;
}

} // namespace

FrameTransform compose(FrameTransform const& outer, FrameTransform const& inner) {
    // outer(inner(p)) = R_o * (R_i * p + t_i) + t_o
    return FrameTransform{outer.rotation * inner.rotation,
                          outer.rotation.rotate(inner.translation) + outer.translation};
}

FrameTransform invert(FrameTransform const& t) {
    // p_local = R^-1 * (p_parent - translation)
    const math::Quatd inv = t.rotation.conjugate();
    return FrameTransform{inv, inv.rotate(t.translation * -1.0)};
}

bool FrameSnapshot::contains(math::FrameId id) const noexcept {
    auto it = std::lower_bound(entries_.begin(), entries_.end(), id,
                               [](Entry const& e, math::FrameId v) { return e.id.value < v.value; });
    return it != entries_.end() && it->id == id;
}

core::Result<FrameTransform> FrameSnapshot::transformToRoot(math::FrameId id) const {
    if (id == rootFrameId()) {
        return FrameTransform{}; // the root's transform to itself is identity
    }
    auto it = std::lower_bound(entries_.begin(), entries_.end(), id,
                               [](Entry const& e, math::FrameId v) { return e.id.value < v.value; });
    if (it == entries_.end() || !(it->id == id)) {
        return core::makeError(core::ErrorCode::NotFound,
                               "frame " + std::to_string(id.value) + " not in snapshot");
    }
    // Compose along the parent chain. Builders guarantee acyclicity and
    // root-termination; the step limit is defensive only.
    FrameTransform to_root = it->local_to_parent;
    math::FrameId parent = it->parent;
    for (std::size_t step = 0; step <= entries_.size(); ++step) {
        if (parent == rootFrameId()) {
            return to_root;
        }
        auto pit = std::lower_bound(entries_.begin(), entries_.end(), parent,
                                    [](Entry const& e, math::FrameId v) { return e.id.value < v.value; });
        if (pit == entries_.end() || !(pit->id == parent)) {
            return core::makeError(core::ErrorCode::Internal,
                                   "snapshot parent chain broken at frame " + std::to_string(parent.value));
        }
        to_root = compose(pit->local_to_parent, to_root);
        parent = pit->parent;
    }
    return core::makeError(core::ErrorCode::Internal, "snapshot parent chain does not terminate (cycle?)");
}

core::Result<FrameTransform> FrameSnapshot::transformBetween(math::FrameId from, math::FrameId to) const {
    auto f = transformToRoot(from);
    if (f.failed()) {
        return f.error();
    }
    if (to == rootFrameId()) {
        return f;
    }
    auto t = transformToRoot(to);
    if (t.failed()) {
        return t.error();
    }
    return compose(invert(t.value()), f.value());
}

core::Result<math::Vec3d> FrameSnapshot::pointInFrame(math::FrameId from, math::FrameId to,
                                                      math::Vec3d const& p) const {
    if (!allFinite(p)) {
        return core::makeError(core::ErrorCode::InvalidArgument, "pointInFrame input point must be finite");
    }
    auto xf = transformBetween(from, to);
    if (xf.failed()) {
        return xf.error();
    }
    return xf.value().rotation.rotate(p) + xf.value().translation;
}

core::Result<void> FrameSnapshotBuilder::add(math::FrameId id, math::FrameId parent,
                                             FrameTransform local_to_parent) {
    if (id == rootFrameId()) {
        return core::makeError(core::ErrorCode::InvalidArgument,
                               "the barycentric root frame cannot be added as a child");
    }
    if (!allFinite(local_to_parent.translation)) {
        return core::makeError(core::ErrorCode::InvalidArgument,
                               "frame transform translation must be finite");
    }
    if (!isUnit(local_to_parent.rotation)) {
        return core::makeError(core::ErrorCode::InvalidArgument,
                               "frame transform rotation must be a unit quaternion");
    }
    if (id == parent) {
        return core::makeError(core::ErrorCode::InvalidArgument, "a frame cannot be its own parent");
    }
    if (parent != rootFrameId() &&
        !std::any_of(entries_.begin(), entries_.end(),
                     [&](FrameSnapshot::Entry const& e) { return e.id == parent; })) {
        return core::makeError(core::ErrorCode::InvalidArgument, "parent frame " +
                                                                     std::to_string(parent.value) +
                                                                     " must be added before its children");
    }
    if (std::any_of(entries_.begin(), entries_.end(),
                    [&](FrameSnapshot::Entry const& e) { return e.id == id; })) {
        return core::makeError(core::ErrorCode::InvalidArgument,
                               "duplicate frame id " + std::to_string(id.value));
    }
    FrameSnapshot::Entry entry;
    entry.id = id;
    entry.parent = parent;
    entry.local_to_parent = local_to_parent;
    entries_.insert(
        std::lower_bound(entries_.begin(), entries_.end(), id,
                         [](FrameSnapshot::Entry const& e, math::FrameId v) { return e.id.value < v.value; }),
        entry);
    return core::Result<void>{};
}

core::Result<FrameSnapshot> FrameSnapshotBuilder::build() const {
    // Defensive: every chain must terminate at the root within N steps.
    // add() ordering already guarantees this; kept so that a future change
    // to add() cannot silently publish a cyclic snapshot.
    for (auto const& e : entries_) {
        math::FrameId parent = e.parent;
        std::size_t steps = 0;
        while (parent != rootFrameId() && steps <= entries_.size()) {
            auto pit = std::lower_bound(
                entries_.begin(), entries_.end(), parent,
                [](FrameSnapshot::Entry const& x, math::FrameId v) { return x.id.value < v.value; });
            if (pit == entries_.end()) {
                return core::makeError(core::ErrorCode::Internal, "unresolved parent in snapshot");
            }
            parent = pit->parent;
            ++steps;
        }
        if (parent != rootFrameId()) {
            return core::makeError(core::ErrorCode::Internal, "cycle detected in snapshot frames");
        }
    }
    return FrameSnapshot(entries_);
}

} // namespace helios::universe
