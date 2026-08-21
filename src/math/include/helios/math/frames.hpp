// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Frame-tagged coordinate types (docs/ARCHITECTURE.md "Coordinates and
// precision"). Compile-time kind tags make cross-kind arithmetic (e.g.
// barycentric minus tile-local) a type error; the runtime FrameId makes
// cross-frame arithmetic within one kind an explicit checked error instead
// of silent garbage. There is deliberately no operator+/- between positions.
#pragma once

#include "helios/core/error.hpp"
#include "helios/math/quat.hpp"
#include "helios/math/vec.hpp"

#include <cstdint>
#include <string>

namespace helios::math {

// Runtime identity of a frame within a compile-time kind
// (e.g. Earth-fixed vs Mars-fixed inside BodyFixed).
struct FrameId {
    std::uint32_t value = 0;
};

[[nodiscard]] constexpr bool operator==(FrameId a, FrameId b) noexcept {
    return a.value == b.value;
}
[[nodiscard]] constexpr bool operator!=(FrameId a, FrameId b) noexcept {
    return !(a == b);
}

// Compile-time spatial-domain tags. Values are never converted between kinds
// implicitly; every domain change goes through an explicit transform.
struct BarycentricTag {};    // Solar-System barycentric root (~1e13 m)
struct BodyFixedTag {};      // one rotating frame per detailed body
struct TileLocalTag {};      // tile-local vertices/samples
struct CameraRelativeTag {}; // per-frame float32 render representation source

template <typename KindTag> class Position {
  public:
    Position() = default;

    Position(FrameId frame, Vec3d coords) : frame_(frame), coords_(coords) {}

    [[nodiscard]] constexpr FrameId frame() const noexcept {
        return frame_;
    }
    [[nodiscard]] constexpr Vec3d const& coords() const noexcept {
        return coords_;
    }

    // Checked same-frame displacement: other - this. Cross-frame use is an
    // InvalidArgument error, never silently computed.
    [[nodiscard]] core::Result<Vec3d> differenceTo(Position const& other) const {
        if (other.frame_ != frame_) {
            return core::makeError(core::ErrorCode::InvalidArgument,
                                   "cross-frame position difference rejected (frame " +
                                       std::to_string(frame_.value) + " vs " +
                                       std::to_string(other.frame_.value) + ")");
        }
        return other.coords_ - coords_;
    }

    // Translate within the same frame (result keeps this position's FrameId).
    [[nodiscard]] Position displacedBy(Vec3d displacement) const {
        return Position{frame_, coords_ + displacement};
    }

  private:
    FrameId frame_{};
    Vec3d coords_{};
};

using BarycentricPosition = Position<BarycentricTag>;
using BodyFixedPosition = Position<BodyFixedTag>;
using TileLocalPosition = Position<TileLocalTag>;
using CameraRelativePosition = Position<CameraRelativeTag>;

// Camera pose in a tagged storage frame: position + orientation (optional
// velocity state arrives with M4 route work, not needed for M1 types).
template <typename KindTag> struct CameraPose {
    Position<KindTag> position{};
    Quatd orientation = Quatd::identity();
};

using BarycentricCameraPose = CameraPose<BarycentricTag>;
using BodyFixedCameraPose = CameraPose<BodyFixedTag>;

} // namespace helios::math
