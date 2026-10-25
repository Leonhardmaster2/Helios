// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "helios/planet/terrain_generator.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace {

using namespace helios::planet;

TerrainGenerator makeGenerator(TerrainGeneratorConfig config = {}) {
    auto result = TerrainGenerator::create(config);
    REQUIRE(result.ok());
    return std::move(result.value());
}

TerrainTile makeTile(TerrainGenerator const& generator, TileKey const& key) {
    auto result = generator.generate(key);
    REQUIRE(result.ok());
    return std::move(result.value());
}

void checkVectorExactly(helios::math::Vec3d const& left, helios::math::Vec3d const& right) {
    CHECK(left.v[0] == right.v[0]);
    CHECK(left.v[1] == right.v[1]);
    CHECK(left.v[2] == right.v[2]);
}

void checkSampleExactly(TerrainSample const& left, TerrainSample const& right) {
    checkVectorExactly(left.direction, right.direction);
    CHECK(left.height_m == right.height_m);
    CHECK(left.parent_compatible_height_m == right.parent_compatible_height_m);
    checkVectorExactly(left.normal, right.normal);
}

} // namespace

TEST_CASE("terrain generator validation is bounded and versioned") {
    TerrainGeneratorConfig config{};
    CHECK(validate(config).ok());

    config.generator_version = kTerrainGeneratorVersion + 1U;
    auto unsupported = validate(config);
    REQUIRE(unsupported.failed());
    // Cast scoped enums before handing them to doctest.  ErrorCode has an
    // ADL-visible toString() that returns std::string_view, while this pinned
    // doctest release expects its own String from enum stringification.
    CHECK(static_cast<int>(unsupported.error().code()) ==
          static_cast<int>(helios::core::ErrorCode::Unsupported));

    config = {};
    config.root_octaves = 0;
    CHECK(validate(config).failed());
    config = {};
    config.max_octaves = static_cast<std::uint8_t>(kTerrainMaximumOctaves + 1U);
    CHECK(validate(config).failed());
    config = {};
    config.height_amplitude_m = config.planet_radius_m;
    CHECK(validate(config).failed());
    config = {};
    config.persistence = 1.0;
    config.height_amplitude_m = config.planet_radius_m * 0.02;
    CHECK(validate(config).failed()); // 16 bands would cross the safe shell bound.
    config = {};
    config.normal_angular_step_rad = std::numeric_limits<double>::quiet_NaN();
    CHECK(validate(config).failed());

    auto generator = makeGenerator();
    CHECK(generator.height({0.0, 0.0, 0.0}, 0).failed());
    CHECK(generator.height({1.0, 0.0, 0.0}, kMaxTileLevel + 1U).failed());
    CHECK(generator.generate(TileKey{CubeFace::PositiveX, 2, 4, 0}).failed());
}

TEST_CASE("terrain content and hashes are deterministic and seed-sensitive") {
    TerrainGeneratorConfig config{};
    config.max_octaves = 8;
    auto first_generator = makeGenerator(config);
    auto second_generator = makeGenerator(config);
    TileKey const key{CubeFace::NegativeZ, 5, 13, 7};
    TerrainTile const first = makeTile(first_generator, key);
    TerrainTile const second = makeTile(second_generator, key);

    REQUIRE(first.samples.size() == kTerrainTileSampleCount);
    REQUIRE(second.samples.size() == kTerrainTileSampleCount);
    CHECK(first.content_hash == second.content_hash);
    CHECK(first.content_hash == stableTerrainContentHash(first));
    for (std::size_t i = 0; i < first.samples.size(); ++i) {
        checkSampleExactly(first.samples[i], second.samples[i]);
    }

    config.seed += 1U;
    TerrainTile const different_seed = makeTile(makeGenerator(config), key);
    CHECK(different_seed.content_hash != first.content_hash);

    TerrainTile modified = first;
    modified.samples[17].height_m += 0.25;
    CHECK(stableTerrainContentHash(modified) != first.content_hash);
}

