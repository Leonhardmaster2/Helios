// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "helios/planet/cube_sphere.hpp"

#include <cmath>
#include <format>
#include <limits>
#include <utility>

namespace helios::planet {
namespace {

[[nodiscard]] core::Error invalid(std::string message) {
    return core::makeError(core::ErrorCode::InvalidArgument, std::move(message));
}

[[nodiscard]] math::Vec3d faceVector(CubeFace face, double u, double v) noexcept {
    // Face-local v points north. The orientation makes every shared edge agree
    // exactly (possibly with the neighbouring edge's sample order reversed).
    switch (face) {
    case CubeFace::PositiveX:
        return {1.0, v, -u};
    case CubeFace::NegativeX:
        return {-1.0, v, u};
    case CubeFace::PositiveY:
        return {u, 1.0, -v};
    case CubeFace::NegativeY:
        return {u, -1.0, v};
    case CubeFace::PositiveZ:
        return {u, v, 1.0};
    case CubeFace::NegativeZ:
        return {-u, v, -1.0};
    }
    return {};
}

[[nodiscard]] std::uint64_t hashByte(std::uint64_t hash, std::uint8_t byte) noexcept {
    constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
    return (hash ^ byte) * kFnvPrime;
}

} // namespace

std::string_view toString(CubeFace face) noexcept {
    switch (face) {
    case CubeFace::PositiveX:
        return "+X";
    case CubeFace::NegativeX:
        return "-X";
    case CubeFace::PositiveY:
        return "+Y";
    case CubeFace::NegativeY:
        return "-Y";
    case CubeFace::PositiveZ:
        return "+Z";
    case CubeFace::NegativeZ:
        return "-Z";
    }
    return "invalid";
}

bool isValid(TileKey const& key) noexcept {
    if (!isValid(key.face) || key.level > kMaxTileLevel) {
        return false;
    }
    std::uint64_t const tiles_per_axis = std::uint64_t{1} << key.level;
    return key.x < tiles_per_axis && key.y < tiles_per_axis;
}

std::string toString(TileKey const& key) {
    return std::format("{}/L{}/{}/{}", toString(key.face), static_cast<unsigned int>(key.level), key.x,
                       key.y);
}

std::uint64_t stableHash64(TileKey const& key) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    hash = hashByte(hash, static_cast<std::uint8_t>(key.face));
    hash = hashByte(hash, key.level);
    for (std::uint32_t value : {key.x, key.y}) {
        for (unsigned int shift = 0; shift < 32; shift += 8) {
            hash = hashByte(hash, static_cast<std::uint8_t>((value >> shift) & 0xffU));
        }
    }
    return hash;
}

core::Result<TileKey> tileParent(TileKey const& key) {
    if (!isValid(key)) {
        return invalid("cannot find parent of invalid tile " + toString(key));
    }
    if (key.level == 0) {
        return core::makeError(core::ErrorCode::OutOfRange,
                               "root cube-face tile has no quadtree parent");
    }
    return TileKey{key.face, static_cast<std::uint8_t>(key.level - 1), key.x / 2U, key.y / 2U};
}

core::Result<TileKey> tileChild(TileKey const& key, std::uint8_t child_index) {
    if (!isValid(key)) {
        return invalid("cannot create child of invalid tile " + toString(key));
    }
    if (child_index >= 4) {
        return invalid("quadtree child index must be in [0,3]");
    }
    if (key.level == kMaxTileLevel) {
        return core::makeError(core::ErrorCode::OutOfRange,
                               "tile is already at the maximum supported level");
    }
    std::uint32_t const child_x = key.x * 2U + (child_index & 1U);
    std::uint32_t const child_y = key.y * 2U + ((child_index >> 1U) & 1U);
    return TileKey{key.face, static_cast<std::uint8_t>(key.level + 1), child_x, child_y};
}

