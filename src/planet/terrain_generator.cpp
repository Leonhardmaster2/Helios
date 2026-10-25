// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "helios/planet/terrain_generator.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>

namespace helios::planet {
namespace {

constexpr double kMaximumPlanetRadiusM = 1.0e13;
constexpr double kMaximumLatticeCoordinate = static_cast<double>(std::uint64_t{1} << 40U);

[[nodiscard]] core::Error invalid(std::string message) {
    return core::makeError(core::ErrorCode::InvalidArgument, std::move(message));
}

[[nodiscard]] bool finitePositive(double value) noexcept {
    return std::isfinite(value) && value > 0.0;
}

[[nodiscard]] bool finiteNonNegative(double value) noexcept {
    return std::isfinite(value) && value >= 0.0;
}

[[nodiscard]] bool allFinite(math::Vec3d const& value) noexcept {
    return std::isfinite(value.v[0]) && std::isfinite(value.v[1]) &&
           std::isfinite(value.v[2]);
}

[[nodiscard]] std::uint64_t splitMix64(std::uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] std::uint64_t latticeHash(std::uint64_t seed, std::int64_t x, std::int64_t y,
                                         std::int64_t z, std::uint8_t octave) noexcept {
    std::uint64_t hash = splitMix64(seed ^ static_cast<std::uint64_t>(octave));
    hash = splitMix64(hash ^ std::bit_cast<std::uint64_t>(x));
    hash = splitMix64(hash ^ std::bit_cast<std::uint64_t>(y));
    return splitMix64(hash ^ std::bit_cast<std::uint64_t>(z));
}

[[nodiscard]] double latticeValue(std::uint64_t seed, std::int64_t x, std::int64_t y,
                                   std::int64_t z, std::uint8_t octave) noexcept {
    // The top 53 bits map exactly onto [0,1), then onto [-1,1). No standard
    // library random distribution is involved, so the mapping is reproducible.
    std::uint64_t const bits = latticeHash(seed, x, y, z, octave) >> 11U;
    double const unit = static_cast<double>(bits) * (1.0 / 9007199254740992.0);
    return unit * 2.0 - 1.0;
}

[[nodiscard]] double fade(double value) noexcept {
    // Quintic interpolation has zero first and second derivatives at lattice
    // boundaries, avoiding visible normal discontinuities.
    return value * value * value * (value * (value * 6.0 - 15.0) + 10.0);
}

[[nodiscard]] double lerp(double a, double b, double t) noexcept {
    return a + (b - a) * t;
}

[[nodiscard]] double valueNoise(math::Vec3d const& direction, double frequency,
                                std::uint64_t seed, std::uint8_t octave) noexcept {
    double const px = direction.v[0] * frequency;
    double const py = direction.v[1] * frequency;
    double const pz = direction.v[2] * frequency;
    auto const x0 = static_cast<std::int64_t>(std::floor(px));
    auto const y0 = static_cast<std::int64_t>(std::floor(py));
    auto const z0 = static_cast<std::int64_t>(std::floor(pz));
    double const tx = fade(px - static_cast<double>(x0));
    double const ty = fade(py - static_cast<double>(y0));
    double const tz = fade(pz - static_cast<double>(z0));

    double values[2][2][2]{};
    for (std::int64_t z = 0; z < 2; ++z) {
        for (std::int64_t y = 0; y < 2; ++y) {
            for (std::int64_t x = 0; x < 2; ++x) {
                values[z][y][x] = latticeValue(seed, x0 + x, y0 + y, z0 + z, octave);
            }
        }
    }
    double const z0y0 = lerp(values[0][0][0], values[0][0][1], tx);
    double const z0y1 = lerp(values[0][1][0], values[0][1][1], tx);
    double const z1y0 = lerp(values[1][0][0], values[1][0][1], tx);
    double const z1y1 = lerp(values[1][1][0], values[1][1][1], tx);
    return lerp(lerp(z0y0, z0y1, ty), lerp(z1y0, z1y1, ty), tz);
}

[[nodiscard]] double rawHeight(TerrainGeneratorConfig const& config,
                               math::Vec3d const& unit_direction,
                               std::uint8_t octave_count) noexcept {
    double height_m = 0.0;
    double amplitude_m = config.height_amplitude_m;
    double frequency = config.base_frequency;
    for (std::uint8_t octave = 0; octave < octave_count; ++octave) {
        height_m += amplitude_m * valueNoise(unit_direction, frequency, config.seed, octave);
        amplitude_m *= config.persistence;
        frequency *= config.lacunarity;
    }
    return height_m;
}

[[nodiscard]] double octaveAmplitudeSum(TerrainGeneratorConfig const& config,
                                         std::uint8_t begin,
                                         std::uint8_t end) noexcept {
    double amplitude_m = config.height_amplitude_m;
    for (std::uint8_t octave = 0; octave < begin; ++octave) {
        amplitude_m *= config.persistence;
    }
    double sum_m = 0.0;
    for (std::uint8_t octave = begin; octave < end; ++octave) {
        sum_m += std::abs(amplitude_m);
        amplitude_m *= config.persistence;
    }
    return std::nextafter(sum_m, std::numeric_limits<double>::infinity());
}

[[nodiscard]] core::Result<math::Vec3d> unitDirection(math::Vec3d const& direction) {
    if (!allFinite(direction)) {
        return invalid("terrain direction must contain only finite values");
    }
    double const magnitude = math::length(direction);
    if (!finitePositive(magnitude) || magnitude < 1.0e-300) {
        return invalid("terrain direction must have non-zero magnitude");
    }
    return direction / magnitude;
}

[[nodiscard]] math::Vec3d canonicalTangent(math::Vec3d const& direction) noexcept {
    // Select the least-aligned cardinal axis. Ties are resolved X, then Y,
    // then Z, which makes this basis a function of direction alone.
    double const ax = std::abs(direction.v[0]);
    double const ay = std::abs(direction.v[1]);
    double const az = std::abs(direction.v[2]);
    math::Vec3d reference{1.0, 0.0, 0.0};
    if (ay < ax && ay <= az) {
        reference = {0.0, 1.0, 0.0};
    } else if (az < ax && az < ay) {
        reference = {0.0, 0.0, 1.0};
    }
    return math::normalized(math::cross(reference, direction));
}

[[nodiscard]] math::Vec3d displacedPosition(TerrainGeneratorConfig const& config,
                                             math::Vec3d const& direction,
                                             std::uint8_t octaves) noexcept {
    return direction * (config.planet_radius_m + rawHeight(config, direction, octaves));
}

[[nodiscard]] math::Vec3d terrainNormal(TerrainGeneratorConfig const& config,
                                        math::Vec3d const& direction,
                                        std::uint8_t octaves) noexcept {
    math::Vec3d const tangent_u = canonicalTangent(direction);
    math::Vec3d const tangent_v = math::cross(direction, tangent_u);
    double const step = config.normal_angular_step_rad;
    math::Vec3d const u_plus = math::normalized(direction + tangent_u * step);
    math::Vec3d const u_minus = math::normalized(direction - tangent_u * step);
    math::Vec3d const v_plus = math::normalized(direction + tangent_v * step);
    math::Vec3d const v_minus = math::normalized(direction - tangent_v * step);
    math::Vec3d const du = displacedPosition(config, u_plus, octaves) -
                           displacedPosition(config, u_minus, octaves);
    math::Vec3d const dv = displacedPosition(config, v_plus, octaves) -
                           displacedPosition(config, v_minus, octaves);
    math::Vec3d normal = math::normalized(math::cross(du, dv));
    if (math::dot(normal, direction) < 0.0) {
        normal = normal * -1.0;
    }
    return normal;
}

[[nodiscard]] std::uint64_t hashByte(std::uint64_t hash, std::uint8_t byte) noexcept {
    constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
    return (hash ^ byte) * kFnvPrime;
}

template <typename T>
[[nodiscard]] std::uint64_t hashUnsigned(std::uint64_t hash, T value) noexcept {
    static_assert(std::is_unsigned_v<T>);
    for (std::size_t shift = 0; shift < sizeof(T) * 8U; shift += 8U) {
        hash = hashByte(hash, static_cast<std::uint8_t>((value >> shift) & T{0xff}));
    }
    return hash;
}

[[nodiscard]] std::uint64_t hashDouble(std::uint64_t hash, double value) noexcept {
    if (value == 0.0) {
        value = 0.0; // Canonicalize negative zero.
    }
    return hashUnsigned(hash, std::bit_cast<std::uint64_t>(value));
}

} // namespace

core::Result<void> validate(TerrainGeneratorConfig const& config) {
    if (config.generator_version != kTerrainGeneratorVersion) {
        return core::makeError(core::ErrorCode::Unsupported,
                               "terrain generator version is not supported by this build");
    }
    if (!finitePositive(config.planet_radius_m) ||
        config.planet_radius_m > kMaximumPlanetRadiusM) {
        return invalid("terrain planet radius must be finite and in (0, 1e13] metres");
    }
    if (!finiteNonNegative(config.height_amplitude_m) ||
        config.height_amplitude_m > config.planet_radius_m * 0.25) {
        return invalid("terrain height amplitude must be finite and at most 25% of radius");
    }
    if (!finitePositive(config.base_frequency) || config.base_frequency > 1.0e9) {
        return invalid("terrain base frequency must be finite and in (0, 1e9]");
    }
    if (!std::isfinite(config.lacunarity) || config.lacunarity < 1.0 ||
        config.lacunarity > 8.0) {
        return invalid("terrain lacunarity must be finite and in [1, 8]");
    }
    if (!std::isfinite(config.persistence) || config.persistence <= 0.0 ||
        config.persistence > 1.0) {
        return invalid("terrain persistence must be finite and in (0, 1]");
    }
    if (config.root_octaves == 0 || config.max_octaves == 0 ||
        config.root_octaves > config.max_octaves ||
        config.max_octaves > kTerrainMaximumOctaves) {
        return invalid("terrain octave counts must satisfy 1 <= root <= max <= 24");
    }
    double maximum_displacement_m = 0.0;
    double octave_amplitude_m = config.height_amplitude_m;
    for (std::uint8_t octave = 0; octave < config.max_octaves; ++octave) {
        maximum_displacement_m += octave_amplitude_m;
        octave_amplitude_m *= config.persistence;
    }
    if (!std::isfinite(maximum_displacement_m) ||
        maximum_displacement_m > config.planet_radius_m * 0.25) {
        return invalid("terrain's total octave amplitude must be at most 25% of radius");
    }
    double maximum_frequency = config.base_frequency;
    for (std::uint8_t octave = 1; octave < config.max_octaves; ++octave) {
        maximum_frequency *= config.lacunarity;
    }
    if (!std::isfinite(maximum_frequency) || maximum_frequency > kMaximumLatticeCoordinate) {
        return invalid("terrain octave frequencies exceed the bounded lattice domain");
    }
    if (!std::isfinite(config.normal_angular_step_rad) ||
        config.normal_angular_step_rad < 1.0e-9 ||
        config.normal_angular_step_rad > 1.0e-2) {
        return invalid("terrain normal step must be finite and in [1e-9, 1e-2] radians");
    }
    return {};
}

std::uint8_t activeTerrainOctaves(TerrainGeneratorConfig const& config,
                                   std::uint8_t level) noexcept {
    unsigned int const requested = static_cast<unsigned int>(config.root_octaves) + level;
    return static_cast<std::uint8_t>(
        std::min<unsigned int>(requested, static_cast<unsigned int>(config.max_octaves)));
}

core::Result<TerrainSample> TerrainTile::sample(std::uint32_t x, std::uint32_t y) const {
    if (x >= kTerrainTileSamplesPerEdge || y >= kTerrainTileSamplesPerEdge) {
        return invalid("terrain sample coordinate is outside the 33x33 tile grid");
    }
    if (samples.size() != kTerrainTileSampleCount) {
        return core::makeError(core::ErrorCode::CorruptData,
                               "terrain tile does not contain exactly 33x33 samples");
    }
    return samples[static_cast<std::size_t>(y) * kTerrainTileSamplesPerEdge + x];
}

std::uint64_t stableTerrainContentHash(TerrainTile const& tile) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    hash = hashUnsigned(hash, static_cast<std::uint8_t>(tile.key.face));
    hash = hashUnsigned(hash, tile.key.level);
    hash = hashUnsigned(hash, tile.key.x);
    hash = hashUnsigned(hash, tile.key.y);
    hash = hashUnsigned(hash, tile.seed);
    hash = hashUnsigned(hash, tile.generator_version);
    hash = hashUnsigned(hash, tile.octave_count);
    hash = hashUnsigned(hash, tile.parent_octave_count);
    hash = hashUnsigned(hash, static_cast<std::uint64_t>(tile.samples.size()));
    hash = hashDouble(hash, tile.min_height_m);
    hash = hashDouble(hash, tile.max_height_m);
    hash = hashDouble(hash, tile.max_observed_parent_delta_m);
    hash = hashDouble(hash, tile.parent_relative_geometric_error_m);
    for (TerrainSample const& sample : tile.samples) {
        for (double component : sample.direction.v) {
            hash = hashDouble(hash, component);
        }
        hash = hashDouble(hash, sample.height_m);
        hash = hashDouble(hash, sample.parent_compatible_height_m);
        for (double component : sample.normal.v) {
            hash = hashDouble(hash, component);
        }
    }
    return hash;
}

