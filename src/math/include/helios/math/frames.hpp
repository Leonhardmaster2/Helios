// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Frame-tagged coordinate types (docs/ARCHITECTURE.md "Coordinates and
// precision"). Compile-time kind tags make cross-kind arithmetic (e.g.
// barycentric minus tile-local) a type error; the runtime FrameId makes
// cross-frame arithmetic within one kind an explicit checked error instead
// of silent garbage. There is deliberately no operator+/- between positions
// and no conversion between kinds or scalar representations.
//
// Scalar representations are explicit per kind:
//   - BarycentricPosition / BodyFixedPosition store binary64 (Vec3d);
//   - TileLocalPosition stores binary32 (Vec3f): tile-local samples are a
//     local-float domain per ARCHITECTURE.md spatial tier 3;
//   - the camera-relative RENDER representation is float32 but is NOT a
//     Position: it is helios_render::NearRenderRecord, produced only by the
//     scene-assembly boundary after the double-precision subtraction.
#pragma once

#include "helios/core/error.hpp"
#include "helios/math/quat.hpp"
#include "helios/math/vec.hpp"

#include <cmath>
#include <cstdint>
#include <string>
#include <type_traits>

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
struct BarycentricTag {}; // Solar-System barycentric root (~1e13 m)
struct BodyFixedTag {};   // one rotating frame per detailed body
struct TileLocalTag {};   // tile-local vertices/samples

template <typename KindTag, typename Scalar> class Position {
  public:
    using ScalarT = Scalar;
    static_assert(std::is_same_v<Scalar, float> || std::is_same_v<Scalar, double>,
                  "Position supports binary32/binary64 scalars");

    Position() = default;

    Position(FrameId frame, Vec<Scalar, 3> coords) : frame_(frame), coords_(coords) {}

    [[nodiscard]] constexpr FrameId frame() const noexcept {
        return frame_;
    }
    [[nodiscard]] constexpr Vec<Scalar, 3> const& coords() const noexcept {
        return coords_;
    }

    // Checked same-frame displacement: other - this ("from here to other").
    // Cross-frame use is an InvalidArgument error; non-finite stored values
    // make both positions invalid to differ.
    [[nodiscard]] core::Result<Vec<Scalar, 3>> differenceTo(Position const& other) const {
        if (other.frame_ != frame_) {
            return core::makeError(core::ErrorCode::InvalidArgument,
                                   "cross-frame position difference rejected (frame " +
                                       std::to_string(frame_.value) + " vs " +
                                       std::to_string(other.frame_.value) + ")");
        }
        Vec<Scalar, 3> d = other.coords_ - coords_;
        for (int i = 0; i < 3; ++i) {
            if (!isFinite(d.v[i])) {
                return core::makeError(core::ErrorCode::InvalidArgument,
                                       "position difference involves non-finite coordinates");
            }
        }
        return d;
    }

    // Translate within the same frame (result keeps this position's FrameId).
    [[nodiscard]] Position displacedBy(Vec<Scalar, 3> displacement) const {
        return Position{frame_, coords_ + displacement};
    }

    [[nodiscard]] bool allFinite() const noexcept {
        for (int i = 0; i < 3; ++i) {
            if (!isFinite(coords_.v[i])) {
                return false;
            }
        }
        return true;
    }

  private:
    static bool isFinite(Scalar s) {
        return std::isfinite(s);
    }

    FrameId frame_{};
    Vec<Scalar, 3> coords_{};
};

template <typename K, typename S>
[[nodiscard]] constexpr bool operator==(Position<K, S> const& a, Position<K, S> const& b) noexcept {
    return a.frame() == b.frame() && a.coords() == b.coords();
}

using BarycentricPosition = Position<BarycentricTag, double>;
using BodyFixedPosition = Position<BodyFixedTag, double>;
using TileLocalPosition = Position<TileLocalTag, float>;

// Camera pose in a tagged storage frame: position + orientation (optional
// velocity state arrives with M4 route work, not needed for M1 types).
template <typename KindTag, typename Scalar = double> struct CameraPose {
    Position<KindTag, Scalar> position{};
    Quatd orientation = Quatd::identity(); // double precision even for float storage
};

using BarycentricCameraPose = CameraPose<BarycentricTag, double>;
using BodyFixedCameraPose = CameraPose<BodyFixedTag, double>;

} // namespace helios::math