core::Result<std::array<TileKey, 4>> tileChildren(TileKey const& key) {
    std::array<TileKey, 4> children{};
    for (std::uint8_t child_index = 0; child_index < children.size(); ++child_index) {
        auto child = tileChild(key, child_index);
        if (child.failed()) {
            return child.error();
        }
        children[child_index] = child.value();
    }
    return children;
}

core::Result<CanonicalFaceSample> canonicalFaceSample(TileKey const& key, std::uint32_t sample_x,
                                                       std::uint32_t sample_y,
                                                       std::uint32_t patch_samples) {
    if (!isValid(key)) {
        return invalid("cannot sample invalid tile " + toString(key));
    }
    if (patch_samples < 2) {
        return invalid("cube-sphere patch must have at least two samples per edge");
    }
    if (sample_x >= patch_samples || sample_y >= patch_samples) {
        return invalid("cube-sphere sample index is outside the patch grid");
    }

    std::uint64_t const intervals = static_cast<std::uint64_t>(patch_samples - 1U);
    std::uint64_t const tiles_per_axis = std::uint64_t{1} << key.level;
    if (intervals > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) /
                        (tiles_per_axis * 2U)) {
        return core::makeError(core::ErrorCode::OutOfRange,
                               "cube-sphere face grid exceeds the signed 64-bit topology range");
    }

    std::uint64_t const denominator = intervals * tiles_per_axis;
    std::uint64_t const global_x = static_cast<std::uint64_t>(key.x) * intervals + sample_x;
    std::uint64_t const global_y = static_cast<std::uint64_t>(key.y) * intervals + sample_y;
    auto const signed_denominator = static_cast<std::int64_t>(denominator);
    auto const signed_global_x = static_cast<std::int64_t>(global_x);
    auto const signed_global_y = static_cast<std::int64_t>(global_y);
    return CanonicalFaceSample{signed_global_x * 2 - signed_denominator,
                               signed_global_y * 2 - signed_denominator, denominator};
}

core::Result<math::Vec3d> cubeSphereDirection(CubeFace face, double u, double v) {
    if (!isValid(face)) {
        return invalid("cube-sphere face is invalid");
    }
    if (!std::isfinite(u) || !std::isfinite(v) || u < -1.0 || u > 1.0 || v < -1.0 || v > 1.0) {
        return invalid("cube-sphere local coordinates must be finite values in [-1,+1]");
    }
    math::Vec3d const cube = faceVector(face, u, v);
    double const magnitude = math::length(cube);
    return cube / magnitude;
}

core::Result<math::Vec3d> cubeSphereDirection(TileKey const& key, std::uint32_t sample_x,
                                               std::uint32_t sample_y,
                                               std::uint32_t patch_samples) {
    auto canonical = canonicalFaceSample(key, sample_x, sample_y, patch_samples);
    if (canonical.failed()) {
        return canonical.error();
    }
    double const denominator = static_cast<double>(canonical.value().denominator);
    double const u = static_cast<double>(canonical.value().u_numerator) / denominator;
    double const v = static_cast<double>(canonical.value().v_numerator) / denominator;
    return cubeSphereDirection(key.face, u, v);
}

core::Result<math::Vec3d> tileCenterDirection(TileKey const& key) {
    // A three-sample patch addresses the exact midpoint without introducing a
    // second interpolation implementation.
    return cubeSphereDirection(key, 1U, 1U, 3U);
}

core::Result<std::array<math::Vec3d, 4>> tileCornerDirections(TileKey const& key) {
    constexpr std::array<std::array<std::uint32_t, 2>, 4> kCorners{{
        {{0U, 0U}},
        {{1U, 0U}},
        {{0U, 1U}},
        {{1U, 1U}},
    }};
    std::array<math::Vec3d, 4> directions{};
    for (std::size_t i = 0; i < directions.size(); ++i) {
        auto direction = cubeSphereDirection(key, kCorners[i][0], kCorners[i][1], 2U);
        if (direction.failed()) {
            return direction.error();
        }
        directions[i] = direction.value();
    }
    return directions;
}

} // namespace helios::planet
