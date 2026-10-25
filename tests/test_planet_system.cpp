// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// The planetary system of the labs (examples/planet_lod_lab/planet_system.*
// and planet_surface.*): orbits, the Roche limit, the ring profile and the
// height field's base level.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "planet_surface.hpp"
#include "planet_system.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>

using namespace helios::examples;

TEST_CASE("a Moon-like moon orbits an Earth-like planet in 27 days") {
    PlanetSystem const system = planetSystemPreset(PlanetSystemKind::EarthLike, 7);
    REQUIRE(system.moon_count == 1);
    double const period_days = 2.0 * std::numbers::pi / moonMeanMotion(system, system.moons[0]) / 86400.0;
    CHECK(period_days == doctest::Approx(27.3).epsilon(0.03));
    // Half a period later the moon is on the far side.
    double const half = std::numbers::pi / moonMeanMotion(system, system.moons[0]);
    double const angle = moonOrbitalAngle(system, system.moons[0], half) - system.moons[0].phase_rad;
    CHECK(std::abs(std::remainder(angle - std::numbers::pi, 2.0 * std::numbers::pi)) < 1.0e-6);
}

TEST_CASE("the Roche limit for ice around a Saturn-like giant is 2.2 radii") {
    PlanetSystem const system = planetSystemPreset(PlanetSystemKind::RingedGasGiant, 7);
    double const roche = planetRocheLimit(system, 920.0) / system.body.radius_m;
    CHECK(roche == doctest::Approx(2.44 * std::cbrt(687.0 / 920.0)).epsilon(1.0e-6));
    CHECK(roche > 2.1);
    CHECK(roche < 2.3);
}

TEST_CASE("the ring profile is finite, non-negative and gapped at the first moon's 2:1 resonance") {
    PlanetSystem const system = planetSystemPreset(PlanetSystemKind::RingedGasGiant, 7);
    REQUIRE(system.rings.enabled);
    std::size_t const samples = 2048;
    std::vector<float> const profile = planetRingOpticalDepth(system, samples);
    REQUIRE(profile.size() == samples);
    float peak = 0.0f;
    for (float value : profile) {
        REQUIRE(std::isfinite(value));
        REQUIRE(value >= 0.0f);
        peak = std::max(peak, value);
    }
    CHECK(peak > 1.0f);
    CHECK(peak < static_cast<float>(system.rings.peak_optical_depth) * 3.0f);
    // The 2:1 inner Lindblad resonance of the innermost moon (Mimas' makes
    // the Cassini division) lies inside the rings and is nearly empty.
    double const resonance = system.moons[0].orbit_radius_m * std::cbrt(0.25);
    REQUIRE(resonance > system.rings.inner_m);
    REQUIRE(resonance < system.rings.outer_m);
    double const u = (resonance - system.rings.inner_m) / (system.rings.outer_m - system.rings.inner_m);
    auto const index = static_cast<std::size_t>(u * static_cast<double>(samples));
    CHECK(profile[std::min(index, samples - 1)] < 0.1f * peak);
    // A system without rings yields an all-zero profile of the asked size.
    PlanetSystem const bare = planetSystemPreset(PlanetSystemKind::EarthLike, 7);
    std::vector<float> const empty = planetRingOpticalDepth(bare, 16);
    REQUIRE(empty.size() == 16);
    CHECK(std::all_of(empty.begin(), empty.end(), [](float v) { return v == 0.0f; }));
}

TEST_CASE("on continental crust the land is floored at the sea level") {
    PlanetBody const body = planetBodyPreset(PlanetBodyKind::EarthLike, 7);
    REQUIRE(body.ocean);
    // Sample directions; where the full-octave height is land, count the
    // depressions below the sea level.  The base level should leave only
    // the coastal zone, where the floor fades out.
    int land = 0;
    int below_sea_inland = 0;
    double const golden = 2.399963229728653;
    for (int i = 0; i < 4000; ++i) {
        double const z = 1.0 - 2.0 * (i + 0.5) / 4000.0;
        double const ring = std::sqrt(std::max(1.0 - z * z, 0.0));
        double const phi = golden * i;
        helios::math::Vec3d const direction{ring * std::cos(phi), ring * std::sin(phi), z};
        double const coarse = planetHeight(body, direction, 7);
        double const full = planetHeight(body, direction, body.max_octaves);
        REQUIRE(std::isfinite(full));
        if (coarse > body.sea_level_m + 300.0) {
            ++land;
            if (full < body.sea_level_m) {
                ++below_sea_inland;
            }
        }
    }
    REQUIRE(land > 200);
    CHECK(below_sea_inland == 0);
}
