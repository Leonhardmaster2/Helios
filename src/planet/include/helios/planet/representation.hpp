// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Projected-size representation policy. A body moves from analytic impostor
// to globe mesh to terrain quadtree through explicit hysteresis bands.
#pragma once

#include "helios/core/error.hpp"

#include <cstdint>
#include <string_view>

namespace helios::planet {

enum class BodyRepresentation : std::uint8_t {
    Impostor = 0,
    Globe = 1,
    Terrain = 2,
};

[[nodiscard]] std::string_view toString(BodyRepresentation representation) noexcept;

struct RepresentationConfig {
    double globe_to_impostor_px = 32.0;
    double impostor_to_globe_px = 48.0;
    double terrain_to_globe_px = 320.0;
    double globe_to_terrain_px = 420.0;
};

[[nodiscard]] core::Result<void> validate(RepresentationConfig const& config);

// Projected screen radius for a sphere. When the eye lies on or inside the
// sphere the result is +infinity, which intentionally selects terrain.
[[nodiscard]] core::Result<double>
projectedSphereRadiusPixels(double sphere_radius_m, double center_distance_m,
                            std::uint32_t viewport_height_px, double vertical_fov_rad);

class RepresentationSelector {
  public:
    [[nodiscard]] static core::Result<RepresentationSelector>
    create(RepresentationConfig config,
           BodyRepresentation initial = BodyRepresentation::Impostor);

    [[nodiscard]] BodyRepresentation current() const noexcept {
        return current_;
    }
    [[nodiscard]] RepresentationConfig const& config() const noexcept {
        return config_;
    }

    // A teleport can cross more than one tier in one update; hysteresis still
    // governs each boundary and prevents chatter while hovering near it.
    [[nodiscard]] core::Result<BodyRepresentation> update(double projected_radius_px);
    void reset(BodyRepresentation representation = BodyRepresentation::Impostor) noexcept;

  private:
    RepresentationSelector(RepresentationConfig config, BodyRepresentation initial)
        : config_(config), current_(initial) {}

    RepresentationConfig config_{};
    BodyRepresentation current_ = BodyRepresentation::Impostor;
};

} // namespace helios::planet
