// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors

#include "solar_system.hpp"

#include "helios/math/vec.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace helios::examples {
namespace {

char const* const kBodyNames[kSolarSystemBodyCount] = {
    "Sun", "Mercury", "Venus", "Earth", "Mars", "Jupiter", "Saturn", "Uranus", "Neptune",
};

[[nodiscard]] PlanetBody makeRockyBody(double radius_m, math::Vec3d const& desert,
                                       math::Vec3d const& rock, double temperature_k,
                                       double craters, std::uint32_t seed) noexcept {
    PlanetBody body = planetBodyPreset(PlanetBodyKind::RockyMoon, seed);
    body.radius_m = radius_m;
    body.ocean = false;
    body.atmosphere = false;
    body.sea_level_m = -20'000.0;
    body.equator_temperature_k = temperature_k;
    body.pole_drop_k = 0.0;
    body.crater_density = craters;
    body.color_desert = desert;
    body.color_rock = rock;
    body.color_snow = desert;
    return body;
}

[[nodiscard]] PlanetBody makeGasGiant(double radius_m, double bands, math::Vec3d const& a,
                                      math::Vec3d const& b, math::Vec3d const& c, double storms,
                                      double temperature_k, std::uint32_t seed) noexcept {
    PlanetBody body = planetBodyPreset(PlanetBodyKind::GasGiant, seed);
    body.radius_m = radius_m;
    body.bands_per_hemisphere = bands;
    body.band_turbulence = 0.8;
    body.storm_density = storms;
    body.band_color_a = a;
    body.band_color_b = b;
    body.band_color_c = c;
    body.equator_temperature_k = temperature_k;
    body.pole_drop_k = 10.0;
    body.lapse_rate_k_per_m = 0.0;
    return body;
}

} // namespace

PlanetBody solarSystemBody(int index, std::uint32_t seed) noexcept {
    switch (index) {
    case 0:
        return planetBodyPreset(PlanetBodyKind::Sun, seed);
    case 1: {
        // Mercury: airless, heavily cratered, dark grey, hot.
        PlanetBody body = makeRockyBody(2'439'700.0, {0.16, 0.15, 0.14}, {0.10, 0.095, 0.09},
                                        440.0, 0.65, seed);
        body.land_bias = 0.25;
        body.mountain_amplitude_m = 3000.0;
        body.detail_amplitude_m = 400.0;
        return body;
    }
    case 2: {
        // Venus: a cloud-shrouded rocky world; the patch shader has no
        // separate cloud layer here, so pale sulphur colours stand in.
        PlanetBody body = makeRockyBody(6'051'800.0, {0.52, 0.44, 0.26}, {0.36, 0.30, 0.18},
                                        737.0, 0.08, seed);
        body.land_bias = 0.9;
        body.ocean_floor_m = 0.0;
        body.platform_m = 300.0;
        body.base_roughness_m = 400.0;
        body.mountain_amplitude_m = 2000.0;
        return body;
    }
    case 3: {
        // Earth: the preset's ocean, land, ice and climate.
        PlanetBody body = planetBodyPreset(PlanetBodyKind::EarthLike, seed);
        body.radius_m = 6'371'000.0;
        body.equator_temperature_k = 300.0;
        body.pole_drop_k = 55.0;
        return body;
    }
    case 4: {
        // Mars: the desert preset with a deeper red palette.
        PlanetBody body = planetBodyPreset(PlanetBodyKind::DesertWorld, seed);
        body.radius_m = 3'389'500.0;
        body.color_desert = {0.42, 0.20, 0.10};
        body.color_rock = {0.24, 0.12, 0.07};
        body.color_snow = {0.86, 0.84, 0.80};
        body.equator_temperature_k = 210.0;
        body.crater_density = 0.35;
        return body;
    }
    case 5:
        // Jupiter: ten bands, the Great Red Spot as a storm field, ochre and
        // cream; the equatorial zone is the brightest.
        return makeGasGiant(69'911'000.0, 10.0, {0.62, 0.46, 0.32}, {0.86, 0.78, 0.64},
                            {0.38, 0.24, 0.16}, 0.7, 165.0, seed);
    case 6:
        // Saturn: pale gold, broad bands, a strong storm field.
        return makeGasGiant(58'232'000.0, 8.0, {0.72, 0.62, 0.44}, {0.90, 0.84, 0.70},
                            {0.50, 0.40, 0.26}, 0.45, 134.0, seed);
    case 7:
        // Uranus: a nearly featureless cyan disk; few, wide bands.
        return makeGasGiant(25'362'000.0, 4.0, {0.38, 0.62, 0.64}, {0.56, 0.76, 0.76},
                            {0.28, 0.48, 0.52}, 0.15, 76.0, seed);
    case 8:
        // Neptune: deeper blue, a dark storm, faint banding.
        return makeGasGiant(24'622'000.0, 5.0, {0.16, 0.30, 0.58}, {0.28, 0.46, 0.76},
                            {0.10, 0.18, 0.40}, 0.5, 72.0, seed);
    default:
        return planetBodyPreset(PlanetBodyKind::EarthLike, seed);
    }
}

char const* solarSystemBodyName(int index) noexcept {
    if (index < 0 || index >= static_cast<int>(kSolarSystemBodyCount)) {
        return "Body";
    }
    return kBodyNames[static_cast<std::size_t>(index)];
}

math::Vec3d solarSystemBodyPosition(int index, double days_since_j2000) noexcept {
    if (index <= 0 || index > static_cast<int>(kSolarSystemBodyCount) - 1) {
        return {};
    }
    SolarSystemBody const& body = kSolarSystemPlanets[static_cast<std::size_t>(index - 1)];
    return solarSystemPosition(body, days_since_j2000);
}

double solarSystemDisplayRadius(int index, double planet_scale, double sun_scale) noexcept {
    if (index <= 0) {
        return kSun.radius_m * std::max(sun_scale, 0.01);
    }
    if (index > static_cast<int>(kSolarSystemBodyCount) - 1) {
        return 0.0;
    }
    SolarSystemBody const& body = kSolarSystemPlanets[static_cast<std::size_t>(index - 1)];
    return body.radius_m * std::max(planet_scale, 0.01);
}

} // namespace helios::examples