TEST_CASE("direction-space terrain is exact across tile and cube-face seams") {
    TerrainGeneratorConfig config{};
    config.max_octaves = 7;
    TerrainGenerator const generator = makeGenerator(config);

    TerrainTile const west =
        makeTile(generator, TileKey{CubeFace::PositiveY, 4, 6, 9});
    TerrainTile const east =
        makeTile(generator, TileKey{CubeFace::PositiveY, 4, 7, 9});
    for (std::uint32_t y = 0; y < kTerrainTileSamplesPerEdge; ++y) {
        auto left = west.sample(kTerrainTileSamplesPerEdge - 1U, y);
        auto right = east.sample(0, y);
        REQUIRE(left.ok());
        REQUIRE(right.ok());
        checkSampleExactly(left.value(), right.value());
    }

    // Cube-sphere topology maps +X's west edge onto +Z's east edge in the
    // same sample order. Terrain is a function of that common direction only.
    TerrainTile const positive_x =
        makeTile(generator, TileKey{CubeFace::PositiveX, 0, 0, 0});
    TerrainTile const positive_z =
        makeTile(generator, TileKey{CubeFace::PositiveZ, 0, 0, 0});
    for (std::uint32_t y = 0; y < kTerrainTileSamplesPerEdge; ++y) {
        auto x_edge = positive_x.sample(0, y);
        auto z_edge = positive_z.sample(kTerrainTileSamplesPerEdge - 1U, y);
        REQUIRE(x_edge.ok());
        REQUIRE(z_edge.ok());
        checkSampleExactly(x_edge.value(), z_edge.value());
    }
}

TEST_CASE("child parent-compatible samples reproduce the parent's band-limited field") {
    TerrainGeneratorConfig config{};
    config.root_octaves = 2;
    config.max_octaves = 10;
    TerrainGenerator const generator = makeGenerator(config);
    TileKey const parent_key{CubeFace::NegativeX, 3, 3, 5};
    auto child_key = tileChild(parent_key, 0);
    REQUIRE(child_key.ok());
    TerrainTile const parent = makeTile(generator, parent_key);
    TerrainTile const child = makeTile(generator, child_key.value());

    CHECK(child.parent_octave_count == parent.octave_count);
    CHECK(child.octave_count == parent.octave_count + 1U);
    for (std::uint32_t parent_y = 0; parent_y <= 16U; ++parent_y) {
        for (std::uint32_t parent_x = 0; parent_x <= 16U; ++parent_x) {
            auto parent_sample = parent.sample(parent_x, parent_y);
            auto child_sample = child.sample(parent_x * 2U, parent_y * 2U);
            REQUIRE(parent_sample.ok());
            REQUIRE(child_sample.ok());
            checkVectorExactly(parent_sample.value().direction, child_sample.value().direction);
            CHECK(child_sample.value().parent_compatible_height_m ==
                  parent_sample.value().height_m);
        }
    }

    CHECK(activeTerrainOctaves(config, 0) == 2);
    CHECK(activeTerrainOctaves(config, 4) == 6);
    CHECK(activeTerrainOctaves(config, 30) == 10);
}

TEST_CASE("terrain bounds normals and parent-relative error are conservative") {
    TerrainGeneratorConfig config{};
    config.height_amplitude_m = 2'000.0;
    config.root_octaves = 3;
    config.max_octaves = 9;
    TerrainTile const tile =
        makeTile(makeGenerator(config), TileKey{CubeFace::PositiveZ, 6, 31, 17});

    double observed_min = std::numeric_limits<double>::infinity();
    double observed_max = -std::numeric_limits<double>::infinity();
    double observed_parent_delta = 0.0;
    for (TerrainSample const& sample : tile.samples) {
        observed_min = std::min(observed_min, sample.height_m);
        observed_max = std::max(observed_max, sample.height_m);
        observed_parent_delta =
            std::max(observed_parent_delta,
                     std::abs(sample.height_m - sample.parent_compatible_height_m));
        CHECK(helios::math::length(sample.direction) == doctest::Approx(1.0).epsilon(2.0e-15));
        CHECK(helios::math::length(sample.normal) == doctest::Approx(1.0).epsilon(2.0e-12));
        CHECK(helios::math::dot(sample.direction, sample.normal) > 0.0);
    }
    CHECK(tile.min_height_m == observed_min);
    CHECK(tile.max_height_m == observed_max);
    CHECK(tile.max_observed_parent_delta_m == observed_parent_delta);
    CHECK(tile.parent_relative_geometric_error_m >= observed_parent_delta);

    // At this level exactly one new octave is introduced. Value noise stays
    // in [-1,1], making its absolute amplitude a global (not sample-only) bound.
    double const introduced_amplitude =
        config.height_amplitude_m * std::pow(config.persistence, tile.parent_octave_count);
    CHECK(tile.parent_relative_geometric_error_m >= introduced_amplitude);
    CHECK(tile.parent_relative_geometric_error_m ==
          doctest::Approx(introduced_amplitude).epsilon(2.0e-15));

    CHECK(tile.sample(kTerrainTileSamplesPerEdge, 0).failed());
}
