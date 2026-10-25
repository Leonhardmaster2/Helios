// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "helios/planet/representation.hpp"

#include <cmath>
#include <limits>
#include <numbers>
#include <string>
#include <utility>

namespace helios::planet {
namespace {

[[nodiscard]] core::Error invalid(std::string message) {
    return core::makeError(core::ErrorCode::InvalidArgument, std::move(message));
}

[[nodiscard]] bool validRepresentation(BodyRepresentation representation) noexcept {
    return static_cast<std::uint8_t>(representation) <=
           static_cast<std::uint8_t>(BodyRepresentation::Terrain);
}

} // namespace

std::string_view toString(BodyRepresentation representation) noexcept {
    switch (representation) {
    case BodyRepresentation::Impostor:
        return "impostor";
    case BodyRepresentation::Globe:
        return "globe";
    case BodyRepresentation::Terrain:
        return "terrain";
    }
    return "invalid";
}

core::Result<void> validate(RepresentationConfig const& config) {
    bool const finite = std::isfinite(config.globe_to_impostor_px) &&
                        std::isfinite(config.impostor_to_globe_px) &&
                        std::isfinite(config.terrain_to_globe_px) &&
                        std::isfinite(config.globe_to_terrain_px);
    if (!finite || config.globe_to_impostor_px < 0.0 ||
        config.globe_to_impostor_px >= config.impostor_to_globe_px ||
        config.impostor_to_globe_px >= config.terrain_to_globe_px ||
        config.terrain_to_globe_px >= config.globe_to_terrain_px) {
        return invalid("representation thresholds must be finite and ordered as "
                       "0 <= globe-exit < globe-enter < terrain-exit < terrain-enter");
    }
    return {};
}

core::Result<double> projectedSphereRadiusPixels(double sphere_radius_m,
                                                  double center_distance_m,
                                                  std::uint32_t viewport_height_px,
                                                  double vertical_fov_rad) {
    if (!std::isfinite(sphere_radius_m) || sphere_radius_m <= 0.0) {
        return invalid("sphere radius must be finite and positive");
    }
    if (!std::isfinite(center_distance_m) || center_distance_m < 0.0) {
        return invalid("sphere center distance must be finite and non-negative");
    }
    if (viewport_height_px == 0) {
        return invalid("projection viewport height must be non-zero");
    }
    if (!std::isfinite(vertical_fov_rad) || vertical_fov_rad <= 0.0 ||
        vertical_fov_rad >= std::numbers::pi) {
        return invalid("projection vertical field of view must be finite and in (0,pi)");
    }
    if (center_distance_m <= sphere_radius_m) {
        return std::numeric_limits<double>::infinity();
    }

    double const focal_length_px =
        static_cast<double>(viewport_height_px) / (2.0 * std::tan(vertical_fov_rad * 0.5));
    double const tangent_distance_m =
        std::sqrt(center_distance_m * center_distance_m - sphere_radius_m * sphere_radius_m);
    return focal_length_px * sphere_radius_m / tangent_distance_m;
}

core::Result<RepresentationSelector>
RepresentationSelector::create(RepresentationConfig config, BodyRepresentation initial) {
    auto validity = validate(config);
    if (validity.failed()) {
        return validity.error();
    }
    if (!validRepresentation(initial)) {
        return invalid("initial body representation is invalid");
    }
    return RepresentationSelector(config, initial);
}

core::Result<BodyRepresentation>
RepresentationSelector::update(double projected_radius_px) {
    if (std::isnan(projected_radius_px) || projected_radius_px < 0.0) {
        return invalid("projected body radius must be non-negative and not NaN");
    }

    switch (current_) {
    case BodyRepresentation::Impostor:
        if (projected_radius_px >= config_.globe_to_terrain_px) {
            current_ = BodyRepresentation::Terrain;
        } else if (projected_radius_px >= config_.impostor_to_globe_px) {
            current_ = BodyRepresentation::Globe;
        }
        break;
    case BodyRepresentation::Globe:
        if (projected_radius_px >= config_.globe_to_terrain_px) {
            current_ = BodyRepresentation::Terrain;
        } else if (projected_radius_px < config_.globe_to_impostor_px) {
            current_ = BodyRepresentation::Impostor;
        }
        break;
    case BodyRepresentation::Terrain:
        if (projected_radius_px < config_.globe_to_impostor_px) {
            current_ = BodyRepresentation::Impostor;
        } else if (projected_radius_px < config_.terrain_to_globe_px) {
            current_ = BodyRepresentation::Globe;
        }
        break;
    }
    return current_;
}

void RepresentationSelector::reset(BodyRepresentation representation) noexcept {
    if (validRepresentation(representation)) {
        current_ = representation;
    }
}

} // namespace helios::planet
