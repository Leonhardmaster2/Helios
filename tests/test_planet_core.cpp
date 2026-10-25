// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "helios/planet/cube_sphere.hpp"
#include "helios/planet/quadtree_lod.hpp"
#include "helios/planet/representation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <set>
#include <vector>

namespace {

using helios::math::Vec3d;
using namespace helios::planet;

constexpr std::array<CubeFace, kCubeFaceCount> kFaces{
    CubeFace::PositiveX, CubeFace::NegativeX, CubeFace::PositiveY,
    CubeFace::NegativeY, CubeFace::PositiveZ, CubeFace::NegativeZ,
};

void checkSame(Vec3d const& left, Vec3d const& right, double epsilon = 0.0) {
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (epsilon == 0.0) {
            CHECK(left.v[axis] == right.v[axis]);
        } else {
            CHECK(left.v[axis] == doctest::Approx(right.v[axis]).epsilon(epsilon));
        }
    }
}

Vec3d direction(TileKey const& key, std::uint32_t x, std::uint32_t y,
                std::uint32_t patch_samples = kDefaultPatchSamples) {
    auto result = cubeSphereDirection(key, x, y, patch_samples);
    REQUIRE(result.ok());
    return result.value();
}

QuadtreeLodSelector selector(QuadtreeLodConfig config) {
    auto result = QuadtreeLodSelector::create(config);
    REQUIRE(result.ok());
    return std::move(result.value());
}

} // namespace

TEST_CASE("cube-sphere tile keys have stable topology identity") {
    TileKey const parent{CubeFace::PositiveZ, 4, 5, 9};
    REQUIRE(isValid(parent));
    CHECK(toString(parent) == "+Z/L4/5/9");
    CHECK(stableHash64(parent) == 6472238166624005417ULL);
    CHECK(stableHash64(parent) == stableHash64(TileKey{CubeFace::PositiveZ, 4, 5, 9}));
    CHECK(stableHash64(parent) != stableHash64(TileKey{CubeFace::PositiveZ, 4, 5, 8}));
    CHECK(TileKeyHash{}(parent) == std::hash<TileKey>{}(parent));

    auto children = tileChildren(parent);
    REQUIRE(children.ok());
    CHECK(children.value()[0] == TileKey{CubeFace::PositiveZ, 5, 10, 18});
    CHECK(children.value()[1] == TileKey{CubeFace::PositiveZ, 5, 11, 18});
    CHECK(children.value()[2] == TileKey{CubeFace::PositiveZ, 5, 10, 19});
    CHECK(children.value()[3] == TileKey{CubeFace::PositiveZ, 5, 11, 19});
    for (TileKey const& child : children.value()) {
        auto recovered_parent = tileParent(child);
        REQUIRE(recovered_parent.ok());
        CHECK(recovered_parent.value() == parent);
    }

    std::vector<TileKey> shuffled{children.value()[3], children.value()[0], children.value()[2],
                                  children.value()[1]};
    std::sort(shuffled.begin(), shuffled.end());
    CHECK(shuffled[0] == children.value()[0]);
    CHECK(shuffled[1] == children.value()[2]);
    CHECK(shuffled[2] == children.value()[1]);
    CHECK(shuffled[3] == children.value()[3]);

    CHECK_FALSE(isValid(TileKey{CubeFace::PositiveX, 3, 8, 0}));
    CHECK(tileParent(TileKey{}).failed());
    CHECK(tileChild(parent, 4).failed());
}

TEST_CASE("cube-sphere samples are unit vectors on every face and level") {
    constexpr std::array<std::uint32_t, 3> kSampleIndices{0, 16, 32};
    for (CubeFace face : kFaces) {
        for (std::uint8_t level : {std::uint8_t{0}, std::uint8_t{3}, std::uint8_t{12}}) {
            std::uint32_t const last = (std::uint32_t{1} << level) - 1U;
            TileKey const key{face, level, last / 3U, last / 2U};
            for (std::uint32_t y : kSampleIndices) {
                for (std::uint32_t x : kSampleIndices) {
                    Vec3d const sample = direction(key, x, y);
                    CHECK(helios::math::length(sample) == doctest::Approx(1.0).epsilon(2.0e-15));
                }
            }
        }
    }
}

