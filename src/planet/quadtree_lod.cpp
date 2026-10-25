// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "helios/planet/quadtree_lod.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>
#include <queue>
#include <string>
#include <utility>

namespace helios::planet {
namespace {

[[nodiscard]] bool finitePositive(double value) noexcept {
    return std::isfinite(value) && value > 0.0;
}

[[nodiscard]] bool allFinite(math::Vec3d const& value) noexcept {
    return std::isfinite(value.v[0]) && std::isfinite(value.v[1]) && std::isfinite(value.v[2]);
}

[[nodiscard]] core::Error invalid(std::string message) {
    return core::makeError(core::ErrorCode::InvalidArgument, std::move(message));
}

[[nodiscard]] double rawScreenSpaceError(double geometric_error_m, double distance_m,
                                         std::uint32_t viewport_height_px,
                                         double vertical_fov_rad) noexcept {
    double const focal_length_px =
        static_cast<double>(viewport_height_px) / (2.0 * std::tan(vertical_fov_rad * 0.5));
    return geometric_error_m * focal_length_px / distance_m;
}

[[nodiscard]] core::Result<TileLodRecord> evaluateTile(QuadtreeLodConfig const& config,
                                                        LodViewInput const& input,
                                                        TileKey const& key) {
    auto center_result = tileCenterDirection(key);
    if (center_result.failed()) {
        return center_result.error();
    }
    auto corners_result = tileCornerDirections(key);
    if (corners_result.failed()) {
        return corners_result.error();
    }

    math::Vec3d const center_direction = center_result.value();
    math::Vec3d const center_position = center_direction * config.planet_radius_m;
    double surface_bound_radius_m = 0.0;
    for (math::Vec3d const& corner : corners_result.value()) {
        double const chord_m = math::length((corner - center_direction) * config.planet_radius_m);
        surface_bound_radius_m = std::max(surface_bound_radius_m, chord_m);
    }

    double const center_distance_m =
        math::length(input.camera_position_body_fixed_m - center_position);
    double const conservative_distance_m =
        std::max(config.minimum_distance_m, center_distance_m - surface_bound_radius_m);
    double const geometric_error_m = geometricErrorMeters(config, key.level);
    double const sse_px = rawScreenSpaceError(geometric_error_m, conservative_distance_m,
                                              input.viewport_height_px, input.vertical_fov_rad);
    return TileLodRecord{key, center_direction, conservative_distance_m, geometric_error_m, sse_px};
}

struct Candidate {
    TileLodRecord record{};
    bool forced = false;
};

struct CandidateLowerPriority {
    [[nodiscard]] bool operator()(Candidate const& left, Candidate const& right) const noexcept {
        if (left.forced != right.forced) {
            return !left.forced;
        }
        if (left.record.screen_space_error_px != right.record.screen_space_error_px) {
            return left.record.screen_space_error_px < right.record.screen_space_error_px;
        }
        // std::priority_queue puts the element considered greatest at top;
        // reverse the key comparison so the lower stable key wins an SSE tie.
        return right.record.key < left.record.key;
    }
};

struct LevelAccumulator {
    LodLevelMetrics public_metrics{};
    double leaf_sse_sum = 0.0;
    bool has_leaf = false;
};

} // namespace

core::Result<void> validate(QuadtreeLodConfig const& config) {
    if (!finitePositive(config.planet_radius_m)) {
        return invalid("planet radius must be finite and positive");
    }
    if (!finitePositive(config.root_geometric_error_m)) {
        return invalid("root geometric error must be finite and positive");
    }
    if (!finitePositive(config.error_scale_per_level) || config.error_scale_per_level > 1.0) {
        return invalid("LOD error scale per level must be finite and in (0,1]");
    }
    if (!std::isfinite(config.merge_threshold_px) || !std::isfinite(config.split_threshold_px) ||
        config.merge_threshold_px < 0.0 ||
        config.split_threshold_px <= config.merge_threshold_px) {
        return invalid("LOD thresholds require 0 <= merge < split");
    }
    if (config.max_level > kMaxTileLevel || config.min_level > config.max_level) {
        return invalid("LOD levels require min <= max <= the cube-sphere level limit");
    }
    if (config.leaf_budget < kCubeFaceCount) {
        return invalid("LOD leaf budget must hold all six cube-face roots");
    }
    if (!finitePositive(config.minimum_distance_m)) {
        return invalid("LOD minimum distance must be finite and positive");
    }

    std::uint64_t required_leaves = kCubeFaceCount;
    for (std::uint8_t level = 0; level < config.min_level; ++level) {
        if (required_leaves > static_cast<std::uint64_t>(config.leaf_budget) / 4U) {
            return invalid("LOD leaf budget cannot satisfy the configured minimum level");
        }
        required_leaves *= 4U;
    }
    if (required_leaves > config.leaf_budget) {
        return invalid("LOD leaf budget cannot satisfy the configured minimum level");
    }
    return {};
}

core::Result<void> validate(LodViewInput const& input) {
    if (!allFinite(input.camera_position_body_fixed_m)) {
        return invalid("LOD camera position must contain finite values");
    }
    if (input.viewport_height_px == 0) {
        return invalid("LOD viewport height must be non-zero");
    }
    if (!std::isfinite(input.vertical_fov_rad) || input.vertical_fov_rad <= 0.0 ||
        input.vertical_fov_rad >= std::numbers::pi) {
        return invalid("LOD vertical field of view must be finite and in (0,pi)");
    }
    return {};
}

double geometricErrorMeters(QuadtreeLodConfig const& config, std::uint8_t level) noexcept {
    return config.root_geometric_error_m *
           std::pow(config.error_scale_per_level, static_cast<double>(level));
}

core::Result<double> screenSpaceErrorPixels(double geometric_error_m, double distance_m,
                                             std::uint32_t viewport_height_px,
                                             double vertical_fov_rad) {
    if (!std::isfinite(geometric_error_m) || geometric_error_m < 0.0) {
        return invalid("geometric error must be finite and non-negative");
    }
    if (!finitePositive(distance_m)) {
        return invalid("SSE distance must be finite and positive");
    }
    if (viewport_height_px == 0) {
        return invalid("SSE viewport height must be non-zero");
    }
    if (!std::isfinite(vertical_fov_rad) || vertical_fov_rad <= 0.0 ||
        vertical_fov_rad >= std::numbers::pi) {
        return invalid("SSE vertical field of view must be finite and in (0,pi)");
    }
    return rawScreenSpaceError(geometric_error_m, distance_m, viewport_height_px,
                               vertical_fov_rad);
}

core::Result<QuadtreeLodSelector> QuadtreeLodSelector::create(QuadtreeLodConfig config) {
    auto validity = validate(config);
    if (validity.failed()) {
        return validity.error();
    }
    return QuadtreeLodSelector(config);
}

core::Result<void> QuadtreeLodSelector::setConfig(QuadtreeLodConfig config,
                                                   bool preserve_hysteresis) {
    auto validity = validate(config);
    if (validity.failed()) {
        return validity.error();
    }
    config_ = config;
    if (!preserve_hysteresis) {
        reset();
    }
    return {};
}

void QuadtreeLodSelector::reset() noexcept {
    previous_split_nodes_.clear();
}

core::Result<LodSelection> QuadtreeLodSelector::select(LodViewInput const& input) {
    auto input_validity = validate(input);
    if (input_validity.failed()) {
        return input_validity.error();
    }

    std::vector<LevelAccumulator> accumulators(static_cast<std::size_t>(config_.max_level) + 1U);
    for (std::size_t level = 0; level < accumulators.size(); ++level) {
        accumulators[level].public_metrics.level = static_cast<std::uint8_t>(level);
    }

    std::map<TileKey, TileLodRecord> leaves;
    std::priority_queue<Candidate, std::vector<Candidate>, CandidateLowerPriority> candidates;

    auto evaluate_and_add = [&](TileKey const& key) -> core::Result<void> {
        auto record = evaluateTile(config_, input, key);
        if (record.failed()) {
            return record.error();
        }
        auto& metrics = accumulators[key.level].public_metrics;
        ++metrics.evaluated_count;
        metrics.max_evaluated_sse_px =
            std::max(metrics.max_evaluated_sse_px, record.value().screen_space_error_px);
        leaves.emplace(key, record.value());
        candidates.push(Candidate{record.value(), key.level < config_.min_level});
        return {};
    };

    constexpr std::array<CubeFace, kCubeFaceCount> kFaces{
        CubeFace::PositiveX, CubeFace::NegativeX, CubeFace::PositiveY,
        CubeFace::NegativeY, CubeFace::PositiveZ, CubeFace::NegativeZ,
    };
    for (CubeFace face : kFaces) {
        auto added = evaluate_and_add(TileKey{face, 0, 0, 0});
        if (added.failed()) {
            return added.error();
        }
    }

    bool budget_limited = false;
    std::set<TileKey> current_split_nodes;
    while (!candidates.empty()) {
        Candidate const candidate = candidates.top();
        candidates.pop();

        auto const leaf = leaves.find(candidate.record.key);
        if (leaf == leaves.end()) {
            continue;
        }
        TileKey const& key = candidate.record.key;
        if (key.level >= config_.max_level) {
            continue;
        }

        bool const was_split = previous_split_nodes_.contains(key);
        double const threshold =
            was_split ? config_.merge_threshold_px : config_.split_threshold_px;
        bool const wants_split = key.level < config_.min_level ||
                                 candidate.record.screen_space_error_px > threshold;
        if (!wants_split) {
            continue;
        }
        if (leaves.size() + 3U > config_.leaf_budget) {
            budget_limited = true;
            break;
        }

        auto children = tileChildren(key);
        if (children.failed()) {
            return children.error();
        }
        leaves.erase(leaf);
        current_split_nodes.insert(key);
        ++accumulators[key.level].public_metrics.split_node_count;
        for (TileKey const& child : children.value()) {
            auto added = evaluate_and_add(child);
            if (added.failed()) {
                return added.error();
            }
        }
    }

    LodSelection selection{};
    selection.leaf_budget = config_.leaf_budget;
    selection.budget_limited = budget_limited;
    selection.active_split_node_count = static_cast<std::uint32_t>(current_split_nodes.size());

    for (TileKey const& key : current_split_nodes) {
        if (!previous_split_nodes_.contains(key)) {
            ++selection.splits_from_previous;
        }
    }
    for (TileKey const& key : previous_split_nodes_) {
        if (!current_split_nodes.contains(key)) {
            ++selection.merges_from_previous;
        }
    }

    selection.leaves.reserve(leaves.size());
    for (auto const& [key, record] : leaves) {
        selection.leaves.push_back(record);
        auto& accumulator = accumulators[key.level];
        auto& metrics = accumulator.public_metrics;
        ++metrics.leaf_count;
        accumulator.leaf_sse_sum += record.screen_space_error_px;
        if (!accumulator.has_leaf) {
            metrics.min_leaf_sse_px = record.screen_space_error_px;
            accumulator.has_leaf = true;
        } else {
            metrics.min_leaf_sse_px =
                std::min(metrics.min_leaf_sse_px, record.screen_space_error_px);
        }
        metrics.max_leaf_sse_px = std::max(metrics.max_leaf_sse_px, record.screen_space_error_px);
        selection.max_leaf_sse_px =
            std::max(selection.max_leaf_sse_px, record.screen_space_error_px);
    }

    selection.levels.reserve(accumulators.size());
    for (LevelAccumulator& accumulator : accumulators) {
        if (accumulator.public_metrics.leaf_count > 0) {
            accumulator.public_metrics.mean_leaf_sse_px =
                accumulator.leaf_sse_sum /
                static_cast<double>(accumulator.public_metrics.leaf_count);
        }
        selection.levels.push_back(accumulator.public_metrics);
    }

    previous_split_nodes_ = std::move(current_split_nodes);
    return selection;
}

} // namespace helios::planet
