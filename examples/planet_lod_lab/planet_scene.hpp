// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// The scene of the planet LOD lab, shared by the lab itself and by the
// deep-sky lab's object menu: where the camera is, the scripted descent from
// astronomical distance to the surface, and the view handed to the patch
// renderer.
#pragma once

#include "planet_patch_renderer.hpp"

#include "helios/math/vec.hpp"
#include "helios/planet/quadtree_lod.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>

namespace helios::examples {

struct PlanetCamera {
    double altitude_agl_m = 993'629'000.0;
    double latitude_deg = 18.0;
    double longitude_deg = -24.0;
    double look_down_deg = 90.0;
};

struct PlanetRouteWaypoint {
    char const* label;
    double segment_seconds;
    double altitude_agl_m;
    double latitude_deg;
    double longitude_deg;
    double look_down_deg;
};

// The first three AGL values are chosen so an Earth-radius body is observed
// from approximately 1e9, 1e8, and 2e7 metres body-centre distance.
inline constexpr std::array<PlanetRouteWaypoint, 10> kPlanetDescentRoute{{
    {"Astronomical scale (1e9 m)", 0.0, 993'629'000.0, 18.0, -24.0, 90.0},
    {"Deep-space approach (1e8 m)", 5.0, 93'629'000.0, 18.4, -22.0, 90.0},
    {"Whole-planet approach (2e7 m)", 5.0, 13'629'000.0, 18.8, -20.0, 90.0},
    {"1000 km orbit", 5.0, 1'000'000.0, 19.4, -16.0, 86.0},
    {"100 km approach", 5.0, 100'000.0, 20.2, -11.0, 72.0},
    {"10 km regional terrain", 5.0, 10'000.0, 21.1, -5.0, 48.0},
    {"1 km local terrain", 5.0, 1'000.0, 22.1, 2.0, 28.0},
    {"100 m final approach", 5.0, 100.0, 23.0, 9.0, 16.0},
    {"10 m ground detail", 5.0, 10.0, 23.7, 15.0, 9.0},
    {"2 m surface eye level", 5.0, 2.0, 24.2, 18.0, 5.0},
}};

class PlanetDescentRoute {
  public:
    bool playing = true;
    double playback_speed = 1.0;

    void restart(PlanetCamera& camera) {
        elapsed_seconds_ = 0.0;
        playing = true;
        sample(camera);
    }

    void advance(double seconds, PlanetCamera& camera) {
        if (!playing) {
            return;
        }
        elapsed_seconds_ = std::min(totalSeconds(),
                                    elapsed_seconds_ + seconds * playback_speed);
        sample(camera);
        if (elapsed_seconds_ >= totalSeconds()) {
            playing = false;
        }
    }

    [[nodiscard]] double progress() const noexcept {
        return totalSeconds() > 0.0 ? elapsed_seconds_ / totalSeconds() : 1.0;
    }

    [[nodiscard]] char const* stageLabel() const noexcept {
        double cursor = 0.0;
        for (std::size_t i = 1; i < kPlanetDescentRoute.size(); ++i) {
            cursor += kPlanetDescentRoute[i].segment_seconds;
            if (elapsed_seconds_ < cursor) {
                return kPlanetDescentRoute[i].label;
            }
        }
        return kPlanetDescentRoute.back().label;
    }

  private:
    [[nodiscard]] static double smoothStep(double value) noexcept {
        double const t = std::clamp(value, 0.0, 1.0);
        return t * t * (3.0 - 2.0 * t);
    }

    [[nodiscard]] static double interpolateLog(double first, double second, double alpha) {
        return std::exp(std::log(first) + (std::log(second) - std::log(first)) * alpha);
    }

    [[nodiscard]] static double totalSeconds() noexcept {
        double result = 0.0;
        for (std::size_t i = 1; i < kPlanetDescentRoute.size(); ++i) {
            result += kPlanetDescentRoute[i].segment_seconds;
        }
        return result;
    }

    void sample(PlanetCamera& camera) const {
        double segment_start = 0.0;
        for (std::size_t i = 1; i < kPlanetDescentRoute.size(); ++i) {
            PlanetRouteWaypoint const& previous = kPlanetDescentRoute[i - 1U];
            PlanetRouteWaypoint const& current = kPlanetDescentRoute[i];
            double const segment_end = segment_start + current.segment_seconds;
            if (elapsed_seconds_ <= segment_end) {
                double const raw_alpha = current.segment_seconds > 0.0
                                             ? (elapsed_seconds_ - segment_start) /
                                                   current.segment_seconds
                                             : 1.0;
                double const alpha = smoothStep(raw_alpha);
                camera.altitude_agl_m = interpolateLog(previous.altitude_agl_m,
                                                       current.altitude_agl_m, alpha);
                camera.latitude_deg = previous.latitude_deg +
                                      (current.latitude_deg - previous.latitude_deg) * alpha;
                camera.longitude_deg = previous.longitude_deg +
                                       (current.longitude_deg - previous.longitude_deg) * alpha;
                camera.look_down_deg = previous.look_down_deg +
                                       (current.look_down_deg - previous.look_down_deg) * alpha;
                return;
            }
            segment_start = segment_end;
        }
        PlanetRouteWaypoint const& final = kPlanetDescentRoute.back();
        camera = {final.altitude_agl_m, final.latitude_deg, final.longitude_deg,
                  final.look_down_deg};
    }

    double elapsed_seconds_ = 0.0;
};

[[nodiscard]] inline PlanetPatchView makePlanetView(PlanetCamera const& camera,
                                                    PlanetPatchSettings const& patches,
                                                    double vertical_fov_deg,
                                                    std::uint32_t viewport_width,
                                                    std::uint32_t viewport_height) {
    constexpr double kRadians = std::numbers::pi / 180.0;
    double const latitude = camera.latitude_deg * kRadians;
    double const longitude = camera.longitude_deg * kRadians;
    double const cos_latitude = std::cos(latitude);
    math::Vec3d const radial{cos_latitude * std::cos(longitude),
                             cos_latitude * std::sin(longitude), std::sin(latitude)};
    math::Vec3d const east{-std::sin(longitude), std::cos(longitude), 0.0};
    math::Vec3d const north = math::normalized(math::cross(radial, east));
    double const look_down = camera.look_down_deg * kRadians;
    math::Vec3d const forward =
        math::normalized(north * std::cos(look_down) - radial * std::sin(look_down));
    math::Vec3d const up = math::normalized(math::cross(east, forward));
    // The ground under the camera: the same height function the GPU draws,
    // with every octave (or the sea surface, where the ground is under water).
    double terrain_height = planetHeight(patches.body, radial, patches.body.max_octaves);
    if (patches.body.ocean) {
        terrain_height = std::max(terrain_height, patches.body.sea_level_m);
    }
    math::Vec3d const position =
        radial * (patches.planet_radius_m + terrain_height + camera.altitude_agl_m);

    PlanetPatchView result;
    result.camera_body_fixed_m = position;
    result.forward_body_fixed = forward;
    result.right_body_fixed = east;
    result.up_body_fixed = up;
    result.vertical_fov_rad = vertical_fov_deg * kRadians;
    result.near_plane_m = 0.25;
    result.viewport_width_px = viewport_width;
    result.viewport_height_px = viewport_height;
    return result;
}

// The LOD policy both labs start from.
[[nodiscard]] inline planet::QuadtreeLodConfig defaultPlanetLod(double planet_radius_m) {
    planet::QuadtreeLodConfig lod{};
    lod.planet_radius_m = planet_radius_m;
    lod.root_geometric_error_m = 200'000.0;
    lod.split_threshold_px = 3.0;
    lod.merge_threshold_px = 2.0;
    lod.max_level = 18;
    lod.leaf_budget = 4096;
    return lod;
}

} // namespace helios::examples
