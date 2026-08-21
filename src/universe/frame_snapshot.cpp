// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "helios/universe/frame_snapshot.hpp"

#include <string>

namespace helios::universe {

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

void FrameSnapshot::add(math::FrameId id, FrameTransform transform) {
    for (auto& e : entries_) {
        if (e.id == id) {
            e.transform = transform;
            return;
        }
    }
    entries_.push_back(Entry{id, transform});
}

core::Result<FrameTransform> FrameSnapshot::transformOf(math::FrameId id) const {
    for (auto const& e : entries_) {
        if (e.id == id) {
            return e.transform;
        }
    }
    return core::makeError(core::ErrorCode::NotFound,
                           "frame " + std::to_string(id.value) + " not in snapshot");
}

core::Result<math::Vec3d> FrameSnapshot::pointInFrame(math::FrameId from, math::FrameId to,
                                                      math::Vec3d const& p) const {
    auto f = transformOf(from);
    if (f.failed()) {
        return f.error();
    }
    auto t = transformOf(to);
    if (t.failed()) {
        return t.error();
    }
    // root <- from, then root -> to.
    const math::Vec3d in_root = f.value().rotation.rotate(p) + f.value().translation;
    const FrameTransform to_local = invert(t.value());
    return to_local.rotation.rotate(in_root) + to_local.translation;
}

} // namespace helios::universe