core::Result<TerrainGenerator> TerrainGenerator::create(TerrainGeneratorConfig config) {
    auto valid = validate(config);
    if (valid.failed()) {
        return valid.error();
    }
    return TerrainGenerator(config);
}

core::Result<double> TerrainGenerator::height(math::Vec3d const& direction,
                                               std::uint8_t level) const {
    if (level > kMaxTileLevel) {
        return invalid("terrain level exceeds the cube-sphere topology limit");
    }
    return heightForOctaves(direction, activeTerrainOctaves(config_, level));
}

core::Result<double> TerrainGenerator::heightForOctaves(math::Vec3d const& direction,
                                                         std::uint8_t octave_count) const {
    if (octave_count > config_.max_octaves) {
        return invalid("requested terrain octave count exceeds configured maximum");
    }
    auto unit = unitDirection(direction);
    if (unit.failed()) {
        return unit.error();
    }
    return rawHeight(config_, unit.value(), octave_count);
}

core::Result<TerrainTile> TerrainGenerator::generate(TileKey const& key) const {
    if (!isValid(key)) {
        return invalid("cannot generate terrain for an invalid tile key");
    }

    TerrainTile tile{};
    tile.key = key;
    tile.seed = config_.seed;
    tile.generator_version = config_.generator_version;
    tile.octave_count = activeTerrainOctaves(config_, key.level);
    tile.parent_octave_count =
        key.level == 0 ? 0 : activeTerrainOctaves(config_, static_cast<std::uint8_t>(key.level - 1));
    tile.samples.reserve(kTerrainTileSampleCount);
    tile.min_height_m = std::numeric_limits<double>::infinity();
    tile.max_height_m = -std::numeric_limits<double>::infinity();

    for (std::uint32_t y = 0; y < kTerrainTileSamplesPerEdge; ++y) {
        for (std::uint32_t x = 0; x < kTerrainTileSamplesPerEdge; ++x) {
            auto direction = cubeSphereDirection(key, x, y, kTerrainTileSamplesPerEdge);
            if (direction.failed()) {
                return direction.error();
            }
            double const height_m = rawHeight(config_, direction.value(), tile.octave_count);
            double const parent_height_m =
                rawHeight(config_, direction.value(), tile.parent_octave_count);
            math::Vec3d const normal = terrainNormal(config_, direction.value(), tile.octave_count);
            tile.samples.push_back(
                TerrainSample{direction.value(), height_m, parent_height_m, normal});
            tile.min_height_m = std::min(tile.min_height_m, height_m);
            tile.max_height_m = std::max(tile.max_height_m, height_m);
            tile.max_observed_parent_delta_m =
                std::max(tile.max_observed_parent_delta_m, std::abs(height_m - parent_height_m));
        }
    }

    tile.parent_relative_geometric_error_m =
        octaveAmplitudeSum(config_, tile.parent_octave_count, tile.octave_count);
    tile.content_hash = stableTerrainContentHash(tile);
    return tile;
}

} // namespace helios::planet