TEST_CASE("canonical cube-face seams and corners agree") {
    TileKey const positive_x{CubeFace::PositiveX, 0, 0, 0};
    TileKey const positive_z{CubeFace::PositiveZ, 0, 0, 0};
    TileKey const negative_z{CubeFace::NegativeZ, 0, 0, 0};
    TileKey const positive_y{CubeFace::PositiveY, 0, 0, 0};
    TileKey const negative_y{CubeFace::NegativeY, 0, 0, 0};

    for (std::uint32_t sample = 0; sample < kDefaultPatchSamples; ++sample) {
        checkSame(direction(positive_x, 0, sample), direction(positive_z, 32, sample));
        checkSame(direction(positive_x, 32, sample), direction(negative_z, 0, sample));
        checkSame(direction(positive_x, sample, 32), direction(positive_y, 32, sample));
        checkSame(direction(positive_x, sample, 0), direction(negative_y, 32, 32 - sample));
    }

    // The 24 face corners collapse to exactly eight geometric directions.
    std::vector<Vec3d> unique_corners;
    for (CubeFace face : kFaces) {
        TileKey const root{face, 0, 0, 0};
        for (std::uint32_t y : {0U, 32U}) {
            for (std::uint32_t x : {0U, 32U}) {
                Vec3d const corner = direction(root, x, y);
                bool const already_present = std::any_of(
                    unique_corners.begin(), unique_corners.end(), [&](Vec3d const& existing) {
                        return helios::math::dot(corner, existing) > 1.0 - 1.0e-15;
                    });
                if (!already_present) {
                    unique_corners.push_back(corner);
                }
            }
        }
    }
    CHECK(unique_corners.size() == 8);
}

TEST_CASE("even child samples are exactly the corresponding parent samples") {
    for (CubeFace face : kFaces) {
        TileKey const parent{face, 3, 3, 5};
        auto children = tileChildren(parent);
        REQUIRE(children.ok());
        for (std::uint32_t parent_y = 0; parent_y < kDefaultPatchSamples; ++parent_y) {
            for (std::uint32_t parent_x = 0; parent_x < kDefaultPatchSamples; ++parent_x) {
                std::uint32_t const child_column = parent_x > 16U ? 1U : 0U;
                std::uint32_t const child_row = parent_y > 16U ? 1U : 0U;
                std::uint32_t const child_index = child_column + child_row * 2U;
                std::uint32_t const child_x =
                    parent_x <= 16U ? parent_x * 2U : (parent_x - 16U) * 2U;
                std::uint32_t const child_y =
                    parent_y <= 16U ? parent_y * 2U : (parent_y - 16U) * 2U;
                checkSame(direction(parent, parent_x, parent_y),
                          direction(children.value()[child_index], child_x, child_y));
            }
        }
    }
}

TEST_CASE("screen-space error is monotonic in distance, detail level, and viewport") {
    auto near_error = screenSpaceErrorPixels(10.0, 100.0, 1000, 1.0);
    auto far_error = screenSpaceErrorPixels(10.0, 200.0, 1000, 1.0);
    auto large_view_error = screenSpaceErrorPixels(10.0, 100.0, 2000, 1.0);
    REQUIRE(near_error.ok());
    REQUIRE(far_error.ok());
    REQUIRE(large_view_error.ok());
    CHECK(near_error.value() > far_error.value());
    CHECK(large_view_error.value() == doctest::Approx(near_error.value() * 2.0));

    QuadtreeLodConfig config{};
    config.root_geometric_error_m = 128.0;
    config.error_scale_per_level = 0.5;
    CHECK(geometricErrorMeters(config, 1) < geometricErrorMeters(config, 0));
    CHECK(geometricErrorMeters(config, 8) < geometricErrorMeters(config, 7));
    CHECK(screenSpaceErrorPixels(-1.0, 1.0, 1, 1.0).failed());
    CHECK(screenSpaceErrorPixels(1.0, 0.0, 1, 1.0).failed());
}

TEST_CASE("quadtree split and merge thresholds provide frame hysteresis") {
    QuadtreeLodConfig config{};
    config.planet_radius_m = 1.0;
    config.root_geometric_error_m = 0.2;
    config.split_threshold_px = 1.0;
    config.merge_threshold_px = 0.5;
    config.max_level = 3;
    config.leaf_budget = 500;
    config.minimum_distance_m = 0.01;

    auto persistent = selector(config);
    LodViewInput view{{20.0, 0.0, 0.0}, 1000, 1.5707963267948966};
    auto near_selection = persistent.select(view);
    REQUIRE(near_selection.ok());
    CHECK(near_selection.value().leaves.size() > 24);

    // All root errors now sit below the split threshold but above the merge
    // threshold. A fresh selector remains coarse; the persistent one keeps
    // its root splits and merges its now-unnecessary descendants.
    view.camera_position_body_fixed_m = {125.0, 0.0, 0.0};
    auto hysteretic = persistent.select(view);
    REQUIRE(hysteretic.ok());
    auto fresh = selector(config).select(view);
    REQUIRE(fresh.ok());
    CHECK(hysteretic.value().leaves.size() == 24);
    CHECK(fresh.value().leaves.size() == 6);
    CHECK(hysteretic.value().merges_from_previous > 0);

    view.camera_position_body_fixed_m = {500.0, 0.0, 0.0};
    auto merged = persistent.select(view);
    REQUIRE(merged.ok());
    CHECK(merged.value().leaves.size() == 6);
    CHECK(merged.value().merges_from_previous == 6);
}

