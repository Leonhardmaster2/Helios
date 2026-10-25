// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "solar_system.hpp"
#include "solar_system_data.hpp"

#include <cmath>
#include <string_view>

using namespace helios;
using namespace helios::examples;

TEST_CASE("the solar-system table has the Sun and eight planets") {
    CHECK(kSolarSystemPlanets.size() == 8);
    CHECK(std::string_view{kSun.name} == "Sun");
    for (std::size_t index = 0; index < kSolarSystemPlanets.size(); ++index) {
        SolarSystemBody const& body = kSolarSystemPlanets[index];
        CHECK(body.name != nullptr);
        CHECK(body.radius_m > 0.0);
        CHECK(body.semi_major_axis_m > 0.0);
        CHECK(body.orbital_period_days > 0.0);
        CHECK(body.mass_kg > 0.0);
        CHECK(body.eccentricity >= 0.0);
        CHECK(body.eccentricity < 1.0);
    }
}

TEST_CASE("J2000 positions are inside the published orbit") {
    for (SolarSystemBody const& body : kSolarSystemPlanets) {
        math::Vec3d const position = solarSystemPosition(body, 0.0);
        double const radius = math::length(position);
        double const perihelion = body.semi_major_axis_m * (1.0 - body.eccentricity);
        double const aphelion = body.semi_major_axis_m * (1.0 + body.eccentricity);
        CHECK(radius > perihelion * 0.999);
        CHECK(radius < aphelion * 1.001);
    }
}

TEST_CASE("Earth is near perihelion at J2000") {
    // J2000.0 is 2000 January 1.5; Earth's perihelion is around January 3.
    SolarSystemBody const& earth = kSolarSystemPlanets[2];
    CHECK(earth.eccentricity == doctest::Approx(0.0167086));
    double const radius_au = math::length(solarSystemPosition(earth, 0.0)) / kAstronomicalUnitM;
    CHECK(radius_au > 0.982);
    CHECK(radius_au < 0.985);
}

TEST_CASE("a full period returns a planet to its starting point") {
    for (SolarSystemBody const& body : kSolarSystemPlanets) {
        math::Vec3d const start = solarSystemPosition(body, 0.0);
        math::Vec3d const end = solarSystemPosition(body, body.orbital_period_days);
        // The Kepler solver is exact for two-body motion; allow a few metres
        // for the float-free double arithmetic and the wrapped mean anomaly.
        CHECK(math::length(end - start) < 10.0);
    }
}

TEST_CASE("the Sun is a star body and the planets are not") {
    PlanetBody const sun = solarSystemBody(0, 7);
    CHECK(sun.star);
    CHECK_FALSE(sun.gas_giant);
    CHECK(sun.radius_m == doctest::Approx(695'700'000.0));

    PlanetBody const earth = solarSystemBody(3, 7);
    CHECK_FALSE(earth.star);
    CHECK(earth.ocean);
    CHECK(earth.radius_m == doctest::Approx(6'371'000.0));

    PlanetBody const jupiter = solarSystemBody(5, 7);
    CHECK_FALSE(jupiter.star);
    CHECK(jupiter.gas_giant);
}

TEST_CASE("the star kind is encoded for the patch shader") {
    shaders::PlanetBodyParams const sun = planetBodyParams(solarSystemBody(0, 7));
    CHECK(sun.bands[0] > 1.5f); // 2 = star, 1 = gas giant, 0 = terrain
    shaders::PlanetBodyParams const earth = planetBodyParams(solarSystemBody(3, 7));
    CHECK(earth.bands[0] < 0.5f);
    CHECK(planetHeight(solarSystemBody(0, 7), {1.0, 0.0, 0.0}, 8) == 0.0);
}

TEST_CASE("display radii follow their separate exaggeration scales") {
    CHECK(solarSystemDisplayRadius(0, 100.0, 2.0) == doctest::Approx(2.0 * kSun.radius_m));
    CHECK(solarSystemDisplayRadius(3, 100.0, 2.0) ==
          doctest::Approx(100.0 * kSolarSystemPlanets[2].radius_m));
    CHECK(solarSystemDisplayRadius(0, 1.0, 1.0) == doctest::Approx(kSun.radius_m));
}
