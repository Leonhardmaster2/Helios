// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Canonical, CPU-only cube-sphere topology. Tile/sample addressing is reduced
// to one integer face grid before conversion to floating point. Consequently,
// samples shared by a parent and child, or by two tiles on one face, are
// bit-identical before normalization and cannot develop cracks from slightly
// different interpolation paths.
#pragma once

#include "helios/core/error.hpp"
#include "helios/math/vec.hpp"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace helios::planet {

// Numeric values are part of TileKey's stable ordering and hash contract.
enum class CubeFace : std::uint8_t {
    PositiveX = 0,
    NegativeX = 1,
    PositiveY = 2,
    NegativeY = 3,
    PositiveZ = 4,
    NegativeZ = 5,
};

inline constexpr std::uint32_t kCubeFaceCount = 6;
inline constexpr std::uint32_t kDefaultPatchSamples = 33;
inline constexpr std::uint8_t kMaxTileLevel = 30;

[[nodiscard]] constexpr bool isValid(CubeFace face) noexcept {
    return static_cast<std::uint8_t>(face) < kCubeFaceCount;
}

[[nodiscard]] std::string_view toString(CubeFace face) noexcept;

// A stable quadtree address. At level L, x and y are in [0, 2^L).
// The defaulted comparison orders face, level, x, then y because the member
// layout is intentional and stable. That order is used for deterministic
// traversal, tie breaking, evidence reports, and cache naming.
struct TileKey {
    CubeFace face = CubeFace::PositiveX;
    std::uint8_t level = 0;
    std::uint32_t x = 0;
    std::uint32_t y = 0;

    [[nodiscard]] constexpr auto operator<=>(TileKey const&) const noexcept = default;
};

[[nodiscard]] bool isValid(TileKey const& key) noexcept;
[[nodiscard]] std::string toString(TileKey const& key);

// Exact 64-bit FNV-1a over the logical fields in a fixed byte order. Unlike
// std::hash combining, this value is reproducible across processes and hosts.
[[nodiscard]] std::uint64_t stableHash64(TileKey const& key) noexcept;

struct TileKeyHash {
    [[nodiscard]] std::size_t operator()(TileKey const& key) const noexcept {
        return static_cast<std::size_t>(stableHash64(key));
    }
};

// Child order is southwest, southeast, northwest, northeast in face-local
// coordinates (x changes in bit 0, y in bit 1).
[[nodiscard]] core::Result<TileKey> tileParent(TileKey const& key);
[[nodiscard]] core::Result<TileKey> tileChild(TileKey const& key, std::uint8_t child_index);
[[nodiscard]] core::Result<std::array<TileKey, 4>> tileChildren(TileKey const& key);

// Rational face-grid coordinate used to make topology observable in tests and
// diagnostics. Local cube coordinates are numerator / denominator, with both
// numerators in [-denominator, +denominator].
struct CanonicalFaceSample {
    std::int64_t u_numerator = 0;
    std::int64_t v_numerator = 0;
    std::uint64_t denominator = 1;

    [[nodiscard]] constexpr auto operator<=>(CanonicalFaceSample const&) const noexcept = default;
};

// sample_x/sample_y address vertices in a patch_samples x patch_samples grid.
// The default is 33x33 (32 intervals), which permits exact parent samples at
// every even child index. Other resolutions >= 2 are supported as long as the
// integer face grid fits the documented level bound.
[[nodiscard]] core::Result<CanonicalFaceSample>
canonicalFaceSample(TileKey const& key, std::uint32_t sample_x, std::uint32_t sample_y,
                    std::uint32_t patch_samples = kDefaultPatchSamples);

// Maps face-local cube coordinates in [-1,+1] to the canonical face plane,
// then normalizes radially to the unit sphere. This low-level overload accepts
// finite values only and is primarily useful for bounds and diagnostics.
[[nodiscard]] core::Result<math::Vec3d> cubeSphereDirection(CubeFace face, double u, double v);

// Integer-addressed production path.
[[nodiscard]] core::Result<math::Vec3d>
cubeSphereDirection(TileKey const& key, std::uint32_t sample_x, std::uint32_t sample_y,
                    std::uint32_t patch_samples = kDefaultPatchSamples);

// Center and corners are convenient for CPU LOD bounds. They use the same
// canonical mapping as mesh vertices.
[[nodiscard]] core::Result<math::Vec3d> tileCenterDirection(TileKey const& key);
[[nodiscard]] core::Result<std::array<math::Vec3d, 4>> tileCornerDirections(TileKey const& key);

} // namespace helios::planet

namespace std {
template <> struct hash<helios::planet::TileKey> {
    [[nodiscard]] std::size_t operator()(helios::planet::TileKey const& key) const noexcept {
        return helios::planet::TileKeyHash{}(key);
    }
};
} // namespace std