TEST_CASE("quadtree leaf budget and tie breaking are hard and deterministic") {
    QuadtreeLodConfig config{};
    config.planet_radius_m = 1.0;
    config.root_geometric_error_m = 1.0e6;
    config.split_threshold_px = 1.0;
    config.merge_threshold_px = 0.5;
    config.max_level = 8;
    config.leaf_budget = 33;
    config.minimum_distance_m = 0.01;
    LodViewInput const view{{10.0, 0.0, 0.0}, 1000, 1.0};

    auto first = selector(config).select(view);
    auto second = selector(config).select(view);
    REQUIRE(first.ok());
    REQUIRE(second.ok());
    REQUIRE(first.value().leaves.size() == 33);
    REQUIRE(second.value().leaves.size() == 33);
    CHECK(first.value().budget_limited);
    CHECK(second.value().budget_limited);
    for (std::size_t i = 0; i < first.value().leaves.size(); ++i) {
        CHECK(first.value().leaves[i].key == second.value().leaves[i].key);
        if (i > 0) {
            CHECK(first.value().leaves[i - 1].key < first.value().leaves[i].key);
        }
    }
}

TEST_CASE("quadtree max level and minimum-level budget are enforced") {
    QuadtreeLodConfig config{};
    config.planet_radius_m = 1.0;
    config.root_geometric_error_m = 1.0e9;
    config.max_level = 2;
    config.leaf_budget = 1000;
    config.minimum_distance_m = 0.01;
    auto selection = selector(config).select({{5.0, 0.0, 0.0}, 1000, 1.0});
    REQUIRE(selection.ok());
    CHECK(selection.value().leaves.size() == 96);
    for (TileLodRecord const& leaf : selection.value().leaves) {
        CHECK(leaf.key.level == 2);
    }

    config.min_level = 2;
    config.leaf_budget = 95;
    CHECK(validate(config).failed());
}

TEST_CASE("body representation transitions use independent enter and exit thresholds") {
    RepresentationConfig config{};
    auto created = RepresentationSelector::create(config);
    REQUIRE(created.ok());
    auto selector = std::move(created.value());

    REQUIRE(selector.update(40.0).ok());
    CHECK(static_cast<std::uint8_t>(selector.current()) ==
          static_cast<std::uint8_t>(BodyRepresentation::Impostor));
    REQUIRE(selector.update(50.0).ok());
    CHECK(static_cast<std::uint8_t>(selector.current()) ==
          static_cast<std::uint8_t>(BodyRepresentation::Globe));
    REQUIRE(selector.update(40.0).ok());
    CHECK(static_cast<std::uint8_t>(selector.current()) ==
          static_cast<std::uint8_t>(BodyRepresentation::Globe));
    REQUIRE(selector.update(31.0).ok());
    CHECK(static_cast<std::uint8_t>(selector.current()) ==
          static_cast<std::uint8_t>(BodyRepresentation::Impostor));

    REQUIRE(selector.update(500.0).ok());
    CHECK(static_cast<std::uint8_t>(selector.current()) ==
          static_cast<std::uint8_t>(BodyRepresentation::Terrain));
    REQUIRE(selector.update(400.0).ok());
    CHECK(static_cast<std::uint8_t>(selector.current()) ==
          static_cast<std::uint8_t>(BodyRepresentation::Terrain));
    REQUIRE(selector.update(300.0).ok());
    CHECK(static_cast<std::uint8_t>(selector.current()) ==
          static_cast<std::uint8_t>(BodyRepresentation::Globe));

    auto far_projection = projectedSphereRadiusPixels(1.0, 100.0, 1000, 1.0);
    auto near_projection = projectedSphereRadiusPixels(1.0, 2.0, 1000, 1.0);
    auto inside = projectedSphereRadiusPixels(1.0, 0.5, 1000, 1.0);
    REQUIRE(far_projection.ok());
    REQUIRE(near_projection.ok());
    REQUIRE(inside.ok());
    CHECK(near_projection.value() > far_projection.value());
    CHECK(std::isinf(inside.value()));
    CHECK(selector.update(std::numeric_limits<double>::quiet_NaN()).failed());
}
