// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Deterministic, CPU-only terrain content for cube-sphere tiles. The height
// field is evaluated in normalized three-dimensional direction space, never
// in face UV space, so two cube faces that address the same direction also
// address exactly the same terrain value.
#pragma once

#include "helios/core/error.hpp"
#include "helios/math/vec.hpp"
#include "helios/planet/cube_sphere.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace helios::planet {

inline constexpr std::uint32_t kTerrainGeneratorVersion = 1;
inline constexpr std::uint32_t kTerrainTileSamplesPerEdge = 33;
inline constexpr std::size_t kTerrainTileSampleCount =
    static_cast<std::size_t>(kTerrainTileSamplesPerEdge) * kTerrainTileSamplesPerEdge;
inline constexpr std::uint8_t kTerrainMaximumOctaves = 24;

struct TerrainGeneratorConfig {
    std::uint64_t seed = 0x48454c494f535631ULL; // "HELIOSV1"
    std::uint32_t generator_version = kTerrainGeneratorVersion;

    double planet_radius_m = 6'371'000.0;
    double height_amplitude_m = 9'000.0;
    double base_frequency = 1.5;
    double lacunarity = 2.0;
    double persistence = 0.5;

    // A level-L tile evaluates min(root_octaves + L, max_octaves) bands.
    // This avoids spending CPU work on wavelengths that cannot be represented
    // by a coarse tile and gives each child a precise parent-compatible field.
    std::uint8_t root_octaves = 4;
    std::uint8_t max_octaves = 16;

    // Normals are central differences of the displaced sphere in a canonical
    // direction-space tangent frame. The fixed angular step makes a sample's
    // normal independent of cube face and neighbouring-tile availability; no
    // height halo or cross-tile read is required.
    double normal_angular_step_rad = 1.0e-5;
};

[[nodiscard]] core::Result<void> validate(TerrainGeneratorConfig const& config);

// Requires a validated configuration. Exposed so LOD diagnostics can display
// the precise level-aware band limit used by generated content.
[[nodiscard]] std::uint8_t activeTerrainOctaves(TerrainGeneratorConfig const& config,
                                                 std::uint8_t level) noexcept;

struct TerrainSample {
    math::Vec3d direction{};
    double height_m = 0.0;
    double parent_compatible_height_m = 0.0;
    math::Vec3d normal{};
};

struct TerrainTile {
    TileKey key{};
    std::uint64_t seed = 0;
    std::uint32_t generator_version = 0;
    std::uint8_t octave_count = 0;
    std::uint8_t parent_octave_count = 0;

    // Row-major 33x33 samples. Directions and normals are included so this
    // object is immediately consumable by a CPU mesh builder.
    std::vector<TerrainSample> samples;

    double min_height_m = 0.0;
    double max_height_m = 0.0;
    double max_observed_parent_delta_m = 0.0;

    // Analytic upper bound on |height - parent-compatible height|. It is the
    // sum of the absolute amplitudes of every octave introduced at this level,
    // rounded upward, and is therefore conservative between sample points too.
    double parent_relative_geometric_error_m = 0.0;

    // FNV-1a over canonical metadata and IEEE-754 sample bits. This deliberately
    // does not use std::hash, whose result need not be stable between processes.
    std::uint64_t content_hash = 0;

    [[nodiscard]] core::Result<TerrainSample> sample(std::uint32_t x,
                                                      std::uint32_t y) const;
};

// Recomputes TerrainTile::content_hash. Useful when validating cache entries.
[[nodiscard]] std::uint64_t stableTerrainContentHash(TerrainTile const& tile) noexcept;

class TerrainGenerator {
  public:
    [[nodiscard]] static core::Result<TerrainGenerator>
    create(TerrainGeneratorConfig config = {});

    [[nodiscard]] TerrainGeneratorConfig const& config() const noexcept {
        return config_;
    }

    // The input may have any finite, non-zero magnitude; it is normalized
    // before evaluation. `level` controls the octave-band limit.
    [[nodiscard]] core::Result<double> height(math::Vec3d const& direction,
                                               std::uint8_t level) const;

    // Lower-level diagnostic path used to prove parent compatibility.
    [[nodiscard]] core::Result<double> heightForOctaves(math::Vec3d const& direction,
                                                         std::uint8_t octave_count) const;

    [[nodiscard]] core::Result<TerrainTile> generate(TileKey const& key) const;

  private:
    explicit TerrainGenerator(TerrainGeneratorConfig config) : config_(config) {}

    TerrainGeneratorConfig config_{};
};

} // namespace helios::planet
