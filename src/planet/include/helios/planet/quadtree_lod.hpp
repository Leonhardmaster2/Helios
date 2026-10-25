// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Deterministic CPU quadtree selection for the cube sphere. This layer owns
// topology and policy only: no Vulkan/Diligent types, residency, or GPU state.
#pragma once

#include "helios/core/error.hpp"
#include "helios/math/vec.hpp"
#include "helios/planet/cube_sphere.hpp"

#include <cstdint>
#include <set>
#include <vector>

namespace helios::planet {

struct QuadtreeLodConfig {
    double planet_radius_m = 6'371'000.0;

    // Conservative geometric deviation represented by a root face. Until a
    // baked error pyramid is available, each child scales this value by
    // error_scale_per_level.
    double root_geometric_error_m = 200'000.0;
    double error_scale_per_level = 0.5;

    // Split at the upper threshold; a node which was split last frame remains
    // split until it falls below the lower threshold.
    double split_threshold_px = 3.0;
    double merge_threshold_px = 2.0;

    std::uint8_t min_level = 0;
    std::uint8_t max_level = 18;
    std::uint32_t leaf_budget = 16'384;

    // Prevents division by zero while the camera touches/intersects a tile's
    // conservative surface bound.
    double minimum_distance_m = 0.25;
};

struct LodViewInput {
    math::Vec3d camera_position_body_fixed_m{};
    std::uint32_t viewport_height_px = 1080;
    double vertical_fov_rad = 1.0471975511965976; // 60 degrees
};

[[nodiscard]] core::Result<void> validate(QuadtreeLodConfig const& config);
[[nodiscard]] core::Result<void> validate(LodViewInput const& input);

[[nodiscard]] double geometricErrorMeters(QuadtreeLodConfig const& config,
                                           std::uint8_t level) noexcept;

// Perspective projection of a world-space error. This function is public so
// tools can graph the exact policy used by the selector.
[[nodiscard]] core::Result<double>
screenSpaceErrorPixels(double geometric_error_m, double distance_m,
                       std::uint32_t viewport_height_px, double vertical_fov_rad);

struct TileLodRecord {
    TileKey key{};
    math::Vec3d center_direction{};
    double conservative_distance_m = 0.0;
    double geometric_error_m = 0.0;
    double screen_space_error_px = 0.0;
};

struct LodLevelMetrics {
    std::uint8_t level = 0;
    std::uint32_t evaluated_count = 0;
    std::uint32_t leaf_count = 0;
    std::uint32_t split_node_count = 0;
    double min_leaf_sse_px = 0.0;
    double max_leaf_sse_px = 0.0;
    double mean_leaf_sse_px = 0.0;
    double max_evaluated_sse_px = 0.0;
};

struct LodSelection {
    // Sorted by TileKey, independent of allocation and hash-container order.
    std::vector<TileLodRecord> leaves;
    std::vector<LodLevelMetrics> levels;

    std::uint32_t leaf_budget = 0;
    std::uint32_t active_split_node_count = 0;
    std::uint32_t splits_from_previous = 0;
    std::uint32_t merges_from_previous = 0;
    double max_leaf_sse_px = 0.0;
    bool budget_limited = false;
};

class QuadtreeLodSelector {
  public:
    [[nodiscard]] static core::Result<QuadtreeLodSelector> create(QuadtreeLodConfig config);

    [[nodiscard]] QuadtreeLodConfig const& config() const noexcept {
        return config_;
    }

    // Useful for an interactive LOD lab. By default a policy change clears
    // hysteresis so before/after results are easy to attribute.
    [[nodiscard]] core::Result<void> setConfig(QuadtreeLodConfig config,
                                                bool preserve_hysteresis = false);

    [[nodiscard]] core::Result<LodSelection> select(LodViewInput const& input);
    void reset() noexcept;

  private:
    explicit QuadtreeLodSelector(QuadtreeLodConfig config) : config_(config) {}

    QuadtreeLodConfig config_{};
    std::set<TileKey> previous_split_nodes_;
};

} // namespace helios::planet
