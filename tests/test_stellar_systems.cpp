// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// The stars of the deep-sky lab's galaxy and their planets
// (examples/deep_sky_lab/stellar_systems.hpp): the mass function, stellar
// states against the Sun and textbook remnants, the bookkeeping that the
// shader mirrors, and the physical constraints the planets and moons obey.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "stellar_systems.hpp"

#include <cmath>
#include <numbers>

using namespace helios::examples;

namespace {

constexpr double kEarthMassKg = 5.9722e24;
constexpr double kSolarMassKg = 1.98892e30;
constexpr double kAuKm = 1.495978707e8;

[[nodiscard]] ParticleStars sampleParticle(std::uint32_t particle, double age_myr) {
    return particleStars(particle, 37500.0, age_myr, -age_myr, false, 30.0, 30.0);
}

} // namespace

TEST_CASE("the Kroupa mass function: mean mass, counts and its inverse") {
    CHECK(imfMeanMass() == doctest::Approx(0.579).epsilon(0.01));
    CHECK(starsInParticle(37500.0) == doctest::Approx(64714.0).epsilon(0.001));
    CHECK(imfTailFraction(kImfMinimumMass) == doctest::Approx(1.0));
    CHECK(imfTailFraction(kImfMaximumMass) == doctest::Approx(0.0).epsilon(1.0e-9));
    for (double mass : {0.1, 0.3, 0.5, 0.8, 1.0, 3.0, 10.0, 60.0}) {
        CHECK(imfTailMass(imfTailFraction(mass)) == doctest::Approx(mass).epsilon(1.0e-6));
    }
    // About 8 % of stars are more massive than the Sun.
    CHECK(imfTailFraction(1.0) == doctest::Approx(0.08).epsilon(0.25));
}

TEST_CASE("single stars: the Sun, giants and remnants") {
    StarState const sun = evolveStar(1.0, 4600.0);
    CHECK(sun.phase == StarPhase::MainSequence);
    CHECK(sun.luminosity == doctest::Approx(1.0).epsilon(0.1));
    CHECK(sun.radius == doctest::Approx(1.0).epsilon(0.1));
    CHECK(sun.temperature_k == doctest::Approx(5772.0).epsilon(0.05));
    CHECK(sun.main_sequence_myr == doctest::Approx(10000.0).epsilon(0.01));
    CHECK(spectralClass(sun).front() == 'G');

    StarState const giant = evolveStar(1.0, 10800.0);
    CHECK(giant.phase == StarPhase::Giant);
    CHECK(giant.luminosity > 2.0);
    CHECK(giant.radius > 2.0);

    StarState const dwarf = evolveStar(1.0, 20000.0);
    CHECK(dwarf.phase == StarPhase::WhiteDwarf);
    CHECK(dwarf.mass == doctest::Approx(0.503).epsilon(0.01));
    CHECK(dwarf.radius < 0.02);
    // The Sun swells to about an astronomical unit at the tip of its giant
    // branches.
    CHECK(dwarf.largest_radius > 100.0);

    CHECK(evolveStar(15.0, 100.0).phase == StarPhase::NeutronStar);
    CHECK(evolveStar(40.0, 100.0).phase == StarPhase::BlackHole);
    CHECK(evolveStar(0.2, 13000.0).phase == StarPhase::MainSequence);

    for (double age : {10.0, 100.0, 1000.0, 10000.0}) {
        CHECK(deadMassAt(age) >= turnoffMassAt(age));
        CHECK(evolveStar(turnoffMassAt(age) * 0.99, age).phase == StarPhase::MainSequence);
        CHECK(evolveStar(deadMassAt(age) * 1.01, age).phase != StarPhase::MainSequence);
    }
    CHECK(turnoffMassAt(10000.0) == doctest::Approx(1.0).epsilon(0.01));
}

TEST_CASE("slots and ranks of a particle's stars") {
    ParticleStars const stars = sampleParticle(17, 5000.0);
    REQUIRE(stars.count > 60000U);
    REQUIRE(stars.dead > 0U);
    REQUIRE(stars.giants > 0U);
    for (std::uint32_t slot = 0; slot < stars.count; slot += 7U) {
        CHECK(slotOfRank(stars, rankOfSlot(stars, slot)) == slot);
    }
    // The living stars come first, giants then the main sequence downwards;
    // the remnants last.
    std::uint32_t const living = stars.count - stars.dead;
    CHECK(evolveStar(initialMassOfRank(stars, rankOfSlot(stars, 0)), stars.age_myr).phase == StarPhase::Giant);
    CHECK(evolveStar(initialMassOfRank(stars, rankOfSlot(stars, living)), stars.age_myr).phase != StarPhase::MainSequence);
    double previous = 1.0e30;
    for (std::uint32_t slot = stars.giants + 2U; slot < living; slot += 97U) {
        double const mass = initialMassOfRank(stars, rankOfSlot(stars, slot));
        CHECK(mass <= previous);
        previous = mass;
    }
    // Every star counted bright enough is.
    double const threshold = 0.5;
    std::uint32_t const bright = slotsBrighterThan(stars, threshold);
    CHECK(bright >= stars.giants);
    for (std::uint32_t slot = stars.giants; slot + 1U < bright; slot += 13U) {
        StarState const star = evolveStar(initialMassOfRank(stars, rankOfSlot(stars, slot)), stars.age_myr);
        CHECK(star.luminosity > 0.5 * threshold);
    }
}

TEST_CASE("the light of the brightest stars matches its table") {
    ResolvedLightTable const table;
    for (double age : {10.0, 300.0, 3000.0, 12000.0}) {
        ParticleStars const stars = sampleParticle(5, age);
        double total = 0.0;
        double brightest = 0.0;
        for (std::uint32_t slot = 0; slot < stars.count; ++slot) {
            double const light = evolveStar(initialMassOfRank(stars, rankOfSlot(stars, slot)), age).luminosity;
            total += light;
            if (slot < 256U) {
                brightest += light;
            }
        }
        double const fraction = brightest / total;
        CHECK(table.fraction(age, 256.0 / stars.count) == doctest::Approx(fraction).epsilon(0.08));
        CHECK(table.fraction(age, 1.0) == doctest::Approx(1.0).epsilon(0.01));
        CHECK(table.fraction(age, 0.0) == 0.0);
    }
}

TEST_CASE("planetary systems are deterministic and physically consistent") {
    std::uint32_t systems_with_planets = 0;
    for (std::uint32_t particle = 100; particle < 140; ++particle) {
        ParticleStars const stars = sampleParticle(particle, 4600.0);
        std::uint32_t const rank = static_cast<std::uint32_t>(imfTailFraction(1.0) * stars.count) + particle;
        StarSystem const a = generateStarSystem(stars, rank);
        StarSystem const b = generateStarSystem(stars, rank);
        REQUIRE(a.planet_count == b.planet_count);
        CHECK(a.name == b.name);
        REQUIRE(a.star.phase == StarPhase::MainSequence);
        CHECK(a.planet_count >= 3U);
        CHECK(a.planet_count <= kMaxPlanets);
        systems_with_planets += a.planet_count > 0 ? 1U : 0U;
        double const star_mass = a.star.mass;
        for (std::uint32_t p = 0; p < a.planet_count; ++p) {
            Planet const& planet = a.planets[p];
            CHECK(planet.orbit_au == b.planets[p].orbit_au);
            CHECK(planet.mass_earth > 0.0);
            CHECK(planet.radius_earth > 0.1);
            // Kepler's third law.
            double const years = planet.period_days / 365.25;
            CHECK(years * years == doctest::Approx(planet.orbit_au * planet.orbit_au * planet.orbit_au /
                                                   (star_mass + planet.mass_earth * 3.0035e-6))
                                       .epsilon(1.0e-6));
            if (p > 0) {
                Planet const& inner = a.planets[p - 1U];
                double const mutual = std::cbrt((inner.mass_earth + planet.mass_earth) * kEarthMassKg /
                                                (3.0 * star_mass * kSolarMassKg)) *
                                      0.5 * (inner.orbit_au + planet.orbit_au);
                CHECK(planet.orbit_au - inner.orbit_au >= 9.99 * mutual);
            }
            // Moons inside 0.4 Hill radii.
            double const hill_km = planet.orbit_au * (1.0 - planet.eccentricity) * kAuKm *
                                   std::cbrt(planet.mass_earth * kEarthMassKg / (3.0 * star_mass * kSolarMassKg));
            for (std::uint32_t m = 0; m < planet.moon_count; ++m) {
                Moon const& moon = planet.moons[m];
                CHECK(moon.orbit_km <= 0.4 * hill_km * 1.0001);
                CHECK(moon.orbit_km > planet.radius_earth * 6371.0 * 2.0);
                CHECK(moon.radius_km > 0.0);
            }
            // Back where it was after one period, and between periapsis and
            // apoapsis on the way.
            helios::math::Vec3d const start = planetPosition(planet, 1.0e15, 0.0);
            helios::math::Vec3d const later = planetPosition(planet, 1.0e15, planet.period_days * 86400.0);
            CHECK(helios::math::length(start - later) < 1.0e-6 * planet.orbit_au);
            double const r = helios::math::length(planetPosition(planet, 1.0e15, planet.period_days * 86400.0 / 3.0));
            CHECK(r >= planet.orbit_au * (1.0 - planet.eccentricity) * 0.999999);
            CHECK(r <= planet.orbit_au * (1.0 + planet.eccentricity) * 1.000001);
        }
    }
    CHECK(systems_with_planets == 40U);
}

TEST_CASE("evolved stars keep only the planets that survive") {
    // Remnants of massive stars: the supernova unbinds every planet.
    for (std::uint32_t rank = 0; rank < 20; ++rank) {
        ParticleStars const stars = sampleParticle(9, 300.0);
        StarSystem const system = generateStarSystem(stars, rank);
        if (system.star.phase == StarPhase::NeutronStar || system.star.phase == StarPhase::BlackHole) {
            CHECK(system.planet_count == 0U);
        }
    }
    // White dwarfs: nothing left inside twice the largest radius, orbits
    // widened by the mass lost.
    ParticleStars const old = sampleParticle(11, 12000.0);
    for (std::uint32_t rank = old.dead - 200U; rank < old.dead; ++rank) {
        StarSystem const system = generateStarSystem(old, rank);
        if (system.star.phase != StarPhase::WhiteDwarf) {
            continue;
        }
        double const engulfed_au = 2.0 * system.star.largest_radius * 0.00465047;
        for (std::uint32_t p = 0; p < system.planet_count; ++p) {
            CHECK(system.planets[p].orbit_au > engulfed_au);
        }
    }
}
